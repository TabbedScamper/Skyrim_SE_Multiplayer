#pragma once
#include <Events/PacketEvent.h>
#include <Messages/WorldStateTable.h>
#include <map>

struct World;
struct RequestWorldState;
struct RequestWorldStateCell;
struct UpdateEvent;

struct WorldStateService
{
    WorldStateService(World&, entt::dispatcher&) noexcept;
    TP_NOCOPYMOVE(WorldStateService);

private:
    void OnState(const PacketEvent<RequestWorldState>&) noexcept;
    void OnCell(const PacketEvent<RequestWorldStateCell>&) noexcept;
    void OnUpdate(const UpdateEvent&) noexcept;
    World& m_world;
    std::map<uint32_t, WorldStateTable> m_parties;
    struct CellReplay { uint32_t Party{}, Leader{}; uint64_t Epoch{}, Cursor{}; GameId Cell{}; };
    using ReplayKey = std::pair<uint32_t, uint64_t>;
    std::map<ReplayKey, CellReplay> m_replays;
    ReplayKey m_replayCursor{};
    uint32_t m_partyCursor{};
    entt::scoped_connection m_stateConnection;
    entt::scoped_connection m_cellConnection;
    entt::scoped_connection m_updateConnection;
};
