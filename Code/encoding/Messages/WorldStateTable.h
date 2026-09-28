#pragma once
#include <Messages/WorldState.h>
#include <algorithm>
#include <map>
#include <vector>

// One party owns one table. Durable values cannot expire while a late joiner
// may need them; storage is bounded by placed references times property kinds.
class WorldStateTable
{
public:
    using Key = std::pair<uint64_t, WorldStateKind>;
    static Key MakeKey(const WorldState& state) { return {state.Reference.LogFormat(), state.Kind}; }
    // Activation (1) and attach baseline (3) only update the durable cache.
    // Only a script command (0) may drive an already connected door.
    static bool ShouldDeliverLive(const WorldState& state)
    {
        return state.Kind != WorldStateKind::Open || state.Scalar == 0;
    }
    void SetAuthority(uint64_t epoch, uint32_t leader)
    {
        if (m_epoch != epoch) { m_latest.clear(); m_cells.clear(); m_sequences.clear(); }
        if (m_epoch != epoch || m_leader != leader) { m_source.clear(); m_offsets = m_sequences; }
        m_epoch = epoch; m_leader = leader;
    }
    bool Accept(uint32_t sender, WorldState& state)
    {
        if (sender != m_leader || state.Epoch != m_epoch || !state.Valid()) return false;
        const auto key = MakeKey(state);
        if (state.Sequence <= m_source[key]) return false;
        const auto ref = key.first;
        if (state.Sequence > UINT64_MAX - m_offsets[ref]) return false;
        m_source[key] = state.Sequence;
        state.Sequence += m_offsets[ref];
        m_sequences[ref] = std::max(m_sequences[ref], state.Sequence);
        if (auto old = m_latest.find(key); old != m_latest.end())
        {
            auto cell = m_cells.find(old->second.Cell.LogFormat());
            if (cell != m_cells.end())
            {
                cell->second.erase(key);
                if (cell->second.empty()) m_cells.erase(cell);
            }
        }
        m_latest.insert_or_assign(key, state);
        m_cells[state.Cell.LogFormat()].insert_or_assign(key, state);
        return true;
    }
    std::vector<WorldState> Snapshot(const GameId& cell) const
    {
        std::vector<WorldState> result;
        const auto found = m_cells.find(cell.LogFormat());
        if (found == m_cells.end()) return result;
        for (const auto& [key, state] : found->second)
            if (state.Kind != WorldStateKind::AnimationEvent) result.push_back(state);
        std::sort(result.begin(), result.end(), [](const auto& a, const auto& b) {
            if (a.Reference != b.Reference) return a.Reference.LogFormat() < b.Reference.LogFormat();
            return a.Sequence < b.Sequence;
        });
        return result;
    }
    // Cursor is a completed reference, so a page cannot split its property
    // sequence. At most eight kinds per reference, sorted only within that page.
    std::vector<WorldState> SnapshotPage(const GameId& cell, uint64_t& cursor, bool& done,
                                       size_t references = 4) const
    {
        std::vector<WorldState> result;
        const auto found = m_cells.find(cell.LogFormat());
        done = found == m_cells.end();
        if (done) return result;
        auto it = found->second.upper_bound({cursor, WorldStateKind::Count});
        while (references && it != found->second.end())
        {
            cursor = it->first.first;
            const auto begin = result.size();
            do
            {
                if (it->second.Kind != WorldStateKind::AnimationEvent) result.push_back(it->second);
                ++it;
            } while (it != found->second.end() && it->first.first == cursor);
            std::sort(result.begin() + begin, result.end(), [](const auto& a, const auto& b) { return a.Sequence < b.Sequence; });
            --references;
        }
        done = it == found->second.end();
        return result;
    }
private:
    uint64_t m_epoch{};
    uint32_t m_leader{};
    std::map<Key, uint64_t> m_source;
    std::map<uint64_t, uint64_t> m_sequences, m_offsets;
    std::map<Key, WorldState> m_latest;
    std::map<uint64_t, std::map<Key, WorldState>> m_cells;
};
