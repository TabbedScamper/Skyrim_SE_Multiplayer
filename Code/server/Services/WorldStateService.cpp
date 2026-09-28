#include <Services/WorldStateService.h>
#include <World.h>
#include <Components.h>
#include <Game/Player.h>
#include <Game/PlayerManager.h>
#include <Services/PartyService.h>
#include <Events/UpdateEvent.h>
#include <Messages/RequestWorldState.h>
#include <Messages/RequestWorldStateCell.h>
#include <Messages/NotifyWorldState.h>

WorldStateService::WorldStateService(World& aWorld, entt::dispatcher& aDispatcher) noexcept
    : m_world(aWorld)
    , m_stateConnection(aDispatcher.sink<PacketEvent<RequestWorldState>>().connect<&WorldStateService::OnState>(this))
    , m_cellConnection(aDispatcher.sink<PacketEvent<RequestWorldStateCell>>().connect<&WorldStateService::OnCell>(this))
    , m_updateConnection(aDispatcher.sink<UpdateEvent>().connect<&WorldStateService::OnUpdate>(this))
{
}

void WorldStateService::OnState(const PacketEvent<RequestWorldState>& aEvent) noexcept
{
    auto* player = aEvent.pPlayer;
    auto* party = m_world.GetPartyService().GetPlayerParty(player);
    if (!party || !player->GetParty().JoinedPartyId || party->SessionState < 2 ||
        !m_world.GetPartyService().IsPlayerLeader(player) || !aEvent.Packet.IsValid())
        return;
    auto& table = m_parties[*player->GetParty().JoinedPartyId];
    table.SetAuthority(party->StartEpoch, party->LeaderPlayerId);
    NotifyWorldState notify;
    notify.LeaderId = party->LeaderPlayerId;
    notify.State = aEvent.Packet.State;
    if (!table.Accept(player->GetId(), notify.State))
        return;
    // Activation is already sent through ObjectService/DoorVote. Cache its end
    // state for late joins without double-driving connected players.
    if (!WorldStateTable::ShouldDeliverLive(notify.State)) return;
    for (auto* member : party->Members)
        if (member != player) member->Send(notify);
}

void WorldStateService::OnCell(const PacketEvent<RequestWorldStateCell>& aEvent) noexcept
{
    auto* player = aEvent.pPlayer;
    auto* party = m_world.GetPartyService().GetPlayerParty(player);
    if (!party || !player->GetParty().JoinedPartyId || !aEvent.Packet.IsValid() ||
        party->SessionState < 2 || party->StartEpoch != aEvent.Packet.Epoch ||
        m_world.GetPartyService().IsPlayerLeader(player))
        return;
    const ReplayKey key{player->GetId(), aEvent.Packet.Cell.LogFormat()};
    // Repeated requests while a page is pending must not restart its cursor.
    const CellReplay next{*player->GetParty().JoinedPartyId,
        party->LeaderPlayerId, party->StartEpoch, 0, aEvent.Packet.Cell};
    auto [found, inserted] = m_replays.try_emplace(key, next);
    if (!inserted && (found->second.Party != next.Party || found->second.Leader != next.Leader || found->second.Epoch != next.Epoch))
        found->second = next;
}

void WorldStateService::OnUpdate(const UpdateEvent&) noexcept
{
    auto it = m_parties.upper_bound(m_partyCursor);
    for (size_t budget = (std::min)(size_t{8}, m_parties.size()); budget; --budget)
    {
        if (it == m_parties.end()) it = m_parties.begin();
        m_partyCursor = it->first;
        const auto* party = m_world.GetPartyService().GetById(it->first);
        if (!party)
            it = m_parties.erase(it);
        else
        {
            it->second.SetAuthority(party->StartEpoch, party->LeaderPlayerId);
            ++it;
        }
    }
    // One four-reference page per update globally, round-robin across players
    // and scopes. No full scope materialization or per-player reference scan.
    if (m_replays.empty()) return;
    auto pending = m_replays.upper_bound(m_replayCursor);
    if (pending == m_replays.end()) pending = m_replays.begin();
    m_replayCursor = pending->first;
    auto& replay = pending->second;
    auto* player = PlayerManager::Get()->GetById(pending->first.first);
    auto* party = player ? m_world.GetPartyService().GetPlayerParty(player) : nullptr;
    if (!party || !player->GetParty().JoinedPartyId || *player->GetParty().JoinedPartyId != replay.Party ||
        party->SessionState < 2 || party->StartEpoch != replay.Epoch || party->LeaderPlayerId != replay.Leader ||
        m_world.GetPartyService().IsPlayerLeader(player))
    {
        m_replays.erase(pending); return;
    }
    auto& table = m_parties[replay.Party];
    table.SetAuthority(replay.Epoch, replay.Leader);
    bool done{};
    for (const auto& state : table.SnapshotPage(replay.Cell, replay.Cursor, done))
    {
        NotifyWorldState notify;
        notify.LeaderId = replay.Leader; notify.State = state;
        if (notify.State.Kind == WorldStateKind::Open || notify.State.Kind == WorldStateKind::AnimationSnapshot) notify.State.Scalar = 2;
        player->Send(notify);
    }
    if (done) m_replays.erase(pending);
}
