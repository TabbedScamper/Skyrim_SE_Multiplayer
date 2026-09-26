#pragma once

#include <Events/PacketEvent.h>
#include <Messages/ReviveData.h>
#include <map>

struct World;
struct Player;
struct UpdateEvent;
struct ReviveRequest;

struct ReviveService
{
    ReviveService(World& aWorld, entt::dispatcher& aDispatcher) noexcept;

private:
    struct State
    {
        ReviveData Data;
        uint32_t PartyId{};
        uint64_t Received{};
        uint64_t GrantUntil{};
    };
    struct Hold
    {
        uint32_t Target{};
        uint64_t Revision{};
        uint64_t Started{};
        uint64_t Received{};
    };

    void OnRequest(const PacketEvent<ReviveRequest>& aEvent) noexcept;
    void OnUpdate(const UpdateEvent&) noexcept;
    bool Eligible(uint32_t aReviver, const Hold& aHold, uint64_t aNow) const noexcept;
    bool Current(uint32_t aPlayer, const State& aState, uint64_t aNow) const noexcept;
    void Broadcast(Player* aPlayer, const ReviveData& aData) const noexcept;

    World& m_world;
    std::map<uint32_t, State> m_states;
    std::map<uint32_t, Hold> m_holds;
    entt::scoped_connection m_requestConnection;
    entt::scoped_connection m_updateConnection;
};
