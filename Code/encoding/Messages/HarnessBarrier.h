#pragma once
#include <Messages/Harness.h>
#include <set>

// Pure policy, shared with protocol tests. Membership never shrinks to turn a
// disconnected participant into a successful run.
struct HarnessBarrier
{
    uint64_t Epoch{}, Run{};
    uint32_t Leader{}, Sequence{};
    std::set<uint32_t> Members, Prepared, Completed;
    bool Aborted{};
    bool Current(uint64_t epoch, uint32_t leader, const std::set<uint32_t>& members) const
    { return !Aborted && Epoch == epoch && Leader == leader && Members == members; }
    bool Begin(uint64_t epoch, uint64_t run, uint32_t leader, const std::set<uint32_t>& members)
    {
        if (!epoch || !run || members.size() < 2 || !members.count(leader)) return false;
        Epoch = epoch; Run = run; Leader = leader; Members = members;
        Sequence = 0; Prepared.clear(); Completed.clear(); Aborted = false; return true;
    }
    bool Step(const HarnessData& data, uint32_t sender)
    {
        if (Aborted || sender != Leader || data.Epoch != Epoch || data.Run != Run ||
            data.Sequence != Sequence + 1 || (Sequence && Completed != Members)) return false;
        Sequence = data.Sequence; Prepared.clear(); Completed.clear(); return true;
    }
    bool Prepare(const HarnessData& data, uint32_t sender)
    {
        if (Aborted || data.Epoch != Epoch || data.Run != Run || data.Sequence != Sequence ||
            !Members.count(sender)) return false;
        return Prepared.insert(sender).second;
    }
    bool CanExecute() const { return !Aborted && Sequence && Prepared == Members; }
    bool Ack(const HarnessData& data, uint32_t sender)
    {
        if (!CanExecute() || data.Epoch != Epoch || data.Run != Run || data.Sequence != Sequence ||
            !Members.count(sender)) return false;
        return Completed.insert(sender).second;
    }
    bool Ready() const { return !Aborted && Sequence && Completed == Members; }
};
