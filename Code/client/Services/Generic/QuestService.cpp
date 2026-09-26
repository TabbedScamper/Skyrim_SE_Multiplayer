#include <TiltedOnlinePCH.h>

#include <Events/ConnectedEvent.h>
#include <Events/DisconnectedEvent.h>
#include <Events/UpdateEvent.h>
#include <Components.h>
#include <Utils.h>

#include <Services/QuestService.h>
#include <Services/ImguiService.h>

#include <PlayerCharacter.h>
#include <Forms/TESQuest.h>
#include <Games/TES.h>
#include <Games/Overrides.h>

#include <Events/EventDispatcher.h>

#include <Messages/RequestQuestUpdate.h>
#include <Messages/NotifyQuestUpdate.h>
#include <Messages/RequestQuestAliasFills.h>

#include <atomic>

namespace
{
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
    uint16_t currentStage = 0;
    if (TESQuest* pQuest = Cast<TESQuest>(TESForm::GetById(apEvent->formId)))
        currentStage = pQuest->currentStage;
    RecordDebugEvent("local_start_stop", apEvent->formId, currentStage, scopedOverride);

    const auto& partyService = m_world.Get().GetPartyService();
    if (scopedOverride || !partyService.IsInParty() || !partyService.IsLeader())
        return BSTEventResult::kOk;

    spdlog::info("Quest start/stop event: {:X}", apEvent->formId);

    if (TESQuest* pQuest = Cast<TESQuest>(TESForm::GetById(apEvent->formId)))
    {
        if (IsNonSyncableQuest(pQuest))
        {
            m_world.GetRunner().Queue([this, formId = pQuest->formID, epoch = partyService.GetStartEpoch()]()
                {
                    const auto& party = m_world.GetPartyService();
                    if (party.IsInParty() && party.IsLeader() && party.GetStartEpoch() == epoch)
                    {
                        if (auto* pCurrentQuest = Cast<TESQuest>(TESForm::GetById(formId)))
                            SendAliasFills(pCurrentQuest);
                    }
                });
            return BSTEventResult::kOk;
        }
     
        if (pQuest->type == TESQuest::Type::None || pQuest->type == TESQuest::Type::Miscellaneous)
        {
            // Perhaps redundant, but necessary. We need the logging and
            // the lambda coming up is queued and runs later
            GameId Id;
            auto& modSys = m_world.GetModSystem();
            if (modSys.GetServerModId(pQuest->formID, Id))
            {
                spdlog::info(__FUNCTION__ ": queuing type none/misc quest gameId {:X} questStage {} questStatus {} questType {} formId {:X} name {}",
                             Id.LogFormat(),  pQuest->currentStage, pQuest->IsStopped() ? RequestQuestUpdate::Stopped : RequestQuestUpdate::Started,
                             static_cast<std::underlying_type_t<TESQuest::Type>>(pQuest->type), 
                             pQuest->formID, pQuest->fullName.value.AsAscii());
            }
        }
        
        m_world.GetRunner().Queue(
            [&, formId = pQuest->formID, stageId = pQuest->currentStage, stopped = pQuest->IsStopped(), type = pQuest->type, epoch = partyService.GetStartEpoch()]()
            {
                GameId Id;
                auto& modSys = m_world.GetModSystem();
                if (modSys.GetServerModId(formId, Id))
                {
                    RequestQuestUpdate update;
                    update.Id = Id;
                    update.Stage = stageId;
                    update.Status = stopped ? RequestQuestUpdate::Stopped : RequestQuestUpdate::Started;
                    update.ClientQuestType = static_cast<std::underlying_type_t<TESQuest::Type>>(type); 
                    update.TransactionId = NextQuestTransactionId();

                    const auto& party = m_world.GetPartyService();
                    if (!party.IsInParty() || !party.IsLeader() || party.GetStartEpoch() != epoch)
                        return;
                    if (auto* pCurrentQuest = Cast<TESQuest>(TESForm::GetById(formId)))
                        SendAliasFills(pCurrentQuest, &update);
                }
            });
    }

    return BSTEventResult::kOk;
}

BSTEventResult QuestService::OnEvent(const TESQuestStageEvent* apEvent, const EventDispatcher<TESQuestStageEvent>*)
{
    const bool scopedOverride = ScopedQuestOverride::IsOverriden();
    RecordDebugEvent("local_stage", apEvent->formId, apEvent->stageId, scopedOverride);

    const auto& partyService = m_world.Get().GetPartyService();
    if (scopedOverride || !partyService.IsInParty() || !partyService.IsLeader())
        return BSTEventResult::kOk;

    spdlog::info("Quest stage event: {:X}, stage: {}", apEvent->formId, apEvent->stageId);

    // there is no reason to even fetch the quest object, since the event provides everything already....
    if (TESQuest* pQuest = Cast<TESQuest>(TESForm::GetById(apEvent->formId)))
    {
        if (IsNonSyncableQuest(pQuest))
        {
            m_world.GetRunner().Queue([this, formId = pQuest->formID, epoch = partyService.GetStartEpoch()]()
                {
                    const auto& party = m_world.GetPartyService();
                    if (party.IsInParty() && party.IsLeader() && party.GetStartEpoch() == epoch)
                    {
                        if (auto* pCurrentQuest = Cast<TESQuest>(TESForm::GetById(formId)))
                            SendAliasFills(pCurrentQuest);
                    }
                });
            return BSTEventResult::kOk;
        }

        if (pQuest->type == TESQuest::Type::None || pQuest->type == TESQuest::Type::Miscellaneous)
        {
            // Perhaps redundant, but necessary. We need the logging and
            // the lambda coming up is queued and runs later
            GameId Id;
            auto& modSys = m_world.GetModSystem();
            if (modSys.GetServerModId(pQuest->formID, Id))
            {
                spdlog::info(__FUNCTION__ ": queuing type none/misc quest gameId {:X} questStage {} questStatus {} questType {} formId {:X} name {}",
                             Id.LogFormat(), pQuest->currentStage,
                             RequestQuestUpdate::StageUpdate,
                             static_cast<std::underlying_type_t<TESQuest::Type>>(pQuest->type),
                             pQuest->formID, pQuest->fullName.value.AsAscii());
            }
        }

        m_world.GetRunner().Queue(
            [&, formId = apEvent->formId, stageId = apEvent->stageId, type = pQuest->type, epoch = partyService.GetStartEpoch()]()
            {
                GameId Id;
                auto& modSys = m_world.GetModSystem();
                if (modSys.GetServerModId(formId, Id))
                {
                    RequestQuestUpdate update;
                    update.Id = Id;
                    update.Stage = stageId;
                    update.Status = RequestQuestUpdate::StageUpdate;
                    update.ClientQuestType = static_cast<std::underlying_type_t<TESQuest::Type>>(type);
                    update.TransactionId = NextQuestTransactionId();

                    const auto& party = m_world.GetPartyService();
                    if (!party.IsInParty() || !party.IsLeader() || party.GetStartEpoch() != epoch)
                        return;
                    if (auto* pCurrentQuest = Cast<TESQuest>(TESForm::GetById(formId)))
                        SendAliasFills(pCurrentQuest, &update);
                }
            });
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

    if (aUpdate.Status != NotifyQuestUpdate::Stopped && !PrepareAliasesForStage(pQuest))
        return;

    ScopedQuestOverride remoteQuestApply;
    bool bResult = false;
    switch (aUpdate.Status)
    {
    case NotifyQuestUpdate::Started:
    {
        bResult = pQuest->IsStageDone(aUpdate.Stage) || pQuest->SetStage(aUpdate.Stage);
        pQuest->SetActive(true);
        spdlog::info("Remote quest started: {:X}, stage: {}", formId, aUpdate.Stage);
        break;
    }
    case NotifyQuestUpdate::StageUpdate:
        bResult = pQuest->IsStageDone(aUpdate.Stage) || pQuest->SetStage(aUpdate.Stage);
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
    m_aliasSession = false;
    m_sentAliasFills.clear();
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
        if (m_aliasSession)
        {
            m_aliasSession = false;
            m_sentAliasFills.clear();
            m_pendingAliasFills.clear();
            std::lock_guard lock(m_aliasMutex);
            m_leaderAliasFills.clear();
        }
        return false;
    }

    if (!m_aliasSession || m_aliasEpoch != party.GetStartEpoch() || m_aliasLeader != party.GetLeaderPlayerId())
    {
        m_sentAliasFills.clear();
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
        std::lock_guard lock(s_hostStagesLock);
        s_hostStages.clear();
        s_hostStagesEpoch = m_aliasEpoch;
    }
    if (m_aliasMembers != party.GetPartyMembers())
    {
        m_aliasMembers = party.GetPartyMembers();
        // A new member needs unchanged running quests as well.
        m_sentAliasFills.clear();
        m_aliasQuestCursor = 0;
        m_nextAliasSample = 0;
    }
    return true;
}

bool QuestService::SendAliasFills(TESQuest* apQuest, const RequestQuestUpdate* apUpdate) noexcept
{
    if (!RefreshAliasSession() || !m_world.GetPartyService().IsLeader())
        return false;

    const auto& aliases = apQuest->aliases;
    if (aliases.length > QuestAliasFills::MaxAliases || aliases.length > aliases.capacity ||
        (aliases.length && !aliases.data))
    {
        spdlog::error("Quest alias snapshot exceeds bounds: quest {:X}, count {}", apQuest->formID, aliases.length);
        return false;
    }

    RequestQuestAliasFills request;
    auto& fills = request.Fills;
    auto& mods = m_world.GetModSystem();
    if (!mods.GetServerModId(apQuest->formID, fills.Id))
        return false;
    fills.AuthorityEpoch = m_aliasEpoch;
    fills.Sequence = NextQuestTransactionId();
    fills.Running = apUpdate ? apUpdate->Status != RequestQuestUpdate::Stopped : !apQuest->IsStopped();
    for (uint32_t i = 0; i < aliases.length; ++i)
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
    }
    std::sort(fills.Entries.begin(), fills.Entries.end(),
        [](const auto& aLeft, const auto& aRight) { return aLeft.AliasId < aRight.AliasId; });

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
        if (fills.Running && it != m_sentAliasFills.end() && it->second == fills.Entries)
            return true;
    }

    if (!fills.IsValid() || !m_world.GetTransport().Send(request))
        return false;
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
        if (!apQuest->GetReferenceAlias(entry.AliasId))
            ready = false;
        resolved.push_back({entry.AliasId, formId});
    }
    std::lock_guard lock(m_aliasMutex);
    if (m_leaderAliasFills.size() >= 4096 && !m_leaderAliasFills.contains(apQuest->formID))
        return false;
    m_leaderAliasFills[apQuest->formID] = {aFills, std::move(resolved), ready};
    return ready;
}

bool QuestService::ApplyAliasFills(TESQuest* apQuest, const Vector<LocalAliasFill>& aFills) noexcept
{
    // Validate the complete snapshot before changing anything. Missing is not empty.
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
            apQuest->ForceAliasReference(entry.AliasId, Cast<TESObjectREFR>(TESForm::GetById(entry.FormId)));
        else
            apQuest->ClearAliasReference(apQuest->GetReferenceAlias(entry.AliasId));
        auto* pApplied = apQuest->GetAliasedRef(entry.AliasId);
        if ((pApplied ? pApplied->formID : 0) != entry.FormId ||
            (!entry.FormId && apQuest->GetAliasHandle(entry.AliasId) != *TESObjectREFR::GetNullHandle()))
            return false;
        spdlog::info("Alias fill: quest {:X} alias {} {:X} -> {:X} (leader's)",
            apQuest->formID, entry.AliasId, previousId, entry.FormId);
    }
    return true;
}

namespace
{
// A stage waiting for the leader's alias fills waits at most this long: a reference that never resolves
// here (unloaded, or unknown to this PC) must not stall the quest.
constexpr uint64_t kAliasHoldMs = 3000;
std::unordered_map<uint32_t, uint64_t> s_aliasHoldSince;

bool AliasHoldExpired(const uint32_t aQuestId) noexcept
{
    const uint64_t now = GetTickCount64();
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
    const auto& party = m_world.GetPartyService();
    if (!party.IsInParty() || party.IsLeader())
        return true;
    if (m_aliasOverflow)
        return false;
    Vector<LocalAliasFill> fills;
    {
        // Do not hold a service mutex while calling an engine function.
        std::lock_guard lock(m_aliasMutex);
        const auto it = m_leaderAliasFills.find(apQuest->formID);
        if (m_aliasEpoch != party.GetStartEpoch() || m_aliasLeader != party.GetLeaderPlayerId() ||
            it == m_leaderAliasFills.end())
            return IsNonSyncableQuest(apQuest);
        fills = it->second.LocalFills;
        if (!it->second.Ready && !AliasHoldExpired(apQuest->formID))
            return false;
    }
    const bool applied = ApplyAliasFills(apQuest, fills);
    if (applied || AliasHoldExpired(apQuest->formID))
    {
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
        if (!it->Fills.HasStage && !fills.HasStage)
        {
            *it = aMessage;
            return;
        }
        break;
    }
    if (m_pendingAliasFills.size() >= 256)
    {
        m_aliasOverflow = true;
        spdlog::error("Quest alias queue full; holding quest stages until the party session is restarted");
        return;
    }
    m_pendingAliasFills.push_back(aMessage);
    ApplyPendingAliasFills();
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
        if (ready && !stopping && !pQuest->IsEnabled())
            ready = PrepareAliasesForStage(pQuest);
        if (ready && !stopping)
        {
            // Cache before startup: its own native stage writes must see these fills too.
            if (fills.HasStage)
                RecordHostStage(formId, fills.Stage, fills.AuthorityEpoch);
            if (!pQuest->IsEnabled())
            {
                ScopedQuestOverride override;
                bool starting = false;
                pQuest->EnsureQuestStarted(starting, true);
            }
            ready = pQuest->IsEnabled() && pQuest->flags != TESQuest::StopStart &&
                !(pQuest->flags & TESQuest::StageWait) && !pQuest->unkFlags;
        }
        if (ready && !stopping)
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
                blocked.insert(formId);
                ++it;
                continue;
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

void QuestService::OnUpdate(const UpdateEvent&) noexcept
{
    if (!RefreshAliasSession())
        return;
    auto* pPlayer = PlayerCharacter::Get();
    if (!pPlayer || !pPlayer->parentCell)
        return;

    const auto now = GetTickCount64();
    const auto& party = m_world.GetPartyService();
    if (!party.IsLeader() && now >= m_nextAliasRetry)
    {
        m_nextAliasRetry = now + 100;
        ApplyPendingAliasFills();
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
    event.InParty = m_world.GetPartyService().IsInParty();
    event.Leader = m_world.GetPartyService().IsLeader();

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
    return    apQuest->stages.Empty() 
           || std::find(kNonSyncableQuestIds.begin(), kNonSyncableQuestIds.end(), apQuest->formID) != kNonSyncableQuestIds.end();
}

void QuestService::DebugDumpQuests()
{
    auto& quests = ModManager::Get()->quests;
    for (TESQuest* pQuest : quests)
        spdlog::info("{:X}|{}|{}|{}", pQuest->formID, (uint8_t)pQuest->type, pQuest->priority, pQuest->idName.AsAscii());
}
