#include <TiltedOnlinePCH.h>

#include <Systems/AnimationSystem.h>

#include <Games/Animation/TESActionData.h>
#include <Games/Animation/ActorMediator.h>
#include <Games/ActorExtension.h>
#include <Games/Skyrim/BSAnimationGraphManager.h>
#include <Games/Skyrim/Havok/ActorPoseDiagnosticViews.h>
#include <Games/Skyrim/NetImmerse/NiAVObject.h>
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

extern thread_local const char* g_animErrorCode;

namespace
{
bool CaptureEvaluatedPose(Actor* apActor, EvaluatedPoseSnapshot& arSnapshot,
    VisualBoneSnapshot& arVisualBones) noexcept
{
    using namespace ActorPoseDiagnosticViews;
    BSAnimationGraphManager* pManager{};
    if (!apActor->animationGraphHolder.GetBSAnimationGraph(&pManager) || !pManager)
        return false;

    bool captured = false;
    bool visualCaptured = false;
    // The exact array the engine last copied onto this actor's bones (PoseCopyAuthority hook on
    // ID 63856) rather than hkbCharacter::poseLocal, which a later copy can differ from.
    if (PoseCopyAuthority::GetCapturedPose(apActor->formID, arSnapshot))
    {
        arSnapshot.GraphDescriptor = apActor->GetExtension() ? apActor->GetExtension()->GraphDescriptorHash : 0;
        captured = true;
    }
    {
        BSScopedLock<BSRecursiveLock> graphLock(pManager->lock);
        const auto count = pManager->animationGraphs.size;
        const auto index = pManager->animationGraphIndex;
        if (count > 0 && count <= 32 && index < count)
        {
            const auto* pGraph = pManager->animationGraphs.Get(index);
            AnimationGraph graph{};
            SIZE_T bytesRead{};
            if (pGraph && ReadProcessMemory(GetCurrentProcess(), pGraph, &graph,
                    sizeof(graph), &bytesRead) && bytesRead == sizeof(graph))
            {
                const auto poseCount = graph.characterInstance.numPoseLocal;
                if (!captured && poseCount > 0 && poseCount <= EvaluatedPoseSnapshot::MaxBones &&
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
                if (renderCount > 0 && renderCount <= VisualBoneSnapshot::MaxBones &&
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

    if (aCapturePose)
    {
        CaptureEvaluatedPose(pActor, update.EvaluatedPose, update.VisualBones);
        if (!update.EvaluatedPose.Bones.empty())
        {
            // A pose captured in the bone-copy hook keeps the tick of the frame it was copied in.
            if (!update.EvaluatedPose.SourceTick)
                update.EvaluatedPose.SourceTick = aMovementSnapshot.Tick;
            animationComponent.LastSentPose = update.EvaluatedPose;
        }
        if (!update.VisualBones.Bones.empty())
        {
            update.VisualBones.SourceTick = aMovementSnapshot.Tick;
            animationComponent.LastSentVisualBones = update.VisualBones;
        }
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
