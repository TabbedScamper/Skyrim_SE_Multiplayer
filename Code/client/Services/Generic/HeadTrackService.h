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
    static bool IsCameraTracking(const Actor* apActor) noexcept;
    // Test bridge (head stuck looking up after a revive): the local camera pitch and state, or a remote player's
    // presented look pitch, plus whether a camera-look override is live on its graph. Degrees, positive = down.
    static std::string DescribeLook(Actor* apActor) noexcept;
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
