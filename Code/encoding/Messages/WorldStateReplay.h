#pragma once
#include <Messages/WorldStateTable.h>
#include <Messages/WorldAnimationReplay.h>
#include <set>

// Pure replay mailbox policy. The caller performs engine work outside this object
// and acknowledges only a successful Apply. No pointer identifies a 3D generation.
class WorldStateReplay
{
public:
    // Retain a burst without discarding script inputs or closing the party.
    // Drain bounds each batch, not total retained memory. A failed batch must
    // precede publications queued while the transport was sending it.
    static void PrependFailed(std::deque<WorldState>& queue, std::deque<WorldState>& failed)
    {
        while (!failed.empty())
        {
            queue.push_front(std::move(failed.back())); failed.pop_back();
        }
    }
    // Bound copying and dispatch as well as native Apply. A single valid state
    // always fits the byte budget; retain the unsent tail in its original order.
    static void Drain(std::deque<WorldState>& source, std::deque<WorldState>& target,
                      size_t maximum = 64, size_t bytes = 128 * 1024)
    {
        while (maximum && !source.empty())
        {
            const auto& state = source.front();
            const auto size = sizeof(WorldState) + state.Animation.size() + state.AnimationData.size();
            if (size > bytes) break;
            bytes -= size; --maximum;
            target.push_back(std::move(source.front())); source.pop_front();
        }
    }
    bool Receive(const WorldState& state)
    {
        if (WorldAnimationReplay::Handles(state.Kind)) return m_animation.Receive(state);
        // Cache observations are not follower commands. Reject them before
        // advancing the revision or scheduling a permanently refused Apply.
        if (!WorldStateTable::ShouldDeliverLive(state) && state.Scalar != 2) return false;
        const auto key = WorldStateTable::MakeKey(state);
        const auto old = m_latest.find(key);
        if (old != m_latest.end() && old->second.Sequence >= state.Sequence) return false;
        m_latest.insert_or_assign(key, state);
        m_pending.insert_or_assign(key, state);
        Schedule(key, 0);
        return true;
    }
    void Attach(uint64_t reference)
    {
        m_animation.Attach(reference);
        for (auto it = m_latest.lower_bound({reference, WorldStateKind::Disabled});
             it != m_latest.end() && it->first.first == reference; ++it)
        {
            auto state = it->second;
            if (state.Kind == WorldStateKind::Open) state.Scalar = 2;
            m_pending.insert_or_assign(it->first, std::move(state));
            Schedule(it->first, 0);
        }
    }
    std::vector<WorldState> Take(size_t maximum, uint64_t now = 0)
    {
        // At most eight animation graph operations; leave the rest of the
        // budget for the existing rubble/door lane.
        auto states = m_animation.Take((std::min)(size_t{8}, (maximum + 1) / 2), now);
        while (states.size() < maximum && !m_ready.empty() && m_ready.begin()->first <= now)
        {
            const auto reference = m_ready.begin()->second.first;
            std::vector<WorldState> referenceStates;
            for (auto it = m_due.lower_bound({reference, WorldStateKind::Disabled});
                 it != m_due.end() && it->first.first == reference; ++it)
                if (it->second <= now) referenceStates.push_back(m_pending.at(it->first));
            std::sort(referenceStates.begin(), referenceStates.end(), [](const auto& a, const auto& b) { return a.Sequence < b.Sequence; });
            for (const auto& state : referenceStates)
            {
                if (states.size() == maximum) break;
                states.push_back(state);
                Unschedule(WorldStateTable::MakeKey(state));
            }
        }
        return states;
    }
    // Every transient failure gets a timed wake, including Set3D notifications
    // that precede actual attachment. No history scan and no low-ID starvation.
    bool Failed(const WorldState& state, uint64_t now, bool unsupported = false)
    {
        if (WorldAnimationReplay::Handles(state.Kind)) return m_animation.Failed(state, now);
        const auto key = WorldStateTable::MakeKey(state);
        const auto pending = m_pending.find(key);
        if (pending == m_pending.end() || pending->second.Sequence != state.Sequence) return false;
        const auto reported = m_reported.find(key);
        const bool first = reported == m_reported.end() || reported->second != state.Sequence;
        m_reported.insert_or_assign(key, state.Sequence);
        if (!unsupported) Schedule(key, now + 1000);
        return first;
    }
    void Applied(const WorldState& state)
    {
        if (WorldAnimationReplay::Handles(state.Kind)) { m_animation.Applied(state); return; }
        const auto it = m_pending.find(WorldStateTable::MakeKey(state));
        if (it != m_pending.end() && it->second.Sequence == state.Sequence)
        {
            Unschedule(it->first);
            m_pending.erase(it);
        }
    }
    void EvictOutside(const std::set<uint32_t>& scopes)
    {
        for (auto it = m_latest.begin(); it != m_latest.end();)
            if (!scopes.count(it->second.Cell.BaseId) && !m_pending.count(it->first))
            {
                m_reported.erase(it->first);
                it = m_latest.erase(it);
            }
            else ++it;
    }
    size_t Pending() const { return m_pending.size() + m_animation.Pending(); }
    bool AnimationNeedsCheckpoint(uint64_t id) const { return m_animation.NeedsCheckpoint(id); }
    void EvictAnimationsOutside(const std::set<uint32_t>& scopes) { m_animation.EvictOutside(scopes); }
    void Clear() { m_latest.clear(); m_pending.clear(); m_ready.clear(); m_due.clear(); m_reported.clear(); m_animation.Clear(); }
private:
    WorldAnimationReplay m_animation;
    void Unschedule(const WorldStateTable::Key& key)
    {
        if (const auto it = m_due.find(key); it != m_due.end())
        {
            m_ready.erase({it->second, key});
            m_due.erase(it);
        }
    }
    void Schedule(const WorldStateTable::Key& key, uint64_t when)
    {
        Unschedule(key);
        m_due.emplace(key, when);
        m_ready.emplace(when, key);
    }
    std::map<WorldStateTable::Key, WorldState> m_latest, m_pending;
    std::map<WorldStateTable::Key, uint64_t> m_due;
    std::set<std::pair<uint64_t, WorldStateTable::Key>> m_ready;
    std::map<WorldStateTable::Key, uint64_t> m_reported;
};
