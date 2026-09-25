#pragma once

#include <unordered_map>

struct World;
struct TransportService;
struct UpdateEvent;
struct DisconnectedEvent;
struct NotifySceneTimeline;

// Records the native scene timeline on both peers and relays leader
// transitions. Deliberately observation-only on followers for this slice.
class SceneTimelineService
{
public:
    SceneTimelineService(World& aWorld, entt::dispatcher& aDispatcher,
        TransportService& aTransport) noexcept;
    TP_NOCOPYMOVE(SceneTimelineService);

private:
    struct SceneState
    {
        uint32_t RawPhaseWord{UINT32_MAX};
        bool Playing{};
        uint64_t SampleMs{};
    };

    void OnUpdate(const UpdateEvent&) noexcept;
    void OnDisconnected(const DisconnectedEvent&) noexcept;
    void OnSceneTimeline(const NotifySceneTimeline&) noexcept;
    void Clear() noexcept;

    World& m_world;
    TransportService& m_transport;
    uint64_t m_epoch{};
    uint64_t m_nextSampleMs{};
    uint64_t m_nextTransactionId{};
    uint64_t m_lastServerSequence{};
    std::unordered_map<uint32_t, SceneState> m_localStates;
    entt::scoped_connection m_updateConnection;
    entt::scoped_connection m_disconnectConnection;
    entt::scoped_connection m_sceneConnection;
};
