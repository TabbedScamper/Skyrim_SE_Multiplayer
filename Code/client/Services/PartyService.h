#pragma once

#include <filesystem>
#include <atomic>
#include <optional>
#include <mutex>
#include <Messages/CheckpointRoster.h>
#include <AuthorityEpoch.h>

struct World;
struct ImguiService;
struct TransportService;
struct UpdateEvent;
struct DisconnectedEvent;
struct NotifyPlayerList;
struct NotifyPartyInfo;
struct NotifyPartyInvite;
struct NotifyPartyJoined;
struct NotifyPartyLeft;
struct NotifyCheckpointSave;
struct NotifyCheckpointRoster;

/**
 * @brief Manages the party of the local player.
 */
struct PartyService
{
    PartyService(World& aWorld, entt::dispatcher& aDispatcher, TransportService& aTransportService) noexcept;
    ~PartyService() = default;

    TP_NOCOPYMOVE(PartyService);

    [[nodiscard]] bool IsInParty() const noexcept { return m_inParty; }
    [[nodiscard]] bool IsLeader() const noexcept { return m_isLeader; }
    [[nodiscard]] uint32_t GetLeaderPlayerId() const noexcept { return m_leaderPlayerId; }
    [[nodiscard]] uint64_t GetStartEpoch() const noexcept { return m_startEpoch; }
    [[nodiscard]] uint32_t GetLeaderGeneration() const noexcept { return m_leaderGeneration; }
    [[nodiscard]] AuthorityEpoch GetAuthorityEpoch() const noexcept
    {
        return {m_startEpoch, m_leaderPlayerId, m_leaderGeneration};
    }
    [[nodiscard]] uint8_t GetSessionState() const noexcept { return m_sessionState; }
    [[nodiscard]] uint8_t GetCampaignMode() const noexcept { return m_campaignMode; }
    [[nodiscard]] const String& GetCurrentCheckpointId() const noexcept { return m_currentCheckpoint; }
    [[nodiscard]] bool IsHandoffCampaign() const noexcept { return m_handoffCampaign; }
    [[nodiscard]] bool IsFollowerCinematicInputGated() const noexcept;

    const Vector<uint32_t>& GetPartyMembers() const noexcept { return m_partyMembers; }
    [[nodiscard]] size_t GetReadyPlayerCount() const noexcept { return m_readyPlayers.size(); }
    [[nodiscard]] bool IsPlayerReady(uint32_t aPlayerId) const noexcept
    {
        return std::find(m_readyPlayers.begin(), m_readyPlayers.end(), aPlayerId) != m_readyPlayers.end();
    }
    const Map<uint32_t, String>& GetPlayers() const noexcept { return m_players; }
    Map<uint32_t, uint64_t>& GetInvitations() noexcept { return m_invitations; }

    void CreateParty() const noexcept;
    void LeaveParty() noexcept;
    void CreateInvite(const uint32_t aPlayerId) const noexcept;
    void AcceptInvite(const uint32_t aInviterId) const noexcept;
    void KickPartyMember(const uint32_t aPlayerId) const noexcept;
    void EndParty() noexcept;
    void DrainForcedLeave() noexcept;
    [[nodiscard]] bool HasPendingCheckpointLaunch() noexcept;
    bool DrainPendingCheckpointLaunch() noexcept;
    void DrainRestartFadeOnMainFrame() noexcept;
    void ChangePartyLeader(const uint32_t aPlayerId) const noexcept;
    // Intercepts a leader's quit/leave while a shared world is running.
    bool BeginHostDeparture(bool aQuitToMenu) noexcept;
    [[nodiscard]] std::string HandoffStatus() const noexcept;
    void CancelHandoff() noexcept;
    void SetReady(bool aReady) const noexcept;
    void SelectCampaign(uint8_t aMode, const String& acCheckpointId = {}) const noexcept;
    void StartTogether(uint8_t aMode, const String& acCheckpointId = {}) const noexcept;
    void SetSessionSettings(bool aOpen, const String& acPassword, bool aKeepPassword = false) const noexcept;
    void SetGameplaySettings(uint32_t aDifficulty, bool aPvpEnabled) const noexcept;
    void ReachWorldReadyBarrier() noexcept;
    // TESLoadGameEvent: a pending in-game reload has happened (the barrier may be reported).
    void NoteGameLoaded() noexcept;

protected:
    void OnUpdate(const UpdateEvent& acEvent) noexcept;
    void OnDisconnected(const DisconnectedEvent& acEvent) noexcept;
    void OnPlayerList(const NotifyPlayerList& acPlayerList) noexcept;
    void OnPartyInfo(const NotifyPartyInfo& acPartyInfo) noexcept;
    void OnCheckpointSave(const NotifyCheckpointSave& acMessage) noexcept;
    void OnCheckpointRoster(const NotifyCheckpointRoster& acMessage) noexcept;
    void OnPartyInvite(const NotifyPartyInvite& acPartyInvite) noexcept;
    void OnPartyJoined(const NotifyPartyJoined& acPartyJoined) noexcept;
    void OnPartyLeft(const NotifyPartyLeft& acPartyLeft) noexcept;

private:
    void DestroyParty() noexcept;
    std::atomic_int m_forcedLeavePending{0}; // 1 leave, 2 capture then leave
    void RefreshFollowerIntroProtection() noexcept;
    void ReleaseFollowerIntroProtection() noexcept;

    Map<uint32_t, String> m_players;
    Map<uint32_t, uint64_t> m_invitations;
    uint64_t m_nextUpdate{0};

    bool m_inParty = false;
    bool m_isLeader = false;
    uint32_t m_leaderPlayerId{};
    Vector<uint32_t> m_partyMembers;
    Vector<uint64_t> m_partySteamIds;
    Vector<uint64_t> m_handoffOrder;
    Map<String, Vector<RosterPosition>> m_checkpointRosters;
    Vector<String> m_rosterOrder;
    uint64_t m_handoffTargetSteamId{};
    String m_handoffCheckpointId;
    String m_handoffSeedCheckpoint;
    bool m_handoffReady{};
    bool m_handoffSaveRequested{};
    bool m_handoffAckSent{};
    bool m_handoffDeparted{};
    bool m_handoffQuitRequested{};
    bool m_handoffAvailabilitySent{};
    bool m_handoffCampaign{};
    String m_lastHandoffFailure;
    uint64_t m_handoffNextSaveAttemptMs{};
    uint64_t m_handoffSaveRequestedAtMs{};
    uint64_t m_handoffNextAckAttemptMs{};
    Vector<uint32_t> m_readyPlayers;
    uint8_t m_campaignMode{};
    // Checkpoint the leader announced; written once this PC is in the world.
    String m_pendingCheckpoint{};
    String m_currentCheckpoint{};
    std::optional<RosterPosition> m_saveRoster;
    String m_saveRosterId;
    // The cell this PC's player has been in since when (checkpoint timing).
    uint32_t m_checkpointCellId{};
    uint64_t m_checkpointCellSinceMs{};
    // When this follower's player last got 3D after a load, and whether its walking camera was cleared since.
    std::chrono::steady_clock::time_point m_player3DSince{};
    bool m_walkingCameraCleared{};
    uint8_t m_sessionState{};
    uint64_t m_startEpoch{};
    uint32_t m_leaderGeneration{};
    // A lobby-selected home character is applied to the shared world before it can produce a carry-home record.
    std::string m_homeSourceKey;
    std::filesystem::path m_homeSourceSnap;
    uint8_t m_homeApplyState{}; // 1 waiting for world, 2 applying, 3 done
    uint64_t m_homeWorldSince{};
    bool m_waitingForWorldReady{};
    bool m_worldGateHeld{};
    bool m_creatorInputReleased{};
    bool m_creatorSeen{};
    bool m_gameplayReadySent{};
    // Character creator together: this player finished and waits for the others (movement held).
    bool m_creatorWaitHeld{};
    // Leader free control: last state sent (leader), last received and whether this follower
    // was already gathered around the leader this session (followers).
    int m_leaderFreeSent{-1};
    uint64_t m_nextLeaderFreeSendMs{};
    bool m_leaderFree{};
    bool m_gatheredAroundLeader{};
    entt::scoped_connection m_leaderControlConnection;
    void OnNotifyLeaderControl(const struct NotifyLeaderControl& acMessage) noexcept;
    uint64_t m_nextCreatorWaitNoticeMs{};
    bool m_followerIntroProtectionHeld{};
    bool m_playerWasEssential{};

    World& m_world;
    TransportService& m_transport;

    entt::scoped_connection m_updateConnection;
    entt::scoped_connection m_disconnectConnection;
    entt::scoped_connection m_playerListConnection;
    entt::scoped_connection m_partyInfoConnection;
    entt::scoped_connection m_checkpointSaveConnection;
    entt::scoped_connection m_checkpointRosterConnection;
    entt::scoped_connection m_partyInviteConnection;
    entt::scoped_connection m_partyJoinedConnection;
    entt::scoped_connection m_partyLeftConnection;
    // An in-game checkpoint reload (party wipe) not yet loaded: this PC's world is still the old one.
    bool m_reloadPending{};
    // A synced load (session start or wipe reload) keeps the screen black until the gameplay barrier.
    bool m_wipeFade{};
    // When the black screen went up (a dead-man release lifts it if the gameplay barrier never comes).
    uint64_t m_wipeFadeSince{};
    // A follower's Continue load held until the leader's world has loaded.
    bool m_deferredLaunch{};
    uint64_t m_deferredSince{};
    String m_deferredCheckpoint;
    std::mutex m_launchLock;
    String m_queuedLaunchCheckpoint;
    uint8_t m_queuedLaunchMode{};
    uint32_t m_queuedLaunchConnection{};
    bool m_queuedLaunch{};
    std::atomic_bool m_restartFadePending{};
    void LaunchCheckpoint(const String& acCheckpointId) noexcept;
    void LaunchCheckpointNow(const String& acCheckpointId, uint8_t aMode) noexcept;
    std::optional<RosterPosition> CaptureRosterPosition() noexcept;
    void SendRosterPosition(const String& acId, const RosterPosition& acPosition) noexcept;
    void SendHandoffSeed(const String& acCheckpointId) noexcept;
};
