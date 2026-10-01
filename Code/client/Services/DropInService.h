#pragma once

#include <Structs/CharacterSnapshot.h>

#include <string>

struct World;
struct TransportService;
struct UpdateEvent;
struct NotifyDropIn;

// Joining a running session with an own character (owner design 2026-09-30: the host's world rules and the host
// never reloads; the joiner brings a character from one of its saves).
// Leader: on Capture, makes a checkpoint save (the checkpoint path) and streams SSC_<id>.ess to the joiner.
// Joiner: receives and verifies the file, loads it on the main loop, holds the screen black, applies its character
// (CharacterSnapshots), steps aside from the leader's spot, reports Loaded and fades in when admitted.
struct DropInService
{
    DropInService(World& aWorld, entt::dispatcher& aDispatcher, TransportService& aTransport) noexcept;
    TP_NOCOPYMOVE(DropInService);

    // Joiner, from the lobby: join the running session as the character in acSnapshotPath (a .snap file).
    // Returns an empty string, or why it cannot start.
    std::string StartJoin(const std::string& acSnapshotPath) noexcept;
    // "idle", "waiting for the host's save", "receiving N%", "loading", "applying your character", "joining",
    // "joined", or "failed: reason".
    [[nodiscard]] std::string Status() const noexcept;

private:
    void OnUpdate(const UpdateEvent&) noexcept;
    void OnMessage(const NotifyDropIn& acMessage) noexcept;
    void SendStep(uint8_t aOp, const std::string& acText = {}) noexcept;
    void Fail(const std::string& acReason, bool aTellServer) noexcept;

    enum class Phase
    {
        Idle,
        // Joiner
        WaitSave,
        Receive,
        Load,
        Apply,
        WaitAdmit,
        // Leader
        Capture,
        Stream
    };

    World& m_world;
    TransportService& m_transport;
    entt::scoped_connection m_updateConnection;
    entt::scoped_connection m_messageConnection;

    Phase m_phase{Phase::Idle};
    uint64_t m_attempt{};
    uint32_t m_joiner{};
    uint64_t m_since{};
    uint64_t m_loadedAt{};
    std::string m_status{"idle"};
    std::string m_pushedStatus{"idle"};
    CharacterSnapshot m_character{};
    // Joiner: the save being received. Leader: the save being sent.
    std::string m_file{};
    uint64_t m_expected{};
    uint64_t m_sent{};
    std::string m_checkpoint{};
};
