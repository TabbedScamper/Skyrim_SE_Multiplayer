#pragma once
#include <Messages/WorldState.h>
#include <deque>
#include <map>
#include <optional>
#include <set>
#include <algorithm>

// Reliable, ordered WorldState transport; only the head of each reference's
// queue is runnable. A failed Open cannot be overtaken by Reset or another Open.
class WorldAnimationReplay
{
    struct Reference
    {
        std::deque<WorldState> Events;
        std::optional<WorldState> Snapshot, Restore;
        uint64_t Received{}, Applied{}, Due{};
        uint64_t Gap{};
        uint32_t Cell{};
        size_t Cursor{};
    };
public:
    static constexpr size_t MaximumHistory = 256;
    static bool Handles(WorldStateKind kind)
    {
        return kind == WorldStateKind::AnimationEvent || kind == WorldStateKind::AnimationSnapshot;
    }
    bool Receive(const WorldState& state)
    {
        const auto id = state.Reference.LogFormat();
        auto& ref = m_refs[id];
        ref.Cell = state.Cell.BaseId;
        if (state.Kind == WorldStateKind::AnimationEvent)
        {
            if (state.Sequence <= ref.Received || (ref.Snapshot && state.Sequence <= ref.Snapshot->Sequence)) return false;
            ref.Received = state.Sequence;
            if (ref.Gap || ref.Events.size() == MaximumHistory)
            {
                // An unavailable graph cannot retain an unlimited command log.
                // Fail visibly and wait for a checkpoint covering the gap. Never
                // run a suffix of the lost input history as if it were complete.
                ref.Gap = state.Sequence;
                ref.Events.clear(); ref.Cursor = 0; ref.Restore.reset();
                m_ready.erase({ref.Due, id});
                return false;
            }
            ref.Events.push_back(state);
            Schedule(id, ref.Due);
            return true;
        }
        // Gap is set only by an event admitted above Snapshot->Sequence. Every
        // subsequent gap event has the same guard. Thus a covering checkpoint
        // cannot be rejected by this monotonic snapshot check.
        if (ref.Snapshot && state.Sequence < ref.Snapshot->Sequence) return false;
        if (ref.Gap && state.Sequence < ref.Gap) return false;
        ref.Snapshot = state;
        if (ref.Gap || state.Scalar == 2) { ref.Gap = 0; Attach(id); }
        // Applied history is needed only until a newer checkpoint covers it.
        Prune(ref);
        Schedule(id, ref.Due);
        return true;
    }
    void Attach(uint64_t id)
    {
        auto it = m_refs.find(id);
        if (it == m_refs.end()) return;
        auto& ref = it->second;
        if (ref.Gap) return;
        ref.Applied = 0;
        ref.Cursor = 0;
        if (ref.Snapshot)
        {
            ref.Restore = *ref.Snapshot;
            ref.Restore->Scalar = 2;
        }
        Schedule(id, 0);
    }
    std::vector<WorldState> Take(size_t maximum, uint64_t now)
    {
        std::vector<WorldState> result;
        while (result.size() < maximum && !m_ready.empty() && m_ready.begin()->first <= now)
        {
            const auto id = m_ready.begin()->second;
            m_ready.erase(m_ready.begin());
            auto& ref = m_refs.at(id);
            if (ref.Restore) result.push_back(*ref.Restore);
            else
            {
                const WorldState* next = ref.Snapshot && ref.Snapshot->Sequence > ref.Applied ? &*ref.Snapshot : nullptr;
                if (ref.Cursor < ref.Events.size() && (!next || ref.Events[ref.Cursor].Sequence < next->Sequence)) next = &ref.Events[ref.Cursor];
                if (next) result.push_back(*next);
            }
        }
        return result;
    }
    void Applied(const WorldState& state)
    {
        auto& ref = m_refs.at(state.Reference.LogFormat());
        ref.Applied = state.Sequence;
        if (state.Kind == WorldStateKind::AnimationSnapshot) ref.Restore.reset();
        ref.Cursor = static_cast<size_t>(std::upper_bound(ref.Events.begin(), ref.Events.end(), state.Sequence,
            [](uint64_t sequence, const WorldState& event) { return sequence < event.Sequence; }) - ref.Events.begin());
        Prune(ref);
        Schedule(state.Reference.LogFormat(), 0);
    }
    bool Failed(const WorldState& state, uint64_t now)
    {
        auto& ref = m_refs.at(state.Reference.LogFormat());
        const bool first = ref.Due == 0;
        Schedule(state.Reference.LogFormat(), now + 1000);
        return first;
    }
    size_t Pending() const { return m_ready.size(); }
    bool NeedsCheckpoint(uint64_t id) const
    {
        const auto it = m_refs.find(id);
        return it != m_refs.end() && it->second.Gap != 0;
    }
    size_t HistorySize(uint64_t id) const
    {
        const auto it = m_refs.find(id);
        return it == m_refs.end() ? 0 : it->second.Events.size();
    }
    void EvictOutside(const std::set<uint32_t>& scopes, size_t budget = 8)
    {
        auto it = m_refs.upper_bound(m_evictCursor);
        for (size_t remaining = (std::min)(budget, m_refs.size()); remaining; --remaining)
        {
            if (it == m_refs.end()) it = m_refs.begin();
            m_evictCursor = it->first;
            if (!scopes.count(it->second.Cell))
            {
                m_ready.erase({it->second.Due, it->first}); it = m_refs.erase(it);
            }
            else ++it;
        }
    }
    void Clear() { m_refs.clear(); m_ready.clear(); m_evictCursor = 0; }
private:
    static void Prune(Reference& ref)
    {
        // Cleanup is bounded too; the cursor avoids rescanning retained history.
        for (unsigned budget = 0; budget < 64 && ref.Snapshot && !ref.Events.empty() &&
             ref.Events.front().Sequence <= ref.Applied && ref.Events.front().Sequence <= ref.Snapshot->Sequence; ++budget)
        {
            ref.Events.pop_front();
            if (ref.Cursor) --ref.Cursor;
        }
    }
    void Schedule(uint64_t id, uint64_t due)
    {
        auto& ref = m_refs.at(id);
        m_ready.erase({ref.Due, id});
        ref.Due = due;
        if (ref.Gap) return;
        if (ref.Restore || (ref.Snapshot && ref.Snapshot->Sequence > ref.Applied) ||
            (!ref.Events.empty() && ref.Events.back().Sequence > ref.Applied)) m_ready.emplace(due, id);
    }
    std::map<uint64_t, Reference> m_refs;
    std::set<std::pair<uint64_t, uint64_t>> m_ready;
    uint64_t m_evictCursor{};
};
