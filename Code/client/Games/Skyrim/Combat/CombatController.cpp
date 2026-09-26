#include "CombatController.h"
#include "CombatTargetSelector.h"
#include <Games/ActorExtension.h>

namespace
{
std::atomic<uint32_t> s_targetTrialActorFormId{0};
std::atomic<uint64_t> s_targetTrialCalls{0};
std::atomic<uint64_t> s_targetTrialOverrides{0};
std::atomic<uint32_t> s_targetTrialLastRequested{0};
std::atomic<uint32_t> s_targetTrialLastPresented{0xFFFFFFFFu};
std::atomic<uint32_t> s_targetTrialLastNative{0};
std::atomic<uintptr_t> s_targetTrialLastCallerRva{0};
}

void CombatController::SetTargetAuthorityTrialActor(uint32_t aActorFormId) noexcept
{
    s_targetTrialActorFormId.store(aActorFormId, std::memory_order_release);
}

CombatController::TargetAuthorityTrialDiagnostics
CombatController::GetTargetAuthorityTrialDiagnostics() noexcept
{
    return {s_targetTrialActorFormId.load(std::memory_order_acquire),
        s_targetTrialCalls.load(std::memory_order_relaxed),
        s_targetTrialOverrides.load(std::memory_order_relaxed),
        s_targetTrialLastRequested.load(std::memory_order_relaxed),
        s_targetTrialLastPresented.load(std::memory_order_relaxed),
        s_targetTrialLastNative.load(std::memory_order_relaxed),
        s_targetTrialLastCallerRva.load(std::memory_order_relaxed)};
}

void ArrayQuickSortRecursiveCombatTargets(GameArray<CombatTargetSelector*>* apArray, uint32_t aiLowIndex,
                                          uint32_t aiHighIndex)
{
    using TArrayQuickSort = void(GameArray<CombatTargetSelector*>* apArray, void* apFunction, uint32_t aiLowIndex, uint32_t aiHighIndex);
    POINTER_SKYRIMSE(TArrayQuickSort, arrayQuickSort, 33285);

    using TSortTargetSelectors = int64_t(int64_t, int64_t);
    POINTER_SKYRIMSE(TSortTargetSelectors, sortTargetSelectors, 33282);

    arrayQuickSort(apArray, sortTargetSelectors, aiLowIndex, aiHighIndex);
}

void CombatController::UpdateTarget()
{
    for (auto* pTargetSelector : targetSelectors)
    {
        if ((pTargetSelector->flags & 2) == 0)
            pTargetSelector->Update();
    }

    if (targetSelectors.length > 1)
        ArrayQuickSortRecursiveCombatTargets(&targetSelectors, 0, targetSelectors.length - 1);

    pActiveTargetSelector = nullptr;
    BSPointerHandle<Actor> newTarget{};

    if (targetSelectors.length)
    {
        for (auto* pTargetSelector : targetSelectors)
        {
            if ((pTargetSelector->flags & 1) != 0 && (pTargetSelector->flags & 2) == 0)
            {
                newTarget = pTargetSelector->SelectTarget();
                if (newTarget)
                {
                    pActiveTargetSelector = pTargetSelector;
                    break;
                }
            }
        }
    }

    // If CombatComponent is attached, don't try to fetch a new target.
    if (Actor* pAttacker = Cast<Actor>(TESObjectREFR::GetByHandle(attackerHandle)))
    {
        const auto view = World::Get().view<FormIdComponent, CombatComponent>();
        const auto it = std::find_if(view.begin(), view.end(), [view, pAttacker](auto entity) { return view.get<FormIdComponent>(entity).Id == pAttacker->formID; });
        if (it != view.end())
            return;
    }

    if (newTarget.handle.iBits == targetHandle)
        return;

    Actor* pNewTarget = Cast<Actor>(TESObjectREFR::GetByHandle(newTarget.handle.iBits));
    SetTarget(pNewTarget);
}

TP_THIS_FUNCTION(TUpdateTarget, void, CombatController);
static TUpdateTarget* RealUpdateTarget = nullptr;

void TP_MAKE_THISCALL(HookUpdateTarget, CombatController)
{
    apThis->UpdateTarget();
}

void CombatController::SetTarget(Actor* apTarget)
{
    TP_THIS_FUNCTION(TSetTarget, void, CombatController, Actor*);
    POINTER_SKYRIMSE(TSetTarget, setTarget, 33235);
    TiltedPhoques::ThisCall(setTarget, this, apTarget);
}

TP_THIS_FUNCTION(TNativeSetTarget, void, CombatController, Actor* apTarget);
static TNativeSetTarget* RealNativeSetTarget = nullptr;

void TP_MAKE_THISCALL(HookNativeSetTarget, CombatController, Actor* apTarget)
{
    const uint32_t selected = s_targetTrialActorFormId.load(
        std::memory_order_acquire);
    if (selected && apThis)
    {
        auto* pAttacker = Cast<Actor>(TESObjectREFR::GetByHandle(
            apThis->attackerHandle));
        if (pAttacker && pAttacker->formID == selected &&
            pAttacker->GetExtension()->IsRemote())
        {
            s_targetTrialCalls.fetch_add(1, std::memory_order_relaxed);
            s_targetTrialLastRequested.store(apTarget ? apTarget->formID : 0,
                std::memory_order_relaxed);
            const auto caller = reinterpret_cast<uintptr_t>(_ReturnAddress());
            const auto base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
            s_targetTrialLastCallerRva.store(caller >= base ? caller - base : 0,
                std::memory_order_relaxed);
            const uint32_t presented = pAttacker->GetExtension()->
                PresentedCombatTargetFormId.load(std::memory_order_acquire);
            s_targetTrialLastPresented.store(presented,
                std::memory_order_relaxed);
            if (presented != 0xFFFFFFFFu)
            {
                auto* pPresented = presented ? Cast<Actor>(TESForm::GetById(
                    presented)) : nullptr;
                if (!presented || pPresented)
                {
                    if (apTarget != pPresented)
                        s_targetTrialOverrides.fetch_add(1,
                            std::memory_order_relaxed);
                    apTarget = pPresented;
                }
            }
        }
    }
    // Every mirrored NPC consumes the presented owner target, including a target that
    // resolves to this PC's real player. Host-owned NPC selectors remain native.
    if (apThis && World::Get().GetTransport().IsConnected())
    {
        auto* attacker = Cast<Actor>(TESObjectREFR::GetByHandle(apThis->attackerHandle));
        if (attacker && attacker->GetExtension()->IsRemote() && !attacker->GetExtension()->IsPlayer())
        {
            const auto presented = attacker->GetExtension()->PresentedCombatTargetFormId.load(std::memory_order_acquire);
            if (presented != UINT32_MAX)
            {
                auto* target = presented ? Cast<Actor>(TESForm::GetById(presented)) : nullptr;
                if (!presented || target)
                    apTarget = target;
            }
        }
    }
    TiltedPhoques::ThisCall(RealNativeSetTarget, apThis, apTarget);
    if (selected && apThis)
    {
        auto* pAttacker = Cast<Actor>(TESObjectREFR::GetByHandle(
            apThis->attackerHandle));
        if (pAttacker && pAttacker->formID == selected &&
            pAttacker->GetExtension()->IsRemote())
        {
            auto* pNative = Cast<Actor>(TESObjectREFR::GetByHandle(
                apThis->targetHandle));
            s_targetTrialLastNative.store(pNative ? pNative->formID : 0,
                std::memory_order_relaxed);
        }
    }
}

static TiltedPhoques::Initializer s_combatControllerHooks(
    []()
    {
#if 0
        POINTER_SKYRIMSE(TUpdateTarget, s_updateTarget, 33236);

        RealUpdateTarget = s_updateTarget.Get();

        TP_HOOK(&RealUpdateTarget, HookUpdateTarget);
#endif
        POINTER_SKYRIMSE(TNativeSetTarget, s_setTarget, 33235);
        RealNativeSetTarget = s_setTarget.Get();
        TP_HOOK(&RealNativeSetTarget, HookNativeSetTarget);
    });

