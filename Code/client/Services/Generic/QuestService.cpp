#include <TiltedOnlinePCH.h>

#include <Events/ConnectedEvent.h>

#include <Services/QuestService.h>
#include <Services/ImguiService.h>

#include <PlayerCharacter.h>
#include <Forms/TESQuest.h>
#include <Games/TES.h>
#include <Games/Overrides.h>

#include <Events/EventDispatcher.h>

#include <Messages/RequestQuestUpdate.h>
#include <Messages/NotifyQuestUpdate.h>

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
            return BSTEventResult::kOk;
     
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
            [&, formId = pQuest->formID, stageId = pQuest->currentStage, stopped = pQuest->IsStopped(), type = pQuest->type]()
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

                    m_world.GetTransport().Send(update);
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
            return BSTEventResult::kOk;

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
            [&, formId = apEvent->formId, stageId = apEvent->stageId, type = pQuest->type]()
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

                    m_world.GetTransport().Send(update);
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

    ScopedQuestOverride remoteQuestApply;
    bool bResult = false;
    switch (aUpdate.Status)
    {
    case NotifyQuestUpdate::Started:
    {
        pQuest->ScriptSetStage(aUpdate.Stage);
        pQuest->SetActive(true);
        bResult = true;
        spdlog::info("Remote quest started: {:X}, stage: {}", formId, aUpdate.Stage);
        break;
    }
    case NotifyQuestUpdate::StageUpdate:
        pQuest->ScriptSetStage(aUpdate.Stage);
        bResult = true;
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
