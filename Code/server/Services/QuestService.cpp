#include <GameServer.h>
#include <Components.h>

#include <World.h>
#include <Services/QuestService.h>

#include <Messages/RequestQuestUpdate.h>
#include <Messages/NotifyQuestUpdate.h>
#include <Messages/RequestQuestAliasFills.h>
#include <Messages/NotifyQuestAliasFills.h>

#include <Setting.h>
#include <CampaignLedger.h>
namespace
{
Console::Setting bEnableMiscQuestSync{"Gameplay:bEnableMiscQuestSync", "Syncs miscellaneous quests (on by default: a follower in a party never runs its own quest stages, so an unsynced one would stall)", true};

}

QuestService::QuestService(World& aWorld, entt::dispatcher& aDispatcher)
    : m_world(aWorld)
{
    m_questUpdateConnection = aDispatcher.sink<PacketEvent<RequestQuestUpdate>>().connect<&QuestService::OnQuestChanges>(this);
    m_aliasFillsConnection = aDispatcher.sink<PacketEvent<RequestQuestAliasFills>>().connect<&QuestService::OnAliasFills>(this);
}

void QuestService::OnQuestChanges(const PacketEvent<RequestQuestUpdate>& acMessage) noexcept
{
    ProcessQuestChanges(acMessage, nullptr);
}

void QuestService::OnAliasFills(const PacketEvent<RequestQuestAliasFills>& acMessage) noexcept
{
    auto& partyService = m_world.GetPartyService();
    auto* pParty = partyService.GetPlayerParty(acMessage.pPlayer);
    const auto& fills = acMessage.Packet.Fills;
    if (!pParty || !partyService.IsPlayerLeader(acMessage.pPlayer) ||
        !acMessage.Packet.IsValid() || !fills.IsValid() || fills.AuthorityEpoch != pParty->StartEpoch)
        return;

    for (auto it = m_aliasSources.begin(); it != m_aliasSources.end();)
    {
        if (!partyService.GetById(it->first))
            it = m_aliasSources.erase(it);
        else
            ++it;
    }
    const auto partyId = *acMessage.pPlayer->GetParty().JoinedPartyId;
    auto& source = m_aliasSources[partyId];
    if (source.Leader != pParty->LeaderPlayerId || source.Epoch != pParty->StartEpoch)
        source = {pParty->LeaderPlayerId, pParty->StartEpoch, 0};
    if (fills.Sequence <= source.Sequence)
        return;
    source.Sequence = fills.Sequence;

    if (fills.HasStage)
    {
        RequestQuestUpdate update{};
        update.Id = fills.Id;
        update.Stage = fills.Stage;
        update.Status = fills.Status;
        update.ClientQuestType = fills.ClientQuestType;
        update.TransactionId = fills.TransactionId;
        if (ProcessQuestChanges(PacketEvent<RequestQuestUpdate>(&update, acMessage.pPlayer), &fills))
            return;
        // A rejected stage must not discard an otherwise valid alias snapshot.
    }

    NotifyQuestAliasFills notify;
    notify.Fills = fills;
    notify.Fills.HasStage = false;
    notify.Fills.Stage = 0;
    notify.Fills.Status = 0;
    notify.Fills.ClientQuestType = 0;
    notify.Fills.TransactionId = 0;
    notify.LeaderPlayerId = pParty->LeaderPlayerId;
    GameServer::Get()->SendToParty(notify, acMessage.pPlayer->GetParty(), acMessage.GetSender());
}

bool QuestService::ProcessQuestChanges(const PacketEvent<RequestQuestUpdate>& acMessage, const QuestAliasFills* apFills) noexcept
{
    const auto& message = acMessage.Packet;

    auto* pPlayer = acMessage.pPlayer;

    auto& partyService = m_world.GetPartyService();
    if (partyService.IsPlayerInParty(pPlayer) && !partyService.IsPlayerLeader(pPlayer))
    {
        spdlog::warn("{}: rejected quest update from non-leader player {}, gameId {:X}, stage {}",
            __FUNCTION__, pPlayer->GetId(), message.Id.LogFormat(), message.Stage);
        return false;
    }

    auto& questComponent = pPlayer->GetQuestLogComponent();
    auto& entries = questComponent.QuestContent.Entries;

    auto questIt = std::find_if(entries.begin(), entries.end(), [&message](const auto& e) { return e.Id == message.Id; });

    NotifyQuestUpdate notify{};
    notify.Id = message.Id;
    notify.Stage = message.Stage;
    notify.Status = message.Status;
    notify.ClientQuestType = message.ClientQuestType;

    if (notify.ClientQuestType == 0 ||  notify.ClientQuestType == 6) // Types None or Miscellaneous. Hard-coded to avoid client header file.
    {
        if (!bEnableMiscQuestSync)
            return false;
        spdlog::info("{}: syncing type none/misc quest to party, gameId {:X} questStage {} questStatus {} questType {}",
                     __FUNCTION__, notify.Id.LogFormat(), notify.Stage, notify.Status, notify.ClientQuestType);
    }

    if (message.Status > RequestQuestUpdate::Stopped)
    {
        spdlog::warn("{}: rejected invalid quest status {} from player {}", __FUNCTION__, message.Status, pPlayer->GetId());
        return false;
    }

    if (message.TransactionId == 0)
    {
        spdlog::warn("{}: rejected quest update without a transaction id from player {}", __FUNCTION__, pPlayer->GetId());
        return false;
    }

    const auto transactionKey = fmt::format("quest:{}:{}", pPlayer->GetId(), message.TransactionId);
    const auto payload = fmt::format(
        "mod={};base={};stage={};status={};type={}",
        message.Id.ModId, message.Id.BaseId, message.Stage, message.Status, message.ClientQuestType);

    Campaign::CommitResult commit;
    try
    {
        commit = m_world.GetCampaignLedger().Commit(transactionKey, "quest", payload);
    }
    catch (const std::exception& exception)
    {
        spdlog::error("{}: failed to commit quest transaction {}: {}", __FUNCTION__, transactionKey, exception.what());
        return false;
    }

    if (!commit.Inserted)
    {
        spdlog::debug("{}: ignored duplicate quest transaction {} at revision {}",
            __FUNCTION__, transactionKey, commit.Entry.Revision);
        return false;
    }

    notify.TransactionId = message.TransactionId;
    notify.Revision = commit.Entry.Revision;
    notify.AuthorityEpoch = commit.Entry.AuthorityEpoch;

    if (message.Status == RequestQuestUpdate::Started || message.Status == RequestQuestUpdate::StageUpdate)
    {
        // in order to prevent bugs when a quest is in progress
        // and being updated we add it as a new quest record to
        // maintain a proper remote questlog state.
        if (questIt == entries.end())
        {
            auto& newQuest = entries.emplace_back();
            newQuest.Id = message.Id;
            newQuest.Stage = message.Stage;

            if (message.Status == RequestQuestUpdate::Started)
            {
                spdlog::debug("Started quest: {:X} stage: {}", message.Id.LogFormat(), message.Stage);

                notify.Status = NotifyQuestUpdate::Started;
            }
            else
            {
                notify.Status = NotifyQuestUpdate::StageUpdate;
            }
        }
        else
        {
            spdlog::debug("Updated quest: {:X}, stage: {}", message.Id.LogFormat(), message.Id.BaseId, message.Stage);

            auto& record = *questIt;
            record.Id = message.Id;
            record.Stage = message.Stage;

            notify.Status = NotifyQuestUpdate::StageUpdate;
        }
    }
    else if (message.Status == RequestQuestUpdate::Stopped)
    {
        spdlog::debug("Stopped quest: {:X}, stage: {}", message.Id.LogFormat(), message.Id.BaseId, message.Stage);

        if (questIt != entries.end())
            entries.erase(questIt);
        else
            spdlog::warn("Unable to delete quest object {:X}", message.Id.LogFormat(), message.Id.BaseId);

        notify.Status = NotifyQuestUpdate::Stopped;
    }

    const auto& partyComponent = acMessage.pPlayer->GetParty();
    if (!partyComponent.JoinedPartyId.has_value())
        return false;

    if (apFills)
    {
        // One packet carries both state and stage, so transport scheduling cannot separate them.
        NotifyQuestAliasFills aliases;
        aliases.Fills = *apFills;
        aliases.Fills.Status = notify.Status;
        aliases.Revision = notify.Revision;
        aliases.LeaderPlayerId = pPlayer->GetId();
        GameServer::Get()->SendToParty(aliases, partyComponent, acMessage.GetSender());
    }
    else
        GameServer::Get()->SendToParty(notify, partyComponent, acMessage.GetSender());
    return true;
}
