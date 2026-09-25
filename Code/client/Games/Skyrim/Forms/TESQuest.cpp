#include <Forms/TESQuest.h>

#include <Services/PapyrusService.h>
#include <Services/QuestService.h>
#include <Services/PartyService.h>

#include <Games/Overrides.h>
#include <World.h>
#include <atomic>

namespace
{
TP_THIS_FUNCTION(TNativeSetStage, bool, TESQuest, uint16_t);
TNativeSetStage* RealNativeSetStage = nullptr;

bool TP_MAKE_THISCALL(HookNativeSetStage, TESQuest, uint16_t aStage)
{
    static std::atomic<uint32_t> s_samples{0};
    const auto sample = s_samples.fetch_add(1, std::memory_order_relaxed);
    const bool isIntroQuest = apThis->formID == 0x0003372B;
    if (sample < 128 || isIntroQuest)
    {
        const auto& party = World::Get().GetPartyService();
        spdlog::info("Native quest stage enter form={:X} from={} to={} party={} leader={} override={} tick={}",
            apThis->formID, apThis->currentStage, aStage, party.IsInParty(), party.IsLeader(),
            ScopedQuestOverride::IsOverriden(), GetTickCount64());
    }

    // A follower's Papyrus/scene VM is not the shared campaign authority.
    // The leader's sequenced NotifyQuestUpdate is the only permitted stage
    // writer while a shared campaign is loading or running. Local-only quests
    // remain native and the scoped host apply passes through this same hook.
    //
    // Exception: a stage the leader has already entered this epoch. The
    // leader's update for it can arrive before this follower's own new game
    // has even started the quest (measured: MQ101 stage 10 arrived ~2 s before
    // the follower's StartNewGame reached it); applying it then does nothing,
    // and suppressing the follower's own write afterwards meant stage 10 -
    // which moves the player into the Helgen cart - never ran, leaving the
    // follower in the void behind the main menu.
    const auto& party = World::Get().GetPartyService();
    if (party.IsInParty() && !party.IsLeader() && party.GetStartEpoch() != 0 &&
        party.GetSessionState() >= 1 && !ScopedQuestOverride::IsOverriden() &&
        !QuestService::IsNonSyncableQuest(apThis) && apThis->currentStage != aStage &&
        QuestService::HostReachedStage(apThis->formID, aStage, party.GetStartEpoch()))
    {
        spdlog::info("Allowed follower quest stage form={:X} from={} to={}: the leader already reached it",
            apThis->formID, apThis->currentStage, aStage);
    }
    else if (party.IsInParty() && !party.IsLeader() && party.GetStartEpoch() != 0 &&
        party.GetSessionState() >= 1 && !ScopedQuestOverride::IsOverriden() &&
        !QuestService::IsNonSyncableQuest(apThis))
    {
        static std::atomic<uint32_t> s_suppressedSamples{0};
        if (s_suppressedSamples.fetch_add(1, std::memory_order_relaxed) < 128)
            spdlog::info("Suppressed follower-local quest stage form={:X} from={} to={} epoch={}",
                apThis->formID, apThis->currentStage, aStage, party.GetStartEpoch());
        return false;
    }

    const bool result = TiltedPhoques::ThisCall(RealNativeSetStage, apThis, aStage);
    if (sample < 128 || isIntroQuest)
        spdlog::info("Native quest stage leave form={:X} stage={} result={}", apThis->formID, apThis->currentStage, result);
    return result;
}
}

TESObjectREFR* TESQuest::GetAliasedRef(uint32_t aAliasID) noexcept
{
    TP_THIS_FUNCTION(TGetAliasedRef, BSPointerHandle<TESObjectREFR>*, TESQuest, BSPointerHandle<TESObjectREFR>*, uint32_t);
    POINTER_SKYRIMSE(TGetAliasedRef, getAliasedRef, 25066);

    BSPointerHandle<TESObjectREFR> result{};
    TiltedPhoques::ThisCall(getAliasedRef, this, &result, aAliasID);

    return TESObjectREFR::GetByHandle(result.handle.iBits);
}

TESQuest::State TESQuest::getState()
{
    if (flags >= 0)
    {
        if (unkFlags)
            return State::WaitingPromotion;
        else if (flags & 1)
            return State::Running;
        else
            return State::Stopped;
    }

    return State::WaitingForStage;
}

void TESQuest::SetCompleted(bool force)
{
    TP_THIS_FUNCTION(TSetCompleted, void, TESQuest, bool);
    POINTER_SKYRIMSE(TSetCompleted, SetCompleted, 24991);
    SetCompleted(this, force);
}

void TESQuest::CompleteAllObjectives()
{
    TP_THIS_FUNCTION(TCompleteAllObjectives, void, TESQuest);
    POINTER_SKYRIMSE(TCompleteAllObjectives, CompleteAll, 23231);
    CompleteAll(this);
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

bool TESQuest::Kill()
{
    using TSetStopped = void(TESQuest*, bool);
    POINTER_SKYRIMSE(TSetStopped, SetStopped, 24987);

    if (flags & Flags::Enabled)
    {
        unkFlags = 0;
        flags = Flags::Completed;
        MarkChanged(2);

        // SetStopped(this, false);
        return true;
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

    using Quest = TESQuest;
    PAPYRUS_FUNCTION(void, Quest, SetCurrentStageID, int);
    s_pSetCurrentStageID(this, stageIndex);
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
        // kill quest init in cold blood
        // TiltedPhoques::Write<uint8_t>(25003, 0xC3);
    });
