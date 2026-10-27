#include <TiltedOnlinePCH.h>

#include <Events/ConnectedEvent.h>
#include <Events/DisconnectedEvent.h>
#include <Events/UpdateEvent.h>
#include <Components.h>
#include <Utils.h>

#include <Services/QuestService.h>
#include <Services/CheckpointSaves.h>
#include <Services/ImguiService.h>
#include <Services/DropInService.h>
#include <Utils/MainThread.h>

#include <PlayerCharacter.h>
#include <Actor.h>
#include <Forms/TESQuest.h>
#include <Games/TES.h>
#include <Games/Overrides.h>

#include <Events/EventDispatcher.h>

#include <Messages/RequestQuestUpdate.h>
#include <Messages/NotifyQuestUpdate.h>
#include <Messages/RequestQuestAliasFills.h>
#include <Services/LobbyService.h>

#include <atomic>

namespace
{
struct RunFrame
{
    uint32_t FormId{};
    uint16_t Stage{};
    uint32_t Mask{};
    bool Overflow{};
    uint8_t ItemCount{};
};
thread_local RunFrame s_runFrames[4];
thread_local uint8_t s_runDepth{};
thread_local uint32_t s_localRuns{};
struct ApplyContext
{
    uint32_t FormId{};
    uint16_t Stage{};
    uint32_t Mask{};
    uint8_t Count{};
    bool Overflow{};
    bool Active{};
};
thread_local ApplyContext s_applyContext;
thread_local bool s_stateOnlyApply{};
uint64_t MaskKey(uint32_t aFormId, uint16_t aStage, uint8_t aKind) noexcept
{
    return (static_cast<uint64_t>(aFormId) << 32) |
        (static_cast<uint64_t>(aStage) << 8) | aKind;
}

uint8_t CountStageItems(const TESQuest::Stage* apStage, bool& aOverflow) noexcept
{
    const auto* pEntry = reinterpret_cast<const GameList<void>::Entry*>(
        reinterpret_cast<const uint8_t*>(apStage) + 8);
    uint16_t count = 0;
    while (pEntry && count < 255)
    {
        if (!pEntry->data)
            break;
        if (reinterpret_cast<const uint8_t*>(pEntry->data)[0x15] >= 32)
            aOverflow = true;
        ++count;
        pEntry = pEntry->next;
    }
    return static_cast<uint8_t>(count);
}

void SnapshotObjectives(TESQuest* apQuest, QuestAliasFills& aRecord) noexcept
{
    if (!apQuest)
        return;
    for (auto* objective : apQuest->objectives)
    {
        if (!objective || aRecord.Objectives.size() >= QuestAliasFills::MaxObjectives)
            break;
        aRecord.Objectives.push_back({objective->stageId, objective->state});
    }
    std::sort(aRecord.Objectives.begin(), aRecord.Objectives.end(),
        [](const auto& a, const auto& b) { return a.Index < b.Index; });
    aRecord.Objectives.erase(std::unique(aRecord.Objectives.begin(), aRecord.Objectives.end(),
        [](const auto& a, const auto& b) { return a.Index == b.Index; }), aRecord.Objectives.end());
    aRecord.HasObjectives = !aRecord.Objectives.empty();
}

uint64_t NextQuestTransactionId() noexcept
{
    static std::atomic_uint64_t transactionId{
        (GetTickCount64() << 16) ^ static_cast<uint64_t>(GetCurrentProcessId())};
    return ++transactionId;
}
}

static TESQuest* FindQuestByNameId(const String& name)
{
    auto& questRegistry = ModManager::Get()->quests;
    auto it = std::find_if(questRegistry.begin(), questRegistry.end(), [name](auto* it) { return std::strcmp(it->idName.AsAscii(), name.c_str()); });

    return it != questRegistry.end() ? *it : nullptr;
}

QuestService::QuestService(World& aWorld, entt::dispatcher& aDispatcher)
    : m_world(aWorld)
{
    m_joinedConnection = aDispatcher.sink<ConnectedEvent>().connect<&QuestService::OnConnected>(this);
    m_questUpdateConnection = aDispatcher.sink<NotifyQuestUpdate>().connect<&QuestService::OnQuestUpdate>(this);
    m_aliasFillsConnection = aDispatcher.sink<NotifyQuestAliasFills>().connect<&QuestService::OnAliasFills>(this);
    m_updateConnection = aDispatcher.sink<UpdateEvent>().connect<&QuestService::OnUpdate>(this);
    m_disconnectConnection = aDispatcher.sink<DisconnectedEvent>().connect<&QuestService::OnDisconnected>(this);

    // A note about the Gameevents:
    // TESQuestStageItemDoneEvent gets fired to late, we instead use TESQuestStageEvent, because it responds immediately.
    // TESQuestInitEvent can be instead managed by start stop quest management.
    // bind game event listeners
    auto* pEventList = EventDispatcherManager::Get();
    pEventList->questStartStopEvent.RegisterSink(this);
    pEventList->questStageEvent.RegisterSink(this);
}

void QuestService::OnConnected(const ConnectedEvent&) noexcept
{
    // TODO: this should be followed with whatever the quest leader selected
    /*
    // deselect any active quests
    auto* pPlayer = PlayerCharacter::Get();
    for (auto& objective : pPlayer->objectives)
    {
        if (auto* pQuest = objective.instance->quest)
            pQuest->SetActive(false);
    }
    */
}

BSTEventResult QuestService::OnEvent(const TESQuestStartStopEvent* apEvent, const EventDispatcher<TESQuestStartStopEvent>*)
{
    const bool scopedOverride = ScopedQuestOverride::IsOverriden();
    auto* pQuest = Cast<TESQuest>(TESForm::GetById(apEvent->formId));
    RecordDebugEvent("local_start_stop", apEvent->formId, pQuest ? pQuest->currentStage : 0, scopedOverride);
    if (!scopedOverride && pQuest)
        CaptureState(pQuest, pQuest->IsStopped() ? QuestAliasFills::Stopped : QuestAliasFills::Started);
    return BSTEventResult::kOk;
}

BSTEventResult QuestService::OnEvent(const TESQuestStageEvent* apEvent, const EventDispatcher<TESQuestStageEvent>*)
{
    if (s_runDepth)
    {
        auto& frame = s_runFrames[s_runDepth - 1];
        if (frame.FormId == apEvent->formId && frame.Stage == apEvent->stageId)
        {
            if (apEvent->itemIndex < 32)
                frame.Mask |= 1u << apEvent->itemIndex;
            else
                frame.Overflow = true;
        }
    }
    return BSTEventResult::kOk;
}

namespace
{
std::mutex s_hostStagesLock;
uint64_t s_hostStagesEpoch{};
std::unordered_map<uint32_t, std::unordered_set<uint16_t>> s_hostStages;

void RecordHostStage(uint32_t aFormId, uint16_t aStage, uint64_t aEpoch) noexcept
{
    std::lock_guard lock(s_hostStagesLock);
    if (s_hostStagesEpoch != aEpoch)
    {
        s_hostStages.clear();
        s_hostStagesEpoch = aEpoch;
    }
    s_hostStages[aFormId].insert(aStage);
}
} // namespace

bool QuestService::HostReachedStage(uint32_t aFormId, uint16_t aStage, uint64_t aEpoch) noexcept
{
    std::lock_guard lock(s_hostStagesLock);
    if (s_hostStagesEpoch != aEpoch)
        return false;
    const auto it = s_hostStages.find(aFormId);
    return it != s_hostStages.end() && it->second.contains(aStage);
}

void QuestService::OnQuestUpdate(const NotifyQuestUpdate& aUpdate) noexcept
{
    ModSystem& modSystem = World::Get().GetModSystem();
    uint32_t formId = modSystem.GetGameId(aUpdate.Id);
    const auto& party = m_world.GetPartyService();
    if (!party.IsInParty() || party.IsLeader())
        return;
    const auto expectedEpoch = party.GetStartEpoch();
    if (party.IsInParty() && expectedEpoch != 0 &&
        (aUpdate.AuthorityEpoch != expectedEpoch || aUpdate.Revision == 0))
    {
        RecordDebugEvent("rejected_quest_epoch", formId, aUpdate.Stage, false);
        spdlog::warn("Rejected quest update form={:X} revision={} epoch={} expectedEpoch={}",
            formId, aUpdate.Revision, aUpdate.AuthorityEpoch, expectedEpoch);
        return;
    }
    if (m_appliedQuestEpoch != aUpdate.AuthorityEpoch)
    {
        m_appliedQuestRevisions.clear();
        m_appliedQuestEpoch = aUpdate.AuthorityEpoch;
    }
    if (aUpdate.Revision != 0)
    {
        const auto it = m_appliedQuestRevisions.find(formId);
        if (it != m_appliedQuestRevisions.end() && aUpdate.Revision <= it->second)
        {
            RecordDebugEvent("rejected_quest_revision", formId, aUpdate.Stage, false);
            return;
        }
    }
    RecordDebugEvent("remote_update", formId, aUpdate.Stage, true);
    if (aUpdate.Status == NotifyQuestUpdate::Started || aUpdate.Status == NotifyQuestUpdate::StageUpdate)
        RecordHostStage(formId, aUpdate.Stage, aUpdate.AuthorityEpoch);
    TESQuest* pQuest = Cast<TESQuest>(TESForm::GetById(formId));
    if (!pQuest)
    {
        spdlog::error("Failed to find quest, base id: {:X}, mod id: {:X}", aUpdate.Id.BaseId, aUpdate.Id.ModId);
        return;
    }

    if (pQuest->type == TESQuest::Type::None || pQuest->type == TESQuest::Type::Miscellaneous)
    {
        spdlog::info(__FUNCTION__ ": receiving type none/misc quest update gameId {:X} questStage {} questStatus {} questType {} formId {:X} name {}",
                     aUpdate.Id.LogFormat(), aUpdate.Stage, aUpdate.Status,
                     aUpdate.ClientQuestType, formId, pQuest->fullName.value.AsAscii());
    }

    // A stopped quest will attach its aliases during native startup. Forcing now would attach twice.
    if (aUpdate.Status != NotifyQuestUpdate::Stopped && pQuest->IsEnabled() && !PrepareAliasesForStage(pQuest))
        return;
    if (aUpdate.Status != NotifyQuestUpdate::Stopped && !pQuest->IsEnabled())
        return;

    ScopedQuestOverride remoteQuestApply;
    // Preserve ScriptSetStage's same-stage guard: startup reports currentStage 0 even when
    // no stage has run. Reapplying it can execute a fragment or fail if stage 0 is absent.
    bool bResult = false;
    switch (aUpdate.Status)
    {
    case NotifyQuestUpdate::Started:
    {
        bResult = pQuest->currentStage == aUpdate.Stage || pQuest->IsStageDone(aUpdate.Stage);
        if (!bResult)
        {
            pQuest->SetStage(aUpdate.Stage);
            bResult = pQuest->IsStageDone(aUpdate.Stage);
        }
        pQuest->SetActive(true);
        spdlog::info("Remote quest started: {:X}, stage: {}", formId, aUpdate.Stage);
        break;
    }
    case NotifyQuestUpdate::StageUpdate:
        bResult = pQuest->currentStage == aUpdate.Stage || pQuest->IsStageDone(aUpdate.Stage);
        if (!bResult)
        {
            pQuest->SetStage(aUpdate.Stage);
            bResult = pQuest->IsStageDone(aUpdate.Stage);
        }
        spdlog::info("Remote quest updated: {:X}, stage: {}", formId, aUpdate.Stage);
        break;
    case NotifyQuestUpdate::Stopped:
        bResult = StopQuest(formId);
        spdlog::info("Remote quest stopped: {:X}, stage: {}", formId, aUpdate.Stage);
        break;
    default: break;
    }

    if (!bResult)
        spdlog::error("Failed to update the client quest state, quest: {:X}, stage: {}, status: {}", formId, aUpdate.Stage, aUpdate.Status);
    else if (aUpdate.Revision != 0)
        m_appliedQuestRevisions[formId] = aUpdate.Revision;
}

void QuestService::OnDisconnected(const DisconnectedEvent&) noexcept
{
    m_captureLeader.store(false, std::memory_order_release);
    m_applyFollower.store(false, std::memory_order_release);
    m_captureEpoch.store(0, std::memory_order_release);
    m_aliasSession = false;
    m_sentAliasFills.clear();
    m_requiredEmptySamples.clear();
    m_pendingAliasFills.clear();
    std::lock_guard lock(m_aliasMutex);
    m_leaderAliasFills.clear();
    m_lastAliasSequence = 0;
    m_aliasOverflow = false;
}

bool QuestService::RefreshAliasSession() noexcept
{
    const auto& party = m_world.GetPartyService();
    if (!m_world.GetTransport().IsConnected() || !party.IsInParty())
    {
        m_captureLeader.store(false, std::memory_order_release);
        m_applyFollower.store(false, std::memory_order_release);
        m_captureEpoch.store(0, std::memory_order_release);
        if (m_aliasSession)
        {
            m_aliasSession = false;
            m_sentAliasFills.clear();
            m_requiredEmptySamples.clear();
            m_pendingAliasFills.clear();
            std::lock_guard lock(m_aliasMutex);
            m_leaderAliasFills.clear();
        }
        return false;
    }

    if (!m_aliasSession || m_aliasEpoch != party.GetStartEpoch())
    {
        m_sentAliasFills.clear();
        m_requiredEmptySamples.clear();
        m_pendingAliasFills.clear();
        {
            std::lock_guard lock(m_aliasMutex);
            m_leaderAliasFills.clear();
            m_aliasEpoch = party.GetStartEpoch();
            m_aliasLeader = party.GetLeaderPlayerId();
        }
        m_aliasSession = true;
        m_aliasOverflow = false;
        m_lastAliasSequence = 0;
        m_aliasQuestCursor = 0;
        m_nextAliasSample = 0;
        m_nextAliasRetry = 0;
        m_aliasRetryAfter = 0;
        m_appliedQuestRevisions.clear();
        {
            std::lock_guard lock(m_runMutex);
            m_runQueue.clear();
            m_dirtyQuests.clear();
            m_questSeq.clear();
            m_runCounts.clear();
            m_pendingRuns.clear();
            m_pendingQuestOrder.clear();
            m_pendingRunCount = 0;
            m_appliedSeq.clear();
            m_hostMasks.clear();
            m_waitSince.clear();
        }
        std::lock_guard lock(s_hostStagesLock);
        s_hostStages.clear();
        s_hostStagesEpoch = m_aliasEpoch;
    }
    if (m_aliasLeader != party.GetLeaderPlayerId())
    {
        {
            std::lock_guard lock(m_aliasMutex);
            m_aliasLeader = party.GetLeaderPlayerId();
        }
        m_sentAliasFills.clear();
        m_requiredEmptySamples.clear();
        m_aliasQuestCursor = 0;
        m_nextAliasSample = 0;
    }
    if (m_aliasMembers != party.GetPartyMembers())
    {
        m_aliasMembers = party.GetPartyMembers();
        // A new member needs unchanged running quests as well.
        m_sentAliasFills.clear();
        m_requiredEmptySamples.clear();
        m_aliasQuestCursor = 0;
        m_nextAliasSample = 0;
    }
    m_captureEpoch.store(party.GetStartEpoch(), std::memory_order_release);
    m_captureSession.store(party.GetSessionState(), std::memory_order_release);
    m_captureLeader.store(party.IsLeader() && QuestStageItemHookReady(), std::memory_order_release);
    m_applyFollower.store(!party.IsLeader() && QuestStageItemHookReady(), std::memory_order_release);
    return true;
}

bool QuestService::SendAliasFills(TESQuest* apQuest, const RequestQuestUpdate* apUpdate,
    const QuestAliasFills* apRun) noexcept
{
    // Only quests that are shared: a follower must never start another PC's system quest to take its fills.
    if (!apQuest || IsNonSyncableQuest(apQuest))
        return false;
    if (!RefreshAliasSession() || !m_world.GetPartyService().IsLeader())
        return false;

    const auto& aliases = apQuest->aliases;
    if ((!apRun || apRun->RecordKind != QuestAliasFills::Skipped) &&
        (aliases.length > QuestAliasFills::MaxAliases || aliases.length > aliases.capacity ||
        (aliases.length && !aliases.data))
        )
    {
        spdlog::error("Quest alias snapshot exceeds bounds: quest {:X}, count {}", apQuest->formID, aliases.length);
        return false;
    }

    RequestQuestAliasFills request;
    auto& fills = request.Fills;
    if (apRun)
        fills = *apRun;
    auto& mods = m_world.GetModSystem();
    if (!mods.GetServerModId(apQuest->formID, fills.Id))
        return false;
    fills.AuthorityEpoch = m_aliasEpoch;
    fills.Sequence = NextQuestTransactionId();
    fills.Running = apUpdate ? apUpdate->Status != RequestQuestUpdate::Stopped : !apQuest->IsStopped();
    Vector<uint32_t> requiredEmptyIds;
    for (uint32_t i = 0; (!apRun || apRun->RecordKind != QuestAliasFills::Skipped) && i < aliases.length; ++i)
    {
        auto* pAlias = aliases.data[i];
        if (!pAlias || pAlias->owningQuest != apQuest || !pAlias->IsReference())
            continue;

        QuestAliasFill entry;
        entry.AliasId = pAlias->aliasID;
        const auto handle = apQuest->GetAliasHandle(entry.AliasId);
        auto* pReference = TESObjectREFR::GetByHandle(handle);
        if (!pReference && handle != *TESObjectREFR::GetNullHandle())
            continue;
        if (pReference)
        {
            if ((pReference->formID >> 24) == 0xFF)
            {
                const auto view = m_world.view<FormIdComponent>();
                const auto it = std::find_if(view.begin(), view.end(), [view, pReference](entt::entity aEntity)
                    { return view.get<FormIdComponent>(aEntity).Id == pReference->formID; });
                if (it == view.end())
                    continue;
                const auto serverId = Utils::GetServerId(*it);
                if (!serverId || !*serverId || *serverId == UINT32_MAX)
                    continue;
                entry.Reference = GameId(QuestAliasFills::ServerReference, *serverId);
            }
            else if (!mods.GetServerModId(pReference->formID, entry.Reference) ||
                mods.GetGameId(entry.Reference) != pReference->formID)
                continue;
        }
        fills.Entries.push_back(entry);
        if (!pReference && !(pAlias->flags & 2))
            requiredEmptyIds.push_back(entry.AliasId);
    }
    std::sort(fills.Entries.begin(), fills.Entries.end(),
        [](const auto& aLeft, const auto& aRight) { return aLeft.AliasId < aRight.AliasId; });
    if (apRun)
    {
        const auto sent = m_sentAliasFills.find(apQuest->formID);
        if (apRun->RecordKind == QuestAliasFills::Skipped ||
            (sent != m_sentAliasFills.end() && sent->second == fills.Entries))
        {
            fills.FillsIncluded = false;
            fills.Entries.clear();
        }
    }

    if (apUpdate)
    {
        fills.HasStage = true;
        fills.Stage = apUpdate->Stage;
        fills.Status = apUpdate->Status;
        fills.ClientQuestType = apUpdate->ClientQuestType;
        fills.TransactionId = apUpdate->TransactionId;
    }
    else
    {
        const auto it = m_sentAliasFills.find(apQuest->formID);
        const auto samples = m_requiredEmptySamples.find(apQuest->formID);
        const bool unconfirmed = std::any_of(requiredEmptyIds.begin(), requiredEmptyIds.end(),
            [samples, this](uint32_t aAliasId) {
                if (samples == m_requiredEmptySamples.end())
                    return true;
                const auto found = samples->second.find(aAliasId);
                return found == samples->second.end() || found->second < 2;
            });
        if (fills.Running && it != m_sentAliasFills.end() && it->second == fills.Entries && !unconfirmed)
            return true;
    }

    if (!fills.IsValid() || !m_world.GetTransport().Send(request))
        return false;
    Map<uint32_t, uint8_t> emptySamples;
    const auto oldSamples = m_requiredEmptySamples.find(apQuest->formID);
    for (const auto aliasId : requiredEmptyIds)
    {
        uint8_t count = 0;
        if (oldSamples != m_requiredEmptySamples.end())
            if (const auto old = oldSamples->second.find(aliasId); old != oldSamples->second.end())
                count = old->second;
        emptySamples[aliasId] = std::min<uint8_t>(2, count + 1);
    }
    if (emptySamples.empty() || !fills.Running)
        m_requiredEmptySamples.erase(apQuest->formID);
    else if (m_requiredEmptySamples.size() < 4096 || m_requiredEmptySamples.contains(apQuest->formID))
        m_requiredEmptySamples[apQuest->formID] = std::move(emptySamples);
    if (!fills.FillsIncluded)
        return true;
    if (!fills.Running)
        m_sentAliasFills.erase(apQuest->formID);
    else if (m_sentAliasFills.size() < 4096 || m_sentAliasFills.contains(apQuest->formID))
        m_sentAliasFills[apQuest->formID] = fills.Entries;
    return true;
}

bool QuestService::ResolveAliasFills(TESQuest* apQuest, const QuestAliasFills& aFills) noexcept
{
    Vector<LocalAliasFill> resolved;
    bool ready = true;
    for (const auto& entry : aFills.Entries)
    {
        uint32_t formId = 0;
        if (entry.Reference)
        {
            if (entry.Reference.ModId == QuestAliasFills::ServerReference)
            {
                if (auto* pReference = Utils::GetByServerId<TESObjectREFR>(entry.Reference.BaseId))
                    formId = pReference->formID;
            }
            else
                formId = m_world.GetModSystem().GetGameId(entry.Reference);
            if (!formId || !Cast<TESObjectREFR>(TESForm::GetById(formId)))
                ready = false;
        }
        auto* pAlias = apQuest->GetReferenceAlias(entry.AliasId);
        if (!pAlias)
            ready = false;
        resolved.push_back({entry.AliasId, formId, pAlias && !(pAlias->flags & 2)});
    }
    std::lock_guard lock(m_aliasMutex);
    if (m_leaderAliasFills.size() >= 4096 && !m_leaderAliasFills.contains(apQuest->formID))
        return false;
    if (const auto previous = m_leaderAliasFills.find(apQuest->formID); previous != m_leaderAliasFills.end())
    {
        for (auto& entry : resolved)
        {
            if (entry.FormId || !entry.Required)
                continue;
            const auto& old = previous->second.LocalFills;
            const auto it = std::lower_bound(old.begin(), old.end(), entry.AliasId,
                [](const LocalAliasFill& aFill, uint32_t aId) { return aFill.AliasId < aId; });
            if (it != old.end() && it->AliasId == entry.AliasId && !it->FormId)
                entry.ClearRequired = it->ClearRequired || previous->second.Fills.Sequence != aFills.Sequence;
        }
    }
    m_leaderAliasFills[apQuest->formID] = {aFills, std::move(resolved), ready};
    return ready;
}

bool QuestService::ApplyAliasFills(TESQuest* apQuest, const Vector<LocalAliasFill>& aFills) noexcept
{
    // Validate the complete snapshot before changing anything. Missing is not empty.
    bool changed = false;
    bool deferred = false;
    for (const auto& entry : aFills)
    {
        if (!apQuest->GetReferenceAlias(entry.AliasId) ||
            (entry.FormId && !Cast<TESObjectREFR>(TESForm::GetById(entry.FormId))))
            return false;
    }
    for (const auto& entry : aFills)
    {
        auto* pPrevious = apQuest->GetAliasedRef(entry.AliasId);
        const uint32_t previousId = pPrevious ? pPrevious->formID : 0;
        if (previousId == entry.FormId &&
            (entry.FormId || apQuest->GetAliasHandle(entry.AliasId) == *TESObjectREFR::GetNullHandle()))
            continue;
        if (entry.FormId)
        {
            auto* pRef = Cast<TESObjectREFR>(TESForm::GetById(entry.FormId));
            auto* pActor = Cast<Actor>(pRef);
            using TIsLeveled = bool(Actor*);
            POINTER_SKYRIMSE(TIsLeveled, isLeveled, 20229);
            if (pActor && isLeveled(pActor))
            {
                using TIsCellLoaded = bool(TES*, TESObjectCELL*, bool);
                POINTER_SKYRIMSE(TIsCellLoaded, isCellLoaded, 13308);
                if (!pActor->parentCell || !isCellLoaded(TES::Get(), pActor->parentCell, false))
                {
                    deferred = true;
                    continue;
                }
            }
            apQuest->ForceAliasReference(entry.AliasId, pRef);
        }
        else if (!(apQuest->GetReferenceAlias(entry.AliasId)->flags & 2) && !entry.ClearRequired)
        {
            spdlog::debug("Alias fill: deferring required alias {} clear on quest {:X} until a second snapshot",
                entry.AliasId, apQuest->formID);
            continue;
        }
        else
            apQuest->ClearAliasReference(apQuest->GetReferenceAlias(entry.AliasId));
        auto* pApplied = apQuest->GetAliasedRef(entry.AliasId);
        if ((pApplied ? pApplied->formID : 0) != entry.FormId ||
            (!entry.FormId && apQuest->GetAliasHandle(entry.AliasId) != *TESObjectREFR::GetNullHandle()))
        {
            if (changed)
                CheckpointSaves::QueueSaveChange(apQuest->formID, 0x10000000);
            return false;
        }
        changed = true;
        spdlog::info("Alias fill: quest {:X} alias {} {:X} -> {:X} (leader's)",
            apQuest->formID, entry.AliasId, previousId, entry.FormId);
    }
    if (changed)
        CheckpointSaves::QueueSaveChange(apQuest->formID, 0x10000000);
    return !deferred;
}

bool QuestService::RunStage(TESQuest::Stage* apStage, TESQuest* apQuest, bool aForce,
    uint8_t aKind, bool (*apOriginal)(TESQuest::Stage*, TESQuest*, bool)) noexcept
{
    if (!apStage || s_runDepth >= std::size(s_runFrames))
        return apOriginal(apStage, apQuest, aForce);
    const uint8_t before = apStage->flags;
    const bool willRun = !(before & 1) || (apQuest && (apQuest->flags & TESQuest::AllowRepeatStages));
    auto& frame = s_runFrames[s_runDepth++];
    bool itemOverflow = false;
    const uint8_t itemCount = CountStageItems(apStage, itemOverflow);
    frame = {apQuest ? apQuest->formID : 0, apStage->stageIndex, 0, itemOverflow, itemCount};
    const auto previousContext = s_applyContext;
    if (!s_applyContext.Active && apQuest && World::Exists() &&
        (aKind == QuestAliasFills::Startup || aKind == QuestAliasFills::Shutdown))
    {
        auto& service = World::Get().GetQuestService();
        if (service.m_applyFollower.load(std::memory_order_acquire))
        {
            std::lock_guard lock(service.m_runMutex);
            if (const auto it = service.m_hostMasks.find(MaskKey(apQuest->formID,
                    apStage->stageIndex, aKind)); it != service.m_hostMasks.end())
                s_applyContext = {apQuest->formID, apStage->stageIndex,
                    it->second.Mask, it->second.Count, it->second.Overflow, true};
        }
    }
    const bool result = apOriginal(apStage, apQuest, aForce);
    s_applyContext = previousContext;
    const auto mask = frame.Mask;
    const auto overflow = frame.Overflow;
    --s_runDepth;
    if (willRun)
        ++s_localRuns;
    if (willRun && apQuest && World::Exists() &&
        World::Get().GetQuestService().m_applyFollower.load(std::memory_order_acquire))
    {
        auto& service = World::Get().GetQuestService();
        std::lock_guard lock(service.m_runMutex);
        service.m_localMasks[MaskKey(apQuest->formID, apStage->stageIndex, QuestAliasFills::SetStage)] = mask;
    }
    if (!willRun || !apQuest || !World::Exists() || ScopedQuestOverride::IsOverriden() ||
        previousContext.Active)
        return result;
    auto& service = World::Get().GetQuestService();
    if (!service.m_captureLeader.load(std::memory_order_acquire) ||
        IsNonSyncableQuest(apQuest) ||
        ((aKind == QuestAliasFills::Startup || aKind == QuestAliasFills::Shutdown) &&
            service.m_captureSession.load(std::memory_order_relaxed) < 3))
        return result;
    QuestAliasFills record;
    record.HasStage = record.HasSeq = record.HasRun = true;
    record.RecordKind = aKind;
    record.Stage = apStage->stageIndex;
    record.Status = RequestQuestUpdate::StageUpdate;
    record.ClientQuestType = static_cast<uint8_t>(apQuest->type);
    record.StageFlagsBefore = before;
    record.ItemCount = itemCount;
    record.ItemMask = mask;
    record.MaskOverflow = overflow;
    record.Forced = aForce;
    record.QuestFlags = apQuest->flags;
    record.CurrentStageAfter = (aKind == QuestAliasFills::Startup || aKind == QuestAliasFills::Shutdown)
        ? std::max(apQuest->currentStage, apStage->stageIndex) : apQuest->currentStage;
    record.Running = !apQuest->IsStopped();
    service.PushRecord(apQuest->formID, std::move(record));
    return result;
}

QuestService::StageContext::StageContext(uint32_t aFormId, uint16_t aStage, uint32_t aMask,
    uint8_t aCount, bool aOverflow, bool aStateOnly) noexcept
    : PreviousForm(s_applyContext.FormId), PreviousStage(s_applyContext.Stage),
      PreviousMask(s_applyContext.Mask), PreviousCount(s_applyContext.Count),
      PreviousOverflow(s_applyContext.Overflow), PreviousActive(s_applyContext.Active),
      PreviousStateOnly(s_stateOnlyApply)
{
    s_applyContext = {aFormId, aStage, aMask, aCount, aOverflow, true};
    s_stateOnlyApply = aStateOnly;
}

QuestService::StageContext::~StageContext() noexcept
{
    s_applyContext = {PreviousForm, PreviousStage, PreviousMask,
        PreviousCount, PreviousOverflow, PreviousActive};
    s_stateOnlyApply = PreviousStateOnly;
}

uint32_t QuestService::LocalRunCount() noexcept { return s_localRuns; }

bool QuestService::ApplyAsFollower() noexcept
{
    return World::Exists() && World::Get().GetQuestService().m_applyFollower.load(std::memory_order_acquire);
}

bool QuestService::GetHostMask(uint32_t aFormId, uint16_t aStage,
    uint32_t& aMask, uint8_t& aCount, bool& aOverflow) noexcept
{
    if (!World::Exists())
        return false;
    auto& service = World::Get().GetQuestService();
    std::lock_guard lock(service.m_runMutex);
    const auto it = service.m_hostMasks.find(MaskKey(aFormId, aStage, QuestAliasFills::SetStage));
    if (it == service.m_hostMasks.end())
        return false;
    aMask = it->second.Mask;
    aCount = it->second.Count;
    aOverflow = it->second.Overflow;
    return true;
}

void QuestService::ApplyStageItem(void* apItem, bool aForce,
    void (*apOriginal)(void*, bool)) noexcept
{
    if (!apItem || !s_runDepth || !s_applyContext.Active)
        return apOriginal(apItem, aForce);
    const auto& frame = s_runFrames[s_runDepth - 1];
    if (frame.FormId != s_applyContext.FormId || frame.Stage != s_applyContext.Stage)
        return apOriginal(apItem, aForce);
    if (frame.ItemCount != s_applyContext.Count || s_applyContext.Overflow)
    {
        static std::atomic_uint32_t samples{};
        if (samples.fetch_add(1, std::memory_order_relaxed) < 32)
            spdlog::warn("QSP native item fallback quest={:X} stage={} localCount={} hostCount={} overflow={}",
                frame.FormId, frame.Stage, frame.ItemCount, s_applyContext.Count, s_applyContext.Overflow);
        return apOriginal(apItem, aForce);
    }
    const auto* bytes = reinterpret_cast<const uint8_t*>(apItem);
    const uint8_t itemIndex = bytes[0x15];
    if (itemIndex >= 32)
    {
        spdlog::warn("QSP native item fallback quest={:X} stage={} item={}",
            frame.FormId, frame.Stage, itemIndex);
        return apOriginal(apItem, aForce);
    }
    if (s_applyContext.Mask & (1u << itemIndex))
        return apOriginal(apItem, true);
    const auto* pQuest = *reinterpret_cast<TESQuest* const*>(bytes + 0x18);
    const auto* pStage = *reinterpret_cast<TESQuest::Stage* const*>(bytes + 0x20);
    if (!pQuest || !pStage)
    {
        spdlog::warn("Quest stage item {} missing quest or stage pointer; native fallback", itemIndex);
        return apOriginal(apItem, aForce);
    }
    TESQuestStageItemDoneEvent event{pQuest->formID, pStage->stageIndex, itemIndex};
    using TPostDone = void(EventDispatcher<TESQuestStageItemDoneEvent>*, const TESQuestStageItemDoneEvent*);
    POINTER_SKYRIMSE(TPostDone, postDone, 25272);
    postDone.Get()(&EventDispatcherManager::Get()->questStageItemDoneEvent, &event);
}

void QuestService::MarkQuestStateDirty(uint32_t aFormId) noexcept
{
    if (!aFormId || !World::Exists() || ScopedQuestOverride::IsOverriden())
        return;
    auto& service = World::Get().GetQuestService();
    if (!service.m_captureLeader.load(std::memory_order_acquire))
        return;
    std::lock_guard lock(service.m_runMutex);
    service.m_dirtyQuests.insert(aFormId);
}

void QuestService::CaptureQuestReset(TESQuest* apQuest) noexcept
{
    if (!apQuest || !World::Exists() || ScopedQuestOverride::IsOverriden())
        return;
    World::Get().GetQuestService().CaptureState(apQuest, QuestAliasFills::Reset);
}

void QuestService::CaptureState(TESQuest* apQuest, uint8_t aKind) noexcept
{
    if (!m_captureLeader.load(std::memory_order_acquire) || IsNonSyncableQuest(apQuest))
        return;
    QuestAliasFills record;
    record.HasStage = record.HasSeq = true;
    record.RecordKind = aKind;
    record.Stage = apQuest->currentStage;
    record.Status = aKind == QuestAliasFills::Started ? RequestQuestUpdate::Started :
        aKind == QuestAliasFills::Stopped ? RequestQuestUpdate::Stopped : RequestQuestUpdate::StageUpdate;
    record.ClientQuestType = static_cast<uint8_t>(apQuest->type);
    record.QuestFlags = apQuest->flags;
    record.CurrentStageAfter = apQuest->currentStage;
    record.Running = !apQuest->IsStopped();
    PushRecord(apQuest->formID, std::move(record));
}

void QuestService::PushRecord(uint32_t aFormId, QuestAliasFills aRecord) noexcept
{
    GameId id;
    if (!m_world.GetModSystem().GetServerModId(aFormId, id))
        return;
    aRecord.Id = id;
    aRecord.AuthorityEpoch = m_captureEpoch.load(std::memory_order_acquire);
    if (!aRecord.AuthorityEpoch)
        return;
    std::lock_guard lock(m_runMutex);
    if (m_runQueue.size() >= 4096)
    {
        spdlog::error("Quest run queue full: refusing to number quest {:X}", aFormId);
        return;
    }
    aRecord.QuestSeq = ++m_questSeq[aFormId];
    if (aRecord.HasRun)
    {
        const uint64_t key = (static_cast<uint64_t>(aFormId) << 16) | aRecord.Stage;
        auto& count = m_runCounts[key];
        if (count != UINT16_MAX)
            ++count;
        aRecord.RunCount = count;
    }
    m_runQueue.push_back(std::move(aRecord));
}

namespace
{
// A stage waiting for the leader's alias fills waits at most this long: a reference that never resolves
// here (unloaded, or unknown to this PC) must not stall the quest.
constexpr uint64_t kAliasHoldMs = 3000;
std::unordered_map<uint32_t, uint64_t> s_aliasHoldSince;
std::mutex s_aliasHoldMutex;

bool AliasHoldExpired(const uint32_t aQuestId) noexcept
{
    const uint64_t now = GetTickCount64();
    std::lock_guard lock(s_aliasHoldMutex);
    const auto [it, inserted] = s_aliasHoldSince.try_emplace(aQuestId, now);
    if (now - it->second < kAliasHoldMs)
        return false;
    spdlog::warn("Alias fill: quest {:X} stage applied without the leader's fills after {} ms (a reference did not resolve here)",
        aQuestId, now - it->second);
    return true;
}
} // namespace

bool QuestService::PrepareAliasesForStage(TESQuest* apQuest) noexcept
{
    if (!m_applyFollower.load(std::memory_order_acquire))
        return true;
    if (s_stateOnlyApply)
        return true;
    if (m_aliasOverflow)
        return false;
    Vector<LocalAliasFill> fills;
    {
        // Do not hold a service mutex while calling an engine function.
        std::lock_guard lock(m_aliasMutex);
        const auto it = m_leaderAliasFills.find(apQuest->formID);
        if (m_aliasEpoch != m_captureEpoch.load(std::memory_order_acquire) ||
            it == m_leaderAliasFills.end())
            return IsNonSyncableQuest(apQuest);
        fills = it->second.LocalFills;
        if (!it->second.Ready && !AliasHoldExpired(apQuest->formID))
            return false;
    }
    const bool applied = ApplyAliasFills(apQuest, fills);
    if (applied || AliasHoldExpired(apQuest->formID))
    {
        std::lock_guard lock(s_aliasHoldMutex);
        s_aliasHoldSince.erase(apQuest->formID);
        return true;
    }
    return false;
}

void QuestService::OnAliasFills(const NotifyQuestAliasFills& aMessage) noexcept
{
    if (!RefreshAliasSession())
        return;
    const auto& party = m_world.GetPartyService();
    const auto& fills = aMessage.Fills;
    if (fills.HasSeq)
    {
        if (!m_applyFollower.load(std::memory_order_acquire) || !aMessage.IsValid() ||
            !fills.IsValid() || fills.AuthorityEpoch != m_aliasEpoch || !aMessage.Revision ||
            (!aMessage.Replay && fills.Sequence <= m_lastAliasSequence))
            return;
        if (!aMessage.Replay)
            m_lastAliasSequence = fills.Sequence;
        const auto formId = m_world.GetModSystem().GetGameId(fills.Id);
        if (!formId)
            return;
        std::lock_guard lock(m_runMutex);
        if (fills.QuestSeq <= m_appliedSeq[formId])
            return;
        auto& queue = m_pendingRuns[formId];
        if ((queue.size() >= 64 || m_pendingRunCount >= 4096) && !queue.contains(fills.QuestSeq))
        {
            spdlog::error("Quest {:X} pending run capacity reached; resync required", formId);
            return;
        }
        if (queue.empty())
            m_pendingQuestOrder.push_back(formId);
        if (queue.try_emplace(fills.QuestSeq, aMessage).second)
            ++m_pendingRunCount;
        m_waitSince.try_emplace(formId, GetTickCount64());
        if (fills.HasRun)
        {
            const uint8_t kind = fills.RecordKind == QuestAliasFills::Startup ||
                fills.RecordKind == QuestAliasFills::Shutdown ? fills.RecordKind : QuestAliasFills::SetStage;
            m_hostMasks[MaskKey(formId, fills.Stage, kind)] =
                {fills.ItemMask, fills.ItemCount, fills.MaskOverflow};
            if (fills.RecordKind == QuestAliasFills::SetStage ||
                fills.RecordKind == QuestAliasFills::CompleteAll)
                RecordHostStage(formId, fills.Stage, fills.AuthorityEpoch);
        }
        return;
    }
    if (party.IsLeader() || !aMessage.IsValid() || !fills.IsValid() ||
        fills.AuthorityEpoch != m_aliasEpoch || aMessage.LeaderPlayerId != m_aliasLeader ||
        fills.Sequence <= m_lastAliasSequence || (fills.HasStage && !aMessage.Revision))
        return;
    m_lastAliasSequence = fills.Sequence;
    if (m_aliasOverflow)
        return;

    if (!fills.Running || (fills.HasStage && fills.Status == NotifyQuestUpdate::Stopped))
    {
        // A stopped quest must not replay stages left waiting for an unavailable reference.
        std::erase_if(m_pendingAliasFills,
            [&fills](const auto& aPending) { return aPending.Fills.Id == fills.Id; });
    }
    // Coalesce only consecutive alias-only snapshots of this quest, never across a stage.
    for (auto it = m_pendingAliasFills.rbegin(); it != m_pendingAliasFills.rend(); ++it)
    {
        if (it->Fills.Id != fills.Id)
            continue;
        const bool hasEmpty = std::any_of(fills.Entries.begin(), fills.Entries.end(),
            [](const QuestAliasFill& aEntry) { return !aEntry.Reference; });
        if (!it->Fills.HasStage && !fills.HasStage && !hasEmpty)
        {
            *it = aMessage;
            return;
        }
        break;
    }
    if (m_pendingAliasFills.size() >= 256)
    {
        m_aliasOverflow = true;
        spdlog::error("Quest alias queue full; holding quest stages until the party session restarts");
        return;
    }
    m_pendingAliasFills.push_back(aMessage);
}

void QuestService::ApplyPendingAliasFills() noexcept
{
    if (m_aliasOverflow)
        return;
    std::unordered_set<uint32_t> blocked;
    size_t budget = 16;
    const auto retryAfter = m_aliasRetryAfter;
    auto it = m_pendingAliasFills.begin();
    for (; it != m_pendingAliasFills.end() && budget;)
    {
        const auto& fills = it->Fills;
        const auto formId = m_world.GetModSystem().GetGameId(fills.Id);
        // Retry fairly without allowing a later snapshot to pass this quest's first pending stage.
        const bool earlier = std::any_of(m_pendingAliasFills.begin(), it,
            [&fills](const auto& aPending) { return aPending.Fills.Id == fills.Id; });
        if (fills.Sequence <= retryAfter || blocked.contains(formId) || earlier)
        {
            ++it;
            continue;
        }
        --budget;
        m_aliasRetryAfter = fills.Sequence;
        auto* pQuest = Cast<TESQuest>(TESForm::GetById(formId));
        if (!pQuest)
        {
            spdlog::error("Cannot resolve alias quest {:X}; discarding its update", fills.Id.LogFormat());
            it = m_pendingAliasFills.erase(it);
            continue;
        }
        const bool stopping = !fills.Running || (fills.HasStage && fills.Status == NotifyQuestUpdate::Stopped);
        bool ready = stopping || ResolveAliasFills(pQuest, fills);
        // Force accepts stopped quests too. Preserve the leader's fills even if local startup
        // cannot satisfy its own conditions, then reapply after startup's native fill pass.

        if (ready && !stopping)
        {
            // Cache before startup: its own native stage writes must see these fills too.
            if (fills.HasStage)
                RecordHostStage(formId, fills.Stage, fills.AuthorityEpoch);
            // Never start a quest here to take the leader's fills: that started quests this PC never
            // triggered (a follower "contracted vampirism"). A quest not running here gets its stage
            // update below as before, and its fills once it runs.
            ready = !pQuest->IsEnabled() || (pQuest->flags != TESQuest::StopStart &&
                !(pQuest->flags & TESQuest::StageWait) && !pQuest->unkFlags);
        }
        if (ready && !stopping && fills.HasStage && !pQuest->IsEnabled())
        {
            blocked.insert(formId);
            ++it;
            continue;
        }
        if (ready && !stopping && pQuest->IsEnabled())
            ready = PrepareAliasesForStage(pQuest);
        if (!ready)
        {
            blocked.insert(formId);
            ++it;
            continue;
        }
        if (fills.HasStage)
        {
            NotifyQuestUpdate update{};
            update.Id = fills.Id;
            update.Stage = fills.Stage;
            update.Status = fills.Status;
            update.ClientQuestType = fills.ClientQuestType;
            update.TransactionId = fills.TransactionId;
            update.AuthorityEpoch = fills.AuthorityEpoch;
            update.Revision = it->Revision;
            OnQuestUpdate(update);
            const auto applied = m_appliedQuestRevisions.find(formId);
            if (applied == m_appliedQuestRevisions.end() || applied->second < update.Revision)
            {
                // Applied once and it did not take (for example a start at a stage the quest does not
                // have): drop it. Retrying every 100 ms flooded the log and stalled the follower.
                spdlog::warn("Alias fill: quest {:X} stage {} update did not apply; dropped", formId, update.Stage);
            }
        }
        if (stopping)
        {
            std::lock_guard lock(m_aliasMutex);
            m_leaderAliasFills.erase(formId);
        }
        it = m_pendingAliasFills.erase(it);
    }
    if (it == m_pendingAliasFills.end())
        m_aliasRetryAfter = 0;
}

namespace
{
bool CanStartSharedQuest(const TESQuest* apQuest) noexcept
{
    if (!apQuest || (apQuest->flags & (TESQuest::Enabled | TESQuest::StageWait)) ||
        apQuest->unkFlags || ((apQuest->flags & TESQuest::RunOnce) && apQuest->alreadyRun))
        return false;
    switch (apQuest->type)
    {
    case TESQuest::Type::MainQuest:
    case TESQuest::Type::MagesGuild:
    case TESQuest::Type::ThievesGuild:
    case TESQuest::Type::DarkBrotherhood:
    case TESQuest::Type::CompanionsQuest:
    case TESQuest::Type::Daedric:
    case TESQuest::Type::CivilWar:
    case TESQuest::Type::DLC02_Dragonborn:
        return true;
    default:
        return false;
    }
}

void ApplyQuestTargetState(TESQuest* apQuest, const QuestAliasFills& aRecord) noexcept
{
    const bool hostFailed = (aRecord.QuestFlags & TESQuest::Failed) != 0;
    const bool localFailed = (apQuest->flags & TESQuest::Failed) != 0;
    const bool localCompleted = (apQuest->flags & TESQuest::Completed) != 0;
    bool refresh = false;
    if (hostFailed && !localFailed)
    {
        if (!localCompleted)
        {
            using TSetFailed = void(TESQuest*, bool);
            POINTER_SKYRIMSE(TSetFailed, setFailed, 24988);
            setFailed.Get()(apQuest, true);
            refresh = true;
        }
        else
        {
            apQuest->flags |= TESQuest::Failed;
            apQuest->MarkChanged(2);
        }
    }
    else if (!hostFailed && (aRecord.QuestFlags & TESQuest::Completed) && !localCompleted)
    {
        apQuest->SetCompleted(true);
        refresh = true;
    }
    if (refresh)
    {
        using TRefresh = void(PlayerCharacter*, TESQuest*);
        POINTER_SKYRIMSE(TRefresh, refreshQuest, 40516);
        if (auto* pPlayer = PlayerCharacter::Get())
            refreshQuest.Get()(pPlayer, apQuest);
    }
    if (!apQuest->IsEnabled() && !(apQuest->flags & TESQuest::StageWait))
        return;
    for (auto* objective : apQuest->objectives)
    {
        if (!objective)
            continue;
        const auto it = std::lower_bound(aRecord.Objectives.begin(), aRecord.Objectives.end(),
            objective->stageId, [](const auto& aEntry, uint16_t aIndex) { return aEntry.Index < aIndex; });
        if (it == aRecord.Objectives.end() || it->Index != objective->stageId ||
            objective->state == it->State)
            continue;
        using TSetObjective = void(TESQuest::Objective*, int, void*, void*);
        POINTER_SKYRIMSE(TSetObjective, setObjective, 23933);
        setObjective.Get()(objective, it->State, nullptr, nullptr);
    }
}

void DirectStageState(TESQuest* apQuest, const QuestAliasFills& aRecord) noexcept
{
    if (aRecord.RecordKind != QuestAliasFills::SetStage)
        return;
    for (auto* stage : apQuest->stages)
        if (stage && stage->stageIndex == aRecord.Stage)
        {
            stage->flags |= 1;
            apQuest->currentStage = std::max(apQuest->currentStage, aRecord.CurrentStageAfter);
            CheckpointSaves::QueueSaveChange(apQuest->formID, 0x80000000);
            return;
        }
}
} // namespace

void QuestService::OnMainFrame() noexcept
{
    const auto dropIn = m_world.GetDropInService().Status();
    if (!m_applyFollower.load(std::memory_order_acquire) || MT::Loading() ||
        (dropIn != "idle" && dropIn != "joined"))
        return;
    // The shared world, or this PC's own New Game intro before its first cell: the first MQ101 stage arrives there and its
    // fragment is what gives the player a cell. Never the lobby world, a reset or the title gap: a record applied there is
    // reverted by the coming reset or load while m_appliedSeq says it is done (Atlas WINDOWS.md 0.2; interim for the
    // CampaignWorld predicate).
    if (!PlayerCharacter::Get() || !(LobbyService::SharedWorld() || LobbyService::NewGameIntroPending()))
        return;
    const auto now = GetTickCount64();
    if (now >= m_nextAliasRetry)
    {
        m_nextAliasRetry = now + 100;
        ApplyPendingAliasFills();
    }
    unsigned startBudget = 2;
    std::unordered_set<uint32_t> blocked;
    for (unsigned budget = 8; budget; --budget)
    {
        NotifyQuestAliasFills message;
        uint32_t formId = 0;
        uint64_t waitSince = 0;
        bool laterStop = false;
        {
            std::lock_guard lock(m_runMutex);
            const size_t scan = std::min<size_t>(32, m_pendingQuestOrder.size());
            for (size_t i = 0; i < scan; ++i)
            {
                const auto candidate = m_pendingQuestOrder.front();
                m_pendingQuestOrder.pop_front();
                m_pendingQuestOrder.push_back(candidate);
                if (blocked.contains(candidate))
                    continue;
                const auto& queue = m_pendingRuns.at(candidate);
                const auto it = queue.find(m_appliedSeq[candidate] + 1);
                if (it == queue.end())
                    continue;
                formId = candidate;
                message = it->second;
                if (const auto since = m_waitSince.find(candidate); since != m_waitSince.end())
                    waitSince = since->second;
                laterStop = std::any_of(std::next(it), queue.end(), [](const auto& entry)
                    { return entry.second.Fills.RecordKind == QuestAliasFills::Stopped; });
                break;
            }
        }
        if (!formId)
            break;
        auto* pQuest = Cast<TESQuest>(TESForm::GetById(formId));
        if (!pQuest)
        {
            spdlog::error("QSP missing local quest {:X}", formId);
            break;
        }
        const auto& record = message.Fills;
        if (record.FillsIncluded && !ResolveAliasFills(pQuest, record))
        {
            if (!laterStop && GetTickCount64() - waitSince < 3000)
            {
                blocked.insert(formId);
                continue;
            }
        }
        bool processed = false;
        {
            ScopedQuestOverride remoteApply;
            switch (record.RecordKind)
            {
            case QuestAliasFills::Skipped:
                processed = true;
                break;
            case QuestAliasFills::Stopped:
                processed = StopQuest(formId);
                break;
            case QuestAliasFills::Reset:
            {
                using TReset = void(TESQuest*);
                POINTER_SKYRIMSE(TReset, resetQuest, 25014);
                resetQuest.Get()(pQuest);
                {
                    std::lock_guard lock(m_runMutex);
                    std::erase_if(m_localMasks, [formId](const auto& entry)
                        { return static_cast<uint32_t>(entry.first >> 32) == formId; });
                }
                processed = true;
                break;
            }
            default:
            {
                if (!pQuest->IsEnabled() && record.RecordKind != QuestAliasFills::State)
                {
                    if ((pQuest->flags & TESQuest::StageWait) || pQuest->unkFlags)
                        break;
                    if (!laterStop && GetTickCount64() - waitSince < 5000)
                        break;
                    if (!laterStop && CanStartSharedQuest(pQuest))
                    {
                        auto& attempts = m_startAttempts[formId];
                        if (attempts == 2)
                        {
                            spdlog::warn("QSP quest {:X} could not start after alias retry; state only", formId);
                            DirectStageState(pQuest, record);
                            ApplyQuestTargetState(pQuest, record);
                            processed = true;
                            break;
                        }
                        if (startBudget)
                        {
                            --startBudget;
                            if (attempts < 2)
                            {
                                if (attempts == 1)
                                {
                                    Vector<LocalAliasFill> fills;
                                    {
                                        std::lock_guard lock(m_aliasMutex);
                                        if (const auto it = m_leaderAliasFills.find(formId);
                                            it != m_leaderAliasFills.end())
                                            fills = it->second.LocalFills;
                                    }
                                    ApplyAliasFills(pQuest, fills);
                                }
                                bool started = false;
                                const bool accepted = pQuest->EnsureQuestStarted(started, true);
                                attempts = accepted && started ? 3 : static_cast<uint8_t>(attempts + 1);
                            }
                        }
                        break;
                    }
                    DirectStageState(pQuest, record);
                    ApplyQuestTargetState(pQuest, record);
                    processed = true;
                    break;
                }
                if (pQuest->getState() != TESQuest::State::Running &&
                    record.RecordKind != QuestAliasFills::State)
                    break;
                if (record.RecordKind == QuestAliasFills::Started)
                {
                    pQuest->SetActive(true);
                    processed = true;
                }
                else if (record.RecordKind == QuestAliasFills::Startup ||
                    record.RecordKind == QuestAliasFills::Shutdown ||
                    record.RecordKind == QuestAliasFills::State)
                    processed = true;
                else if (record.RecordKind == QuestAliasFills::SetStage ||
                    record.RecordKind == QuestAliasFills::CompleteAll)
                {
                    const bool repeat = (pQuest->flags & TESQuest::AllowRepeatStages) != 0;
                    if (record.RecordKind == QuestAliasFills::SetStage && !repeat &&
                        pQuest->IsStageDone(record.Stage))
                    {
                        std::lock_guard lock(m_runMutex);
                        if (const auto it = m_localMasks.find(MaskKey(formId, record.Stage,
                                QuestAliasFills::SetStage)); it != m_localMasks.end() &&
                            it->second != record.ItemMask)
                            spdlog::warn("QSP local mask differs quest={:X} stage={} local={:X} host={:X}",
                                formId, record.Stage, it->second, record.ItemMask);
                        processed = true;
                    }
                    else
                    {
                        const bool aliasesReady = PrepareAliasesForStage(pQuest);
                        const bool stateOnly = !aliasesReady &&
                            (laterStop || GetTickCount64() - waitSince >= 3000);
                        if (!aliasesReady && !stateOnly)
                            break;
                        if (stateOnly)
                            spdlog::warn("stage_state_only_unresolved quest={:X} stage={}", formId, record.Stage);
                        StageContext context(formId, record.Stage, stateOnly ? 0 : record.ItemMask,
                            record.ItemCount, record.MaskOverflow, stateOnly);
                        const auto before = LocalRunCount();
                        if (record.RecordKind == QuestAliasFills::SetStage)
                            pQuest->SetStage(record.Stage);
                        else
                        {
                            for (auto* stage : pQuest->stages)
                                if (stage && stage->stageIndex == record.Stage)
                                {
                                    using TRunner = bool(TESQuest::Stage*, TESQuest*, bool);
                                    POINTER_SKYRIMSE(TRunner, runner, 25252);
                                    runner.Get()(stage, pQuest, false);
                                    break;
                                }
                        }
                        processed = repeat || record.RecordKind == QuestAliasFills::CompleteAll ?
                            LocalRunCount() != before : pQuest->IsStageDone(record.Stage);
                    }
                }
                if (processed)
                    ApplyQuestTargetState(pQuest, record);
                break;
            }
            }
        }
        if (!processed)
        {
            blocked.insert(formId);
            continue;
        }
        std::lock_guard lock(m_runMutex);
        m_appliedSeq[formId] = record.QuestSeq;
        m_pendingRuns[formId].erase(record.QuestSeq);
        --m_pendingRunCount;
        if (m_pendingRuns[formId].empty())
        {
            m_waitSince.erase(formId);
            m_pendingRuns.erase(formId);
            std::erase(m_pendingQuestOrder, formId);
        }
        else
            m_waitSince[formId] = GetTickCount64();
        m_startAttempts.erase(formId);
        spdlog::debug("QSP applied quest={:X} seq={} kind={} stage={}",
            formId, record.QuestSeq, record.RecordKind, record.Stage);
    }
}

void QuestService::OnUpdate(const UpdateEvent&) noexcept
{
    if (!RefreshAliasSession())
        return;
    auto* pPlayer = PlayerCharacter::Get();
    if (!pPlayer || !pPlayer->parentCell)
        return;

    const auto now = GetTickCount64();
    const auto& party = m_world.GetPartyService();
    if (party.IsLeader())
    {
        std::unordered_set<uint32_t> changedFills;
        Vector<uint32_t> dirty;
        {
            std::lock_guard lock(m_runMutex);
            for (auto it = m_dirtyQuests.begin(); it != m_dirtyQuests.end() && dirty.size() < 32;)
            {
                dirty.push_back(*it);
                it = m_dirtyQuests.erase(it);
            }
        }
        for (const auto formId : dirty)
            if (auto* pQuest = Cast<TESQuest>(TESForm::GetById(formId)))
                CaptureState(pQuest, QuestAliasFills::State);
        for (unsigned budget = 32; budget; --budget)
        {
            QuestAliasFills record;
            {
                std::lock_guard lock(m_runMutex);
                if (m_runQueue.empty())
                    break;
                record = std::move(m_runQueue.front());
                m_runQueue.pop_front();
            }
            if (record.AuthorityEpoch != m_aliasEpoch)
                continue;
            auto* pQuest = Cast<TESQuest>(TESForm::GetById(m_world.GetModSystem().GetGameId(record.Id)));
            if (!pQuest)
                continue;
            if (changedFills.contains(pQuest->formID))
            {
                std::lock_guard lock(m_runMutex);
                m_runQueue.push_front(std::move(record));
                break;
            }
            if (record.RecordKind != QuestAliasFills::Skipped)
                SnapshotObjectives(pQuest, record);
            RequestQuestUpdate update;
            update.Id = record.Id;
            update.Stage = record.Stage;
            update.Status = record.Status;
            update.ClientQuestType = record.ClientQuestType;
            update.TransactionId = NextQuestTransactionId();
            const auto oldIt = m_sentAliasFills.find(pQuest->formID);
            const bool hadFills = oldIt != m_sentAliasFills.end();
            const auto oldFills = hadFills ? oldIt->second : Vector<QuestAliasFill>{};
            if (SendAliasFills(pQuest, &update, &record))
            {
                const auto newIt = m_sentAliasFills.find(pQuest->formID);
                if (hadFills != (newIt != m_sentAliasFills.end()) ||
                    (newIt != m_sentAliasFills.end() && oldFills != newIt->second))
                    changedFills.insert(pQuest->formID);
                continue;
            }
            record.RecordKind = QuestAliasFills::Skipped;
            record.HasRun = false;
            record.Forced = record.MaskOverflow = false;
            record.StageFlagsBefore = record.ItemCount = 0;
            record.ItemMask = record.RunCount = 0;
            record.HasObjectives = false;
            record.Objectives.clear();
            if (SendAliasFills(pQuest, &update, &record))
                continue;
            std::lock_guard lock(m_runMutex);
            m_runQueue.push_front(std::move(record));
            break;
        }
    }
    if (now < m_nextAliasSample)
        return;
    auto* pMods = ModManager::Get();
    if (!pMods)
        return;

    // Spread a sweep over frames and pause three seconds between sweeps.
    size_t budget = 16;
    while (m_aliasQuestCursor < pMods->quests.length && budget--)
    {
        auto* pQuest = pMods->quests[m_aliasQuestCursor++];
        if (!pQuest)
            continue;
        if (!pQuest->IsEnabled())
        {
            m_sentAliasFills.erase(pQuest->formID);
            m_requiredEmptySamples.erase(pQuest->formID);
            continue;
        }
        if (pQuest->flags == TESQuest::StopStart || (pQuest->flags & TESQuest::StageWait) || pQuest->unkFlags)
            continue;
        if (party.IsLeader())
            SendAliasFills(pQuest);
        else
        {
            const bool pending = std::any_of(m_pendingAliasFills.begin(), m_pendingAliasFills.end(),
                [this, pQuest](const auto& aMessage)
                { return m_world.GetModSystem().GetGameId(aMessage.Fills.Id) == pQuest->formID; });
            if (pending)
                continue;
            QuestAliasFills fills;
            {
                std::lock_guard lock(m_aliasMutex);
                const auto it = m_leaderAliasFills.find(pQuest->formID);
                if (it == m_leaderAliasFills.end())
                    continue;
                fills = it->second.Fills;
            }
            if (ResolveAliasFills(pQuest, fills))
                PrepareAliasesForStage(pQuest);
        }
    }
    if (m_aliasQuestCursor >= pMods->quests.length)
    {
        m_aliasQuestCursor = 0;
        m_nextAliasSample = now + 3000;
    }
}

void QuestService::RecordDebugEvent(const char* acKind, uint32_t aFormId, uint16_t aStage,
    bool aScopedOverride) noexcept
{
    DebugEvent event;
    event.TimeMs = GetTickCount64();
    event.FormId = aFormId;
    event.Stage = aStage;
    event.Kind = acKind;
    event.ScopedOverride = aScopedOverride;
    event.InParty = m_captureEpoch.load(std::memory_order_acquire) != 0;
    event.Leader = m_captureLeader.load(std::memory_order_acquire);

    std::scoped_lock lock(m_debugEventMutex);
    event.Sequence = ++m_debugEventSequence;
    m_debugEvents.emplace_back(std::move(event));
    constexpr size_t cMaxDebugEvents = 128;
    while (m_debugEvents.size() > cMaxDebugEvents)
        m_debugEvents.pop_front();
}

Vector<QuestService::DebugEvent> QuestService::GetRecentDebugEvents() const
{
    std::scoped_lock lock(m_debugEventMutex);
    return {m_debugEvents.begin(), m_debugEvents.end()};
}

bool QuestService::StopQuest(uint32_t aformId)
{
    TESQuest* pQuest = Cast<TESQuest>(TESForm::GetById(aformId));
    if (pQuest)
    {
        pQuest->SetActive(false);
        pQuest->SetStopped();
        return true;
    }

    return false;
}

static constexpr std::array kNonSyncableQuestIds = std::to_array<uint32_t>({
    0x2BA16,   // Werewolf transformation quest
    0x20071D0, // Vampire transformation quest
    0x3AC44,   // MS13BleakFallsBarrowLeverScene
    // 0xFE014801,  // Unknown dynamic ID, kept as note, maybe lookup correct ID this game?
    0xF2593 // Skill experience quest
});

bool QuestService::IsNonSyncableQuest(TESQuest* apQuest)
{
    // Quests with no quest stages are never synced. Most TESQues::Type:: quests should
    // be synced, including Type::None and Type::Miscellaneous, but there are a few
    // known exceptions that should be excluded that are in the table.
    // Type None and Miscellaneous quests run on each PC: they include the engine's system quests
    // (diseases, vampirism, dialogue and favor managers) that must not be started on another PC, and
    // the server does not relay them by default (Gameplay:bEnableMiscQuestSync).
    return    apQuest->stages.Empty()
           || apQuest->type == TESQuest::Type::None || apQuest->type == TESQuest::Type::Miscellaneous
           || std::find(kNonSyncableQuestIds.begin(), kNonSyncableQuestIds.end(), apQuest->formID) != kNonSyncableQuestIds.end();
}

void QuestService::DebugDumpQuests()
{
    auto& quests = ModManager::Get()->quests;
    for (TESQuest* pQuest : quests)
        spdlog::info("{:X}|{}|{}|{}", pQuest->formID, (uint8_t)pQuest->type, pQuest->priority, pQuest->idName.AsAscii());
}
