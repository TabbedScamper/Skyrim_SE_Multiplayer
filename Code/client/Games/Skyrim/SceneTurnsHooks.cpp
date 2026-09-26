#include <TiltedOnlinePCH.h>
#include "SceneTurnsHooks.h"

#include <Actor.h>
#include <PlayerCharacter.h>
#include <Forms/TESQuest.h>
#include <Forms/TESIdleForm.h>
#include <Games/ActorExtension.h>
#include <Games/Animation/IAnimationGraphManagerHolder.h>
#include <Services/Generic/SceneTurnsService.h>
#include <Services/PapyrusService.h>
#include <World.h>

namespace
{
// CommonLib BGSSceneAction, checked against 1.7.104 IDs 24160 and 24234.
struct SceneAction
{
    void** Vtable;
    uint32_t ActorAlias;
    uint16_t StartPhase;
    uint16_t EndPhase;
    uint32_t Flags;
    uint8_t Status;
    uint8_t Pad15[3];
    uint32_t Index;
    uint32_t Pad1C;
};
static_assert(sizeof(SceneAction) == 0x20);
static_assert(offsetof(SceneAction, Status) == 0x14);

using SetupIdle = bool(__fastcall)(void*, Actor*, uint32_t, TESIdleForm*, bool, bool, TESObjectREFR*);
using PhaseCompletion = bool(__fastcall)(void*, BGSScene*, uint32_t, bool);
SetupIdle* s_setupIdle{};
PhaseCompletion* s_phaseCompletion{};
thread_local bool s_replaying{};

TESQuest* ParentQuest(BGSScene* apScene) noexcept
{
    // Native scene update 1403A97A0 / ID 24160 reads the parent at +0x98.
    return *reinterpret_cast<TESQuest**>(reinterpret_cast<uint8_t*>(apScene) + 0x98);
}

BGSScene* CurrentScene(Actor* apActor) noexcept
{
    // TESObjectREFR::GetCurrentScene, virtual slot 0x4A, called by ID 24160.
    using Function = BGSScene*(__fastcall*)(Actor*);
    return reinterpret_cast<Function>((*reinterpret_cast<void***>(apActor))[0x4A])(apActor);
}

bool FindStep(Actor* apActor, TESIdleForm* apIdle, TESObjectREFR* apTarget,
    uint32_t aDefaultAction, SceneTurnsNative::IdleStep& aStep) noexcept
{
    if (!apActor || !apIdle || apActor == PlayerCharacter::Get() ||
        apActor->GetExtension()->IsRemote())
        return false;
    auto* pScene = CurrentScene(apActor);
    if (!pScene || !pScene->isPlaying || pScene->rawPhaseWord >= pScene->phases.length ||
        !pScene->actions.data || pScene->actions.length > pScene->actions.capacity ||
        pScene->actions.length > 1024)
        return false;
    auto* pQuest = ParentQuest(pScene);
    if (!pQuest)
        return false;

    SceneAction* pMatch{};
    for (uint32_t i = 0; i < pScene->actions.length; ++i)
    {
        auto* pAction = static_cast<SceneAction*>(pScene->actions.data[i]);
        if (!pAction || !(pAction->Status & 1) || (pAction->Status & 2) ||
            (pAction->Flags & (1u << 16)) || pAction->StartPhase > pScene->rawPhaseWord ||
            pAction->EndPhase != pScene->rawPhaseWord ||
            pQuest->GetAliasedRef(pAction->ActorAlias) != apActor)
            continue;
        using GetType = uint32_t(__fastcall*)(SceneAction*);
        const auto type = reinterpret_cast<GetType>(pAction->Vtable[7])(pAction);
        // SpeakSound uses ActionDialogue (0x5C), including Helgen's response idle.
        // A package idle qualifies only with an explicit, resolved player target.
        const bool dialogue = type == 0 && aDefaultAction == 0x5C;
        if (!dialogue && type != 1)
            continue;
        bool targetsPlayer = apTarget && apTarget == PlayerCharacter::Get();
        if (apTarget && !targetsPlayer)
            continue;
        if (dialogue && !apTarget)
        {
            // Dialogue start 1403AB540 uses headtrack-player or alias +0x28.
            const auto alias = *reinterpret_cast<uint32_t*>(reinterpret_cast<uint8_t*>(pAction) + 0x28);
            targetsPlayer = (pAction->Flags & (1u << 17)) != 0 ||
                pQuest->GetAliasedRef(alias) == PlayerCharacter::Get();
        }
        if (!targetsPlayer)
            continue;
        if (pMatch)
            return false; // Ambiguous ownership: never replay a guessed action.
        pMatch = pAction;
    }
    if (!pMatch)
        return false;
    aStep = {pScene->formID, pQuest->formID, pScene->rawPhaseWord, pMatch->Index,
        apActor->GetHandle().handle.iBits, apActor->formID, apIdle->formID,
        aDefaultAction, GetTickCount64()};
    return true;
}

bool __fastcall HookSetupIdle(void* apProcess, Actor* apActor, uint32_t aAction,
    TESIdleForm* apIdle, bool aCheckConditions, bool aForce, TESObjectREFR* apTarget)
{
    SceneTurnsNative::IdleStep step{};
    const bool capture = SceneTurnsService::IsAuthoritative() && !s_replaying &&
        FindStep(apActor, apIdle, apTarget, aAction, step);
    const bool accepted = s_setupIdle(apProcess, apActor, aAction, apIdle,
        aCheckConditions, aForce, apTarget);
    if (accepted && capture)
        SceneTurnsService::Capture(step);
    return accepted;
}

bool __fastcall HookPhaseCompletion(void* apPhase, BGSScene* apScene,
    uint32_t aPhaseIndex, bool aActionsComplete)
{
    // Gate BEFORE ID 24305 can enqueue the phase-end fragment. Conditions can
    // override the all-actions-complete input, so changing that input is unsafe.
    if (apScene && SceneTurnsService::HoldPhase(apScene->formID, aPhaseIndex))
        return false;
    return s_phaseCompletion(apPhase, apScene, aPhaseIndex, aActionsComplete);
}

static TiltedPhoques::Initializer s_sceneTurnsHooks([]()
    {
        if (!SceneTurnsNative::IsEnabled())
            return;
        POINTER_SKYRIMSE(SetupIdle, setupIdle, 39256);
        POINTER_SKYRIMSE(PhaseCompletion, phaseCompletion, 24305);
        s_setupIdle = setupIdle.Get();
        s_phaseCompletion = phaseCompletion.Get();
        TP_HOOK(&s_setupIdle, HookSetupIdle);
        TP_HOOK(&s_phaseCompletion, HookPhaseCompletion);
    });
}

namespace SceneTurnsNative
{
bool IsEnabled() noexcept
{
    static const bool enabled = []()
    {
        char value[8]{};
        return GetEnvironmentVariableA("SKYRIM_COOP_SCENE_TURNS", value, sizeof(value)) == 1 && value[0] == '1';
    }();
    return enabled;
}

bool IsCurrent(const IdleStep& aStep, bool& aActionComplete) noexcept
{
    auto* pForm = TESForm::GetById(aStep.SceneId);
    // SCEN is form type 0x7A in TES5; don't reinterpret a replaced form after load.
    if (!pForm || static_cast<uint8_t>(pForm->formType) != 0x7A)
        return false;
    auto* pScene = static_cast<BGSScene*>(pForm);
    auto* pQuest = ParentQuest(pScene);
    if (!pQuest || pQuest->formID != aStep.QuestId || !pScene->isPlaying ||
        pScene->rawPhaseWord != aStep.Phase || !pScene->actions.data ||
        pScene->actions.length > pScene->actions.capacity || pScene->actions.length > 1024)
        return false;
    for (uint32_t i = 0; i < pScene->actions.length; ++i)
    {
        const auto* pAction = static_cast<const SceneAction*>(pScene->actions.data[i]);
        if (pAction && pAction->Index == aStep.ActionIndex)
        {
            auto* pActor = pQuest->GetAliasedRef(pAction->ActorAlias);
            if (!pActor || pActor->formID != aStep.ActorId ||
                pActor->GetHandle().handle.iBits != aStep.ActorHandle)
                return false;
            aActionComplete = (pAction->Status & 2) != 0;
            return true;
        }
    }
    return false;
}

bool Replay(const IdleStep& aStep, Actor* apActor, Actor* apTarget) noexcept
{
    auto* pIdle = TESForm::GetById(aStep.IdleId);
    if (!apActor || !apActor->currentProcess || !apTarget || !pIdle ||
        static_cast<uint8_t>(pIdle->formType) != 78 || !s_setupIdle)
        return false;
    s_replaying = true;
    // The native pipeline still reaches Animation.cpp's existing streaming hook.
    // No dialogue is replayed, so INFO/stage side effects cannot be duplicated.
    const bool accepted = s_setupIdle(apActor->currentProcess, apActor,
        aStep.DefaultAction, static_cast<TESIdleForm*>(pIdle), true, false, apTarget);
    s_replaying = false;
    return accepted;
}

bool Approach(Actor* apActor, Actor* apTarget) noexcept
{
    const auto& papyrus = World::Get().ctx().at<PapyrusService>();
    const void* pKeep = papyrus.Get("Actor", "KeepOffsetFromActor");
    const void* pClear = papyrus.Get("Actor", "ClearKeepOffsetFromActor");
    if (!pKeep || !pClear || !GameVM::Get() || !GameVM::Get()->virtualMachine)
        return false;
    // 140A010D0 / 54709: Actor + eight floats; native movement planner, no teleport.
    PapyrusFunction<void, Actor, Actor*, float, float, float, float, float, float, float, float> keep(pKeep);
    keep(apActor, apTarget, 0.f, 90.f, 0.f, 0.f, 0.f, 180.f, 15.f, 30.f);
    return true;
}

void ReleaseApproach(Actor* apActor) noexcept
{
    const void* pClear = World::Get().ctx().at<PapyrusService>().Get("Actor", "ClearKeepOffsetFromActor");
    if (apActor && pClear && GameVM::Get() && GameVM::Get()->virtualMachine)
    {
        PapyrusFunction<void, Actor> clear(pClear);
        clear(apActor);
    }
}

bool AnimationBusy(Actor* apActor, bool& aBusy) noexcept
{
    if (!apActor || !apActor->GetNiNode())
        return false;
    BSFixedString driven("bAnimationDriven");
    BSFixedString synced("bIsSynced");
    bool a{}, b{};
    const bool haveA = apActor->animationGraphHolder.GetVariableBool(&driven, &a);
    const bool haveB = apActor->animationGraphHolder.GetVariableBool(&synced, &b);
    aBusy = a || b;
    // This is a conservative lifecycle signal, not a universal clip-end API.
    // An idle that never asserts either variable must time out, never succeed.
    return haveA && haveB;
}
}
