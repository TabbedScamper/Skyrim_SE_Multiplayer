#pragma once

#include <Events/PacketEvent.h>
#include <unordered_map>

struct World;
struct SceneTimelineRequest;

// Orders leader-owned scene observations. This first slice does not write
// follower native scene state; it establishes the measured authority stream.
class SceneTimelineService
{
public:
    SceneTimelineService(World& aWorld, entt::dispatcher& aDispatcher) noexcept;
    TP_NOCOPYMOVE(SceneTimelineService);

private:
    void OnSceneTimeline(const PacketEvent<SceneTimelineRequest>& acMessage) noexcept;

    World& m_world;
    uint64_t m_nextSequence{};
    std::unordered_map<uint32_t, std::pair<uint64_t, uint64_t>> m_lastTransactionByLeader;
    entt::scoped_connection m_sceneConnection;
};
