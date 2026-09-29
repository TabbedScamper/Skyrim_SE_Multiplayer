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

// Owner rule (2026-09-28): a revive takes twice as long (6 s instead of 3 s) while the reviver, the downed player, or
// any party member within 2000 u of the downed player is in combat or being targeted.
bool ReviveService::CombatAround(uint32_t aReviver, uint32_t aTarget) const noexcept
{
    const auto target = m_states.find(aTarget);
    if (target == m_states.end())
        return false;
    for (const auto& [id, state] : m_states)
    {
        if (state.PartyId != target->second.PartyId || !state.Data.InCombat || !state.Data.Alive)
            continue;
        if (id == aReviver || id == aTarget)
            return true;
        if (state.Data.WorldSpace != target->second.Data.WorldSpace ||
            (!state.Data.WorldSpace && state.Data.Cell != target->second.Data.Cell))
            continue;
        const auto delta = static_cast<const glm::vec3&>(state.Data.Position) -
            static_cast<const glm::vec3&>(target->second.Data.Position);
        if (glm::dot(delta, delta) <= 2000.f * 2000.f)
            return true;
    }
    return false;
}

bool ReviveService::Eligible(uint32_t aReviver, const Hold& aHold, uint64_t aNow) const noexcept
{
    const auto reviver = m_states.find(aReviver);
    const auto target = m_states.find(aHold.Target);
    if (aReviver == aHold.Target || reviver == m_states.end() || target == m_states.end() ||
        !Current(aReviver, reviver->second, aNow) || !Current(aHold.Target, target->second, aNow) ||
        reviver->second.PartyId != target->second.PartyId ||
        reviver->second.Data.Down || reviver->second.Data.Dead || !reviver->second.Data.Alive ||
        !target->second.Data.Down || target->second.Data.Dead || target->second.Data.Revision != aHold.Revision)
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

bool ReviveService::IsFallen(Player* aPlayer, uint64_t aEpoch) const noexcept
{
    if (!aPlayer || !aPlayer->GetParty().JoinedPartyId)
        return false;
    const auto it = m_fallen.find(aPlayer->GetId());
    return it != m_fallen.end() && it->second.PartyId == *aPlayer->GetParty().JoinedPartyId && it->second.Epoch == aEpoch;
}

bool ReviveService::IsFallenPlayer(uint32_t aPlayerId) const noexcept
{
    auto* player = m_world.GetPlayerManager().GetById(aPlayerId);
    auto* party = player ? m_world.GetPartyService().GetPlayerParty(player) : nullptr;
    return party && IsFallen(player, party->StartEpoch);
}

// Owner rule (2026-09-28): when nobody is standing (every member down or fallen, e.g. the last player goes down or
// bleeds out alone) everyone collapses like a vanilla death, then the party reloads the latest save with everyone
// alive. A member without a current state (loading) is never counted as down.
void ReviveService::CheckWipes(uint64_t aNow) noexcept
{
    std::map<uint32_t, bool> parties;
    for (const auto& [id, state] : m_states)
        parties.emplace(state.PartyId, true);
    for (const auto& [partyId, unused] : parties)
    {
        const auto* party = m_world.GetPartyService().GetById(partyId);
        if (!party || party->SessionState < 3 || party->Members.empty())
            continue;
        // Look up without creating: a default entry read as an 8 s old wipe restarted every new session
        // (2026-09-28: "party 0 wiped, reloading" right after each start, duplicating the follower's NPCs).
        if (const auto it = m_wipes.find(partyId); it != m_wipes.end() && it->second.Epoch == party->StartEpoch)
            continue;
        const bool nobodyStanding = std::all_of(party->Members.begin(), party->Members.end(), [&](Player* aMember) {
            const auto state = m_states.find(aMember->GetId());
            return state != m_states.end() && state->second.PartyId == partyId &&
                state->second.Data.Epoch == party->StartEpoch && aNow - state->second.Received <= 3500 &&
                (state->second.Data.Down || state->second.Data.Dead);
        });
        if (!nobodyStanding)
            continue;
        m_wipes[partyId] = {party->StartEpoch, aNow, false};
        NotifyRevive notice;
        notice.Action = ReviveAction::Wipe;
        notice.Epoch = party->StartEpoch;
        notice.Revision = 1;
        for (auto* member : party->Members)
        {
            m_fallen.erase(member->GetId());
            member->Send(notice);
        }
        m_holds.clear();
        spdlog::info("Revive: party {} wiped (epoch {})", partyId, party->StartEpoch);
    }
    for (auto& [partyId, wipe] : m_wipes)
    {
        if (wipe.Done || !wipe.At || aNow - wipe.At < 8000)
            continue;
        wipe.Done = true;
        const auto* party = m_world.GetPartyService().GetById(partyId);
        if (!party || party->StartEpoch != wipe.Epoch || party->SessionState < 3)
            continue;
        if (m_world.GetPartyService().RestartFromCheckpoint(partyId))
            continue;
        // No checkpoint in this session: everyone gets back up where they fell.
        spdlog::info("Revive: party {} wiped without a checkpoint, reviving in place", partyId);
        for (auto* member : party->Members)
        {
            const auto state = m_states.find(member->GetId());
            if (state == m_states.end())
                continue;
            NotifyRevive raise;
            static_cast<ReviveData&>(raise) = state->second.Data;
            raise.Action = ReviveAction::Raise;
            raise.PlayerId = member->GetId();
            raise.ReviverId = 0;
            member->Send(raise);
        }
    }
}

void ReviveService::SetFallen(Player* aPlayer, uint64_t aEpoch) noexcept
{
    m_fallen[aPlayer->GetId()] = {*aPlayer->GetParty().JoinedPartyId, aEpoch, aPlayer->GetUsername().c_str()};
}

// Owner design (2026-09-28): an ally out of combat with full magicka calls a fallen player back beside them. The
// server only accepts it for a fallen party member while the caster is alive, up and out of combat.
void ReviveService::OnRaise(Player* aCaster, const ReviveRequest& aRequest, uint64_t aNow) noexcept
{
    const auto casterId = aCaster->GetId();
    const auto caster = m_states.find(casterId);
    auto* target = m_world.GetPlayerManager().GetById(aRequest.PlayerId);
    auto* party = m_world.GetPartyService().GetPlayerParty(aCaster);
    if (caster == m_states.end() || !Current(casterId, caster->second, aNow) || !target || target == aCaster ||
        !party || std::find(party->Members.begin(), party->Members.end(), target) == party->Members.end() ||
        caster->second.Data.Down || caster->second.Data.Dead || !caster->second.Data.Alive ||
        CombatAround(casterId, casterId) || !IsFallen(target, party->StartEpoch))
    {
        spdlog::info("Revive: raise of player {} by {} refused", aRequest.PlayerId, aCaster->GetUsername());
        return;
    }
    m_fallen.erase(target->GetId());
    NotifyRevive raise;
    static_cast<ReviveData&>(raise) = caster->second.Data;
    raise.Action = ReviveAction::Raise;
    raise.Epoch = party->StartEpoch;
    raise.PlayerId = target->GetId();
    raise.ReviverId = casterId;
    raise.Dead = false;
    if (const auto it = m_states.find(target->GetId()); it != m_states.end())
    {
        raise.Revision = it->second.Data.Revision;
        it->second.RaisedRevision = it->second.Data.Revision;
        it->second.Data.Dead = false;
        Broadcast(target, it->second.Data);
    }
    target->Send(raise);
    spdlog::info("Revive: {} called back {}", aCaster->GetUsername(), target->GetUsername());
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
        // Fallen is sticky: set by the owner when it bleeds out, cleared only by a raise (or a new campaign). A report
        // still carrying the raised revision crossed the raise in flight and must not undo it (the player would stand
        // up while the server kept it fallen for good).
        if (request.Dead && state.RaisedRevision && request.Revision <= state.RaisedRevision)
            state.Data.Dead = false;
        else if (request.Dead && !IsFallen(player, request.Epoch))
        {
            SetFallen(player, request.Epoch);
            spdlog::info("Revive: {} has fallen", player->GetUsername());
        }
        else if (!IsFallen(player, request.Epoch))
        {
            // A fallen player who reconnected: restored by name only when no other member shares it.
            const DepartedKey departed{partyId, player->GetUsername().c_str()};
            const auto it = m_departed.find(departed);
            if (it != m_departed.end() && it->second == request.Epoch &&
                std::count_if(party->Members.begin(), party->Members.end(),
                    [&](Player* aMember) { return aMember->GetUsername() == player->GetUsername(); }) == 1)
            {
                SetFallen(player, request.Epoch);
                m_departed.erase(it);
                spdlog::info("Revive: {} rejoined fallen", player->GetUsername());
            }
        }
        state.Data.Dead = IsFallen(player, request.Epoch);
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
    // The downed player sees who is reviving them and the progress (their overlay meter): every accepted hold is
    // forwarded to the target, and so is its end.
    const auto notifyTarget = [&](uint32_t aTarget, ReviveAction aAction)
    {
        const auto state = m_states.find(aTarget);
        auto* target = m_world.GetPlayerManager().GetById(aTarget);
        if (state == m_states.end() || !target)
            return;
        NotifyRevive notice;
        static_cast<ReviveData&>(notice) = state->second.Data;
        notice.Action = aAction;
        notice.ReviverId = id;
        target->Send(notice);
        static uint32_t s_logs{};
        if (s_logs++ < 200)
            spdlog::info("Revive: {} notice to player {} from reviver {}", aAction == ReviveAction::Hold ? "hold" : "cancel",
                aTarget, id);
    };
    if (request.Action == ReviveAction::Cancel)
    {
        if (const auto held = m_holds.find(id); held != m_holds.end())
            notifyTarget(held->second.Target, ReviveAction::Cancel);
        m_holds.erase(id);
        return;
    }
    if (request.Action == ReviveAction::Raise)
    {
        OnRaise(player, request, now);
        return;
    }
    if (request.Action != ReviveAction::Hold && request.Action != ReviveAction::Finish)
        return;
    Hold candidate{request.PlayerId, request.Revision, now, now};
    if (!Eligible(id, candidate, now))
    {
        if (const auto held = m_holds.find(id); held != m_holds.end())
            notifyTarget(held->second.Target, ReviveAction::Cancel);
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
        notifyTarget(candidate.Target, ReviveAction::Hold);
        return;
    }
    const uint64_t required = CombatAround(id, candidate.Target) ? 6000 : 3000;
    if (it == m_holds.end() || it->second.Target != candidate.Target ||
        it->second.Revision != candidate.Revision || now - it->second.Started < required ||
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
    CheckWipes(now);
    std::erase_if(m_fallen, [&](const auto& entry) {
        if (m_world.GetPlayerManager().GetById(entry.first))
            return false;
        m_departed[{entry.second.PartyId, entry.second.Username}] = entry.second.Epoch;
        return true;
    });
}
