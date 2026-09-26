#pragma once

#include <deque>
#include <unordered_map>

struct Actor;
struct World;
struct Movement;
struct ServerReferencesMoveRequest;
struct DisconnectedEvent;
struct UpdateEvent;

struct HeadTrackService
{
    HeadTrackService(World& aWorld, entt::dispatcher& aDispatcher) noexcept;

    static void FillLocalMovement(Movement& aMovement) noexcept;
    // Called on the movement presentation timeline, including stationary players.
    void UpdateRemote(Actor* apActor, uint64_t aTick) noexcept;

private:
    struct Sample
    {
        uint64_t Tick{};
        glm::vec2 Look{}; // pitch, yaw in radians
        bool Present{};
    };
    struct Track
    {
        entt::entity Entity{entt::null};
        uint32_t Epoch{};
        uint64_t ReceivedAt{};
        std::deque<Sample> Samples;
    };

    void OnMovement(const ServerReferencesMoveRequest& acMessage) noexcept;
    void OnDisconnected(const DisconnectedEvent&) noexcept;
    void OnUpdate(const UpdateEvent&) noexcept;

    World& m_world;
    std::unordered_map<uint32_t, Track> m_tracks;
    entt::scoped_connection m_movementConnection;
    entt::scoped_connection m_disconnectedConnection;
    entt::scoped_connection m_updateConnection;
};
