#include <TiltedOnlinePCH.h>
#include <Services/Generic/HeadTrackService.h>

#include <World.h>
#include <Games/References.h>
#include <PlayerCharacter.h>
#include <AI/AIProcess.h>
#include <BSAnimationGraphManager.h>
#include <Camera/PlayerCamera.h>
#include <Camera/TESCameraState.h>
#include <Interface/UI.h>
#include <NetImmerse/NiCamera.h>
#include <NetImmerse/NiNode.h>
#include <Events/DisconnectedEvent.h>
#include <Events/UpdateEvent.h>
#include <Messages/ServerReferencesMoveRequest.h>
#include <Structs/Movement.h>

#include <cmath>

namespace
{
constexpr float kPi = static_cast<float>(TiltedPhoques::Pi);
constexpr float kTau = 2.f * kPi;
constexpr uint64_t kStaleMs = 1000;
constexpr size_t kMaxSamples = 32;
constexpr float kLookStrength = 0.75f; // TDM's default; retain native bone/cone limits.

// Research: TDM Hooks.cpp::ProcessTracking and UpdateCameraHeadtracking;
// SmoothCam thirdperson.cpp::SetCameraRotation; ImprovedCameraSE-NG
// ImprovedCameraSE.cpp::UpdateHeadTracking. Use native graph look-at, not bone
// rotations. Keep camera state restrictions and yield to native target slots.
// 1.7.104 corpus: ProcessTracking ID 38009 / 1406B3F20, point target ID 39887 /
// 140723510, highest active target type ID 39486 / 1406FF0E0. The latter reads
// AIProcess+10 -> HighProcess+158 (six priority flags), without changing them.
// NiCamera::WindowPointToRay ID 70630 / 140EF0050 confirms forward is column 0
// of world.rotate (7C, 88, 94); parent camera-root axes are different.
// Reference-research handoff (docs/REFERENCE_RESEARCH.md is outside edit scope):
// TDM 57b913a src/Hooks.cpp:1394-1534 and DirectionalMovementHandler.cpp:2307-2351:
// adopt native point targeting and Settings.h:113's 0.75 strength; retain the
// native limits instead of the round-6 180-degree override. Its optional spine
// tracking is disabled here because our remote pose excludes only head/neck.
// SmoothCam 66f3960 SmoothCam/source/thirdperson.cpp:685-711 confirms the camera
// axis. ImprovedCameraSE-NG 2e441c1 ImprovedCamera/source/skyrimse/
// ImprovedCameraSE.cpp:506-528,1411-1446 supports first-person restrictions and
// rendered view rotation. GTS_Plugin 1798652 src/hooks/headTracking.cpp:9-44
// also adjusts a full 3D point and calls 39887; its scale correction/custom
// GTSPitchOverride behavior is not appropriate for unscaled vanilla players.
// hiryuu19's published source at nexusmods.com/skyrim/mods/58146?tab=posts uses
// IsNPC/SetHeadTracking/SetLookAt; reject its failed direct head-node rotation.
// ConditionalExpressionsExtended Source/Scripts/CondiExp_Expression_Util.psc:53
// changes facial expressions, not the skeletal camera target.
using TProcessTracking = void(Actor*, float, NiAVObject*);
TProcessTracking* s_processTracking = nullptr;
using TGetLookAt = bool(Actor*);
TGetLookAt* s_getLookAt = nullptr;

// References: https://github.com/alandtse/CommonLibSSE-NG/blob/master/include/RE/B/BSLookAtModifier.h
// https://github.com/adamhynek/activeragdoll/blob/master/src/main.cpp (modifier phase)
// https://github.com/ersh1/TrueDirectionalMovement/blob/master/src/Hooks.cpp (spine/priority)
// Read-only view for confirming that the submitted target reaches the modifier.
// 63278 / 140BBA0C0 checks allBonesValid and bones.size before calling
// 63350 / 140BBEAB0; 63351 / 140BBF5C0 applies the per-bone gains/limits.
struct LookAtModifier
{
    uint8_t Pad[0x50];
    bool LookAtTarget;
    uint8_t Pad51[0x60 - 0x51];
    int32_t BoneCount;
    uint8_t Pad64[0x90 - 0x64];
    float TargetLocation[4];
    bool TargetOutsideLimits;
    uint8_t PadA1[0xB8 - 0xA1];
    bool LookAtCamera;
    uint8_t PadB9[0xCC - 0xB9];
    bool AllBonesValid;
};
static_assert(offsetof(LookAtModifier, TargetLocation) == 0x90);
static_assert(offsetof(LookAtModifier, AllBonesValid) == 0xCC);
using TModifyLookAt = void(LookAtModifier*, const void*, void*);
TModifyLookAt* s_modifyLookAt = nullptr;

struct PresentedLook
{
    glm::vec2 Look{};
    uint64_t ReceivedAt{};
    bool Present{};
};
std::mutex s_presentedLock;
std::unordered_map<uint32_t, PresentedLook> s_presented;

struct GraphOverride
{
    Actor* ActorPtr{};
    NiNode* Root{};
    AIProcess* Process{};
    bool IsNPC{};
    bool Spine{};
    bool HeadTracking{};
    const void* Character{};
    glm::vec2 Look{};
    NiPoint3 Target{};
    uint64_t SubmittedAt{};
    uint64_t AppliedAt{};
    uint64_t LoggedAt{};
};
std::mutex s_graphLock;
std::unordered_map<uint32_t, GraphOverride> s_graphOverrides;

void HookModifyLookAt(LookAtModifier* apModifier, const void* apContext, void* apOutput)
{
    // hkbContext+0 is the character, also read by native modify (63278).
    // Match a registered graph address instead of guessing the character's
    // owner or dereferencing a retained actor from an animation worker.
    const void* pCharacter = apContext ? *static_cast<const void* const*>(apContext) : nullptr;
    // Observe the actual native modifier, not just a successful SetVariable.
    // Do not enlarge its cone to 180 degrees: TDM bounds the requested look.
    s_modifyLookAt(apModifier, apContext, apOutput);
    // The same generator-output gates used by 63278 before modifyInternal.
    const auto* pOutput = apOutput ? *static_cast<const uint8_t* const*>(apOutput) : nullptr;
    if (pCharacter && pOutput && *reinterpret_cast<const int32_t*>(pOutput + 4) > 2 &&
        *reinterpret_cast<const int16_t*>(pOutput + 0x32) > 0)
    {
        const auto now = GetTickCount64();
        std::lock_guard lock(s_graphLock);
        for (auto& [id, state] : s_graphOverrides)
        {
            if (state.Character == pCharacter && now - state.SubmittedAt <= kStaleMs)
            {
                const auto& target = state.Target;
                if (apModifier->LookAtTarget && !apModifier->LookAtCamera &&
                    !apModifier->TargetOutsideLimits && apModifier->AllBonesValid &&
                    apModifier->BoneCount > 0 &&
                    std::abs(apModifier->TargetLocation[0] - target.x) < 0.1f &&
                    std::abs(apModifier->TargetLocation[1] - target.y) < 0.1f &&
                    std::abs(apModifier->TargetLocation[2] - target.z) < 0.1f)
                    state.AppliedAt = now;
                break;
            }
        }
    }
}

float ShortestAngle(float aFrom, float aTo) noexcept
{
    return std::remainder(aTo - aFrom, kTau);
}

bool CanTrack(const Actor* apActor) noexcept
{
    if (!apActor || (apActor->flags1 & (1u << 3)) != 0 ||
        !apActor->currentProcess || !apActor->currentProcess->middleProcess ||
        !apActor->currentProcess->unk8)
        return false;
    const auto flags = apActor->actorState.flags1;
    // ActorState1: life 21..24, knock 25..27, attack 28..31; sleeping = 7.
    // Actor::BOOL_BITS::kHasSceneExtra above yields to scripted scenes (TDM).
    // Do not force a head/spine animation over death, ragdoll, or combat.
    return (flags & 0xFFE00000u) == 0 && ((flags >> 14) & 0xF) != 7 &&
        (flags & (1u << 8)) == 0 && (apActor->actorState.flags2 & (1u << 8)) == 0;
}

bool NativeTargetWins(Actor* apActor) noexcept
{
    using TTargetType = uint32_t(AIProcess*);
    POINTER_SKYRIMSE(TTargetType, s_targetType, 39486);
    // Action, script, combat, dialogue and procedure targets all win. The
    // incidental default target (0) is the only slot camera look supersedes.
    return s_targetType.Get()(apActor->currentProcess) != 0;
}

bool ReadCameraLook(glm::vec2& aLook, bool aAllowFirstPerson) noexcept
{
    auto* pCamera = PlayerCamera::Get();
    if (!pCamera || !pCamera->state || !pCamera->cameraNode)
        return false;
    const auto state = pCamera->state->id;
    // Native first person, third person, horse and dragon. Exclude vanity,
    // free camera, tween, killmove, furniture and bleedout cameras.
    if (state != 9 && state != 10 && state != 12 && !(aAllowFirstPerson && state == 0))
        return false;
    auto* pCameraNode = pCamera->GetNiCamera();
    if (!pCameraNode)
        return false;
    // NiCamera's forward axis is column 0; the camera root uses column 1.
    // Read the rendered camera so SmoothCam's free rotation is included.
    const auto& rotation = pCameraNode->world.rotate;
    const float x = rotation.entry[0][0];
    const float y = rotation.entry[1][0];
    const float z = rotation.entry[2][0];
    const float length = std::sqrt(x * x + y * y + z * z);
    if (!std::isfinite(length) || length < 0.001f)
        return false;
    const float horizontal = std::sqrt(x * x + y * y);
    // At a pitch pole, forward XY has no stable yaw. Camera column 2 is the
    // root's right axis (SmoothCam:705,708), so it retains heading there.
    const float yaw = horizontal < 0.001f * length ?
        std::atan2(-rotation.entry[1][2], rotation.entry[0][2]) : std::atan2(x, y);
    aLook = {std::atan2(-z, horizontal), yaw};
    if (!std::isfinite(aLook.y))
        return false;
    return true;
}

bool LocalCameraLook(glm::vec2& aLook, bool aAllowFirstPerson) noexcept
{
    auto* pPlayer = PlayerCharacter::Get();
    if (!CanTrack(pPlayer) || NativeTargetWins(pPlayer))
        return false;
    static BSFixedString s_dialogue("Dialogue Menu");
    static BSFixedString s_loading("Loading Menu");
    static BSFixedString s_creator("RaceSex Menu");
    if (auto* pUI = UI::Get(); pUI && (pUI->numPausesGame || pUI->GetMenuOpen(s_dialogue) ||
        pUI->GetMenuOpen(s_loading) || pUI->GetMenuOpen(s_creator)))
        return false;
    return ReadCameraLook(aLook, aAllowFirstPerson);
}

NiPoint3 CameraTarget(const Actor* apActor, const NiPoint3& aHead, const glm::vec2& aLook) noexcept
{
    // TDM DirectionalMovementHandler.cpp:2340-2351: attenuate camera pitch
    // and the yaw offset from the body, not the body's absolute heading.
    const float pitch = aLook.x * kLookStrength;
    const float yaw = apActor->rotation.z + ShortestAngle(apActor->rotation.z, aLook.y) * kLookStrength;
    const float horizontal = std::cos(pitch);
    NiPoint3 target = aHead;
    target.x += 500.f * std::sin(yaw) * horizontal;
    target.y += 500.f * std::cos(yaw) * horizontal;
    target.z -= 500.f * std::sin(pitch);
    return target;
}

void RestoreGraph(Actor* apActor) noexcept
{
    GraphOverride previous;
    {
        std::lock_guard lock(s_graphLock);
        const auto it = s_graphOverrides.find(apActor->formID);
        if (it == s_graphOverrides.end())
            return;
        previous = it->second;
        s_graphOverrides.erase(it);
    }
    // An unloaded/replaced graph must not inherit another graph's saved flags.
    if (previous.ActorPtr != apActor || previous.Root != apActor->GetNiNode() ||
        previous.Process != apActor->currentProcess)
        return;
    static BSFixedString s_isNPC("IsNPC");
    static BSFixedString s_spine("bHeadTrackSpine");
    apActor->animationGraphHolder.SetVariableBool(&s_isNPC, previous.IsNPC);
    apActor->animationGraphHolder.SetVariableBool(&s_spine, previous.Spine);
    apActor->actorState.flags2 = (apActor->actorState.flags2 & ~(1u << 3)) |
        (previous.HeadTracking ? (1u << 3) : 0u);
    if (apActor->currentProcess && !NativeTargetWins(apActor))
    {
        // 39889 / 140723580 clears the native channel's target-present bit.
        // Remote actors do not run native targeting to clear a stale camera
        // target for us. Never clear an active higher-priority native target.
        using TClearTarget = void(AIProcess*);
        POINTER_SKYRIMSE(TClearTarget, s_clearTarget, 39889);
        s_clearTarget.Get()(apActor->currentProcess);
    }
}

bool ApplyCameraTarget(Actor* apActor)
{
    if (!apActor)
        return false;

    glm::vec2 look{};
    if (apActor == PlayerCharacter::Get())
    {
        if (!LocalCameraLook(look, false))
        {
            RestoreGraph(apActor);
            return false;
        }
    }
    else
    {
        const auto* pExtension = apActor->GetExtension();
        if (!pExtension || !pExtension->IsRemotePlayer() || !CanTrack(apActor) || NativeTargetWins(apActor))
        {
            RestoreGraph(apActor);
            return false;
        }
        bool present = false;
        {
            std::lock_guard lock(s_presentedLock);
            const auto it = s_presented.find(apActor->formID);
            if (it != s_presented.end() && it->second.Present &&
                GetTickCount64() - it->second.ReceivedAt <= kStaleMs)
            {
                look = it->second.Look;
                present = true;
            }
        }
        if (!present)
        {
            RestoreGraph(apActor);
            return false;
        }
    }

    // TDM's disable mode yields when the camera is behind the character.
    if (std::abs(ShortestAngle(apActor->rotation.z, look.y)) > 2.f * kPi / 3.f)
    {
        RestoreGraph(apActor);
        return false;
    }
    auto* pRoot = apActor->GetNiNode();
    if (!pRoot)
    {
        RestoreGraph(apActor);
        return false;
    }
    static BSFixedString s_head("NPC Head [Head]");
    auto* pHead = pRoot->GetByName(s_head);
    if (!pHead)
    {
        RestoreGraph(apActor);
        return false;
    }
    static BSFixedString s_isNPC("IsNPC");
    static BSFixedString s_spine("bHeadTrackSpine");
    GraphOverride previous{apActor, pRoot, apActor->currentProcess};
    if (!apActor->animationGraphHolder.GetVariableBool(&s_isNPC, &previous.IsNPC) ||
        !apActor->animationGraphHolder.GetVariableBool(&s_spine, &previous.Spine))
    {
        RestoreGraph(apActor);
        return false;
    }
    previous.HeadTracking = (apActor->actorState.flags2 & (1u << 3)) != 0;
    BSAnimationGraphManager* pManager{};
    if (!apActor->animationGraphHolder.GetBSAnimationGraph(&pManager) || !pManager)
    {
        RestoreGraph(apActor);
        return false;
    }
    {
        BSScopedLock<BSRecursiveLock> lock(pManager->lock);
        const auto index = pManager->animationGraphIndex;
        if (index < pManager->animationGraphs.size)
        {
            if (auto* pGraph = pManager->animationGraphs.Get(index))
                previous.Character = &pGraph->character;
        }
    }
    pManager->Release();
    if (!previous.Character)
    {
        RestoreGraph(apActor);
        return false;
    }
    {
        std::lock_guard lock(s_graphLock);
        const auto it = s_graphOverrides.find(apActor->formID);
        if (it != s_graphOverrides.end() && it->second.ActorPtr == apActor &&
            it->second.Root == pRoot && it->second.Process == apActor->currentProcess &&
            it->second.Character == previous.Character)
            previous = it->second;
        previous.Look = look;
        previous.Target = CameraTarget(apActor, pHead->world.translate, look);
        previous.SubmittedAt = GetTickCount64();
        s_graphOverrides[apActor->formID] = previous;
    }
    // TDM exposes this as an option. Use head/neck only on both peers: the
    // remote pose stream owns the spine and excludes only head/neck bones.
    // Allocating pitch to the spine here would discard it at pose copy.
    if (!apActor->animationGraphHolder.SetVariableBool(&s_isNPC, true) ||
        !apActor->animationGraphHolder.SetVariableBool(&s_spine, false))
    {
        RestoreGraph(apActor);
        return false;
    }
    apActor->actorState.flags2 |= 1u << 3;
    using TSetTarget = void(AIProcess*, Actor*, NiPoint3&);
    POINTER_SKYRIMSE(TSetTarget, s_setTarget, 39887);
    s_setTarget.Get()(apActor->currentProcess, apActor, previous.Target);
    return true;
}

bool HookGetLookAt(Actor* apActor)
{
    // Research handoff for docs/REFERENCE_RESEARCH.md (outside this task's
    // write scope): 37964 / 1406AF230 reads MiddleHighProcess+326. Its sole
    // caller, ActorLookAtChannel 42782 / 1407C6FF0, supplies bHeadTracking.
    // Manager update 63358 / 140BC0680 samples channels before copying them
    // into each graph and before behavior evaluation (63587 / 140BCEDB0).
    // Unlike ProcessTracking this also runs for our remote graph-only path,
    // after received animation variables have been restored. Reapply the
    // native point target/IsNPC here, without re-enabling remote AI.
    ApplyCameraTarget(apActor);
    return s_getLookAt(apActor);
}

void HookProcessTracking(Actor* apActor, float aDelta, NiAVObject* apObject)
{
    // Keep the override stable while active, as TDM does. RestoreGraph only
    // runs when yielding (first person, native target, combat, stale packet).
    s_processTracking(apActor, aDelta, apObject);
    ApplyCameraTarget(apActor);
}

static TiltedPhoques::Initializer s_headTrackHooks([]()
{
    // Both Character and PlayerCharacter vtable slot 0x122 resolve to this
    // native function in 1.7.104. Chain the original, never replace NPC logic.
    POINTER_SKYRIMSE(TProcessTracking, s_tracking, 38009);
    s_processTracking = s_tracking.Get();
    TP_HOOK(&s_processTracking, HookProcessTracking);
    POINTER_SKYRIMSE(TGetLookAt, s_lookAt, 37964);
    s_getLookAt = s_lookAt.Get();
    TP_HOOK(&s_getLookAt, HookGetLookAt);
    // BSLookAtModifier::modify, vtable slot 23, ID 63278 / 140BBA0C0.
    // PLANCK also intercepts this phase; offsets above come from this exe.
    POINTER_SKYRIMSE(TModifyLookAt, s_modify, 63278);
    s_modifyLookAt = s_modify.Get();
    TP_HOOK(&s_modifyLookAt, HookModifyLookAt);
});
}

HeadTrackService::HeadTrackService(World& aWorld, entt::dispatcher& aDispatcher) noexcept
    : m_world(aWorld)
    , m_movementConnection(aDispatcher.sink<ServerReferencesMoveRequest>().connect<&HeadTrackService::OnMovement>(this))
    , m_disconnectedConnection(aDispatcher.sink<DisconnectedEvent>().connect<&HeadTrackService::OnDisconnected>(this))
    , m_updateConnection(aDispatcher.sink<UpdateEvent>().connect<&HeadTrackService::OnUpdate>(this))
{
}

void HeadTrackService::FillLocalMovement(Movement& aMovement) noexcept
{
    glm::vec2 look{};
    aMovement.HasLookDirection = LocalCameraLook(look, true);
    aMovement.LookDirection = 0;
    if (!aMovement.HasLookDirection)
        return;
    float yaw = std::fmod(look.y, kTau);
    if (yaw < 0.f)
        yaw += kTau;
    const auto packedYaw = static_cast<uint32_t>(std::lround(yaw * (65535.f / kTau)));
    const auto packedPitch = static_cast<uint32_t>(std::lround(
        std::clamp(look.x / kPi + 0.5f, 0.f, 1.f) * 65535.f));
    aMovement.LookDirection = (packedPitch << 16) | packedYaw;
}

bool HeadTrackService::IsCameraTracking(const Actor* apActor) noexcept
{
    if (!CanTrack(apActor))
        return false;
    std::lock_guard lock(s_graphLock);
    const auto it = s_graphOverrides.find(apActor->formID);
    return it != s_graphOverrides.end() && it->second.ActorPtr == apActor &&
        it->second.Process == apActor->currentProcess &&
        it->second.AppliedAt != 0 &&
        GetTickCount64() - it->second.AppliedAt <= kStaleMs;
}

void HeadTrackService::OnMovement(const ServerReferencesMoveRequest& acMessage) noexcept
{
    const auto now = GetTickCount64();
    // Only registered remote players can allocate look histories. NPC camera
    // fields, if received, are ignored and never touch their tracking graphs.
    auto players = m_world.view<PlayerComponent, RemoteComponent>();
    for (const auto entity : players)
    {
        const auto& remote = players.get<RemoteComponent>(entity);
        const auto update = acMessage.Updates.find(remote.Id);
        if (update == acMessage.Updates.end())
            continue;
        auto& track = m_tracks[remote.Id];
        if (track.Entity != entity || track.Epoch != remote.OwnershipEpoch || now - track.ReceivedAt > kStaleMs)
            track = Track{entity, remote.OwnershipEpoch};
        const auto& movement = update->second.UpdatedMovement;
        Sample sample{acMessage.Tick,
            {(static_cast<float>(movement.LookDirection >> 16) / 65535.f - 0.5f) * kPi,
             static_cast<float>(movement.LookDirection & 0xFFFF) * (kTau / 65535.f)},
            movement.HasLookDirection};
        auto& samples = track.Samples;
        auto it = std::lower_bound(samples.begin(), samples.end(), sample.Tick,
            [](const Sample& a, uint64_t tick) { return a.Tick < tick; });
        if (it != samples.end() && it->Tick == sample.Tick)
            *it = sample;
        else
            samples.insert(it, sample);
        while (samples.size() > kMaxSamples)
            samples.pop_front();
        track.ReceivedAt = now;
    }
}

void HeadTrackService::UpdateRemote(Actor* apActor, uint64_t aTick) noexcept
{
    if (!apActor || !apActor->GetExtension() || !apActor->GetExtension()->IsRemotePlayer())
        return;
    const auto token = Utils::GetRemoteOwnershipToken(apActor->formID);
    if (!token)
        return;
    const auto it = m_tracks.find(token->ServerId);
    if (it == m_tracks.end() || it->second.Epoch != token->OwnershipEpoch)
        return;
    auto& track = it->second;
    auto& samples = track.Samples;
    if (samples.empty())
        return;
    while (samples.size() > 2 && aTick >= samples[1].Tick)
        samples.pop_front();
    auto sample = samples.front();
    // No look from the future, no extrapolation across packet loss, and use
    // the discrete enable flag at its own timestamp (dialogue takes effect).
    if (aTick < sample.Tick || aTick - sample.Tick > kStaleMs)
        sample.Present = false;
    if (samples.size() > 1 && aTick >= sample.Tick)
    {
        const auto& next = samples[1];
        if (aTick >= next.Tick)
        {
            sample = next;
            if (aTick - sample.Tick > kStaleMs)
                sample.Present = false;
        }
        else if (sample.Present && next.Present && next.Tick - sample.Tick <= kStaleMs)
        {
            const float alpha = static_cast<float>(aTick - sample.Tick) / static_cast<float>(next.Tick - sample.Tick);
            sample.Look.x += (next.Look.x - sample.Look.x) * alpha;
            sample.Look.y += ShortestAngle(sample.Look.y, next.Look.y) * alpha;
        }
    }
    std::lock_guard lock(s_presentedLock);
    s_presented[apActor->formID] = {sample.Look, track.ReceivedAt, sample.Present};
}

void HeadTrackService::OnDisconnected(const DisconnectedEvent&) noexcept
{
    m_tracks.clear();
    std::lock_guard lock(s_presentedLock);
    s_presented.clear();
    // RestoreGraph runs on each actor's next tracking/channel pass. Do not
    // dereference engine actors from the network disconnect callback.
}

void HeadTrackService::OnUpdate(const UpdateEvent&) noexcept
{
    const auto now = GetTickCount64();
    static uint64_t s_pitchLoggedAt{};
    glm::vec2 look{};
    auto* pPlayer = PlayerCharacter::Get();
    if (pPlayer && now - s_pitchLoggedAt >= 2000 && ReadCameraLook(look, true) &&
        std::abs(look.x) > kPi / 6.f)
    {
        auto head = pPlayer->position;
        if (auto* pRoot = pPlayer->GetNiNode())
        {
            static BSFixedString s_head("NPC Head [Head]");
            if (auto* pHead = pRoot->GetByName(s_head))
                head = pHead->world.translate;
        }
        auto target = CameraTarget(pPlayer, head, look);
        glm::vec2 allowedLook{};
        bool applied = false;
        if (LocalCameraLook(allowedLook, false))
        {
            std::lock_guard lock(s_graphLock);
            const auto it = s_graphOverrides.find(pPlayer->formID);
            if (it != s_graphOverrides.end() && it->second.ActorPtr == pPlayer &&
                it->second.Root == pPlayer->GetNiNode() &&
                it->second.Process == pPlayer->currentProcess &&
                now - it->second.SubmittedAt <= kStaleMs)
            {
                target = it->second.Target;
                applied = it->second.AppliedAt != 0 && now - it->second.AppliedAt <= kStaleMs;
            }
        }
        // Degrees, positive down. applied means this target reached an active
        // native modifier recently; it is not a claim about rendered pixels.
        // Native first person deliberately reports false; its remote body
        // still receives the full camera look through FillLocalMovement.
        spdlog::info("Head track: pitch {} target {:.0f},{:.0f},{:.0f} applied={}",
            look.x * (180.f / kPi), target.x, target.y, target.z, applied);
        s_pitchLoggedAt = now;
    }
    std::erase_if(m_tracks, [&](const auto& entry)
    {
        return now - entry.second.ReceivedAt > kStaleMs || !m_world.valid(entry.second.Entity) ||
            !m_world.all_of<PlayerComponent, RemoteComponent>(entry.second.Entity);
    });
    {
        std::lock_guard lock(s_presentedLock);
        std::erase_if(s_presented, [now](const auto& entry) { return now - entry.second.ReceivedAt > kStaleMs; });
    }
    {
        std::lock_guard lock(s_graphLock);
        // A paused but loaded graph still needs its saved flags restored on
        // resuming. Only discard records for graphs that no longer exist;
        // resolve the live form rather than dereferencing a retained pointer.
        std::erase_if(s_graphOverrides, [](const auto& entry)
        {
            auto* pActor = Cast<Actor>(TESForm::GetById(entry.first));
            return pActor != entry.second.ActorPtr || pActor->GetNiNode() != entry.second.Root ||
                pActor->currentProcess != entry.second.Process;
        });
        for (auto& [id, state] : s_graphOverrides)
        {
            if (id == 0x14 || now - state.LoggedAt < 2000 ||
                now - state.SubmittedAt > kStaleMs || std::abs(state.Look.x) <= kPi / 6.f)
                continue;
            spdlog::info("Head track remote {:X}: pitch {} target {:.0f},{:.0f},{:.0f} applied={}",
                id, state.Look.x * (180.f / kPi), state.Target.x, state.Target.y, state.Target.z,
                state.AppliedAt != 0 && now - state.AppliedAt <= kStaleMs);
            state.LoggedAt = now;
        }
    }
}
