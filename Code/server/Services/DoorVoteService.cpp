#include <Services/DoorVoteService.h>
#include <Services/ReviveService.h>
#include <World.h>
#include <Components.h>
#include <GameServer.h>
#include <Game/Player.h>
#include <Events/UpdateEvent.h>
#include <Messages/DoorVoteRequest.h>
#include <Messages/NotifyDoorVote.h>
#include <cmath>

DoorVoteService::DoorVoteService(World& aWorld, entt::dispatcher& aDispatcher) noexcept
    : m_world(aWorld)
    , m_requestConnection(aDispatcher.sink<PacketEvent<DoorVoteRequest>>().connect<&DoorVoteService::OnRequest>(this))
    , m_updateConnection(aDispatcher.sink<UpdateEvent>().connect<&DoorVoteService::OnUpdate>(this))
{
}

bool DoorVoteService::IsNear(Player* aPlayer, const DoorVoteData& aData, float aRadius) const noexcept
{
    if (!aPlayer || !aPlayer->GetCharacter())
        return false;
    // A test-cell move has no door to stand at.
    if (aData.Door && aData.Door == aData.Destination)
        return true;
    const auto& cell = aPlayer->GetCellComponent();
    if (aData.WorldSpace ? cell.WorldSpaceId != aData.WorldSpace : cell.Cell != aData.Cell)
        return false;
    const auto* movement = m_world.try_get<MovementComponent>(*aPlayer->GetCharacter());
    if (!movement)
        return false;
    const auto delta = movement->Position - static_cast<const glm::vec3&>(aData.Position);
    const float distance = glm::dot(delta, delta);
    return std::isfinite(distance) && distance <= aRadius * aRadius;
}

bool DoorVoteService::IsCurrent(uint32_t aPartyId, const Vote& aVote) const noexcept
{
    const auto* party = m_world.GetPartyService().GetById(aPartyId);
    if (!party || party->StartEpoch != aVote.Data.Epoch || party->LeaderPlayerId != aVote.Leader ||
        party->Members.size() != aVote.Members.size())
        return false;
    for (const auto* member : party->Members)
        if (!aVote.Members.count(member->GetId()))
            return false;
    return true;
}

void DoorVoteService::Broadcast(Vote& aVote, DoorVoteAction aAction, const String& aNotice) const noexcept
{
    NotifyDoorVote message;
    static_cast<DoorVoteData&>(message) = aVote.Data;
    message.Action = aAction;
    message.Notice = aNotice.substr(0, 512);
    message.ReadyCount = static_cast<uint32_t>(aVote.Ready.size());
    message.TotalCount = static_cast<uint32_t>(aVote.Members.size());
    for (auto id : aVote.Members)
    {
        if (auto* member = m_world.GetPlayerManager().GetById(id))
        {
            message.Ready = aVote.Ready.count(id) != 0;
            member->Send(message);
        }
    }
}

void DoorVoteService::Cancel(Vote& aVote, const char* aReason) const noexcept
{
    spdlog::info("Door vote: cancelled ({})", aReason);
    Broadcast(aVote, DoorVoteAction::Cancel, "Door vote cancelled");
}

void DoorVoteService::Changed(Vote& aVote, bool aExtendDeadline) noexcept
{
    // A fallen member spectates and cannot vote: it is carried through with the party (owner question 2026-09-28).
    // Downed members still hold the vote, so nobody leaves a bleeding ally behind.
    for (const auto id : aVote.Members)
        if (m_world.GetReviveService().IsFallenPlayer(id))
            aVote.Ready.insert(id);
    aVote.Data.Tick = 0;
    // A player's vote restarts the 120 s window; a carry-through or a prune does not (repeated falls could stretch a
    // vote forever).
    if (aExtendDeadline)
        aVote.Deadline = GameServer::Get()->GetTick() + 120000;
    spdlog::info("Door vote: vote {}/{}", aVote.Ready.size(), aVote.Members.size());
    const auto notice = fmt::format("{} wants to go through {}: {}/{} ready. Activate the door to go.",
        aVote.InitiatorName, aVote.Data.Name, aVote.Ready.size(), aVote.Members.size());
    Broadcast(aVote, DoorVoteAction::State, String(notice.c_str()));
    if (aVote.Ready.size() == aVote.Members.size())
    {
        aVote.Data.Tick = GameServer::Get()->GetTick() + 1000;
        aVote.Deadline = aVote.Data.Tick + 300000;
        spdlog::info("Door vote: go at tick {}", aVote.Data.Tick);
        Broadcast(aVote, DoorVoteAction::Go, {});
    }
}

void DoorVoteService::OnRequest(const PacketEvent<DoorVoteRequest>& aEvent) noexcept
{
    auto* player = aEvent.pPlayer;
    const auto& request = aEvent.Packet;
    auto* party = m_world.GetPartyService().GetPlayerParty(player);
    if (!request.IsValid() || !party || party->Members.size() < 2 ||
        request.Epoch != party->StartEpoch || !player->GetParty().JoinedPartyId ||
        std::find(party->Members.begin(), party->Members.end(), player) == party->Members.end())
        return;
    const auto partyId = *player->GetParty().JoinedPartyId;
    auto it = m_votes.find(partyId);
    if (it != m_votes.end() && !IsCurrent(partyId, it->second))
    {
        Cancel(it->second, "party changed");
        m_votes.erase(it);
        it = m_votes.end();
    }
    const auto now = GameServer::Get()->GetTick();
    if (request.Action == DoorVoteAction::TestCell)
    {
        // Test harness only: the leader moves the whole party into a cell through the door barrier (Go at a shared
        // tick, everyone loads, the world gate holds until all have). No door and no proximity.
        if (party->LeaderPlayerId != player->GetId() || !request.Destination || request.Destination.ModId == UINT32_MAX ||
            request.Name.empty())
            return;
        if (it != m_votes.end())
        {
            Cancel(it->second, "test cell move");
            m_votes.erase(it);
        }
        Vote vote;
        vote.Data = request;
        vote.Data.Action = DoorVoteAction::Vote;
        vote.Data.Door = request.Destination;
        vote.Data.Tick = 0;
        vote.Data.VoteId = ++m_nextVoteId;
        vote.Initiator = player->GetId();
        vote.InitiatorName = player->GetUsername().substr(0, 80);
        vote.Leader = party->LeaderPlayerId;
        vote.Deadline = now + 120000;
        for (const auto* member : party->Members)
        {
            vote.Members.insert(member->GetId());
            vote.Ready.insert(member->GetId());
        }
        auto& started = m_votes.emplace(partyId, std::move(vote)).first->second;
        spdlog::info("Door vote: test cell {:X} for the party of {}", request.Destination.BaseId, player->GetUsername());
        Changed(started);
        return;
    }
    if (request.Action == DoorVoteAction::Vote)
    {
        if (!request.Door || !request.Cell || !request.Destination || request.Door.ModId == UINT32_MAX ||
            request.Name.empty() || !IsNear(player, request, 400.f))
            return;
        if (it != m_votes.end())
        {
            auto& vote = it->second;
            if (vote.Data.Tick && now >= vote.Data.Tick)
                return;
            if (request.VoteId != 0 && request.VoteId != vote.Data.VoteId)
                return;
            if (vote.Data.Door != request.Door)
            {
                if (request.VoteId != 0)
                    return;
                Cancel(vote, "another door");
                m_votes.erase(it);
                it = m_votes.end();
            }
        }
        if (it == m_votes.end())
        {
            // A nonzero generation belongs to a retired vote, never a fresh start.
            if (request.VoteId != 0)
                return;
            Vote vote;
            vote.Data = request;
            vote.Data.Tick = 0;
            vote.Data.VoteId = ++m_nextVoteId;
            vote.Initiator = player->GetId();
            vote.InitiatorName = player->GetUsername().substr(0, 80);
            vote.Leader = party->LeaderPlayerId;
            vote.Deadline = now + 120000;
            for (const auto* member : party->Members)
                vote.Members.insert(member->GetId());
            it = m_votes.emplace(partyId, std::move(vote)).first;
            spdlog::info("Door vote: started by {} for {:X}", player->GetUsername(), request.Door.BaseId);
        }
        auto& vote = it->second;
        if (request.Cell != vote.Data.Cell || request.Destination != vote.Data.Destination ||
            !IsNear(player, vote.Data, 400.f))
            return;
        if (vote.Ready.insert(player->GetId()).second)
            Changed(vote);
        return;
    }
    if (it == m_votes.end() || request.VoteId != it->second.Data.VoteId || request.Door != it->second.Data.Door)
        return;
    auto& vote = it->second;
    if (request.Action == DoorVoteAction::Failed && vote.Ready.count(player->GetId()))
    {
        Cancel(vote, "a player could not activate the door");
        m_votes.erase(it);
    }
    else if (request.Action == DoorVoteAction::Withdraw)
    {
        if (vote.Data.Tick && now >= vote.Data.Tick)
            return;
        if (player->GetId() == vote.Initiator)
        {
            const auto notice = fmt::format("{} left the door", vote.InitiatorName);
            Broadcast(vote, DoorVoteAction::State, String(notice.c_str()));
            Cancel(vote, "initiator left the door");
            m_votes.erase(it);
        }
        else if (vote.Ready.erase(player->GetId()))
        {
            const auto notice = fmt::format("{} left the door", player->GetUsername().substr(0, 80));
            Broadcast(vote, DoorVoteAction::State, String(notice.c_str()));
            Changed(vote);
        }
    }
    else if (request.Action == DoorVoteAction::Loaded && vote.Data.Tick && now >= vote.Data.Tick &&
        player->GetCellComponent().Cell == vote.Data.Destination)
    {
        vote.Loaded.insert(player->GetId());
        if (vote.Loaded.size() == vote.Members.size())
        {
            spdlog::info("Door vote: all {} players loaded for vote {}", vote.Members.size(), vote.Data.VoteId);
            Broadcast(vote, DoorVoteAction::Release, {});
            m_votes.erase(it);
        }
    }
}

void DoorVoteService::OnUpdate(const UpdateEvent&) noexcept
{
    const auto now = GameServer::Get()->GetTick();
    for (auto it = m_votes.begin(); it != m_votes.end();)
    {
        auto& vote = it->second;
        // A fallen initiator spectates from elsewhere (its hidden body follows the camera): not a departure.
        const bool departed = (!vote.Data.Tick || now < vote.Data.Tick) &&
            !m_world.GetReviveService().IsFallenPlayer(vote.Initiator) &&
            !IsNear(m_world.GetPlayerManager().GetById(vote.Initiator), vote.Data, 400.f);
        if (!IsCurrent(it->first, vote) || now >= vote.Deadline || departed)
        {
            if (departed)
            {
                const auto notice = fmt::format("{} left the door", vote.InitiatorName);
                Broadcast(vote, DoorVoteAction::State, String(notice.c_str()));
            }
            Cancel(vote, departed ? "initiator left the door" : now >= vote.Deadline ? "timed out" : "party changed");
            it = m_votes.erase(it);
            continue;
        }
        if (!vote.Data.Tick || now < vote.Data.Tick)
        {
            bool changed = false;
            // A member who fell mid-vote is carried through at once, not at the next vote event (the vote used to
            // sit until its 120 s deadline; Muse review 2026-09-29).
            for (const auto id : vote.Members)
                if (!vote.Ready.count(id) && m_world.GetReviveService().IsFallenPlayer(id))
                    changed = true;
            for (auto ready = vote.Ready.begin(); ready != vote.Ready.end();)
            {
                auto* player = m_world.GetPlayerManager().GetById(*ready);
                // Fallen members spectate from wherever they are; distance does not apply to them.
                if (!IsNear(player, vote.Data, 400.f) && !m_world.GetReviveService().IsFallenPlayer(*ready))
                {
                    ready = vote.Ready.erase(ready);
                    vote.Data.Tick = 0;
                    const auto notice = fmt::format("{} left the door", player ? player->GetUsername() : String("A player"));
                    Broadcast(vote, DoorVoteAction::State, String(notice.c_str()));
                    changed = true;
                }
                else
                    ++ready;
            }
            if (changed)
                Changed(vote, false);
        }
        ++it;
    }
}
