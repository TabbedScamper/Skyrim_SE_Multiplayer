#include <TiltedOnlinePCH.h>

#include <Systems/AnimationSystem.h>

#include <Games/Animation/TESActionData.h>
#include <Games/Animation/ActorMediator.h>
#include <Games/ActorExtension.h>
#include <Games/Skyrim/BSAnimationGraphManager.h>
#include <Games/Skyrim/Havok/ActorPoseDiagnosticViews.h>
#include <Games/Skyrim/NetImmerse/NiAVObject.h>
#include <Games/Skyrim/NetImmerse/NiNode.h>
#include <Combat/CombatController.h>
#include <Utils.h>

#include <Games/References.h>

#include <Forms/BGSAction.h>
#include <AI/AIProcess.h>
#include <Misc/MiddleProcess.h>

#include <Messages/ClientReferencesMoveRequest.h>

#include <Components.h>
#include <World.h>

#include <Forms/TESObjectCELL.h>
#include <Forms/TESWorldSpace.h>
#include <Games/Skyrim/Havok/PoseCopyAuthority.h>
#include <Services/GameTestService.h>
#include <Services/CorpseRagdollService.h>
#include <Services/CutsceneFollow.h>
extern thread_local bool g_mirroringLeaderIdle;
extern thread_local bool g_forceAnimation;
#include <PlayerCharacter.h>
#include <Forms/TESIdleForm.h>
#include <Services/TransportService.h>
#include <atomic>
#include <chrono>
#include <cmath>
#include <limits>
#include <mutex>
#include <unordered_map>
#include <utility>

// A/B switch (test command copy_native_tracking): run native head tracking/expressions after a copy's graph update.
std::atomic<bool> g_copyNativeTracking{true};

extern thread_local const char* g_animErrorCode;

namespace
{
// CommonLibSSE-NG BSAnimationUpdateData; native construction is ID 20119
// (0x1402FD930), filled by Actor slot 0x79, ID 38054 (0x1406B6300).
// https://github.com/CharmedBaryon/CommonLibSSE-NG/blob/main/include/RE/B/BSAnimationUpdateData.h
struct GraphUpdateData
{
    float Delta{};
    uint32_t Padding{};
    void* Callback{};
    TESObjectREFR* Reference{};
    const NiPoint3* EyePosition{};
    void* UpdateFunctor{};
    uint16_t MinimumBones{};
    bool ForceUpdate{};
    bool UseDistance{true};
    bool Visible{true};
    bool Unk2D{};
    bool Unk2E{true};
    bool Unk2F{};
};
static_assert(sizeof(GraphUpdateData) == 0x30);
static_assert(offsetof(GraphUpdateData, Visible) == 0x2C);

// Research handoff for docs/REFERENCE_RESEARCH.md (outside this task's write scope):
// PLANCK src/main.cpp::BShkbAnimationGraph_PreGenerate_Hook uses forceUpdate for
// active actors. Adopt that policy, not its VR cull-state offsets. CommonLib's
// BSAnimationUpdateData supplies the layout; the 1.7.104 corpus supplies semantics:
// 38054/1406B6300 fills visibility, 32899/140553FC0 gates the manager, and
// 63587/140BCEDB0 gates behavior update + its callback + generation on +2A.
// ForceUpdate bypasses both that gate and 63575/140BCC2C0 bone reduction.
// Preserve host action/callback processing as well as behavior evaluation.
struct AnimationInterest
{
    const Actor* ActorPtr{};
    float DistanceSquared{};
    uint64_t LastForcedTick{};
    size_t Rank{};
};
// Max-sync: every actor near a player is forced every frame (was a 64-actor rotating slice).
constexpr size_t kInterestActorsPerFrame = 512;
// Owner: every NPC the host runs is animated as if seen, even when the follower is across the map (same
// worldspace or cell). Was 4096 u around a player.
constexpr float kInterestRadius = 1.0e7f;
std::mutex s_interestLock;
std::unordered_map<uint32_t, AnimationInterest> s_animationInterest;
uint64_t s_interestPublishedAt{}, s_interestFrame{};
size_t s_interestFirst{};
std::atomic_bool s_hasAnimationInterest{};
// Conservative membership filter: collisions only take the existing lock.
// Publish the union before replacing the table, then the new bits afterward.
// No actor can be dropped because two form IDs hash to the same bit.
std::array<std::atomic<uint64_t>, 16> s_interestBits{};
size_t InterestBit(uint32_t aId) noexcept { return (aId * 2654435761u) >> 22; }
uint64_t s_interestFrames{}, s_interestActors{}, s_interestUs{}, s_interestMaxActors{}, s_interestMaxUs{};
uint64_t s_frameActors{}, s_frameUs{}, s_interestDeferred{}, s_interestOverflow{};
std::atomic<uint64_t> s_fallbackGraphUpdates{}, s_interestGraphUpdates{}, s_posePackets{};
std::atomic<uint64_t> s_remoteAnimationTasks{}, s_fallbackGraphUs{};
std::atomic<uint64_t> s_playerGraphCalls{}, s_playerGraphDeltaUs{}, s_playerGraphLongSteps{};

// Publish all nearby owned actors independently of this packet's pose selection.
// Only the serialization thread visits ECS. Native workers use pointer-checked,
// expiring entries and never dereference pointers taken from the registry.
void PublishAnimationInterest(World& aWorld, uint64_t aBatchTick)
{
    static uint64_t lastBatchTick = ~uint64_t{};
    if (lastBatchTick == aBatchTick)
        return;
    lastBatchTick = aBatchTick;
    const auto started = HostFrameCost::Begin();
    struct PublishCost
    {
        std::chrono::steady_clock::time_point Started;
        ~PublishCost() { HostFrameCost::End(9, Started); }
    } cost{started};
    std::vector<Actor*> observers;
    auto players = aWorld.view<FormIdComponent, PlayerComponent>();
    for (auto entity : players)
    {
        auto* actor = Cast<Actor>(TESForm::GetById(players.get<FormIdComponent>(entity).Id));
        const auto* extension = actor ? actor->GetExtension() : nullptr;
        if (extension && actor->formID != 0x14 && extension->IsRemote() &&
            actor->parentCell && actor->GetNiNode())
            observers.push_back(actor);
    }
    // The owner's own surroundings count as well: looking away from an NPC must not change how the owner
    // simulates it (owner repro 2026-09-27: horses float, carts break, whenever the host looks away).
    if (auto* self = PlayerCharacter::Get(); self && self->parentCell && self->GetNiNode() && !observers.empty())
        observers.push_back(self);
    std::vector<std::pair<uint32_t, AnimationInterest>> candidates;
    auto owned = aWorld.view<LocalComponent, LocalAnimationComponent, FormIdComponent>();
    for (auto entity : owned)
    {
        // Without a loaded receiver every distance test would fail. Publish the
        // empty table below as usual so previous receiver interest expires.
        if (observers.empty())
            break;
        auto* actor = Cast<Actor>(TESForm::GetById(owned.get<FormIdComponent>(entity).Id));
        const auto* extension = actor ? actor->GetExtension() : nullptr;
        if (!extension || actor->formID == 0x14 || extension->IsRemote() ||
            !actor->parentCell || !actor->currentProcess || !actor->GetNiNode() ||
            actor->IsDeleted() || actor->IsDisabled() || ((actor->actorState.flags1 >> 21) & 0x7F) != 0)
            continue;
        float nearest = (std::numeric_limits<float>::max)();
        for (const auto* observer : observers)
        {
            const auto* cell = actor->parentCell;
            if (cell != observer->parentCell &&
                (!cell->worldspace || cell->worldspace != observer->parentCell->worldspace))
                continue;
            const auto d = actor->position - observer->position;
            nearest = (std::min)(nearest, d.x * d.x + d.y * d.y + d.z * d.z);
        }
        if (nearest <= kInterestRadius * kInterestRadius)
            candidates.push_back({actor->formID, {actor, nearest}});
    }
    // Stable rank supports a rotating frame slice without sorting or allocating
    // on animation workers. Retain every candidate, including crowded cells.
    std::sort(candidates.begin(), candidates.end(), [](const auto& a, const auto& b) {
        return a.first < b.first;
    });
    size_t rank = 0;
    std::unordered_map<uint32_t, AnimationInterest> next;
    std::array<uint64_t, 16> bits{};
    next.reserve(candidates.size());
    for (auto& [id, interest] : candidates)
    {
        interest.Rank = rank++;
        next.emplace(id, interest);
        const auto bit = InterestBit(id);
        bits[bit / 64] |= uint64_t{1} << (bit % 64);
    }
    {
        // Allocate and reclaim tables outside the worker lock. Carry only the
        // duplicate-evaluation guard across publication, with pointer identity.
        std::lock_guard lock(s_interestLock);
        for (size_t i = 0; i < bits.size(); ++i)
            s_interestBits[i].fetch_or(bits[i], std::memory_order_release);
        for (auto& [id, interest] : next)
        {
            const auto it = s_animationInterest.find(id);
            if (it != s_animationInterest.end() && it->second.ActorPtr == interest.ActorPtr)
                interest.LastForcedTick = it->second.LastForcedTick;
        }
        s_interestOverflow = 0;
        s_animationInterest.swap(next);
        s_interestPublishedAt = GetTickCount64();
        s_hasAnimationInterest.store(!s_animationInterest.empty(), std::memory_order_release);
        for (size_t i = 0; i < bits.size(); ++i)
            s_interestBits[i].store(bits[i], std::memory_order_release);
    }
}

bool ClaimAnimationInterest(Actor* apActor)
{
    if (!s_hasAnimationInterest.load(std::memory_order_acquire))
        return false;
    const auto bit = InterestBit(apActor->formID);
    if (!(s_interestBits[bit / 64].load(std::memory_order_acquire) & (uint64_t{1} << (bit % 64))))
        return false;
    struct ClaimCost
    {
        std::chrono::steady_clock::time_point Started{HostFrameCost::Begin()};
        ~ClaimCost() { HostFrameCost::End(10, Started); }
    } cost;
    std::lock_guard lock(s_interestLock);
    const auto it = s_animationInterest.find(apActor->formID);
    if (it == s_animationInterest.end() || it->second.ActorPtr != apActor ||
        GetTickCount64() - s_interestPublishedAt > 250)
        return false;
    // SetCurrentTick is published once per main frame, unlike wall-clock buckets.
    const auto frame = PoseCopyAuthority::GetCurrentTick();
    if (!frame)
        return false;
    if (s_interestFrame != frame)
    {
        if (s_interestFrame && HostFrameCost::Session())
        {
            ++s_interestFrames;
            s_interestActors += s_frameActors;
            s_interestUs += s_frameUs;
            s_interestMaxActors = (std::max)(s_interestMaxActors, s_frameActors);
            s_interestMaxUs = (std::max)(s_interestMaxUs, s_frameUs);
        }
        s_interestFrame = frame;
        s_frameActors = s_frameUs = 0;
        s_interestFirst = (s_interestFirst + kInterestActorsPerFrame) % s_animationInterest.size();
    }
    auto& interest = it->second;
    if (interest.LastForcedTick == frame)
        return false;
    const auto rank = (interest.Rank + s_animationInterest.size() -
        s_interestFirst % s_animationInterest.size()) % s_animationInterest.size();
    if (rank >= kInterestActorsPerFrame || s_frameActors >= kInterestActorsPerFrame)
    {
        if (HostFrameCost::Session())
            ++s_interestDeferred;
        return false;
    }
    interest.LastForcedTick = frame;
    ++s_frameActors;
    return true;
}

// The host's camera cull suspends an actor's character controller (bhkCharacterController flags +0x218:
// kNoSim 1<<17, kFarAway 1<<18, kQuickSimulate 1<<20) while its AI path still moves it: owner-reported
// "horses float and carts go nuts whenever the host looks away", seen by the follower too. For owned actors a
// remote player is near (this interest set), keep the controller fully simulated. Muse diag-offscreen.
// Default off: holding kFarAway clear did not change the host float (measured 2026-09-28); kept as a switch.
std::atomic<bool> s_offscreenSimulate{false};
std::atomic<uint64_t> s_offscreenClears{};
constexpr uint32_t kControllerOffscreenBits = (1u << 17) | (1u << 18) | (1u << 20);

void KeepControllerSimulated(Actor* apActor) noexcept
{
    auto* process = apActor->currentProcess;
    if (!process || !process->middleProcess)
        return;
    auto* controller = *reinterpret_cast<uint8_t**>(reinterpret_cast<uint8_t*>(process->middleProcess) + 0x250);
    if (!controller)
        return;
    auto* flags = reinterpret_cast<uint32_t*>(controller + 0x218);
    if (*flags & kControllerOffscreenBits)
    {
        *flags &= ~kControllerOffscreenBits;
        s_offscreenClears.fetch_add(1, std::memory_order_relaxed);
    }
}

// Membership only (no frame claim): owned actor near any player, published within the last 250 ms.
bool HasAnimationInterest(uint32_t aFormId) noexcept
{
    if (!s_hasAnimationInterest.load(std::memory_order_acquire))
        return false;
    const auto bit = InterestBit(aFormId);
    if (!(s_interestBits[bit / 64].load(std::memory_order_acquire) & (uint64_t{1} << (bit % 64))))
        return false;
    std::lock_guard lock(s_interestLock);
    return s_animationInterest.contains(aFormId) && GetTickCount64() - s_interestPublishedAt <= 250;
}

// E37992 / 0x1406B1620 copies the actor 3D's culled bit (NiAVObject flags +0xF4 bit 20) into the character
// controller's kFarAway (+0x218 bit 18) every update. Recorded: 0x2280509 normally, 0x22C0509 exactly while
// the host looked away, when horses held height and dropped ~185 u (run 232211). Clearing it afterwards
// (21,839 clears) was too late: the engine had already used it. Decide it here for actors near any player.
using TUpdateFarAway = void(Actor*, void*);
TUpdateFarAway* s_realUpdateFarAway{};

void HookUpdateFarAway(Actor* apActor, void* apController)
{
    s_realUpdateFarAway(apActor, apController);
    if (!apActor || !apController || !s_offscreenSimulate.load(std::memory_order_relaxed))
        return;
    auto* flags = reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(apController) + 0x218);
    if (!(*flags & (1u << 18)))
        return;
    const auto* extension = apActor->GetExtension();
    // Owner: every NPC the host runs is treated as seen (not only those near a player).
    if (!extension || extension->IsRemote() || !World::Get().GetTransport().IsConnected())
        return;
    *flags &= ~(1u << 18);
    s_offscreenClears.fetch_add(1, std::memory_order_relaxed);
}

// "Render them all" at the cull itself (owner 2026-09-28). NiCullingProcess::SetFrustum (E71081) copies the camera
// frustum (left/right/top/bottom as slopes at unit distance, near, far, ortho) and builds the cull planes from it.
// Widening only these extents makes nearly everything within ~178 degrees pass the cull, so the engine treats it as
// seen and runs its full per-frame work (skeleton world transforms, physics->reference sync). What appears on screen
// is unchanged: the projection used for drawing is separate. Live switch wide_cull for paired A/B.
struct NiFrustumView { float Left, Right, Top, Bottom, Near, Far; bool Ortho; };
using TSetFrustum = void(void*, const NiFrustumView*);
TSetFrustum* s_realSetFrustum{};
std::atomic<bool> s_wideCull{false};
std::atomic<uint64_t> s_wideCullCalls{};

void HookSetFrustum(void* apProcess, const NiFrustumView* apFrustum)
{
    if (apFrustum && !apFrustum->Ortho && s_wideCull.load(std::memory_order_relaxed) &&
        World::Get().GetTransport().IsConnected())
    {
        NiFrustumView wide = *apFrustum;
        constexpr float kSlope = 50.f; // tan(88.9 deg)
        wide.Left = -kSlope; wide.Right = kSlope; wide.Top = kSlope; wide.Bottom = -kSlope;
        s_wideCullCalls.fetch_add(1, std::memory_order_relaxed);
        return s_realSetFrustum(apProcess, &wide);
    }
    s_realSetFrustum(apProcess, apFrustum);
}

// The engine's actual "is this actor seen" decision (read 2026-09-28). HighActorCuller vtable slot 1, E40335
// (0x14073C5C0), runs for every high-process actor each frame: it tests the actor 3D bound against the host camera
// frustum (0x141514CE0) and writes the result twice: NiAVObject flags +0xF4 bit 20 on the 3D root when outside, and
// the actor's visibility nibble +0x27C (bit 0 tested, bit 1 inside the frustum, bit 2 faded in; 7 = fully seen).
// Everything downstream reads those two results, not the renderer:
//  - E37992 copies bit 20 into the character controller's kFarAway (controller quick-simulates, reference lags);
//  - 0x1406B3F20 recomputes Actor boolBits kWasInFrustrum (+0xE8 bit 21) from +0x27C every update, which is why
//    latching that bit from the main thread did nothing;
//  - 0x140784240 (ProcessLists actor update) passes "visible" from bit 20 into the actor's update;
//  - E37434, 0x140707C70, AIProcess::RandomlyPlaySpecialIdles and 0x14067F1D0 branch on (+0x27C & 7) == 7.
// Owner: every NPC the host runs is treated as seen, wherever the camera looks. So after the native test, a locally
// owned actor with a faded-in 3D gets the "seen" result. The renderer still frustum-culls for drawing on its own.
using THighActorCull = void(void*, Actor*);
THighActorCull* s_realHighActorCull{};
std::atomic<bool> s_forceSeen{true}; // paired A/B run 20260928-0902: floats 7.74 -> 0/min, cart jumps 47 -> 0/min
std::atomic<uint64_t> s_forceSeenChanged{}, s_forceSeenCalls{};

void HookHighActorCull(void* apCuller, Actor* apActor)
{
    s_realHighActorCull(apCuller, apActor);
    if (!apActor || !s_forceSeen.load(std::memory_order_relaxed))
        return;
    auto& visibility = *reinterpret_cast<uint32_t*>(reinterpret_cast<uint8_t*>(apActor) + 0x27C);
    if ((visibility & 7) == 7)
        return;
    const auto* extension = apActor->GetExtension();
    // NPCs and creatures only. A player's own body is deliberately "not seen" in first person (the Helgen intro starts
    // there); forcing it seen changed the local player's update path and the bound-hands pose stopped holding (owner
    // report after force_seen shipped).
    if (!extension || extension->IsRemote() || extension->IsPlayer() || apActor->formID == 0x14 ||
        !World::Get().GetTransport().IsConnected())
        return;
    using TGet3D = NiAVObject*(TESObjectREFR*);
    POINTER_SKYRIMSE(TGet3D, get3D, 19735);
    auto* root = get3D(apActor);
    if (!root)
        return;
    // Slot 5 AsFadeNode; BSFadeNode current fade at +0x130 (same reads as E40335).
    using TAsFadeNode = uint8_t*(NiAVObject*);
    auto* fade = reinterpret_cast<TAsFadeNode*>((*reinterpret_cast<void***>(root))[5])(root);
    s_forceSeenCalls.fetch_add(1, std::memory_order_relaxed);
    if (!fade || !(*reinterpret_cast<float*>(fade + 0x130) >= 1e-05f))
        return;
    *reinterpret_cast<uint32_t*>(fade + 0xF4) &= ~(1u << 20);
    visibility = (visibility & ~0xFu) | 7u;
    s_forceSeenChanged.fetch_add(1, std::memory_order_relaxed);
}

TP_THIS_FUNCTION(TFillActorUpdateData, void, Actor, GraphUpdateData*);
TFillActorUpdateData* s_realFillActorUpdateData{};
thread_local const GraphUpdateData* t_interestUpdateData{};

void TP_MAKE_THISCALL(HookFillActorUpdateData, Actor, GraphUpdateData* apData)
{
    TiltedPhoques::ThisCall(s_realFillActorUpdateData, apThis, apData);
    t_interestUpdateData = nullptr;
    const auto* extension = apThis->GetExtension();
    if (!extension || extension->IsRemote() || ((apThis->actorState.flags1 >> 21) & 0x7F) != 0 ||
        !apThis->currentProcess || !apThis->parentCell || apThis->IsDeleted() || apThis->IsDisabled() ||
        !(apData->Delta > 0.f) || !std::isfinite(apData->Delta) || !ClaimAnimationInterest(apThis))
        return;
    // The registry proves World exists. Disable overrides promptly on disconnect;
    // without a new serialization pass entries also expire after 250 ms.
    if (!World::Get().GetTransport().IsConnected())
        return;
    apData->Visible = true;
    apData->ForceUpdate = true;
    t_interestUpdateData = apData;
    if (s_offscreenSimulate.load(std::memory_order_relaxed))
        KeepControllerSimulated(apThis);
}


TP_THIS_FUNCTION(TUpdateGraphManager, void, BSAnimationGraphManager, const GraphUpdateData*);
TUpdateGraphManager* s_realUpdateGraphManager{};

void TP_MAKE_THISCALL(HookUpdateGraphManager, BSAnimationGraphManager, const GraphUpdateData* apData)
{
    const bool interested = apData && t_interestUpdateData == apData;
    t_interestUpdateData = nullptr;
    if (!interested)
        return TiltedPhoques::ThisCall(s_realUpdateGraphManager, apThis, apData);
    const auto started = HostFrameCost::Begin();
    TiltedPhoques::ThisCall(s_realUpdateGraphManager, apThis, apData);
    const auto elapsed = started == std::chrono::steady_clock::time_point{} ? 0 :
        static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - started).count());
    if (started != std::chrono::steady_clock::time_point{})
    {
        HostFrameCost::Add(11, elapsed * 1000);
        std::lock_guard lock(s_interestLock);
        s_frameUs += elapsed;
    }
    s_interestGraphUpdates.fetch_add(1, std::memory_order_relaxed);
}

TP_THIS_FUNCTION(TActorUpdateAnimation, void, Actor, float);
TActorUpdateAnimation* s_realActorUpdateAnimation{};
TP_THIS_FUNCTION(TQueuedAnimationUpdate, void, TESObjectREFR, float);
TQueuedAnimationUpdate* s_realQueuedAnimationUpdate{};

bool UpdateRemoteGraph(Actor* apThis, float aDelta, bool aTransformSynced)
{
    const auto* extension = apThis ? apThis->GetExtension() : nullptr;
    if (!extension || !extension->IsRemote() ||
        !World::Get().GetTransport().IsConnected() ||
        ((apThis->actorState.flags1 >> 21) & 0x7F) != 0 ||
        CorpseRagdollService::IsFollowingOwner(apThis->formID))
        return false;

    const auto* player = PlayerCharacter::Get();
    bool nearby = false;
    if (player && player->parentCell && apThis->parentCell && !apThis->IsDead())
    {
        const auto* cell = apThis->parentCell;
        const auto d = apThis->position - player->position;
        nearby = (cell == player->parentCell ||
            (cell->worldspace && cell->worldspace == player->parentCell->worldspace)) &&
            d.x * d.x + d.y * d.y + d.z * d.z <= kInterestRadius * kInterestRadius;
    }
    // Fresh pose samples must not suspend the graph between packets. The final
    // bone-copy hook still interpolates the owner samples over the local result.
    if (!nearby && !PoseCopyAuthority::NeedsLocalGraph(apThis->formID))
        return false;

    // Replace one native animation opportunity, never add a tick in the movement
    // queue (ActorProcess, ID 37356) or the network-thread AnimationSystem::Update.
    auto* root = apThis->GetNiNode();
    if (!(aDelta > 0.f) || !std::isfinite(aDelta) || !root || !root->parent ||
        !apThis->currentProcess || !apThis->parentCell ||
        apThis->IsDeleted() || apThis->IsDisabled())
        return true;
    const auto started = HostFrameCost::Begin();
    struct RemoteGraphCost
    {
        bool Player;
        std::chrono::steady_clock::time_point Started;
        ~RemoteGraphCost()
        {
            if (Player)
                HostFrameCost::End(7, Started);
        }
    } cost{extension->IsRemotePlayer(), started};
    // Preserve the graph's world transform/scale synchronization before evaluating
    // (ID 37364, 0x14067DA60). The queued caller has already done this.
    if (!aTransformSynced)
    {
        TP_THIS_FUNCTION(TSyncGraphTransform, bool, Actor);
        POINTER_SKYRIMSE(TSyncGraphTransform, syncGraphTransform, 37364);
        TiltedPhoques::ThisCall(syncGraphTransform, apThis);
    }
    GraphUpdateData data{};
    data.Delta = aDelta;
    using TFillUpdateData = void (*)(Actor*, GraphUpdateData*);
    auto** table = *reinterpret_cast<void***>(apThis);
    reinterpret_cast<TFillUpdateData>(table[0x79])(apThis, &data);
    // No local simulation callbacks (one includes fall damage). Nearby proxies
    // evaluate every native opportunity; far proxies retain native graph LOD.
    data.Callback = nullptr;
    data.UpdateFunctor = nullptr;
    data.ForceUpdate = nearby;
    if (nearby)
        data.Visible = true;
    BSAnimationGraphManager* manager{};
    if (apThis->animationGraphHolder.GetBSAnimationGraph(&manager) && manager)
    {
        // ID 32899 (0x140553FC0) gates ID 63358 (0x140BC0680) on
        // ShouldAnimGraphUpdate. A remote proxy need not have that locally
        // simulated flag; ID 63358 still applies LOD.
        TiltedPhoques::ThisCall(s_realUpdateGraphManager, manager, &data);
        manager->Release();
        // The native update this replaces (20119 / 0x1402FD930) then runs every biped slot's own graph
        // (15654 / 0x140217BE0 on entry + 0x60 for 0x2A slots): a beast tail is the slot 10 addon's
        // TailAnimationGraphManagerHolder, whose bones are not in the actor's graph. Skipping that loop left
        // every copy's tail in its bind pose (straight out) while the owner's swung (Helgen Keep, 2026-09-29).
        if (auto* biped = static_cast<uint8_t*>(apThis->actorWeightData))
            for (uint32_t slot = 0; slot < 0x2A; ++slot)
            {
                auto* holder = *reinterpret_cast<IAnimationGraphManagerHolder**>(biped + 0x10 + slot * 0x78 + 0x60);
                BSAnimationGraphManager* slotManager{};
                if (!holder || !holder->GetBSAnimationGraph(&slotManager) || !slotManager)
                    continue;
                TiltedPhoques::ThisCall(s_realUpdateGraphManager, slotManager, &data);
                slotManager->Release();
            }
        if (extension->IsRemotePlayer() && started != std::chrono::steady_clock::time_point{})
        {
            s_playerGraphCalls.fetch_add(1, std::memory_order_relaxed);
            s_playerGraphDeltaUs.fetch_add(static_cast<uint64_t>((std::min)(aDelta, 60.f) * 1000000.f), std::memory_order_relaxed);
            if (aDelta > 0.05f)
                s_playerGraphLongSteps.fetch_add(1, std::memory_order_relaxed);
        }
        s_fallbackGraphUpdates.fetch_add(1, std::memory_order_relaxed);
        if (started != std::chrono::steady_clock::time_point{})
            s_fallbackGraphUs.fetch_add(static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - started).count()), std::memory_order_relaxed);
    }
    // The normal finalize still passes through PoseCopyAuthority's copy hook
    // (63856). Its living fallback resets the blend during gaps and blends incoming
    // owner samples over 150 ms, including when we return to the native fresh path.
    return true;
}

void TP_MAKE_THISCALL(HookActorUpdateAnimation, Actor, float aDelta)
{
    if (!UpdateRemoteGraph(apThis, aDelta, false))
        return TiltedPhoques::ThisCall(s_realActorUpdateAnimation, apThis, aDelta);
    // The native update this replaced (37361 / 0x14067D590) follows the graph with head tracking (Actor vtable
    // slot 0x122, 38009, where HeadTrackService aims a remote player's head at its owner's camera) and the facial
    // expression update (slot 0x123), gated on a living, upright actor with a processed middle-high process.
    // Skipping them froze every nearby copy's head (Helgen Keep, 2026-09-29: the owner's pitch arrived, the copy's
    // look-at override was 5 minutes old). Queued (distant) updates, 20123, never run tracking natively either.
    const auto flags1 = apThis->actorState.flags1;
    const auto* process = reinterpret_cast<const uint8_t*>(apThis->currentProcess);
    if (!g_copyNativeTracking.load(std::memory_order_relaxed) || !apThis->GetNiNode() || !process || process[0x137] || ((flags1 >> 21) & 0xF) != 0 ||
        (flags1 & 0xE000000) != 0 || (flags1 & 0x3C000) == 0x1C000 || !(aDelta > 0.f) || !std::isfinite(aDelta))
        return;
    using TActorFrame = void(Actor*, float);
    auto** table = *reinterpret_cast<void***>(apThis);
    reinterpret_cast<TActorFrame*>(table[0x122])(apThis, aDelta);
    reinterpret_cast<TActorFrame*>(table[0x123])(apThis, aDelta);
}

void TP_MAKE_THISCALL(HookQueuedAnimationUpdate, TESObjectREFR, float aDelta)
{
    auto* actor = Cast<Actor>(apThis);
    if (actor)
    {
        if (actor->GetExtension() && actor->GetExtension()->IsRemote())
            s_remoteAnimationTasks.fetch_add(1, std::memory_order_relaxed);
        if (UpdateRemoteGraph(actor, aDelta, true))
            return;
    }
    TiltedPhoques::ThisCall(s_realQueuedAnimationUpdate, apThis, aDelta);
}

TiltedPhoques::Initializer s_distantAnimationInitializer([]() {
    // 1.7.104: ProcessLists 41370/0x1407836E0 queues 41453/0x1407898E0,
    // which syncs the graph then directly calls 20123/0x1402FDEB0. It never
    // dispatches Actor slot 0x7D. Keep 37361 for the alternate serial path;
    // 20123 replaces the queued evaluation in place, preserving the native task
    // barrier and delta. The per-actor 38054 override runs before the 32899 gate.
    // PLANCK's src/main.cpp PlayerCharacter_UpdateAnimation_Hook is player-only:
    // https://github.com/adamhynek/activeragdoll/blob/master/src/main.cpp
    // Adopt the native-phase replacement pattern, not its VR offsets or a
    // player-vtable-only hook for NPCs. CommonLib's update-data layout is above.
    POINTER_SKYRIMSE(TActorUpdateAnimation, actorUpdate, 37361);
    POINTER_SKYRIMSE(TQueuedAnimationUpdate, queuedUpdate, 20123);
    POINTER_SKYRIMSE(TUpdateGraphManager, graphUpdate, 63358);
    POINTER_SKYRIMSE(TFillActorUpdateData, fillUpdateData, 38054);
    s_realActorUpdateAnimation = actorUpdate.Get();
    s_realQueuedAnimationUpdate = queuedUpdate.Get();
    s_realUpdateGraphManager = graphUpdate.Get();
    s_realFillActorUpdateData = fillUpdateData.Get();
    TP_HOOK(&s_realActorUpdateAnimation, HookActorUpdateAnimation);
    TP_HOOK(&s_realQueuedAnimationUpdate, HookQueuedAnimationUpdate);
    TP_HOOK(&s_realUpdateGraphManager, HookUpdateGraphManager);
    TP_HOOK(&s_realFillActorUpdateData, HookFillActorUpdateData);
    POINTER_SKYRIMSE(THighActorCull, highActorCull, 40335);
    s_realHighActorCull = highActorCull.Get();
    TP_HOOK(&s_realHighActorCull, HookHighActorCull);
    POINTER_SKYRIMSE(TSetFrustum, setFrustum, 71081);
    s_realSetFrustum = setFrustum.Get();
    TP_HOOK(&s_realSetFrustum, HookSetFrustum);
    POINTER_SKYRIMSE(TUpdateFarAway, updateFarAway, 37992);
    s_realUpdateFarAway = updateFarAway.Get();
    TP_HOOK(&s_realUpdateFarAway, HookUpdateFarAway);
});

bool CaptureEvaluatedPose(Actor* apActor, EvaluatedPoseSnapshot& arSnapshot,
    VisualBoneSnapshot& arVisualBones) noexcept
{
    using namespace ActorPoseDiagnosticViews;
    BSAnimationGraphManager* pManager{};
    if (!apActor->animationGraphHolder.GetBSAnimationGraph(&pManager) || !pManager)
        return false;

    bool captured = false;
    bool visualCaptured = false;
    const bool captureDiagnostics = GameTestService::IsDiagnosticCaptureArmed();
    const bool living = ((apActor->actorState.flags1 >> 21) & 0x7F) == 0;
    // The exact array the engine last copied onto this actor's bones (PoseCopyAuthority hook on
    // ID 63856) rather than hkbCharacter::poseLocal, which a later copy can differ from.
    if (PoseCopyAuthority::GetCapturedPose(apActor->formID, arSnapshot))
    {
        arSnapshot.GraphDescriptor = apActor->GetExtension() ? apActor->GetExtension()->GraphDescriptorHash : 0;
        captured = true;
    }
    // poseLocal has no evaluation timestamp. Reading it after the copy hook expires and
    // stamping it with this packet's tick turned culled living actors into fresh frozen poses.
    if ((!captured && !living) || captureDiagnostics)
    {
        BSScopedLock<BSRecursiveLock> graphLock(pManager->lock);
        const auto count = pManager->animationGraphs.size;
        const auto index = apActor->formID == 0x14 && living ?
            PoseCopyAuthority::DrawnGraphIndex(apActor, pManager, 0u) : pManager->animationGraphIndex;
        if (count > 0 && count <= 32 && index < count)
        {
            const auto* pGraph = pManager->animationGraphs.Get(index);
            AnimationGraph graph{};
            SIZE_T bytesRead{};
            if (pGraph && ReadProcessMemory(GetCurrentProcess(), pGraph, &graph,
                    sizeof(graph), &bytesRead) && bytesRead == sizeof(graph))
            {
                const auto poseCount = graph.characterInstance.numPoseLocal;
                if (!captured && !living && poseCount > 0 && poseCount <= EvaluatedPoseSnapshot::MaxBones &&
                    graph.characterInstance.poseLocal)
                {
                    std::array<QsTransform, EvaluatedPoseSnapshot::MaxBones> nativePose{};
                    const auto byteCount = static_cast<size_t>(poseCount) * sizeof(QsTransform);
                    bytesRead = 0;
                    if (ReadProcessMemory(GetCurrentProcess(), graph.characterInstance.poseLocal,
                            nativePose.data(), byteCount, &bytesRead) && bytesRead == byteCount)
                    {
                        arSnapshot.GraphDescriptor = apActor->GetExtension() ?
                            apActor->GetExtension()->GraphDescriptorHash : 0;
                        arSnapshot.Bones.resize(poseCount);
                        for (int32_t i = 0; i < poseCount; ++i)
                        {
                            const auto& source = nativePose[i];
                            auto& target = arSnapshot.Bones[i];
                            std::copy_n(source.translation, 3, target.Translation.begin());
                            std::copy_n(source.rotation, 4, target.Rotation.begin());
                            std::copy_n(source.scale, 3, target.Scale.begin());
                        }
                        captured = arSnapshot.IsValid();
                    }
                }

                const auto renderCount = graph.boneNodes.length;
                if (captureDiagnostics &&
                    renderCount > 0 && renderCount <= VisualBoneSnapshot::MaxBones &&
                    graph.boneNodes.capacity >= renderCount && graph.boneNodes.data &&
                    graph.rootNode)
                {
                    std::array<BoneNodeEntry, VisualBoneSnapshot::MaxBones> nodes{};
                    const auto byteCount = static_cast<size_t>(renderCount) * sizeof(BoneNodeEntry);
                    bytesRead = 0;
                    if (ReadProcessMemory(GetCurrentProcess(), graph.boneNodes.data,
                            nodes.data(), byteCount, &bytesRead) && bytesRead == byteCount)
                    {
                        NiTransform rootWorld{};
                        bytesRead = 0;
                        if (ReadProcessMemory(GetCurrentProcess(),
                                reinterpret_cast<const uint8_t*>(graph.rootNode) +
                                    offsetof(NiAVObject, world), &rootWorld,
                                sizeof(rootWorld), &bytesRead) && bytesRead == sizeof(rootWorld))
                        {
                            auto& root = arVisualBones.RootWorld;
                            root.Present = true;
                            std::copy_n(&rootWorld.rotate.entry[0][0], 9,
                                root.Rotation.begin());
                            root.Translation = {rootWorld.translate.x,
                                rootWorld.translate.y, rootWorld.translate.z};
                            root.Scale = rootWorld.scale;
                        }
                        arVisualBones.GraphDescriptor = apActor->GetExtension() ?
                            apActor->GetExtension()->GraphDescriptorHash : 0;
                        arVisualBones.Bones.resize(renderCount);
                        uint32_t presentCount = 0;
                        for (uint32_t i = 0; i < renderCount; ++i)
                        {
                            const auto* pNode = nodes[i].node;
                            NiTransform local{};
                            bytesRead = 0;
                            if (!pNode || !ReadProcessMemory(GetCurrentProcess(),
                                    reinterpret_cast<const uint8_t*>(pNode) +
                                        offsetof(NiAVObject, local), &local,
                                    sizeof(local), &bytesRead) || bytesRead != sizeof(local))
                                continue;
                            auto& bone = arVisualBones.Bones[i];
                            bone.Present = true;
                            std::copy_n(&local.rotate.entry[0][0], 9, bone.Rotation.begin());
                            bone.Translation = {local.translate.x, local.translate.y,
                                local.translate.z};
                            bone.Scale = local.scale;
                            ++presentCount;
                        }
                        visualCaptured = arVisualBones.RootWorld.Present &&
                            presentCount >= renderCount / 2 &&
                            arVisualBones.IsValid();
                    }
                }
            }
        }
    }
    pManager->Release();
    if (!captured)
        arSnapshot = {};
    if (!visualCaptured)
        arVisualBones = {};
    return captured || visualCaptured;
}
}

void AnimationSystem::Update(World& aWorld, Actor* apActor, RemoteAnimationComponent& aAnimationComponent, const uint64_t aTick) noexcept
{
    auto& actions = aAnimationComponent.TimePoints;

    // Retain the established single-action budget until paired observations
    // establish event ordering across native evaluations. Never burst ForceAction.
    const auto session = HostFrameCost::Session();
    const bool player = session && apActor->GetExtension() && apActor->GetExtension()->IsRemotePlayer();
    const uint32_t budget = 1;
    struct ActionStats
    {
        uint64_t ReportAt{}, LastSeen{}, Replayed{}, LateMax{}, BacklogMax{}, Failures{}, NotReady{};
        uint64_t Frames{}, DueAfterBudget{}, LastReplayedTick{};
    };
    static std::unordered_map<uint32_t, ActionStats> stats;
    static uint64_t lastSession{}, nextPrune{};
    ActionStats* sample = nullptr;
    uint64_t wallNow{};
    if (player)
    {
        wallNow = GetTickCount64();
        if (lastSession != session)
        {
            stats.clear();
            lastSession = session;
            nextPrune = 0;
        }
        if (wallNow >= nextPrune)
        {
            std::erase_if(stats, [wallNow](const auto& entry) { return wallNow - entry.second.LastSeen > 10000; });
            nextPrune = wallNow + 5000;
        }
        sample = &stats[apActor->formID];
        if (!sample->ReportAt)
            sample->ReportAt = wallNow + 5000;
        sample->LastSeen = wallNow;
        ++sample->Frames;
    }
    for (uint32_t processed = 0; processed < budget && !actions.empty() && actions.front().Tick <= aTick; ++processed)
    {
        if (player)
        {
            sample->LateMax = (std::max)(sample->LateMax, aTick - actions.front().Tick);
            sample->BacklogMax = (std::max)(sample->BacklogMax, static_cast<uint64_t>(actions.size()));
        }
        // Check if animation graph is ready before attempting to play animations
        if (!apActor->animationGraphHolder.IsReady())
        {
            // Retain the action and report readiness stalls as well as late replay.
            if (player)
                ++sample->NotReady;
            break;
        }
        if (aAnimationComponent.ReplayCount > 0 && aAnimationComponent.ResetAnimationGraphForReplay)
        {
            apActor->animationGraphHolder.RevertAnimationGraphManager();
            aAnimationComponent.ResetAnimationGraphForReplay = false;
        }

        const auto& first = actions.front();
        if (player)
        {
            ++sample->Replayed;
            sample->LastReplayedTick = first.Tick;
        }

        const auto actionId = first.ActionId;
        const auto targetId = first.TargetId;

        const auto pAction = Cast<BGSAction>(TESForm::GetById(actionId));
        const auto pTarget = Cast<TESObjectREFR>(TESForm::GetById(targetId));

        apActor->actorState.flags1 = first.State1;
        apActor->actorState.flags2 = first.State2;

        apActor->LoadAnimationVariables(first.Variables);

        aAnimationComponent.LastRanAction = first;

        // Play the animation
        TESActionData actionData(first.Type & 0x3, apActor, pAction, pTarget);
        actionData.eventName = BSFixedString(first.EventName.c_str());
        actionData.idleForm = Cast<TESIdleForm>(TESForm::GetById(first.IdleId));
        actionData.someFlag = ((first.Type & 0x4) != 0) ? 1 : 0;

        const auto result = ActorMediator::Get()->ForceAction(&actionData);
        aAnimationComponent.LastRanActionResult = result != 0;
        // Cutscene follow: this follower's own character plays the leader's scene idles too, so its first-person
        // camera takes the same motion (measured 2026-09-30: the leader's IdleExecutionerChop_Player dropped his
        // camera 50 u onto the block while hers stayed at eye height). Walking camera idles belong to CameraService.
        // ("player" above is only set during a frame-cost session; test the copy itself.)
        if (first.IdleId && first.IdleId != 0x10C00C && first.IdleId != 0x10C00D &&
            apActor->GetExtension() && apActor->GetExtension()->IsRemotePlayer() &&
            apActor->formID == CutsceneFollow::LeaderFormId() && actionData.idleForm &&
            CutsceneFollow::LeaderIdleNeeded(first.IdleId))
        {
            if (auto* pLocal = PlayerCharacter::Get())
            {
                // The same forced action as the leader's copy just ran: PlayIdle checks the idle's conditions, which
                // fail on this character (IdleExecutionerChop_Player refused, 2026-09-30 14:13) because its scene
                // partner (the headsman) is the leader's, not this PC's.
                TESActionData mirror(first.Type & 0x3, pLocal, pAction, pTarget);
                mirror.eventName = BSFixedString(first.EventName.c_str());
                mirror.idleForm = actionData.idleForm;
                mirror.someFlag = actionData.someFlag;
                // Forced like Actor.cpp's replays: not broadcast as this player's own action.
                g_mirroringLeaderIdle = g_forceAnimation = true;
                const bool played = ActorMediator::Get()->ForceAction(&mirror) != 0;
                g_mirroringLeaderIdle = g_forceAnimation = false;
                if (played)
                    CutsceneFollow::NoteLeaderIdleMirrored(first.IdleId);
                spdlog::info("Cutscene follow: mirrored the leader's idle {:08X} ('{}') on this player played={}",
                    first.IdleId, first.EventName, played);
            }
        }
        // Revive check (2026-09-28): the host's copy of a revived player replayed its get-up over and over.
        if (player)
        {
            // Only while the owner is out of the alive state, and 15 s after.
            static uint64_t s_lifeUntil{};
            static uint32_t s_logs{};
            const auto life = (first.State1 >> 21) & 0xF;
            if (life != 0)
                s_lifeUntil = GetTickCount64() + 15000;
            if (GetTickCount64() < s_lifeUntil && s_logs++ < 400)
                spdlog::info("Remote player action: form={:X} event='{}' action={:X} idle={:X} life={} result={}",
                    apActor->formID, first.EventName, actionId, first.IdleId, (first.State1 >> 21) & 0xF, result);
        }
        if (player && !result)
            ++sample->Failures;

        if (aAnimationComponent.ReplayCount > 0)
            aAnimationComponent.ReplayCount--;

        actions.pop_front();
    }
    if (player)
    {
        // Only inspect the front, not the whole queue. Future events are not a
        // replay backlog. This distinguishes budget pressure from readiness stalls.
        sample->DueAfterBudget += !actions.empty() && actions.front().Tick <= aTick ? 1 : 0;
        if (wallNow >= sample->ReportAt)
        {
            spdlog::info("Remote player actions: form={:X} replayed={} maxLateMs={} maxQueue={} failures={} notReadyFrames={} frames={} dueAfterBudgetFrames={} lastReplayedTick={} presentationTick={} wallMs={} budget=1 (interval, this player; same-tick events retained)",
                apActor->formID, sample->Replayed, sample->LateMax, sample->BacklogMax,
                sample->Failures, sample->NotReady, sample->Frames, sample->DueAfterBudget,
                sample->LastReplayedTick, aTick, wallNow);
            *sample = {wallNow + 5000, wallNow};
        }
    }
}

void AnimationSystem::Setup(World& aWorld, const entt::entity aEntity) noexcept
{
    aWorld.emplace_or_replace<RemoteAnimationComponent>(aEntity);
}

void AnimationSystem::Clean(World& aWorld, const entt::entity aEntity) noexcept
{
    if (aWorld.all_of<RemoteAnimationComponent>(aEntity))
        aWorld.remove<RemoteAnimationComponent>(aEntity);
}

void AnimationSystem::AddActionsForReplay(RemoteAnimationComponent& aAnimationComponent,
                                          const ActionReplayChain& acReplay) noexcept
{
    aAnimationComponent.TimePoints.insert(aAnimationComponent.TimePoints.end(), acReplay.Actions.begin(),
                                          acReplay.Actions.end());
    aAnimationComponent.ReplayCount = acReplay.Actions.size();
    aAnimationComponent.ResetAnimationGraphForReplay = acReplay.ResetAnimationGraph;
}

void AnimationSystem::AddAction(RemoteAnimationComponent& aAnimationComponent, const std::string& acActionDiff) noexcept
{
    auto itor = std::begin(aAnimationComponent.TimePoints);
    const auto end = std::cend(aAnimationComponent.TimePoints);

    auto& lastProcessedAction = aAnimationComponent.LastProcessedAction;

    TiltedPhoques::ViewBuffer buffer((uint8_t*)acActionDiff.data(), acActionDiff.size());
    Buffer::Reader reader(&buffer);

    lastProcessedAction.ApplyDifferential(reader);

    aAnimationComponent.TimePoints.push_back(lastProcessedAction);
}

void AnimationSystem::Serialize(World& aWorld, ClientReferencesMoveRequest& aMovementSnapshot, LocalComponent& localComponent, LocalAnimationComponent& animationComponent, FormIdComponent& formIdComponent, bool aCapturePose)
{
    const auto pForm = TESForm::GetById(formIdComponent.Id);
    const auto pActor = Cast<Actor>(pForm);
    if (!pActor)
        return;

    auto& update = aMovementSnapshot.Updates[localComponent.Id];
    auto& movement = update.UpdatedMovement;
    update.CombatTargetServerId = 0;
    if (pActor->pCombatController && pActor->pCombatController->targetHandle)
    {
        update.CombatTargetServerId = 0xFFFFFFFFu;
        auto* pTarget = Cast<Actor>(TESObjectREFR::GetByHandle(
            pActor->pCombatController->targetHandle));
        if (pTarget)
        {
            auto token = Utils::GetLocalOwnershipToken(pTarget->formID);
            if (!token)
                token = Utils::GetRemoteOwnershipToken(pTarget->formID);
            if (token)
                update.CombatTargetServerId = token->ServerId;
        }
    }

    if (!GameTestService::IsDiagnosticCaptureArmed())
        animationComponent.LastSentVisualBones = {};
    PublishAnimationInterest(aWorld, aMovementSnapshot.Tick);
    // Pose bandwidth is independent of native graph interest. Read only completed
    // bone-copy samples and reserve one of the 33 packet slots for the local player.
    static uint64_t captureBatchTick{};
    static uint32_t captureBatchActors{};
    if (captureBatchTick != aMovementSnapshot.Tick)
    {
        captureBatchTick = aMovementSnapshot.Tick;
        captureBatchActors = 0;
    }
    if (aCapturePose && (pActor->formID == 0x14 || captureBatchActors < 32))
    {
        if (pActor->formID != 0x14)
            ++captureBatchActors;
        CaptureEvaluatedPose(pActor, update.EvaluatedPose, update.VisualBones);
        if (!update.EvaluatedPose.Bones.empty())
        {
            // A pose captured in the bone-copy hook keeps the tick of the frame it was copied in.
            if (!update.EvaluatedPose.SourceTick)
                update.EvaluatedPose.SourceTick = aMovementSnapshot.Tick;
            animationComponent.LastSentPose = update.EvaluatedPose;
            s_posePackets.fetch_add(1, std::memory_order_relaxed);
        }
        // Render-bone snapshots were a second full copy of the skeleton per pose; bone playback
        // (PoseCopyAuthority) uses the evaluated pose, so keep them local for diagnostics only.
        if (!update.VisualBones.Bones.empty())
        {
            update.VisualBones.SourceTick = aMovementSnapshot.Tick;
            animationComponent.LastSentVisualBones = update.VisualBones;
            update.VisualBones = {};
        }
        else
            animationComponent.LastSentVisualBones = {};
    }
    static uint64_t nextGraphReportMs{};
    static uint64_t graphReportSession{};
    const auto profileSession = HostFrameCost::Session();
    if (profileSession && profileSession != graphReportSession)
    {
        graphReportSession = profileSession;
        nextGraphReportMs = GetTickCount64() + 5000;
        s_playerGraphCalls.exchange(0, std::memory_order_relaxed);
        s_playerGraphDeltaUs.exchange(0, std::memory_order_relaxed);
        s_playerGraphLongSteps.exchange(0, std::memory_order_relaxed);
        std::lock_guard lock(s_interestLock);
        s_interestFrames = s_interestActors = s_interestUs = s_interestMaxActors = s_interestMaxUs = 0;
        s_interestDeferred = s_frameUs = 0;
    }
    if (profileSession && GetTickCount64() >= nextGraphReportMs)
    {
        const auto reportNow = GetTickCount64();
        nextGraphReportMs = reportNow + 5000;
        spdlog::info("Distant animation: graphOnlyCalls={} interestCalls={} posesQueued={} remoteAnimationTasks={} graphOnlyUs={} (cumulative, far native LOD retained)",
            s_fallbackGraphUpdates.load(std::memory_order_relaxed),
            s_interestGraphUpdates.load(std::memory_order_relaxed),
            s_posePackets.load(std::memory_order_relaxed),
            s_remoteAnimationTasks.load(std::memory_order_relaxed),
            s_fallbackGraphUs.load(std::memory_order_relaxed));
        uint64_t frames, actors, us, maxActors, maxUs, deferred, overflow, registered;
        {
            std::lock_guard lock(s_interestLock);
            frames = std::exchange(s_interestFrames, 0);
            actors = std::exchange(s_interestActors, 0);
            us = std::exchange(s_interestUs, 0);
            maxActors = std::exchange(s_interestMaxActors, 0);
            maxUs = std::exchange(s_interestMaxUs, 0);
            deferred = std::exchange(s_interestDeferred, 0);
            overflow = s_interestOverflow;
            registered = s_animationInterest.size();
        }
        const auto playerCalls = s_playerGraphCalls.exchange(0, std::memory_order_relaxed);
        const auto playerDeltaUs = s_playerGraphDeltaUs.exchange(0, std::memory_order_relaxed);
        spdlog::info("Remote player graph: wallMs={} calls={} meanNativeDeltaUs={} stepsOver50Ms={} (interval, all players, graph-only native opportunities; independent of 50 ms network cadence)",
            GetTickCount64(), playerCalls, playerCalls ? playerDeltaUs / playerCalls : 0,
            s_playerGraphLongSteps.exchange(0, std::memory_order_relaxed));
        spdlog::info("Animation interest: registered={} frames={} forcedActorsPerFrame={:.2f} graphUsPerFrame={:.2f} maxActors={} maxGraphUs={} deferred={} overflow={} budget={} radius={} (interval, no minimum spacing, rotating 64-actor native opportunity budget; graph timing only while profiling, excludes async finalize)",
            registered, frames, frames ? double(actors) / frames : 0.0, frames ? double(us) / frames : 0.0,
            maxActors, maxUs, deferred, overflow, kInterestActorsPerFrame, kInterestRadius);
    }

    if (const auto pCell = pActor->parentCell)
        World::Get().GetModSystem().GetServerModId(pCell->formID, movement.CellId.ModId, movement.CellId.BaseId);

    if (const auto pWorldSpace = pActor->GetWorldSpace())
        World::Get().GetModSystem().GetServerModId(pWorldSpace->formID, movement.WorldSpaceId.ModId, movement.WorldSpaceId.BaseId);

    movement.Position = pActor->position;

    movement.Rotation.x = pActor->rotation.x;
    movement.Rotation.y = pActor->rotation.z;

    pActor->SaveAnimationVariables(movement.Variables);

    if (pActor->currentProcess && pActor->currentProcess->middleProcess)
    {
        movement.Direction = pActor->currentProcess->middleProcess->direction;
    }

    for (auto& entry : animationComponent.Actions)
    {
        update.ActionEvents.push_back(entry);
        animationComponent.LastSentAction = entry;
    }

    auto latestAction = animationComponent.GetLatestAction();

    if (latestAction)
        localComponent.CurrentAction = latestAction.MoveResult();

    animationComponent.Actions.clear();
}

bool AnimationSystem::Serialize(World& aWorld, const ActionEvent& aActionEvent, const ActionEvent& aLastProcessedAction, std::string* apData)
{
    uint32_t actionBaseId = 0;
    uint32_t actionModId = 0;
    if (!aWorld.GetModSystem().GetServerModId(aActionEvent.ActionId, actionModId, actionBaseId))
        return false;

    uint32_t targetBaseId = 0;
    uint32_t targetModId = 0;
    if (!aWorld.GetModSystem().GetServerModId(aActionEvent.TargetId, targetModId, targetBaseId))
        return false;

    uint8_t scratch[1 << 14];
    TiltedPhoques::ViewBuffer buffer(scratch, std::size(scratch));
    Buffer::Writer writer(&buffer);
    aActionEvent.GenerateDifferential(aLastProcessedAction, writer);

    apData->assign(buffer.GetData(), buffer.GetData() + writer.Size());

    return true;
}

void AnimationSystem::SetOffscreenSimulate(bool aEnabled) noexcept
{
    s_offscreenSimulate.store(aEnabled, std::memory_order_relaxed);
}

std::string AnimationSystem::OffscreenSimulateJson() noexcept
{
    return fmt::format("\"enabled\":{},\"clears\":{}", s_offscreenSimulate.load(), s_offscreenClears.load());
}

bool AnimationSystem::IsNearAnyPlayer(uint32_t aFormId) noexcept
{
    return HasAnimationInterest(aFormId);
}

bool AnimationSystem::IsOffscreenSimulateEnabled() noexcept
{
    return s_offscreenSimulate.load(std::memory_order_relaxed);
}

void AnimationSystem::CountOffscreenClear() noexcept
{
    s_offscreenClears.fetch_add(1, std::memory_order_relaxed);
}

void AnimationSystem::SetWideCull(bool aEnabled) noexcept
{
    s_wideCull.store(aEnabled, std::memory_order_relaxed);
    spdlog::info("Wide cull: {}", aEnabled);
}

std::string AnimationSystem::WideCullJson() noexcept
{
    return fmt::format("\"enabled\":{},\"calls\":{}", s_wideCull.load(), s_wideCullCalls.load());
}

void AnimationSystem::SetForceSeen(bool aEnabled) noexcept
{
    s_forceSeen.store(aEnabled, std::memory_order_relaxed);
    spdlog::info("Force seen: {}", aEnabled);
}

std::string AnimationSystem::ForceSeenJson() noexcept
{
    return fmt::format("\"enabled\":{},\"checked\":{},\"changed\":{}", s_forceSeen.load(), s_forceSeenCalls.load(),
        s_forceSeenChanged.load());
}
