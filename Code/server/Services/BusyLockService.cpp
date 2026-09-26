#include <Services/BusyLockService.h>
#include <World.h>
#include <Components.h>
#include <GameServer.h>
#include <Game/Player.h>
#include <Events/UpdateEvent.h>
#include <Events/PlayerLeaveEvent.h>
#include <Messages/BusyLockRequest.h>
#include <Messages/NotifyBusyLock.h>

namespace
{
const char* ReasonText(BusyLockReason aReason)
{
    switch (aReason)
    {
    case BusyLockReason::Closed: return "menu closed";
    case BusyLockReason::Cancelled: return "cancelled";
    case BusyLockReason::Timeout: return "request timed out";
    case BusyLockReason::Load: return "owner loaded";
    case BusyLockReason::Death: return "owner died";
    case BusyLockReason::PartyLeft: return "party left";
    case BusyLockReason::Disconnected: return "disconnected";
    }
    return "cancelled";
}
}

BusyLockService::BusyLockService(World& aWorld, entt::dispatcher& aDispatcher) noexcept
    : m_world(aWorld)
    , m_requestConnection(aDispatcher.sink<PacketEvent<BusyLockRequest>>().connect<&BusyLockService::OnRequest>(this))
    , m_updateConnection(aDispatcher.sink<UpdateEvent>().connect<&BusyLockService::OnUpdate>(this))
    , m_leaveConnection(aDispatcher.sink<PlayerLeaveEvent>().connect<&BusyLockService::OnLeave>(this))
{
}

bool BusyLockService::CanonicalReference(const GameId& aReference, GameId& aCanonical) const noexcept
{
    aCanonical = aReference;
    if (aReference.ModId != UINT32_MAX)
        return aReference && aReference.BaseId <= 0xFFFFFF;

    const auto entity = static_cast<entt::entity>(aReference.BaseId);
    if (!m_world.valid(entity))
        return false;
    const auto* character = m_world.try_get<CharacterComponent>(entity);
    if (character && character->IsPlayer())
        return false;
    const auto* form = m_world.try_get<FormIdComponent>(entity);
    // Temporary NPCs intentionally have no FormIdComponent in CreateCharacter.
    if (!character && !m_world.try_get<ObjectComponent>(entity))
        return false;
    // A follower can have a temporary local proxy for a static host reference.
    // Canonicalize both requests to the static GameId, avoiding two locks for one NPC.
    if (form && form->Id && form->Id.ModId != UINT32_MAX)
        aCanonical = form->Id;
    return true;
}

const char* BusyLockService::Expired(const Lease& aLease, uint64_t aNow) const noexcept
{
    if (aNow >= aLease.Deadline)
        return "heartbeat timed out";
    auto* player = m_world.GetPlayerManager().GetById(aLease.Owner);
    if (!player)
        return "disconnected";
    const auto* party = m_world.GetPartyService().GetById(aLease.PartyId);
    if (!party || party->Members.size() < 2 || player->GetParty().JoinedPartyId != aLease.PartyId ||
        party->StartEpoch != aLease.Data.Epoch ||
        std::find(party->Members.begin(), party->Members.end(), player) == party->Members.end())
        return "party left or session changed";
    if (!player->GetCharacter() || *player->GetCharacter() != aLease.Character ||
        player->GetCellComponent().WorldSpaceId != aLease.WorldSpace ||
        (!aLease.WorldSpace && player->GetCellComponent().Cell != aLease.Cell))
        return "owner loaded or changed cell";
    const auto* character = m_world.try_get<CharacterComponent>(aLease.Character);
    if (!character || character->IsDead())
        return "owner died or unloaded";
    return nullptr;
}

void BusyLockService::ReleaseOwner(uint32_t aOwner, const char* aReason) noexcept
{
    for (auto it = m_leases.begin(); it != m_leases.end();)
    {
        if (it->second.Owner != aOwner)
        {
            ++it;
            continue;
        }
        spdlog::info("Busy lock: {:X} released ({})", it->first.second, aReason);
        it = m_leases.erase(it);
    }
}

void BusyLockService::OnLeave(const PlayerLeaveEvent& aEvent) noexcept
{
    ReleaseOwner(aEvent.pPlayer->GetId(), "disconnected");
    m_lastRequest.erase(aEvent.pPlayer->GetId());
}

void BusyLockService::OnUpdate(const UpdateEvent&) noexcept
{
    const auto now = GameServer::Get()->GetTick();
    for (auto it = m_leases.begin(); it != m_leases.end();)
    {
        if (const auto* reason = Expired(it->second, now))
        {
            spdlog::info("Busy lock: {:X} released ({})", it->first.second, reason);
            it = m_leases.erase(it);
        }
        else
            ++it;
    }
}

void BusyLockService::OnRequest(const PacketEvent<BusyLockRequest>& aEvent) noexcept
{
    const auto& request = aEvent.Packet;
    auto* player = aEvent.pPlayer;
    if (!player || !request.IsValid() || !request.RequestId || !request.Reference ||
        request.Action > BusyLockAction::Release || !request.Holder.empty())
        return;
    const auto owner = player->GetId();
    const auto now = GameServer::Get()->GetTick();

    // Releases remain valid after leaving a party or loading. Only the exact owner's
    // request can be retired, so late packets cannot release a replacement lease.
    if (request.Action == BusyLockAction::Release)
    {
        m_lastRequest[owner] = (std::max)(m_lastRequest[owner], request.RequestId);
        for (auto it = m_leases.begin(); it != m_leases.end(); ++it)
        {
            const auto& lease = it->second;
            if (lease.Owner == owner && lease.Data.RequestId == request.RequestId &&
                lease.Data.Epoch == request.Epoch && lease.Data.Reference == request.Reference)
            {
                spdlog::info("Busy lock: {:X} released ({})", it->first.second, ReasonText(request.Reason));
                m_leases.erase(it);
                break;
            }
        }
        return;
    }

    auto* party = m_world.GetPartyService().GetPlayerParty(player);
    if (!party || !player->GetParty().JoinedPartyId || party->Members.size() < 2 ||
        party->StartEpoch != request.Epoch || !player->GetCharacter() ||
        std::find(party->Members.begin(), party->Members.end(), player) == party->Members.end())
        return;
    const auto* character = m_world.try_get<CharacterComponent>(*player->GetCharacter());
    if (!character || character->IsDead())
        return;
    GameId reference{};
    if (!CanonicalReference(request.Reference, reference))
        return;
    const Key key{*player->GetParty().JoinedPartyId, reference.LogFormat()};
    auto it = m_leases.find(key);
    if (it != m_leases.end())
    {
        if (const auto* reason = Expired(it->second, now))
        {
            spdlog::info("Busy lock: {:X} released ({})", key.second, reason);
            m_leases.erase(it);
            it = m_leases.end();
        }
    }
    const bool owns = it != m_leases.end() && it->second.Owner == owner &&
        it->second.Data.RequestId == request.RequestId && it->second.Data.Epoch == request.Epoch;
    if (request.Action == BusyLockAction::Heartbeat)
    {
        if (owns)
        {
            it->second.Deadline = now + 300000;
            it->second.Data.Kind = request.Kind;
        }
        return;
    }

    NotifyBusyLock reply;
    static_cast<BusyLockData&>(reply) = request;
    if (owns)
    {
        reply.Action = BusyLockAction::Granted;
        player->Send(reply);
        return;
    }
    auto& last = m_lastRequest[owner];
    if (request.RequestId <= last)
    {
        reply.Action = BusyLockAction::Unavailable;
        player->Send(reply);
        return;
    }
    last = request.RequestId;
    if (it != m_leases.end() && it->second.Owner != owner)
    {
        reply.Action = BusyLockAction::Denied;
        reply.Holder = it->second.Data.Holder;
        reply.HolderPlayerId = it->second.Data.HolderPlayerId;
        reply.Kind = it->second.Data.Kind;
        spdlog::info("Busy lock: {:X} denied ({} holds it)", key.second, reply.Holder);
    }
    else
    {
        ReleaseOwner(owner, "another interaction");
        Lease lease;
        lease.Data = request;
        lease.Data.Holder = player->GetUsername().substr(0, 80);
        lease.Data.HolderPlayerId = player->GetId();
        lease.Owner = owner;
        lease.PartyId = key.first;
        lease.Character = *player->GetCharacter();
        lease.Cell = player->GetCellComponent().Cell;
        lease.WorldSpace = player->GetCellComponent().WorldSpaceId;
        lease.Deadline = now + 300000;
        m_leases.emplace(key, std::move(lease));
        reply.Action = BusyLockAction::Granted;
        spdlog::info("Busy lock: {:X} granted to {}", key.second, player->GetUsername());
    }
    player->Send(reply);
}
