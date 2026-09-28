#pragma once

#include <array>

struct World;
struct UpdateEvent;

// Read-only, explicitly armed evidence for missing player animation offsets.
struct PlayerStateTrace
{
    PlayerStateTrace(World&, entt::dispatcher&) noexcept;
    ~PlayerStateTrace();
    TP_NOCOPYMOVE(PlayerStateTrace);

private:
    struct Snapshot
    {
        uint32_t Actor{}, Life{}, Index{}, Count{};
        uintptr_t Manager{}, Node{};
        std::array<uintptr_t, 4> Graphs{}, Behaviors{};
        bool operator==(const Snapshot&) const = default;
    };
    void OnUpdate(const UpdateEvent&) noexcept;
    void OnAction(const struct ActionEvent&) noexcept;
    Snapshot ReadSnapshot(struct Actor*) const noexcept;

    World& m_world;
    entt::scoped_connection m_updateConnection;
    entt::scoped_connection m_actionConnection;
    std::array<Snapshot, 32> m_previous{};
    uint64_t m_nextSample{};
    uint32_t m_records{}, m_snapshots{};
    bool m_armed{}, m_creator{};
};
