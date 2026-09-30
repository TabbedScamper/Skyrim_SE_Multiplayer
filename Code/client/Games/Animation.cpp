#include <TiltedOnlinePCH.h>

#include <Games/References.h>

#include <Forms/BGSAction.h>
#include <Forms/TESIdleForm.h>

#include <Structs/ActionEvent.h>

#include <Games/Animation/ActorMediator.h>
#include <Games/Animation/TESActionData.h>

#include <Misc/BSFixedString.h>

#include <World.h>
#include <Services/PartyService.h>
#include <Services/CutsceneFollow.h>
#include <Services/CameraService.h>
#include <atomic>

TP_THIS_FUNCTION(TPerformAction, uint8_t, ActorMediator, TESActionData* apAction);
static TPerformAction* RealPerformAction;

// TODO: make scoped override
thread_local bool g_forceAnimation = false;

uint8_t TP_MAKE_THISCALL(HookPerformAction, ActorMediator, TESActionData* apAction)
{
    auto pActor = apAction->actor;
    const auto pExtension = pActor->GetExtension();

    if (!pExtension->IsRemote() || g_forceAnimation)
    {
        const char* pEventName = apAction->eventName.AsAscii();
        const bool walkingStart = pEventName && strcmp(pEventName, "IdleWalkingCameraStart") == 0;
        const bool walkingEnd = pEventName && strcmp(pEventName, "IdleWalkingCameraEnd") == 0;
        if (!g_forceAnimation && pActor->formID == 0x14 && pEventName &&
            strstr(pEventName, "WalkingCamera") != nullptr)
        {
            // During cutscene follow this player actually walks with the host. Its camera graph
            // must run too. Keep suppressing stray starts after control has returned.
            const auto& party = World::Get().GetPartyService();
            if (party.IsInParty() && !party.IsLeader() && !CutsceneFollow::IsActive() &&
                strstr(pEventName, "WalkingCameraStart") != nullptr)
            {
                static std::atomic<uint64_t> s_nextSuppressionLogMs{};
                const auto now = GetTickCount64();
                auto next = s_nextSuppressionLogMs.load(std::memory_order_relaxed);
                if (now >= next && s_nextSuppressionLogMs.compare_exchange_strong(
                        next, now + 10000, std::memory_order_relaxed))
                    spdlog::info("Follower: skipped local walking-camera start ({})", pEventName);
                return 0;
            }
            spdlog::info("Local player walking-camera action event={} tick={} idleForm={:08X} "
                "targetForm={:08X} caller={}", pEventName, GetTickCount64(),
                apAction->idleForm ? apAction->idleForm->formID : 0,
                apAction->target ? apAction->target->formID : 0,
                fmt::ptr(_ReturnAddress()));
        }
        ActionEvent action;
        action.State1 = pActor->actorState.flags1;
        action.State2 = pActor->actorState.flags2;
        action.Type = apAction->unkInput | (apAction->someFlag ? 0x4 : 0);
        action.Tick = World::Get().GetTick();
        action.ActorId = pActor->formID;
        action.ActionId = apAction->action->formID;
        action.TargetId = apAction->target ? apAction->target->formID : 0;

        pActor->SaveAnimationVariables(action.Variables);

        const auto res = TiltedPhoques::ThisCall(RealPerformAction, apThis, apAction);

        // Execution block (2026-09-30): the leader's first-person camera follows a scene idle the follower's own
        // camera graph never plays. Record every local player action while cutscene follow is on.
        if (pActor->formID == 0x14 && CutsceneFollow::IsActive() && pEventName)
            spdlog::info("Cutscene action: local player event={} target={} idleForm={:08X} result={} forced={}",
                pEventName, apAction->targetEventName.AsAscii() ? apAction->targetEventName.AsAscii() : "",
                apAction->idleForm ? apAction->idleForm->formID : 0, res, g_forceAnimation);

        if (res && pActor->formID == 0x14 && apAction->idleForm)
        {
            if (walkingStart)
                CameraService::NoteWalkingCameraIdle(apAction->idleForm->formID, true);
            else if (walkingEnd)
                CameraService::NoteWalkingCameraIdle(apAction->idleForm->formID, false);
        }

        // spdlog::debug("Action event name: {}, target name: {}", apAction->eventName.AsAscii(), apAction->targetEventName.AsAscii());

        // This is a weird case where it gets spammed and doesn't do much, not sure if it still needs to be sent over the network
        if (apAction->someFlag == 1 || g_forceAnimation)
            return res;

        action.EventName = apAction->eventName.AsAscii();
        action.TargetEventName = apAction->targetEventName.AsAscii();
        action.IdleId = apAction->idleForm ? apAction->idleForm->formID : 0;

        // Save for later
        if (res)
        {
            pExtension->LatestAnimation = action;
            pExtension->LatestAnimationDispatch = 0;
        }

        World::Get().GetRunner().Trigger(action);

        return res;
    }

    return 0;
}

ActorMediator* ActorMediator::Get() noexcept
{
    POINTER_SKYRIMSE(ActorMediator*, s_actorMediator, 403567);

    return *(s_actorMediator.Get());
}

bool ActorMediator::PerformAction(TESActionData* apAction) noexcept
{
    if (apAction->actor->formID == 0x13482)
    {
        /*static Set<uint32_t> s_ids;

        spdlog::error("New frame");
        for(auto i = 0; i < action.Variables.size(); ++i)
        {
            auto& oldVars = pExtension->LatestVariables.Variables;
            auto& newVars = action.Variables;
            if(oldVars[i] != newVars[i] && s_ids.count(i) == 0)
            {
                //s_ids.insert(i);
                spdlog::info("Var {} changed from {} to {}", i, oldVars[i], newVars[i]);
            }
        }*/
        // spdlog::info("Play animation name: {} with idle {:X} and target {:X} and unk {:X}", apAction->action->keyword.AsAscii(), (apAction->idleForm ? apAction->idleForm->formID : 0), (apAction->target ? apAction->target->formID : 0), apAction->unkInput);
    }

    const auto res = TiltedPhoques::ThisCall(RealPerformAction, this, apAction);
    // const auto res = RePerformAction(apAction, aValue);

    if (res && apAction->actor->formID == 0x13482)
    {
        //    spdlog::info("Passed !");
    }

    return res != 0;
}

bool ActorMediator::ForceAction(TESActionData* apAction) noexcept
{
    TP_THIS_FUNCTION(TAnimationStep, uint8_t, ActorMediator, TESActionData*);

    POINTER_SKYRIMSE(TAnimationStep, PerformComplexAction, 38953);

    uint8_t result = 0;

    auto pActor = static_cast<Actor*>(apAction->actor);
    if (pActor)
    {
        result = TiltedPhoques::ThisCall(PerformComplexAction, this, apAction);
        // 38953 / 1406DFF90 already calls 39004 / 1406E2250 with the action's result.
        // A second call here omitted its third argument and could repeat state transitions
        // (including furniture state) using an undefined success byte.
    }

    return result;
}

ActionInput::ActionInput(uint32_t aParam1, Actor* apActor, BGSAction* apAction, TESObjectREFR* apTarget)
{
    // skip vtable as we never use this directly
    actor = apActor;
    target = apTarget;
    action = apAction;
    unkInput = aParam1;
}

void ActionInput::Release()
{
    actor.Release();
    target.Release();
}

ActionOutput::ActionOutput()
    : eventName("")
    , targetEventName("")
{
    // skip vtable as we never use this directly

    result = 0;
    targetIdleForm = nullptr;
    idleForm = nullptr;
    unk1 = 0;
}

void ActionOutput::Release()
{
    eventName.Release();
    targetEventName.Release();
}

BGSActionData::BGSActionData(uint32_t aParam1, Actor* apActor, BGSAction* apAction, TESObjectREFR* apTarget)
    : ActionInput(aParam1, apActor, apAction, apTarget)
{
    // skip vtable as we never use this directly
    someFlag = 0;
}

TESActionData::TESActionData(uint32_t aParam1, Actor* apActor, BGSAction* apAction, TESObjectREFR* apTarget)
    : BGSActionData(aParam1, apActor, apAction, apTarget)
{
    POINTER_SKYRIMSE(void*, s_vtbl, 188603);

    someFlag = false;

    *reinterpret_cast<void**>(this) = s_vtbl.Get();
}

TESActionData::~TESActionData()
{
    ActionOutput::Release();
    ActionInput::Release();
}

static TiltedPhoques::Initializer s_animationHook(
    []()
    {
        POINTER_SKYRIMSE(TPerformAction, performAction, 38949);

        RealPerformAction = performAction.Get();

        TP_HOOK(&RealPerformAction, HookPerformAction);
    });
