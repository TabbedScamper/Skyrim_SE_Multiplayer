#pragma once

#include <array>

struct World;
struct UpdateEvent;
struct ActorAddedEvent;
struct ActorRemovedEvent;

// Read-only evidence for natives lost across a cell transition. Native reads
// run at Main::Update's final call; ECS observations stay on the UpdateEvent worker.
struct OrphanTrace
{
    OrphanTrace(World&, entt::dispatcher&) noexcept;
    TP_NOCOPYMOVE(OrphanTrace);

private:
    struct Entry
    {
        uint32_t Form{};
        uint64_t RemovedAt{};
        unsigned Sample{};
        bool Rediscovered{};
    };
    void OnRemoved(const ActorRemovedEvent&) noexcept;
    void OnAdded(const ActorAddedEvent&) noexcept;
    void OnUpdate(const UpdateEvent&) noexcept;
    void Log(const Entry&, const char*) const noexcept;

    World& m_world;
    std::array<Entry, 64> m_entries{};
    uint64_t m_nextSample{};
    entt::scoped_connection m_removed, m_added, m_update;
};
