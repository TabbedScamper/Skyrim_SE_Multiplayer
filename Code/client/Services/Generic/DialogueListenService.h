#pragma once
#include <Games/Skyrim/DialogueListenHooks.h>

struct World;
struct TransportService;
struct UpdateEvent;
struct DisconnectedEvent;
struct SubtitleEvent;
struct NotifyDialogueListen;

namespace DialogueListen
{
// Safe from activation hooks: execution is queued onto World/RunnerService.
void Begin(uint32_t aNpcFormId, uint32_t aSpeakerPlayerId) noexcept;
void End() noexcept;
}

struct DialogueListenService
{
    DialogueListenService(World&, entt::dispatcher&, TransportService&) noexcept;
    void Begin(uint32_t aNpcFormId, uint32_t aSpeakerPlayerId) noexcept;
    void End(const char* aReason = "cancelled") noexcept;
private:
    void OnUpdate(const UpdateEvent&) noexcept;
    void OnDisconnected(const DisconnectedEvent&) noexcept;
    void OnState(const NotifyDialogueListen&) noexcept;
    void OnSubtitle(const SubtitleEvent&) noexcept;
    uint32_t ServerId(uint32_t aFormId) const noexcept;
    bool Near(uint32_t aFormId) const noexcept;
    bool PartyMember(uint32_t aPlayerId) const noexcept;
    void PushUI() noexcept;
    World& m_world;
    TransportService& m_transport;
    DialogueListenNative::Presentation m_presentation;
    DialogueListenState m_sent;
    DialogueListenState m_received;
    uint64_t m_session{};
    uint64_t m_revision{};
    uint32_t m_listenForm{};
    uint32_t m_speaker{};
    uint64_t m_waitUntil{};
    uint64_t m_epoch{};
    bool m_listening{};
    uint32_t m_subtitleForm{};
    String m_subtitle;
    uint32_t m_duration{};
    uint64_t m_lineSerial{};
    uint64_t m_lineTick{};
    uint64_t m_hideSerial{};
    entt::scoped_connection m_updateConnection;
    entt::scoped_connection m_disconnectConnection;
    entt::scoped_connection m_stateConnection;
    entt::scoped_connection m_subtitleConnection;
};
