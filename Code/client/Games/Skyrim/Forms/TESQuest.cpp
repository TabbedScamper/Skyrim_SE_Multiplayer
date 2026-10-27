#include <Forms/TESQuest.h>

#include <Services/PapyrusService.h>
#include <Services/QuestService.h>
#include <Services/PartyService.h>

#include <Games/Overrides.h>
#include <World.h>
#include <Utils/MainThread.h>
#include <atomic>

namespace
{
TP_THIS_FUNCTION(TNativeSetStage, bool, TESQuest, uint16_t);
TNativeSetStage* RealNativeSetStage = nullptr;
TP_THIS_FUNCTION(TStageRunner, bool, TESQuest::Stage, TESQuest*, bool);
TStageRunner* RealStageRunner = nullptr;
using TStageItem = void(void*, bool);
TStageItem* RealStageItem = nullptr;
std::atomic_bool s_itemHookReady{};
TP_THIS_FUNCTION(TSetObjectiveState, void, TESQuest::Objective, int, void*, void*);
TSetObjectiveState* RealSetObjectiveState = nullptr;
TP_THIS_FUNCTION(TSetQuestFlag, void, TESQuest, bool);
TSetQuestFlag* RealSetCompleted = nullptr;
TSetQuestFlag* RealSetFailed = nullptr;
TP_THIS_FUNCTION(TResetQuest, void, TESQuest);
TResetQuest* RealResetQuest = nullptr;

bool TP_MAKE_THISCALL(HookStageRunner, TESQuest::Stage, TESQuest* apQuest, bool aForce)
{
    MT::Scope inStep(MT::Ctx::InStep);
    const auto caller = reinterpret_cast<uintptr_t>(_ReturnAddress());
    static VersionDbPtr<void> setStage(25004), startStages(25000), stopStages(25001), completeAll(23231);
    const auto kind = caller == reinterpret_cast<uintptr_t>(startStages.Get()) + 0x48 ? QuestAliasFills::Startup :
        caller == reinterpret_cast<uintptr_t>(stopStages.Get()) + 0x48 ? QuestAliasFills::Shutdown :
        caller == reinterpret_cast<uintptr_t>(completeAll.Get()) + 0x3F ? QuestAliasFills::CompleteAll :
        QuestAliasFills::SetStage;
    if (kind == QuestAliasFills::SetStage &&
        caller != reinterpret_cast<uintptr_t>(setStage.Get()) + 0x74)
    {
        static std::atomic_uint32_t unknownCalls{};
        if (unknownCalls.fetch_add(1, std::memory_order_relaxed) < 16)
            spdlog::warn("QSP runner has an unclassified caller at {:X}; treating it as SetStage", caller);
    }
    return QuestService::RunStage(apThis, apQuest, aForce, kind, RealStageRunner);
}

void HookStageItem(void* apItem, bool aForce)
{
    MT::Scope inStep(MT::Ctx::InStep);
    QuestService::ApplyStageItem(apItem, aForce, RealStageItem);
}

bool InstallStageItemCall(TStageRunner* apRunner) noexcept
{
    auto* site = reinterpret_cast<uint8_t*>(apRunner) + 0xB0; // 0x1403E1A60 / 25252+0xB0
    static VersionDbPtr<void> nativeItem(25260);
    if (*site != 0xE8)
        return false;
    int32_t oldOffset{};
    std::memcpy(&oldOffset, site + 1, sizeof(oldOffset));
    const auto original = reinterpret_cast<uintptr_t>(site + 5) + oldOffset;
    if (original != reinterpret_cast<uintptr_t>(nativeItem.Get()))
        return false;
    const auto delta = reinterpret_cast<intptr_t>(&HookStageItem) -
        reinterpret_cast<intptr_t>(site + 5);
    if (delta < INT32_MIN || delta > INT32_MAX)
        return false;
    DWORD oldProtection{};
    if (!VirtualProtect(site, 5, PAGE_EXECUTE_READWRITE, &oldProtection))
        return false;
    const auto offset = static_cast<int32_t>(delta);
    std::memcpy(site + 1, &offset, sizeof(offset));
    DWORD ignored{};
    VirtualProtect(site, 5, oldProtection, &ignored);
    FlushInstructionCache(GetCurrentProcess(), site, 5);
    RealStageItem = reinterpret_cast<TStageItem*>(original);
    return true;
}

void TP_MAKE_THISCALL(HookSetObjectiveState, TESQuest::Objective, int aState, void* a3, void* a4)
{
    MT::Scope inStep(MT::Ctx::InStep);
    TiltedPhoques::ThisCall(RealSetObjectiveState, apThis, aState, a3, a4);
    if (apThis && apThis->parent)
        QuestService::MarkQuestStateDirty(apThis->parent->formID);
}

void TP_MAKE_THISCALL(HookSetCompleted, TESQuest, bool aValue)
{
    MT::Scope inStep(MT::Ctx::InStep);
    TiltedPhoques::ThisCall(RealSetCompleted, apThis, aValue);
    QuestService::MarkQuestStateDirty(apThis->formID);
}

void TP_MAKE_THISCALL(HookSetFailed, TESQuest, bool aValue)
{
    MT::Scope inStep(MT::Ctx::InStep);
    TiltedPhoques::ThisCall(RealSetFailed, apThis, aValue);
    QuestService::MarkQuestStateDirty(apThis->formID);
}

void TP_MAKE_THISCALL(HookResetQuest, TESQuest)
{
    MT::Scope inStep(MT::Ctx::InStep);
    static VersionDbPtr<void> beginStartup(32485);
    const bool startupReset = reinterpret_cast<uintptr_t>(_ReturnAddress()) ==
        reinterpret_cast<uintptr_t>(beginStartup.Get()) + 0x6A;
    TiltedPhoques::ThisCall(RealResetQuest, apThis);
    if (!startupReset)
        QuestService::CaptureQuestReset(apThis);
}

bool TP_MAKE_THISCALL(HookNativeSetStage, TESQuest, uint16_t aStage)
{
    MT::Scope inStep(MT::Ctx::InStep);
    static std::atomic<uint32_t> s_samples{0};
    const auto sample = s_samples.fetch_add(1, std::memory_order_relaxed);
    const bool isIntroQuest = apThis->formID == 0x0003372B;
    const bool applyAsFollower = QuestService::ApplyAsFollower();
    const bool overridden = ScopedQuestOverride::IsOverriden();
    if (sample < 128 || isIntroQuest)
        spdlog::info("Native quest stage enter form={:X} from={} to={} follower={} override={} tick={}",
            apThis->formID, apThis->currentStage, aStage, applyAsFollower, overridden, GetTickCount64());

    std::optional<QuestService::StageContext> allowedContext;
    // The scoped apply passes the authority branch, then still reaches the alias gate.
    if (applyAsFollower && !overridden && !QuestService::IsNonSyncableQuest(apThis))
    {
        uint32_t mask{};
        uint8_t count{};
        bool overflow{};
        if (apThis->currentStage != aStage && !apThis->IsStageDone(aStage) &&
            QuestService::GetHostMask(apThis->formID, aStage, mask, count, overflow))
            allowedContext.emplace(apThis->formID, aStage, mask, count, overflow);
        else
        {
            static std::atomic<uint32_t> s_suppressedSamples{0};
            if (s_suppressedSamples.fetch_add(1, std::memory_order_relaxed) < 128)
                spdlog::info("Suppressed follower-local quest stage form={:X} from={} to={}",
                    apThis->formID, apThis->currentStage, aStage);
            return false;
        }
    }

    // Quest startup may refill aliases after the network snapshot arrived.
    // The native stage writer is the last gate before stage fragments are queued.
    if (applyAsFollower &&
        !World::Get().GetQuestService().PrepareAliasesForStage(apThis))
        return false;

    const bool result = TiltedPhoques::ThisCall(RealNativeSetStage, apThis, aStage);
    if (sample < 128 || isIntroQuest)
        spdlog::info("Native quest stage leave form={:X} stage={} result={}", apThis->formID, apThis->currentStage, result);
    return result;
}
}

bool QuestStageItemHookReady() noexcept
{
    return s_itemHookReady.load(std::memory_order_acquire);
}

TESObjectREFR* TESQuest::GetAliasedRef(uint32_t aAliasID) noexcept
{
    return TESObjectREFR::GetByHandle(GetAliasHandle(aAliasID));
}

uint32_t TESQuest::GetAliasHandle(uint32_t aAliasID) noexcept
{
    TP_THIS_FUNCTION(TGetAliasedRef, BSPointerHandle<TESObjectREFR>*, TESQuest, BSPointerHandle<TESObjectREFR>*, uint32_t);
    POINTER_SKYRIMSE(TGetAliasedRef, getAliasedRef, 25066);

    BSPointerHandle<TESObjectREFR> result{};
    TiltedPhoques::ThisCall(getAliasedRef, this, &result, aAliasID);

    return result.handle.iBits;
}

bool BGSBaseAlias::IsReference() const noexcept
{
    // 1.7.104: ID 25042 tests QType (vtable slot 3), not the fill type.
    return std::strcmp(QType().AsAscii(), "Ref") == 0;
}

BGSBaseAlias* TESQuest::GetReferenceAlias(uint32_t aAliasId) noexcept
{
    TP_THIS_FUNCTION(TGetReferenceAlias, BGSBaseAlias*, TESQuest, uint32_t);
    POINTER_SKYRIMSE(TGetReferenceAlias, getReferenceAlias, 25042);
    return TiltedPhoques::ThisCall(getReferenceAlias, this, aAliasId);
}

void TESQuest::ForceAliasReference(uint32_t aAliasId, TESObjectREFR* apReference) noexcept
{
    // ID 25052 (VA 1403D4FA0) maintains handles, persistence, reservations and scenes.
    TP_THIS_FUNCTION(TForceAliasReference, void, TESQuest, uint32_t, TESObjectREFR*);
    POINTER_SKYRIMSE(TForceAliasReference, forceAliasReference, 25052);
    TiltedPhoques::ThisCall(forceAliasReference, this, aAliasId, apReference);
}

void TESQuest::ClearAliasReference(BGSBaseAlias* apAlias) noexcept
{
    // ID 25051 (VA 1403D4BC0) is the cleanup used by ForceRefTo and Papyrus Clear.
    // The leader can have an empty required alias too; do not change its authored flags.
    TP_THIS_FUNCTION(TClearAliasReference, void, TESQuest, BGSBaseAlias*);
    POINTER_SKYRIMSE(TClearAliasReference, clearAliasReference, 25051);
    TiltedPhoques::ThisCall(clearAliasReference, this, apAlias);
}

TESQuest::State TESQuest::getState()
{
    const bool enabled = (flags & Flags::Enabled) != 0;
    const bool stageWait = (flags & Flags::StageWait) != 0;
    if (enabled && !stageWait && !unkFlags)
        return State::Running;
    if (enabled && unkFlags)
        return State::WaitingPromotion;
    if (stageWait || enabled)
        return State::WaitingForStage;
    return State::Stopped;
}

void TESQuest::SetCompleted(bool force)
{
    TP_THIS_FUNCTION(TSetCompleted, void, TESQuest, bool);
    POINTER_SKYRIMSE(TSetCompleted, SetCompleted, 24991);
    SetCompleted(this, force);
}

void TESQuest::SetActive(bool toggle)
{
    if (toggle)
        flags |= 0x800;
    else
        flags &= 0xF7FF;
}

bool TESQuest::IsStageDone(uint16_t stageIndex)
{
    for (Stage* it : stages)
    {
        if (it->stageIndex == stageIndex)
            return it->IsDone();
    }

    return false;
}

bool TESQuest::EnsureQuestStarted(bool& success, bool force)
{
    TP_THIS_FUNCTION(TSetRunning, bool, TESQuest, bool*, bool);
    POINTER_SKYRIMSE(TSetRunning, SetRunning, 25003);
    return SetRunning(this, &success, force);
}

bool TESQuest::SetStage(uint16_t newStage)
{
    ScopedQuestOverride _;

    TP_THIS_FUNCTION(TSetStage, bool, TESQuest, uint16_t);
    POINTER_SKYRIMSE(TSetStage, SetStage, 25004);
    return SetStage(this, newStage);
}

void TESQuest::ScriptSetStage(uint16_t stageIndex)
{
    if (currentStage == stageIndex || IsStageDone(stageIndex))
        return;

    SetStage(stageIndex);
}

void TESQuest::SetStopped()
{
    flags &= 0xFFFE;
    MarkChanged(2);
}

static TiltedPhoques::Initializer s_questInitHooks(
    []()
    {
        POINTER_SKYRIMSE(TNativeSetStage, nativeSetStage, 25004);
        RealNativeSetStage = nativeSetStage.Get();
        TP_HOOK(&RealNativeSetStage, HookNativeSetStage);
        POINTER_SKYRIMSE(TStageRunner, stageRunner, 25252);
        POINTER_SKYRIMSE(TSetObjectiveState, setObjectiveState, 23933);
        POINTER_SKYRIMSE(TSetQuestFlag, setCompleted, 24991);
        POINTER_SKYRIMSE(TSetQuestFlag, setFailed, 24988);
        POINTER_SKYRIMSE(TResetQuest, resetQuest, 25014);
        RealStageRunner = stageRunner.Get();
        RealSetObjectiveState = setObjectiveState.Get();
        RealSetCompleted = setCompleted.Get();
        RealSetFailed = setFailed.Get();
        RealResetQuest = resetQuest.Get();
        s_itemHookReady.store(InstallStageItemCall(RealStageRunner), std::memory_order_release);
        if (!QuestStageItemHookReady())
            spdlog::critical("QSP item mask call-site patch failed at 25252+0xB0");
        TP_HOOK(&RealStageRunner, HookStageRunner);
        TP_HOOK(&RealSetObjectiveState, HookSetObjectiveState);
        TP_HOOK(&RealSetCompleted, HookSetCompleted);
        TP_HOOK(&RealSetFailed, HookSetFailed);
        TP_HOOK(&RealResetQuest, HookResetQuest);
        // kill quest init in cold blood
        // TiltedPhoques::Write<uint8_t>(25003, 0xC3);
    });
