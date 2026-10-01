#pragma once
#include <Events/PacketEvent.h>
#include <Messages/DropIn.h>
#include <map>

struct World;
struct UpdateEvent;

// Joining a running session (owner design 2026-09-30). The server only relays and checks: the joiner asks, the leader
// saves and streams its save, the joiner loads it with its own character and reports loaded, the server replays the
// world around the joiner and admits it. The host never reloads.
struct DropInService
{
    DropInService(World&, entt::dispatcher&);

private:
    struct Attempt
    {
        uint32_t Joiner{};
        uint32_t Leader{};
        uint32_t PartyId{};
        uint64_t Deadline{};
        uint64_t Received{};
    };
    void OnRequest(const PacketEvent<RequestDropIn>&) noexcept;
    void OnUpdate(const UpdateEvent&) noexcept;
    void Send(uint32_t aPlayerId, const DropInData& acData) const noexcept;
    void Abort(uint64_t aAttempt, const char* acReason) noexcept;

    World& m_world;
    std::map<uint64_t, Attempt> m_attempts;
    entt::scoped_connection m_request, m_update;
};
