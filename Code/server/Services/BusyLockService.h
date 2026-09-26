#pragma once

#include <Events/PacketEvent.h>
#include <Messages/BusyLockData.h>
#include <map>

struct World;
struct Player;
struct UpdateEvent;
struct PlayerLeaveEvent;
struct BusyLockRequest;

struct BusyLockService
{
    BusyLockService(World& aWorld, entt::dispatcher& aDispatcher) noexcept;

private:
    struct Lease
    {
        BusyLockData Data;
        uint32_t Owner{};
        uint32_t PartyId{};
        entt::entity Character{entt::null};
        GameId Cell{};
        GameId WorldSpace{};
        uint64_t Deadline{};
    };
    using Key = std::pair<uint32_t, uint64_t>;

    void OnRequest(const PacketEvent<BusyLockRequest>& aEvent) noexcept;
    void OnUpdate(const UpdateEvent&) noexcept;
    void OnLeave(const PlayerLeaveEvent& aEvent) noexcept;
    const char* Expired(const Lease& aLease, uint64_t aNow) const noexcept;
    void ReleaseOwner(uint32_t aOwner, const char* aReason) noexcept;
    bool CanonicalReference(const GameId& aReference, GameId& aCanonical) const noexcept;

    World& m_world;
    // One lease per reference per party, and at most one per player. No pairwise state.
    std::map<Key, Lease> m_leases;
    // A release received before its acquire retires that request too. Heartbeats never acquire.
    std::map<uint32_t, uint64_t> m_lastRequest;
    entt::scoped_connection m_requestConnection, m_updateConnection, m_leaveConnection;
};
