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

// Research: TDM Hooks.cpp::ProcessTracking and UpdateCameraHeadtracking;
// SmoothCam thirdperson.cpp::SetCameraRotation; ImprovedCameraSE-NG
// ImprovedCameraSE.cpp::UpdateHeadTracking. Use native graph look-at, not bone
// rotations. Keep camera state restrictions and yield to native target slots.
// 1.7.104 corpus: ProcessTracking ID 38009 / 1406B3F20, point target ID 39887 /
// 140723510, highest active target type ID 39486 / 1406FF0E0. The latter reads
// AIProcess+10 -> HighProcess+158 (six priority flags), without changing them.
// NiCamera::WindowPointToRay ID 70630 / 140EF0050 confirms forward is column 0
// of world.rotate (7C, 88, 94); parent camera-root axes are different.
using TProcessTracking = void(Actor*, float, NiAVObject*);
TProcessTracking* s_processTracking = nullptr;

// References: https://github.com/alandtse/CommonLibSSE-NG/blob/master/include/RE/B/BSLookAtModifier.h
// https://github.com/adamhynek/activeragdoll/blob/master/src/main.cpp (modifier phase)
// https://github.com/ersh1/TrueDirectionalMovement/blob/master/src/Hooks.cpp (spine/priority)
// CommonLibSSE-NG BSLookAtModifier; confirmed in 1.7.104 modifyInternal,
// ID 63350 / 140BBEAB0. Only the overall target cone is overridden, never
// the individual bone limits/gains used by ID 63351 / 140BBF5C0.
struct LookAtModifier
{
    uint8_t Pad[0x78];
    float LimitAngleDegrees;
};
static_assert(offsetof(LookAtModifier, LimitAngleDegrees) == 0x78);
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
    uint64_t AppliedAt{};
};
std::mutex s_graphLock;
std::unordered_map<uint32_t, GraphOverride> s_graphOverrides;

void HookModifyLookAt(LookAtModifier* apModifier, const void* apContext, void* apOutput)
{
    // hkbContext+0 is the character, also read by native modify (63278).
    // Match a registered graph address instead of guessing the character's
    // owner or dereferencing a retained actor from an animation worker.
    const void* pCharacter = apContext ? *static_cast<const void* const*>(apContext) : nullptr;
    bool cameraTarget = false;
    if (pCharacter)
    {
        const auto now = GetTickCount64();
        std::lock_guard lock(s_graphLock);
        for (const auto& [id, state] : s_graphOverrides)
        {
            if (state.Character == pCharacter && now - state.AppliedAt <= kStaleMs)
            {
                cameraTarget = true;
                break;
            }
        }
    }
    if (!cameraTarget)
        return s_modifyLookAt(apModifier, apContext, apOutput);

    // Outside the cone, modifyInternal either switches tracking off or
    // constructs a yaw-only direction about world up, losing target pitch.
    // Accept our full 3D camera target for this evaluation; the native bone
    // limits still distribute a bounded rotation over spine, neck and head.
    const float limit = apModifier->LimitAngleDegrees;
    apModifier->LimitAngleDegrees = 180.f;
    s_modifyLookAt(apModifier, apContext, apOutput);
    apModifier->LimitAngleDegrees = limit;
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
    aLook = {std::atan2(-z, std::sqrt(x * x + y * y)), std::atan2(x, y)};
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
}

void HookProcessTracking(Actor* apActor, float aDelta, NiAVObject* apObject)
{
    if (!apActor)
        return s_processTracking(apActor, aDelta, apObject);

    // Restore before the native pass so its own targets and combat decisions
    // see the original flags. The graph retains our override only until then.
    RestoreGraph(apActor);
    s_processTracking(apActor, aDelta, apObject);

    glm::vec2 look{};
    if (apActor == PlayerCharacter::Get())
    {
        if (!LocalCameraLook(look, false))
            return;
    }
    else
    {
        const auto* pExtension = apActor->GetExtension();
        if (!pExtension || !pExtension->IsRemotePlayer() || !CanTrack(apActor) || NativeTargetWins(apActor))
            return;
        std::lock_guard lock(s_presentedLock);
        const auto it = s_presented.find(apActor->formID);
        if (it == s_presented.end() || !it->second.Present ||
            GetTickCount64() - it->second.ReceivedAt > kStaleMs)
            return;
        look = it->second.Look;
    }

    auto* pRoot = apActor->GetNiNode();
    if (!pRoot)
        return;
    static BSFixedString s_head("NPC Head [Head]");
    auto* pHead = pRoot->GetByName(s_head);
    if (!pHead)
        return;
    static BSFixedString s_isNPC("IsNPC");
    static BSFixedString s_spine("bHeadTrackSpine");
    GraphOverride previous{apActor, pRoot, apActor->currentProcess};
    if (!apActor->animationGraphHolder.GetVariableBool(&s_isNPC, &previous.IsNPC) ||
        !apActor->animationGraphHolder.GetVariableBool(&s_spine, &previous.Spine))
        return;
    previous.HeadTracking = (apActor->actorState.flags2 & (1u << 3)) != 0;
    BSAnimationGraphManager* pManager{};
    if (!apActor->animationGraphHolder.GetBSAnimationGraph(&pManager) || !pManager)
        return;
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
        return;
    previous.AppliedAt = GetTickCount64();
    {
        std::lock_guard lock(s_graphLock);
        s_graphOverrides[apActor->formID] = previous;
    }
    apActor->actorState.flags2 |= 1u << 3;
    apActor->animationGraphHolder.SetVariableBool(&s_isNPC, true);
    // Let the native per-bone limits share some look rotation with the upper
    // body, as TDM does. RestoreGraph restores this before native targeting.
    apActor->animationGraphHolder.SetVariableBool(&s_spine, true);
    const float horizontal = std::cos(look.x);
    NiPoint3 target = pHead->world.translate;
    target.x += 500.f * std::sin(look.y) * horizontal;
    target.y += 500.f * std::cos(look.y) * horizontal;
    target.z -= 500.f * std::sin(look.x);
    using TSetTarget = void(AIProcess*, Actor*, NiPoint3&);
    POINTER_SKYRIMSE(TSetTarget, s_setTarget, 39887);
    s_setTarget.Get()(apActor->currentProcess, apActor, target);
}

static TiltedPhoques::Initializer s_headTrackHooks([]()
{
    // Both Character and PlayerCharacter vtable slot 0x122 resolve to this
    // native function in 1.7.104. Chain the original, never replace NPC logic.
    POINTER_SKYRIMSE(TProcessTracking, s_tracking, 38009);
    s_processTracking = s_tracking.Get();
    TP_HOOK(&s_processTracking, HookProcessTracking);
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
    // RestoreGraph runs on each actor's next native tracking pass. Do not
    // dereference engine actors from the network disconnect callback.
}

void HeadTrackService::OnUpdate(const UpdateEvent&) noexcept
{
    const auto now = GetTickCount64();
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
    }
}
