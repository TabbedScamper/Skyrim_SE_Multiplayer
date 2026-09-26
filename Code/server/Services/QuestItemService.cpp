#include <Services/QuestItemService.h>
#include <World.h>
#include <Components.h>
#include <Game/Player.h>
#include <Messages/RequestQuestItems.h>
#include <Messages/NotifyQuestItems.h>
#include <CampaignLedger.h>

namespace
{
Campaign::QuestItem Durable(const QuestItemState& aItem)
{
    return {aItem.BaseId.ModId, aItem.BaseId.BaseId, aItem.QuestId.ModId, aItem.QuestId.BaseId,
        aItem.AliasId, aItem.ReferenceId.ModId, aItem.ReferenceId.BaseId, aItem.Count,
        aItem.QuestObject, aItem.Active, aItem.Revision, aItem.QuestInstance};
}
}

QuestItemService::QuestItemService(World& aWorld, entt::dispatcher& aDispatcher)
    : m_world(aWorld)
    , m_requestConnection(aDispatcher.sink<PacketEvent<RequestQuestItems>>().connect<&QuestItemService::OnRequest>(this))
{
    for (const auto& item : m_world.GetCampaignLedger().ReadQuestItems())
        m_items.push_back({{item.ModId, item.BaseId}, {item.QuestModId, item.QuestBaseId},
            {item.ReferenceModId, item.ReferenceBaseId}, item.AliasId, item.Count,
            item.QuestObject, item.Active, item.Revision, item.QuestInstance});
    spdlog::info("Quest items: loaded {} durable records including hand-ins", m_items.size());
}

void QuestItemService::SendSnapshot(Player* aPlayer, uint64_t aEpoch, uint64_t aToken) const
{
    size_t offset = 0;
    uint32_t page = 0;
    do
    {
        NotifyQuestItems reply;
        reply.Epoch = aEpoch;
        reply.Token = aToken;
        reply.Snapshot = true;
        reply.Page = page++;
        const auto end = (std::min)(offset + NotifyQuestItems::MaxItems, m_items.size());
        reply.Items.assign(m_items.begin() + offset, m_items.begin() + end);
        reply.Complete = end == m_items.size();
        aPlayer->Send(reply);
        offset = end;
    } while (offset < m_items.size());
}

void QuestItemService::OnRequest(const PacketEvent<RequestQuestItems>& aEvent) noexcept
{
    auto* player = aEvent.pPlayer;
    auto& parties = m_world.GetPartyService();
    auto* party = parties.GetPlayerParty(player);
    const auto& request = aEvent.Packet;
    if (!request.IsValid() || !request.ValidPayload() || !party || !player->GetParty().JoinedPartyId ||
        !player->GetCharacter() || request.Epoch != party->StartEpoch || party->SessionState < 2 ||
        std::find(party->Members.begin(), party->Members.end(), player) == party->Members.end())
        return;
    const auto partyId = *player->GetParty().JoinedPartyId;
    if (m_partyId && *m_partyId != partyId && parties.GetById(*m_partyId))
        return;
    m_partyId = partyId;

    try
    {
        if (request.Action == QuestItemAction::Snapshot)
        {
            SendSnapshot(player, request.Epoch, request.Token);
            return;
        }
        auto current = std::find_if(m_items.begin(), m_items.end(), [&](const auto& item) { return item.SameKey(request.Item); });
        const bool leader = parties.IsPlayerLeader(player);
        // Acquisitions are set membership, not additive inventory deltas. Five peers finding
        // the same reference creates one entitlement. Only the leader can consume it.
        if (request.Action == QuestItemAction::Release && (!leader || current == m_items.end()))
            return;
        if (current != m_items.end() && (request.Action == QuestItemAction::Acquire ||
            !current->Active || current->Revision != request.Item.Revision))
        {
            NotifyQuestItems reply;
            reply.Epoch = request.Epoch;
            reply.Token = request.Token;
            reply.Items.push_back(*current);
            player->Send(reply);
            return;
        }
        if (request.Action == QuestItemAction::Acquire && request.Item.Revision)
            return;

        auto item = request.Action == QuestItemAction::Release ? *current : request.Item;
        item.Active = request.Action == QuestItemAction::Acquire;
        const auto metadata = m_world.GetCampaignLedger().GetMetadata();
        const auto transaction = fmt::format("quest-item:{}:{}:{}:{}", metadata.AuthorityEpoch,
            request.Epoch, player->GetId(), request.Token);
        const auto commit = m_world.GetCampaignLedger().SetQuestItem(transaction, Durable(item), item.Revision);
        item.Revision = commit.Entry.Revision;
        if (current == m_items.end())
            m_items.push_back(item);
        else
            *current = item;

        NotifyQuestItems notify;
        notify.Epoch = request.Epoch;
        notify.Token = request.Token;
        notify.Items.push_back(item);
        // Includes the picker and all members, even in other cells.
        for (auto* member : party->Members)
            member->Send(notify);
        spdlog::info("Quest items: {} base={:X}:{:X} quest={:X}:{:X} alias={} count={} revision={} members={}",
            item.Active ? "acquired" : "handed in", item.BaseId.ModId, item.BaseId.BaseId,
            item.QuestId.ModId, item.QuestId.BaseId, item.AliasId, item.Count, item.Revision, party->Members.size());
    }
    catch (const std::exception& e)
    {
        spdlog::error("Quest items: durable mutation/snapshot failed: {}", e.what());
    }
}
