#pragma once

#include <Events/UpdateEvent.h>

struct World;
struct Player;

/**
 * Bridges the running server to the local diagnostics MCP through an
 * append-only command mailbox and JSONL telemetry file.
 *
 * The mailbox intentionally supports only visible messages, bug markers, and
 * read-only snapshots. It is not a general-purpose server console.
 */
struct DiagnosticsService
{
    DiagnosticsService(World& aWorld, entt::dispatcher& aDispatcher) noexcept;
    ~DiagnosticsService() noexcept = default;

    TP_NOCOPYMOVE(DiagnosticsService);

    void RecordPlayerMessage(const Player& acPlayer, const String& acMessage) const noexcept;

private:
    void OnUpdate(const UpdateEvent& acEvent) noexcept;
    void ProcessMailbox() noexcept;
    void ProcessCommand(const String& acLine) noexcept;
    void BroadcastMessage(const String& acMessage) const noexcept;
    void WriteSnapshot(const String& acRequestId, const String& acReason) const noexcept;

    World& m_world;
    entt::scoped_connection m_updateConnection;
    std::chrono::steady_clock::time_point m_nextPoll{};
    std::streamoff m_commandOffset{};
};
