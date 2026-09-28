#include <Services/HarnessService.h>
#include <World.h>
#include <GameServer.h>
#include <Game/Player.h>
#include <Services/PartyService.h>
#include <Events/UpdateEvent.h>
#include <console/Setting.h>

namespace
{
Console::Setting<bool> sHarness{"Harness:bEnabled", "Allow test-build party scenario execution", false, Console::SettingsFlags::kLocked};
bool Enabled()
{
#ifdef SEAMLESS_HARNESS
    return sHarness;
#else
    return false;
#endif
}
std::set<uint32_t> Members(const PartyService::Party& party)
{
    std::set<uint32_t> result;
    for (auto* member : party.Members) result.insert(member->GetId());
    return result;
}
bool Current(const HarnessBarrier& barrier, const PartyService::Party& party)
{
    if (barrier.Aborted || barrier.Epoch != party.StartEpoch || barrier.Leader != party.LeaderPlayerId ||
        barrier.Members.size() != party.Members.size() || party.Members.size() > 16) return false;
    // PartyService owns unique membership. Verify it without rebuilding an
    // allocating set on every update/ack; at most 16 logarithmic lookups.
    for (auto* member : party.Members)
        if (!member || !barrier.Members.count(member->GetId())) return false;
    return true;
}
}
HarnessService::HarnessService(World& world, entt::dispatcher& dispatcher) : m_world(world)
    , m_request(dispatcher.sink<PacketEvent<RequestHarness>>().connect<&HarnessService::OnRequest>(this))
    , m_update(dispatcher.sink<UpdateEvent>().connect<&HarnessService::OnUpdate>(this))
{
    if (Enabled()) spdlog::info("Harness server buildTag={}", BUILD_BRANCH "@" BUILD_COMMIT);
}

void HarnessService::Broadcast(const Run& run, HarnessOp op, const String& payload) const
{
    NotifyHarness message;
    static_cast<HarnessData&>(message) = run.Step;
    message.Op = op; message.Sender = run.Barrier.Leader;
    if (op != HarnessOp::Step) message.Payload = payload;
    for (auto id : run.Barrier.Members)
        if (auto* player = m_world.GetPlayerManager().GetById(id)) player->Send(message);
}
void HarnessService::OnRequest(const PacketEvent<RequestHarness>& event) noexcept
{
    if (!Enabled() || !event.pPlayer || !event.Packet.IsValid() || !event.Packet.Valid()) return;
    auto* party = m_world.GetPartyService().GetPlayerParty(event.pPlayer);
    const auto& data = event.Packet;
    if (!party || !event.pPlayer->GetParty().JoinedPartyId || party->StartEpoch != data.Epoch) return;
    if (party->Members.size() > 16) return;
    const auto sender = event.pPlayer->GetId();
    if (std::find(party->Members.begin(), party->Members.end(), event.pPlayer) == party->Members.end()) return;
    const auto partyId = *event.pPlayer->GetParty().JoinedPartyId;
    if (!m_runs.count(partyId))
    {
        if (data.Op != HarnessOp::Step || sender != party->LeaderPlayerId || data.Sequence != 1) return;
        // Retain completed run IDs for replay rejection, with a fixed bound on
        // both lifetime storage and the per-update watchdog traversal.
        if (m_runs.size() >= 64)
        {
            NotifyHarness reject;
            static_cast<HarnessData&>(reject) = data;
            reject.Op = HarnessOp::Abort; reject.Sender = sender;
            reject.Payload = "server_harness_party_limit_64";
            event.pPlayer->Send(reject);
            return;
        }
    }
    auto& run = m_runs[partyId];
    if (data.Op == HarnessOp::Step)
    {
        if (sender != party->LeaderPlayerId) return;
        if (data.Run != run.Barrier.Run)
        {
            if (data.Sequence != 1 || data.Run <= run.Barrier.Run ||
                (!run.Barrier.Aborted && run.Barrier.Sequence && !run.Barrier.Ready())) return;
            if (!run.Barrier.Begin(data.Epoch, data.Run, sender, Members(*party))) return;
            run.Finished = false;
        }
        if (run.Finished) return;
        if (!Current(run.Barrier, *party) || !run.Barrier.Step(data, sender)) return;
        run.Step = data; run.Step.Sender = sender;
        // Client deadlines include a bounded load allowance. Server is the
        // final transport watchdog, including preparation and execution.
        // Max party scale3: 2*(590s*3+120s+300s) +300s load =4680s.
        run.Deadline = GameServer::Get()->GetTick() + 4800000;
        Broadcast(run, HarnessOp::Step);
        spdlog::info("Harness: run={} step={} members={}", data.Run, data.Sequence, party->Members.size());
    }
    else if (Current(run.Barrier, *party) &&
        data.Run == run.Barrier.Run && data.Sequence == run.Barrier.Sequence)
    {
        if (data.Op == HarnessOp::Abort)
        {
            run.Barrier.Aborted = true;
            Broadcast(run, HarnessOp::Abort, data.Payload);
        }
        else if (data.Op == HarnessOp::Finish && sender == run.Barrier.Leader && run.Barrier.Ready())
            run.Finished = true;
        else if (data.Op == HarnessOp::Prepared && run.Barrier.Prepare(data, sender))
        {
            spdlog::info("Harness: prepared run={} step={} player={} state={}", data.Run, data.Sequence, sender, data.Payload.c_str());
            if (run.Barrier.CanExecute()) Broadcast(run, HarnessOp::Execute);
        }
        else if (data.Op == HarnessOp::Done && run.Barrier.Ack(data, sender) && run.Barrier.Ready())
        {
            Broadcast(run, HarnessOp::Barrier);
            run.Deadline = GameServer::Get()->GetTick() + 600000;
        }
    }
}
void HarnessService::OnUpdate(const UpdateEvent&) noexcept
{
    if (!Enabled()) return;
    for (auto& [id, run] : m_runs)
    {
        if (!run.Barrier.Sequence || run.Barrier.Aborted || run.Finished) continue;
        const auto* party = m_world.GetPartyService().GetById(id);
        if (!party || !Current(run.Barrier, *party) ||
            GameServer::Get()->GetTick() > run.Deadline)
        {
            run.Barrier.Aborted = true;
            Broadcast(run, HarnessOp::Abort, "party changed or step deadline expired");
        }
    }
}
