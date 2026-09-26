#pragma once

#include <Messages/NotifyPartyUnstuck.h>
#include <Games/Events.h>
#include <atomic>
#include <optional>

struct World;
struct UpdateEvent;
struct DisconnectedEvent;
struct NotifyPlayerControlState;

// Implemented beside PlayerControlSync. Explicit recovery may bypass the scene
// guard; normal control reconciliation keeps its existing lifetime checks.
namespace UnstuckControls
{
bool Capture(PlayerControlState& aState) noexcept;
bool Apply(const PartyUnstuckState& acState, uint32_t& aBefore, uint32_t& aAfter) noexcept;
}

struct UnstuckReset : BSTEventSink<TESLoadGameEvent>
{
    UnstuckReset(World& aWorld, entt::dispatcher& aDispatcher) noexcept;
    ~UnstuckReset() noexcept;
    void ConnectUpdate(entt::dispatcher& aDispatcher) noexcept;
    BSTEventResult OnEvent(const TESLoadGameEvent*, const EventDispatcher<TESLoadGameEvent>*) override;
    bool Capture(RequestPartyUnstuck& aRequest) noexcept;
    void Queue(const NotifyPartyUnstuck& acMessage) noexcept;

private:
    void BeforeCamera(const UpdateEvent&) noexcept;
    void OnUpdate(const UpdateEvent&) noexcept;
    void OnDisconnect(const DisconnectedEvent&) noexcept;
    void OnControls(const NotifyPlayerControlState& acMessage) noexcept;
    bool Apply(const NotifyPartyUnstuck& acMessage, bool aRestoreFurniture) noexcept;

    World& m_world;
    std::optional<NotifyPartyUnstuck> m_pending;
    std::optional<NotifyPartyUnstuck> m_release;
    uint64_t m_expires{};
    uint64_t m_arrivalAfter{};
    bool m_cleared{};
    bool m_graphReset{};
    bool m_sawFree{};
    bool m_reapplyControls{};
    uint64_t m_controlSequence{};
    std::atomic<bool> m_loaded{};
    entt::scoped_connection m_updateConnection;
    entt::scoped_connection m_beforeCameraConnection;
    entt::scoped_connection m_disconnectConnection;
    entt::scoped_connection m_controlConnection;
};
