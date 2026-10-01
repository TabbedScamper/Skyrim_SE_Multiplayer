#include <Services/DropInService.h>
#include <World.h>
#include <GameServer.h>
#include <Game/Player.h>
#include <Services/PartyService.h>
#include <Services/CharacterService.h>
#include <Events/UpdateEvent.h>

namespace
{
// One join takes a save, a transfer of a few MB and a load: two minutes is generous, and a stuck attempt never blocks
// the next one.
constexpr uint64_t kAttemptMs = 120000;

uint64_t NowMs() noexcept
{
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}
} // namespace

DropInService::DropInService(World& world, entt::dispatcher& dispatcher)
    : m_world(world)
    , m_request(dispatcher.sink<PacketEvent<RequestDropIn>>().connect<&DropInService::OnRequest>(this))
    , m_update(dispatcher.sink<UpdateEvent>().connect<&DropInService::OnUpdate>(this))
{
}

void DropInService::Send(uint32_t aPlayerId, const DropInData& acData) const noexcept
{
    NotifyDropIn message;
    static_cast<DropInData&>(message) = acData;
    if (auto* player = m_world.GetPlayerManager().GetById(aPlayerId))
        player->Send(message);
}

void DropInService::Abort(uint64_t aAttempt, const char* acReason) noexcept
{
    const auto it = m_attempts.find(aAttempt);
    if (it == m_attempts.end())
        return;
    DropInData abort;
    abort.Attempt = aAttempt;
    abort.Joiner = it->second.Joiner;
    abort.Op = DropInOp::Abort;
    abort.Text = acReason;
    Send(it->second.Joiner, abort);
    Send(it->second.Leader, abort);
    spdlog::warn("Drop-in {:X}: player {} aborted ({})", aAttempt, it->second.Joiner, acReason);
    m_attempts.erase(it);
}

void DropInService::OnRequest(const PacketEvent<RequestDropIn>& acEvent) noexcept
{
    auto* pSender = acEvent.pPlayer;
    const auto& data = acEvent.Packet;
    if (!pSender || !data.IsValid() || !data.Valid())
        return;
    auto& parties = m_world.GetPartyService();
    auto* pParty = parties.GetPlayerParty(pSender);
    const auto sender = pSender->GetId();

    if (data.Op == DropInOp::Join)
    {
        // A member of a running session (state 3) who is not its leader, one attempt at a time per joiner.
        if (!pParty || pParty->SessionState != 3 || sender == pParty->LeaderPlayerId || !pSender->GetParty().JoinedPartyId)
        {
            DropInData refuse = data;
            refuse.Op = DropInOp::Abort;
            refuse.Text = !pParty ? "not in a party" : pParty->SessionState != 3 ? "the session is not running" : "the host cannot join";
            Send(sender, refuse);
            return;
        }
        std::erase_if(m_attempts, [sender](const auto& aEntry) { return aEntry.second.Joiner == sender; });
        // The leader streams one save at a time: a second joiner waits rather than silently stranding the first.
        const auto leader = pParty->LeaderPlayerId;
        const bool leaderBusy = std::any_of(m_attempts.begin(), m_attempts.end(),
            [leader](const auto& aEntry) { return aEntry.second.Leader == leader; });
        if (leaderBusy || m_attempts.contains(data.Attempt))
        {
            DropInData refuse = data;
            refuse.Op = DropInOp::Abort;
            refuse.Text = leaderBusy ? "another player is joining right now; try again in a moment" : "try again";
            Send(sender, refuse);
            return;
        }
        m_attempts[data.Attempt] = {sender, pParty->LeaderPlayerId, *pSender->GetParty().JoinedPartyId, NowMs() + kAttemptMs};
        DropInData capture = data;
        capture.Op = DropInOp::Capture;
        capture.Joiner = sender;
        Send(pParty->LeaderPlayerId, capture);
        spdlog::info("Drop-in {:X}: player {} joins as '{}', asking leader {} for its save", data.Attempt, sender,
            data.Text.c_str(), pParty->LeaderPlayerId);
        return;
    }

    const auto it = m_attempts.find(data.Attempt);
    if (it == m_attempts.end())
        return;
    auto& attempt = it->second;
    switch (data.Op)
    {
    case DropInOp::Chunk:
    case DropInOp::Done:
        // Only the leader streams, only to its joiner.
        if (sender != attempt.Leader)
            return;
        if (data.Op == DropInOp::Chunk)
            attempt.Received += data.Bytes.size();
        else
            spdlog::info("Drop-in {:X}: leader sent {} bytes ({}) to player {}", data.Attempt, data.Total, data.Text.c_str(),
                attempt.Joiner);
        {
            DropInData relay = data;
            relay.Joiner = attempt.Joiner;
            Send(attempt.Joiner, relay);
        }
        return;
    case DropInOp::Loaded:
    {
        if (sender != attempt.Joiner)
            return;
        // Everything around the joiner as it is now (the other players and the actors in range), then admitted.
        m_world.GetCharacterService().ReplayToPlayer(pSender);
        DropInData admit = data;
        admit.Op = DropInOp::Admit;
        Send(attempt.Joiner, admit);
        spdlog::info("Drop-in {:X}: player {} loaded the leader's world and is admitted", data.Attempt, attempt.Joiner);
        m_attempts.erase(it);
        return;
    }
    case DropInOp::Abort:
        if (sender == attempt.Joiner || sender == attempt.Leader)
            Abort(data.Attempt, data.Text.empty() ? "cancelled" : data.Text.c_str());
        return;
    default:
        return;
    }
}

void DropInService::OnUpdate(const UpdateEvent&) noexcept
{
    if (m_attempts.empty())
        return;
    const auto now = NowMs();
    std::vector<std::pair<uint64_t, const char*>> stale;
    for (const auto& [id, attempt] : m_attempts)
    {
        if (now > attempt.Deadline)
            stale.emplace_back(id, "timed out");
        else if (!m_world.GetPlayerManager().GetById(attempt.Joiner) || !m_world.GetPlayerManager().GetById(attempt.Leader))
            stale.emplace_back(id, "a player left");
    }
    for (const auto& [id, reason] : stale)
        Abort(id, reason);
}
