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
#include <Services/TransportService.h>
#include <atomic>
#include <chrono>
#include <cmath>
#include <limits>
#include <mutex>
#include <unordered_map>

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

struct PoseRequest
{
    const Actor* ActorPtr{};
    NiPoint3 EyePosition{};
    uint64_t RequestedAt{};
    bool Evaluating{};
    bool Evaluated{};
};
std::mutex s_poseRequestLock;
std::unordered_map<uint32_t, PoseRequest> s_poseRequests;
std::atomic<uint64_t> s_fallbackGraphUpdates{}, s_interestGraphUpdates{}, s_posePackets{};
std::atomic<uint64_t> s_remoteAnimationTasks{}, s_fallbackGraphUs{};

// Selection runs on the network update thread. Native graph evaluation consumes
// these one-shot requests on its own thread, then the next snapshot reads the copy.
bool PreparePoseCapture(World& aWorld, Actor* apActor, bool aSelected)
{
    const auto now = GetTickCount64();
    bool capture = false;
    {
        std::lock_guard lock(s_poseRequestLock);
        for (auto it = s_poseRequests.begin(); it != s_poseRequests.end();)
            it = now - it->second.RequestedAt > 250 ? s_poseRequests.erase(it) : std::next(it);
        const auto it = s_poseRequests.find(apActor->formID);
        if (it != s_poseRequests.end() && it->second.ActorPtr == apActor && it->second.Evaluated)
        {
            capture = true;
            s_poseRequests.erase(it);
        }
    }
    // The player and physics-owned skeletons already have their own native update paths.
    if (apActor->formID == 0x14 || ((apActor->actorState.flags1 >> 21) & 0x7F) != 0)
        return aSelected;
    if (!aSelected || !apActor->parentCell)
        return capture;

    NiPoint3 eye{};
    float nearest = (std::numeric_limits<float>::max)();
    const auto consider = [&](Actor* apPlayer) {
        if (!apPlayer || !apPlayer->parentCell || !apPlayer->GetNiNode())
            return;
        const auto* cell = apActor->parentCell;
        if (cell != apPlayer->parentCell &&
            (!cell->worldspace || cell->worldspace != apPlayer->parentCell->worldspace))
            return;
        const auto d = apActor->position - apPlayer->position;
        const float distance = d.x * d.x + d.y * d.y + d.z * d.z;
        if (distance < nearest)
        {
            nearest = distance;
            eye = apPlayer->position;
        }
    };
    consider(PlayerCharacter::Get());
    auto players = aWorld.view<FormIdComponent, PlayerComponent>();
    for (auto entity : players)
        consider(Cast<Actor>(TESForm::GetById(players.get<FormIdComponent>(entity).Id)));
    if (nearest < (std::numeric_limits<float>::max)())
    {
        std::lock_guard lock(s_poseRequestLock);
        // Do not replace an unconsumed request on a slow native frame.
        // Allow a completed batch and its replacement to coexist while actors are
        // serialized in arbitrary order. Both capture and new selection are capped.
        if (s_poseRequests.size() < 64 && !s_poseRequests.contains(apActor->formID))
            s_poseRequests[apActor->formID] = {apActor, eye, now, false, false};
    }
    return capture;
}

TP_THIS_FUNCTION(TUpdateGraphManager, void, BSAnimationGraphManager, const GraphUpdateData*);
TUpdateGraphManager* s_realUpdateGraphManager{};

void TP_MAKE_THISCALL(HookUpdateGraphManager, BSAnimationGraphManager, const GraphUpdateData* apData)
{
    auto* actor = apData ? Cast<Actor>(apData->Reference) : nullptr;
    PoseRequest request{};
    if (actor && !actor->GetExtension()->IsRemote() &&
        ((actor->actorState.flags1 >> 21) & 0x7F) == 0)
    {
        std::lock_guard lock(s_poseRequestLock);
        const auto it = s_poseRequests.find(actor->formID);
        if (it != s_poseRequests.end() && it->second.ActorPtr == actor &&
            !it->second.Evaluating && !it->second.Evaluated &&
            GetTickCount64() - it->second.RequestedAt <= 250)
        {
            request = it->second;
            it->second.Evaluating = true;
        }
    }
    // A request proves World has been initialized; do not access it during early
    // native animation startup when the request registry is still empty.
    if (!request.ActorPtr || !World::Get().GetTransport().IsConnected())
        return TiltedPhoques::ThisCall(s_realUpdateGraphManager, apThis, apData);

    auto data = *apData;
    // Remote camera frusta are not transmitted. Conservatively include the selected
    // actor once from its nearest player's position, retaining native distance/bone
    // LOD (63575/0x140BCC2C0, 63587/0x140BCEDB0); never set ForceUpdate
    // or force a full skeleton.
    data.EyePosition = &request.EyePosition;
    data.Visible = true;
    TiltedPhoques::ThisCall(s_realUpdateGraphManager, apThis, &data);
    s_interestGraphUpdates.fetch_add(1, std::memory_order_relaxed);
    {
        std::lock_guard lock(s_poseRequestLock);
        const auto it = s_poseRequests.find(actor->formID);
        if (it != s_poseRequests.end() && it->second.ActorPtr == actor &&
            it->second.RequestedAt == request.RequestedAt)
            it->second.Evaluated = true;
    }
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
        !PoseCopyAuthority::NeedsLocalGraph(apThis->formID))
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
    // Keep the native visibility and distance policy, but no actor post-update
    // functors (one includes fall damage), forced full-rate evaluation, or AI work.
    data.Callback = nullptr;
    data.UpdateFunctor = nullptr;
    data.ForceUpdate = false;
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
    // barrier, delta, actor selection, visibility and 63587/63575 LOD policy.
    // PLANCK's src/main.cpp PlayerCharacter_UpdateAnimation_Hook is player-only:
    // https://github.com/adamhynek/activeragdoll/blob/master/src/main.cpp
    // Adopt the native-phase replacement pattern, not its VR offsets or a
    // player-vtable-only hook for NPCs. CommonLib's update-data layout is above.
    POINTER_SKYRIMSE(TActorUpdateAnimation, actorUpdate, 37361);
    POINTER_SKYRIMSE(TQueuedAnimationUpdate, queuedUpdate, 20123);
    POINTER_SKYRIMSE(TUpdateGraphManager, graphUpdate, 63358);
    s_realActorUpdateAnimation = actorUpdate.Get();
    s_realQueuedAnimationUpdate = queuedUpdate.Get();
    s_realUpdateGraphManager = graphUpdate.Get();
    TP_HOOK(&s_realActorUpdateAnimation, HookActorUpdateAnimation);
    TP_HOOK(&s_realQueuedAnimationUpdate, HookQueuedAnimationUpdate);
    TP_HOOK(&s_realUpdateGraphManager, HookUpdateGraphManager);
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
    // A delayed native batch can complete several requests together. Bound actual
    // captures as well as selection; reserve one of the 33 slots for the local player.
    static uint64_t captureBatchTick{};
    static uint32_t captureBatchActors{};
    if (captureBatchTick != aMovementSnapshot.Tick)
    {
        captureBatchTick = aMovementSnapshot.Tick;
        captureBatchActors = 0;
    }
    const bool captureReady = PreparePoseCapture(aWorld, pActor, aCapturePose);
    if (captureReady && (pActor->formID == 0x14 || captureBatchActors < 32))
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
        spdlog::info("Distant animation: graphOnlyCalls={} interestCalls={} posesQueued={} remoteAnimationTasks={} graphOnlyUs={} (cumulative, native LOD retained)",
            s_fallbackGraphUpdates.load(std::memory_order_relaxed),
            s_interestGraphUpdates.load(std::memory_order_relaxed),
            s_posePackets.load(std::memory_order_relaxed),
            s_remoteAnimationTasks.load(std::memory_order_relaxed),
            s_fallbackGraphUs.load(std::memory_order_relaxed));
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
