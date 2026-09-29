#pragma once

#include <Events/PacketEvent.h>
#include <Structs/GameId.h>
#include <unordered_map>
#include <Services/PartyService.h>

struct World;
struct PlayerLeaveCellEvent;
struct ActivateRequest;
struct LockChangeRequest;
struct AssignObjectsRequest;
struct ScriptAnimationRequest;
struct PhysicsReferencesMoveRequest;
struct PhysicsLeaseRequest;


/**
 * @brief Manages (interactive) objects and relays interactions with said objects.
 */
class ObjectService
{
public:
    ObjectService(World& aWorld, entt::dispatcher& aDispatcher);

private:
    void OnPlayerLeaveCellEvent(const PlayerLeaveCellEvent& acEvent) noexcept;
    void OnAssignObjectsRequest(const PacketEvent<AssignObjectsRequest>&) noexcept;
    void OnActivate(const PacketEvent<ActivateRequest>&) const noexcept;
    void OnLockChange(const PacketEvent<LockChangeRequest>&) const noexcept;
    void OnScriptAnimationRequest(const PacketEvent<ScriptAnimationRequest>&) noexcept;
    void OnPhysicsReferencesMove(const PacketEvent<PhysicsReferencesMoveRequest>&) noexcept;
    void OnPhysicsLease(const PacketEvent<PhysicsLeaseRequest>&) noexcept;
    void BroadcastLease(const PartyService::Party& acParty, const GameId& acId, uint32_t aHolder) const noexcept;

    World& m_world;
    // Loose world objects a player is carrying (hold-to-grab): that player streams them, not the leader. Per party;
    // a lease expires 10 s after its holder's last update (a holder that vanished cannot keep an object).
    struct Lease
    {
        uint32_t Holder{};
        uint64_t Epoch{};
        uint64_t LastUpdate{};
    };
    std::unordered_map<uint32_t, std::unordered_map<GameId, Lease>> m_leases;

    entt::scoped_connection m_leaveCellConnection;
    entt::scoped_connection m_assignObjectConnection;
    entt::scoped_connection m_activateConnection;
    entt::scoped_connection m_lockChangeConnection;
    entt::scoped_connection m_scriptAnimationConnection;
    entt::scoped_connection m_physicsMoveConnection;
    entt::scoped_connection m_physicsLeaseConnection;
};
