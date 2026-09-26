#include <Services/DialogueListenService.h>
#include <World.h>
#include <Components.h>
#include <Game/Player.h>
#include <GameServer.h>
#include <Events/UpdateEvent.h>
#include <Messages/RequestDialogueListen.h>
#include <cmath>

DialogueListenService::DialogueListenService(World& aWorld, entt::dispatcher& aDispatcher) noexcept
    : m_world(aWorld)
    , m_requestConnection(aDispatcher.sink<PacketEvent<RequestDialogueListen>>().connect<&DialogueListenService::OnRequest>(this))
    , m_updateConnection(aDispatcher.sink<UpdateEvent>().connect<&DialogueListenService::OnUpdate>(this))
{
}

bool DialogueListenService::Near(Player* aPlayer, uint32_t aNpc, float aRadius) const noexcept
{
    if (!aPlayer || !aPlayer->GetCharacter()) return false;
    const auto entity = static_cast<entt::entity>(aNpc);
    const auto* npc = m_world.try_get<CharacterComponent>(entity);
    const auto* movement = m_world.try_get<MovementComponent>(entity);
    const auto* cell = m_world.try_get<CellIdComponent>(entity);
    const auto* player = m_world.try_get<CharacterComponent>(*aPlayer->GetCharacter());
    const auto* position = m_world.try_get<MovementComponent>(*aPlayer->GetCharacter());
    if (!npc || npc->IsPlayer() || npc->IsDead() || !movement || !cell ||
        !player || player->IsDead() || !position) return false;
    const auto& playerCell = aPlayer->GetCellComponent();
    if (cell->WorldSpaceId ? cell->WorldSpaceId != playerCell.WorldSpaceId :
        cell->Cell != playerCell.Cell) return false;
    const auto delta = position->Position - movement->Position;
    const float distance = glm::dot(delta, delta);
    return std::isfinite(distance) && distance <= aRadius * aRadius;
}

bool DialogueListenService::Current(const Stream& aStream) const noexcept
{
    const auto* party = m_world.GetPartyService().GetById(aStream.Party);
    auto* speaker = m_world.GetPlayerManager().GetById(aStream.Message.Speaker);
    if (!party || !speaker || party->StartEpoch != aStream.Message.State.Epoch ||
        !speaker->GetParty().JoinedPartyId || *speaker->GetParty().JoinedPartyId != aStream.Party ||
        std::find(party->Members.begin(), party->Members.end(), speaker) == party->Members.end()) return false;
    return Near(speaker, aStream.Message.State.NpcServerId, 600.f);
}

void DialogueListenService::Close(Stream& aStream) noexcept
{
    auto message = aStream.Message;
    message.State.Active = false;
    for (const auto id : aStream.Recipients)
        if (auto* member = m_world.GetPlayerManager().GetById(id)) member->Send(message);
    aStream.Recipients.clear();
}

void DialogueListenService::Broadcast(Stream& aStream, bool aChanged) noexcept
{
    const auto* party = m_world.GetPartyService().GetById(aStream.Party);
    if (!party) return;
    std::unordered_set<uint32_t> nearby;
    for (auto* member : party->Members)
    {
        const auto id = member->GetId();
        if (id == aStream.Message.Speaker || !Near(member, aStream.Message.State.NpcServerId, 400.f)) continue;
        nearby.insert(id);
        if (aChanged || !aStream.Recipients.count(id)) member->Send(aStream.Message);
    }
    auto closed = aStream.Message;
    closed.State.Active = false;
    for (const auto id : aStream.Recipients)
        if (!nearby.count(id))
            if (auto* member = m_world.GetPlayerManager().GetById(id)) member->Send(closed);
    aStream.Recipients = std::move(nearby);
}

void DialogueListenService::OnRequest(const PacketEvent<RequestDialogueListen>& aEvent) noexcept
{
    const auto& request = aEvent.Packet;
    auto* player = aEvent.pPlayer;
    auto* party = m_world.GetPartyService().GetPlayerParty(player);
    if (!request.IsValid() || !request.State.Valid() || !party || !player->GetParty().JoinedPartyId ||
        request.State.Epoch != party->StartEpoch ||
        std::find(party->Members.begin(), party->Members.end(), player) == party->Members.end()) return;
    const auto partyId = *player->GetParty().JoinedPartyId;
    if (request.Query)
    {
        const auto it = m_streams.find(request.Speaker);
        if (it != m_streams.end() && it->second.Party == partyId && Current(it->second) &&
            it->second.Message.State.NpcServerId == request.State.NpcServerId &&
            Near(player, request.State.NpcServerId, 400.f))
        {
            player->Send(it->second.Message);
            it->second.Recipients.insert(player->GetId());
        }
        return;
    }
    auto& last = m_lastRevision[player->GetId()];
    if (last.first == request.State.Epoch && request.State.Revision <= last.second) return;
    auto it = m_streams.find(player->GetId());
    if (it != m_streams.end() && !Current(it->second))
    { Close(it->second); m_streams.erase(it); it = m_streams.end(); }
    if (it != m_streams.end())
    {
        const auto& previous = it->second.Message.State;
        if (request.State.Session < previous.Session || request.State.Revision <= previous.Revision) return;
    }
    if (!request.State.Active)
    {
        last = {request.State.Epoch, request.State.Revision};
        if (it != m_streams.end() && request.State.Session == it->second.Message.State.Session)
        { Close(it->second); m_streams.erase(it); }
        return;
    }
    if (!Near(player, request.State.NpcServerId, 600.f)) return;
    // NPC identity comes from the server entity, never the publisher's load order.
    const auto* form = m_world.try_get<FormIdComponent>(static_cast<entt::entity>(request.State.NpcServerId));
    if (form && form->Id && request.State.Npc != form->Id) return;
    for (const auto& [speaker, stream] : m_streams)
        if (speaker != player->GetId() && stream.Party == partyId && Current(stream) &&
            stream.Message.State.NpcServerId == request.State.NpcServerId) return;
    if (it != m_streams.end() && (it->second.Message.State.Session != request.State.Session ||
        it->second.Message.State.NpcServerId != request.State.NpcServerId)) Close(it->second);
    auto& stream = m_streams[player->GetId()];
    stream.Party = partyId;
    stream.Message.Speaker = player->GetId();
    stream.Message.State = request.State;
    last = {request.State.Epoch, request.State.Revision};
    Broadcast(stream, true);
}

void DialogueListenService::OnUpdate(const UpdateEvent&) noexcept
{
    const auto now = GameServer::Get()->GetTick();
    if (now < m_nextUpdate) return;
    m_nextUpdate = now + 250;
    for (auto it = m_lastRevision.begin(); it != m_lastRevision.end();)
        if (!m_world.GetPlayerManager().GetById(it->first)) it = m_lastRevision.erase(it);
        else ++it;
    for (auto it = m_streams.begin(); it != m_streams.end();)
    {
        if (!Current(it->second)) { Close(it->second); it = m_streams.erase(it); }
        else { Broadcast(it->second, false); ++it; }
    }
}
