#include <Services/ReviveService.h>
#include <World.h>
#include <Components.h>
#include <GameServer.h>
#include <Game/Player.h>
#include <Events/UpdateEvent.h>
#include <Messages/ReviveRequest.h>
#include <Messages/NotifyRevive.h>
#include <cmath>

ReviveService::ReviveService(World& aWorld, entt::dispatcher& aDispatcher) noexcept
    : m_world(aWorld)
    , m_requestConnection(aDispatcher.sink<PacketEvent<ReviveRequest>>().connect<&ReviveService::OnRequest>(this))
    , m_updateConnection(aDispatcher.sink<UpdateEvent>().connect<&ReviveService::OnUpdate>(this))
{
}

bool ReviveService::Current(uint32_t aPlayer, const State& aState, uint64_t aNow) const noexcept
{
    auto* player = m_world.GetPlayerManager().GetById(aPlayer);
    auto* party = player ? m_world.GetPartyService().GetPlayerParty(player) : nullptr;
    return party && player->GetParty().JoinedPartyId == aState.PartyId &&
        party->StartEpoch == aState.Data.Epoch && aNow - aState.Received <= 3500 &&
        std::find(party->Members.begin(), party->Members.end(), player) != party->Members.end();
}

bool ReviveService::Eligible(uint32_t aReviver, const Hold& aHold, uint64_t aNow) const noexcept
{
    const auto reviver = m_states.find(aReviver);
    const auto target = m_states.find(aHold.Target);
    if (aReviver == aHold.Target || reviver == m_states.end() || target == m_states.end() ||
        !Current(aReviver, reviver->second, aNow) || !Current(aHold.Target, target->second, aNow) ||
        reviver->second.PartyId != target->second.PartyId ||
        reviver->second.Data.Down || !reviver->second.Data.Alive || reviver->second.Data.InCombat ||
        !target->second.Data.Down || target->second.Data.Revision != aHold.Revision)
        return false;

    auto* player = m_world.GetPlayerManager().GetById(aReviver);
    auto* downed = m_world.GetPlayerManager().GetById(aHold.Target);
    if (!player->GetCharacter() || !downed->GetCharacter())
        return false;
    const auto& cell = player->GetCellComponent();
    const auto& targetCell = downed->GetCellComponent();
    if (!cell.Cell || !targetCell.Cell || cell.WorldSpaceId != targetCell.WorldSpaceId ||
        (!cell.WorldSpaceId && cell.Cell != targetCell.Cell))
        return false;
    const auto* character = m_world.try_get<CharacterComponent>(*player->GetCharacter());
    const auto* movement = m_world.try_get<MovementComponent>(*player->GetCharacter());
    const auto* targetMovement = m_world.try_get<MovementComponent>(*downed->GetCharacter());
    if (!character || character->IsDead() || !movement || !targetMovement)
        return false;
    const auto delta = movement->Position - targetMovement->Position;
    const float distance = glm::dot(delta, delta);
    return std::isfinite(distance) && distance <= 200.f * 200.f;
}

void ReviveService::Broadcast(Player* aPlayer, const ReviveData& aData) const noexcept
{
    auto* party = m_world.GetPartyService().GetPlayerParty(aPlayer);
    if (!party)
        return;
    NotifyRevive message;
    static_cast<ReviveData&>(message) = aData;
    for (auto* member : party->Members)
        member->Send(message);
}

void ReviveService::OnRequest(const PacketEvent<ReviveRequest>& aEvent) noexcept
{
    auto* player = aEvent.pPlayer;
    auto* party = m_world.GetPartyService().GetPlayerParty(player);
    const auto& request = aEvent.Packet;
    if (!request.IsValid() || !party || !player->GetParty().JoinedPartyId ||
        request.Epoch != party->StartEpoch || !request.Revision || !player->GetCharacter() ||
        std::find(party->Members.begin(), party->Members.end(), player) == party->Members.end())
        return;
    const auto id = player->GetId();
    const auto now = GameServer::Get()->GetTick();
    if (request.Action == ReviveAction::State)
    {
        auto& state = m_states[id];
        const auto partyId = *player->GetParty().JoinedPartyId;
        if (state.PartyId == partyId && state.Data.Epoch == request.Epoch &&
            request.Revision < state.Data.Revision)
            return;
        if (state.PartyId != partyId || state.Data.Epoch != request.Epoch)
            state = {};
        const auto* movement = m_world.try_get<MovementComponent>(*player->GetCharacter());
        if (!movement)
            return;
        if (state.Data.Revision != request.Revision)
            state.GrantUntil = 0;
        state.Data = request;
        state.Data.PlayerId = id;
        state.Data.ReviverId = 0;
        const auto* character = m_world.try_get<CharacterComponent>(*player->GetCharacter());
        state.Data.Alive = request.Alive && character && !character->IsDead();
        state.Data.Cell = player->GetCellComponent().Cell;
        state.Data.WorldSpace = player->GetCellComponent().WorldSpaceId;
        state.Data.Position = movement->Position;
        state.PartyId = partyId;
        state.Received = now;
        Broadcast(player, state.Data);
        return;
    }
    if (request.Action == ReviveAction::Cancel)
    {
        m_holds.erase(id);
        return;
    }
    if (request.Action != ReviveAction::Hold && request.Action != ReviveAction::Finish)
        return;
    Hold candidate{request.PlayerId, request.Revision, now, now};
    if (!Eligible(id, candidate, now))
    {
        m_holds.erase(id);
        return;
    }
    auto it = m_holds.find(id);
    if (request.Action == ReviveAction::Hold)
    {
        if (it == m_holds.end() || it->second.Target != candidate.Target ||
            it->second.Revision != candidate.Revision || now - it->second.Received > 1000)
        {
            m_holds[id] = candidate;
            spdlog::info("Revive: revive started by {}", player->GetUsername());
        }
        else
            it->second.Received = now;
        return;
    }
    if (it == m_holds.end() || it->second.Target != candidate.Target ||
        it->second.Revision != candidate.Revision || now - it->second.Started < 3000 ||
        now - it->second.Received > 1000)
        return;
    auto& target = m_states.at(candidate.Target);
    if (now < target.GrantUntil)
        return;
    // Keep the owner down until its acknowledgement. A grant is not a completed recovery.
    target.GrantUntil = now + 3500;
    NotifyRevive grant;
    static_cast<ReviveData&>(grant) = target.Data;
    grant.Action = ReviveAction::Grant;
    grant.ReviverId = id;
    m_world.GetPlayerManager().GetById(candidate.Target)->Send(grant);
    m_holds.erase(it);
}

void ReviveService::OnUpdate(const UpdateEvent&) noexcept
{
    const auto now = GameServer::Get()->GetTick();
    std::erase_if(m_holds, [&](const auto& entry) {
        return now - entry.second.Received > 1000 || !Eligible(entry.first, entry.second, now);
    });
    std::erase_if(m_states, [&](const auto& entry) { return !Current(entry.first, entry.second, now); });
}
