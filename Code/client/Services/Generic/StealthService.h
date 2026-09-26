#pragma once
#include <Combat/PlayerCombat.h>
#include <unordered_map>

struct World;
struct TransportService;
struct UpdateEvent;
struct DisconnectedEvent;
struct NotifyPlayerCombatState;

struct StealthService
{
    StealthService(World& aWorld, entt::dispatcher& aDispatcher, TransportService& aTransport);
    ~StealthService();
    TP_NOCOPYMOVE(StealthService);

private:
    void OnUpdate(const UpdateEvent&);
    void OnDisconnected(const DisconnectedEvent&);
    void OnNotify(const NotifyPlayerCombatState&);
    void OnImpact(const PlayerCombat::Impact&);
    void Reset();
    bool Active() const;
    void Send(PlayerCombatState aState);
    void Apply(Actor* apActor, const PlayerCombatState& aState);

    struct Track
    {
        PlayerCombatState State;
        uint64_t ReceivedAt{};
        uint32_t FormId{};
        uint32_t Handle{};
        bool WasTeammate{};
        uint64_t NextDiagnostic{};
    };
    World& m_world;
    TransportService& m_transport;
    std::unordered_map<uint32_t, Track> m_tracks;
    struct Received
    {
        uint32_t OwnershipEpoch{};
        uint64_t Sequence{};
    };
    std::unordered_map<uint64_t, Received> m_received;
    uint64_t m_epoch{};
    uint32_t m_leader{};
    uint64_t m_sequence{};
    uint64_t m_nextUpdate{};
    entt::scoped_connection m_update;
    entt::scoped_connection m_disconnect;
    entt::scoped_connection m_notify;
    entt::scoped_connection m_impact;
};
