#pragma once

#include <Games/Skyrim/SceneTurnsHooks.h>
#include <memory>

struct World;
struct TransportService;
struct UpdateEvent;
struct DisconnectedEvent;

// Experimental host choreography. Effects remain owned by the original fragments.
// Enable before launching the host with SKYRIM_COOP_SCENE_TURNS=1.
struct SceneTurnsService
{
    SceneTurnsService(World& aWorld, entt::dispatcher& aDispatcher,
        TransportService& aTransport) noexcept;
    ~SceneTurnsService();
    TP_NOCOPYMOVE(SceneTurnsService);

    static void Capture(const SceneTurnsNative::IdleStep& aStep) noexcept;
    static bool IsAuthoritative() noexcept;
    static bool HoldPhase(uint32_t aSceneId, uint32_t aPhase) noexcept;

private:
    void OnUpdate(const UpdateEvent&) noexcept;
    void OnDisconnected(const DisconnectedEvent&) noexcept;
    void Clear() noexcept;

    struct State;
    std::unique_ptr<State> m_state;
    World& m_world;
    TransportService& m_transport;
    entt::scoped_connection m_updateConnection;
    entt::scoped_connection m_disconnectConnection;
};
