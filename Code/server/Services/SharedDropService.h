#pragma once

#include <Events/PacketEvent.h>
#include <Messages/SharedDropData.h>
#include <map>
#include <set>

struct World;
struct Player;
struct UpdateEvent;
struct RequestSharedDrop;

struct SharedDropService
{
    SharedDropService(World&, entt::dispatcher&);

private:
    struct Drop
    {
        SharedDropData Data;
        SharedDropClaim Claim;
        uint32_t Party{};
        std::set<uint32_t> Seen, Ready;
        bool Retired{};
    };
    void OnRequest(const PacketEvent<RequestSharedDrop>&) noexcept;
    void OnUpdate(const UpdateEvent&) noexcept;
    void Send(Player*, const Drop&, SharedDropAction, uint64_t aToken = 0) const;
    void Broadcast(const Drop&, SharedDropAction) const;
    bool Near(const Player*, const Drop&) const;
    Player* FindPlayer(uint32_t) const;
    World& m_world;
    // Retain create receipts and terminal claims for the connection's lifetime.
    std::map<std::pair<uint32_t, uint64_t>, uint32_t> m_receipts;
    uint64_t m_nextUpdate{};
    entt::scoped_connection m_requestConnection, m_updateConnection;
};
