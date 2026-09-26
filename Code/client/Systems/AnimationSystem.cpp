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
#include <Services/TransportService.h>
#include <atomic>
#include <chrono>
#include <cmath>
#include <limits>
#include <mutex>
#include <unordered_map>
#include <utility>

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
    uint64_t SelectedTick{};
};
constexpr size_t kInterestActorsPerFrame = 64;
constexpr size_t kInterestCapacity = 256;
constexpr float kInterestRadius = 4096.f;
std::mutex s_interestLock;
std::unordered_map<uint32_t, AnimationInterest> s_animationInterest;
uint64_t s_interestPublishedAt{}, s_interestFrame{};
uint64_t s_interestFrames{}, s_interestActors{}, s_interestUs{}, s_interestMaxActors{}, s_interestMaxUs{};
uint64_t s_frameActors{}, s_frameUs{}, s_interestDeferred{}, s_interestOverflow{};
std::atomic<uint64_t> s_fallbackGraphUpdates{}, s_interestGraphUpdates{}, s_posePackets{};
std::atomic<uint64_t> s_remoteAnimationTasks{}, s_fallbackGraphUs{};

// Publish all nearby owned actors independently of this packet's pose selection.
// Only the serialization thread visits ECS. Native workers use pointer-checked,
// expiring entries and never dereference pointers taken from the registry.
void PublishAnimationInterest(World& aWorld, uint64_t aBatchTick)
{
    static uint64_t lastBatchTick = ~uint64_t{};
    if (lastBatchTick == aBatchTick)
        return;
    lastBatchTick = aBatchTick;
    std::vector<Actor*> observers;
    auto players = aWorld.view<FormIdComponent, PlayerComponent>();
    for (auto entity : players)
    {
        auto* actor = Cast<Actor>(TESForm::GetById(players.get<FormIdComponent>(entity).Id));
        if (actor && actor->formID != 0x14 && actor->GetExtension()->IsRemote() &&
            actor->parentCell && actor->GetNiNode())
            observers.push_back(actor);
    }
    std::vector<std::pair<uint32_t, AnimationInterest>> candidates;
    auto owned = aWorld.view<LocalComponent, LocalAnimationComponent, FormIdComponent>();
    for (auto entity : owned)
    {
        auto* actor = Cast<Actor>(TESForm::GetById(owned.get<FormIdComponent>(entity).Id));
        if (!actor || actor->formID == 0x14 || actor->GetExtension()->IsRemote() ||
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
    std::sort(candidates.begin(), candidates.end(), [](const auto& a, const auto& b) {
        return a.second.DistanceSquared == b.second.DistanceSquared ? a.first < b.first :
            a.second.DistanceSquared < b.second.DistanceSquared;
    });
    std::lock_guard lock(s_interestLock);
    s_interestOverflow = candidates.size() > kInterestCapacity ? candidates.size() - kInterestCapacity : 0;
    if (candidates.size() > kInterestCapacity)
        candidates.resize(kInterestCapacity);
    std::unordered_map<uint32_t, AnimationInterest> next;
    for (auto& [id, interest] : candidates)
    {
        const auto it = s_animationInterest.find(id);
        if (it != s_animationInterest.end() && it->second.ActorPtr == interest.ActorPtr)
        {
            interest.LastForcedTick = it->second.LastForcedTick;
            interest.SelectedTick = it->second.SelectedTick;
        }
        next.emplace(id, interest);
    }
    s_animationInterest.swap(next);
    s_interestPublishedAt = GetTickCount64();
}

bool ClaimAnimationInterest(Actor* apActor)
{
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
        if (s_interestFrame)
        {
            ++s_interestFrames;
            s_interestActors += s_frameActors;
            s_interestUs += s_frameUs;
            s_interestMaxActors = (std::max)(s_interestMaxActors, s_frameActors);
            s_interestMaxUs = (std::max)(s_interestMaxUs, s_frameUs);
        }
        s_interestFrame = frame;
        s_frameActors = s_frameUs = 0;
        std::vector<std::pair<uint32_t, AnimationInterest*>> order;
        for (auto& [id, interest] : s_animationInterest)
            order.push_back({id, &interest});
        // Oldest scheduled first avoids native task order starving a remote player.
        // Rotate even actors with no native task so they cannot hold every slot.
        std::sort(order.begin(), order.end(), [](const auto& a, const auto& b) {
            if (a.second->SelectedTick != b.second->SelectedTick)
                return a.second->SelectedTick < b.second->SelectedTick;
            return a.second->DistanceSquared == b.second->DistanceSquared ? a.first < b.first :
                a.second->DistanceSquared < b.second->DistanceSquared;
        });
        for (size_t i = 0; i < order.size() && i < kInterestActorsPerFrame; ++i)
            order[i].second->SelectedTick = frame;
    }
    auto& interest = it->second;
    if (interest.LastForcedTick == frame)
        return false;
    if (interest.SelectedTick != frame || s_frameActors >= kInterestActorsPerFrame)
    {
        ++s_interestDeferred;
        return false;
    }
    interest.LastForcedTick = frame;
    ++s_frameActors;
    return true;
}

TP_THIS_FUNCTION(TFillActorUpdateData, void, Actor, GraphUpdateData*);
TFillActorUpdateData* s_realFillActorUpdateData{};
thread_local const GraphUpdateData* t_interestUpdateData{};

void TP_MAKE_THISCALL(HookFillActorUpdateData, Actor, GraphUpdateData* apData)
{
    TiltedPhoques::ThisCall(s_realFillActorUpdateData, apThis, apData);
    t_interestUpdateData = nullptr;
    if (apThis->GetExtension()->IsRemote() || ((apThis->actorState.flags1 >> 21) & 0x7F) != 0 ||
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
}

TP_THIS_FUNCTION(TUpdateGraphManager, void, BSAnimationGraphManager, const GraphUpdateData*);
TUpdateGraphManager* s_realUpdateGraphManager{};

void TP_MAKE_THISCALL(HookUpdateGraphManager, BSAnimationGraphManager, const GraphUpdateData* apData)
{
    const bool interested = apData && t_interestUpdateData == apData;
    t_interestUpdateData = nullptr;
    if (!interested)
        return TiltedPhoques::ThisCall(s_realUpdateGraphManager, apThis, apData);
    const auto started = std::chrono::steady_clock::now();
    TiltedPhoques::ThisCall(s_realUpdateGraphManager, apThis, apData);
    const auto elapsed = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - started).count());
    {
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
    if (!apThis->GetExtension()->IsRemote() ||
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
    const auto started = std::chrono::steady_clock::now();
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
        s_fallbackGraphUpdates.fetch_add(1, std::memory_order_relaxed);
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
        TiltedPhoques::ThisCall(s_realActorUpdateAnimation, apThis, aDelta);
}

void TP_MAKE_THISCALL(HookQueuedAnimationUpdate, TESObjectREFR, float aDelta)
{
    auto* actor = Cast<Actor>(apThis);
    if (actor)
    {
        if (actor->GetExtension()->IsRemote())
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
        const auto index = apActor->formID == 0x14 && living ? 0u : pManager->animationGraphIndex;
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

    const auto it = std::begin(actions);
    if (it != std::end(actions) && it->Tick <= aTick)
    {
        // Check if animation graph is ready before attempting to play animations
        if (!apActor->animationGraphHolder.IsReady())
        {
            // Animation graph not ready, keep the action in queue and try again later
            return;
        }
        if (aAnimationComponent.ReplayCount > 0 && aAnimationComponent.ResetAnimationGraphForReplay)
        {
            apActor->animationGraphHolder.RevertAnimationGraphManager();
            aAnimationComponent.ResetAnimationGraphForReplay = false;
        }

        const auto& first = *it;

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

        if (aAnimationComponent.ReplayCount > 0)
            aAnimationComponent.ReplayCount--;

        actions.pop_front();
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
    if (const auto reportNow = GetTickCount64(); reportNow >= nextGraphReportMs)
    {
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
        spdlog::info("Animation interest: registered={} frames={} forcedActorsPerFrame={:.2f} graphUsPerFrame={:.2f} maxActors={} maxGraphUs={} deferred={} overflow={} budget={} radius={} (interval, graph call time excludes async finalize)",
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
