#pragma once

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
    [[nodiscard]] uint8_t GetSessionState() const noexcept { return m_sessionState; }
    [[nodiscard]] bool IsFollowerCinematicInputGated() const noexcept;

    const Vector<uint32_t>& GetPartyMembers() const noexcept { return m_partyMembers; }
    [[nodiscard]] size_t GetReadyPlayerCount() const noexcept { return m_readyPlayers.size(); }
    const Map<uint32_t, String>& GetPlayers() const noexcept { return m_players; }
    Map<uint32_t, uint64_t>& GetInvitations() noexcept { return m_invitations; }

    void CreateParty() const noexcept;
    void LeaveParty() const noexcept;
    void CreateInvite(const uint32_t aPlayerId) const noexcept;
    void AcceptInvite(const uint32_t aInviterId) const noexcept;
    void KickPartyMember(const uint32_t aPlayerId) const noexcept;
    void ChangePartyLeader(const uint32_t aPlayerId) const noexcept;
    void SetReady(bool aReady) const noexcept;
    void SelectCampaign(uint8_t aMode, const String& acCheckpointId = {}) const noexcept;
    void StartTogether(uint8_t aMode, const String& acCheckpointId = {}) const noexcept;
    void SetSessionSettings(bool aOpen, const String& acPassword) const noexcept;
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
    void OnPartyInvite(const NotifyPartyInvite& acPartyInvite) noexcept;
    void OnPartyJoined(const NotifyPartyJoined& acPartyJoined) noexcept;
    void OnPartyLeft(const NotifyPartyLeft& acPartyLeft) noexcept;

private:
    void DestroyParty() noexcept;
    void RefreshFollowerIntroProtection() noexcept;
    void ReleaseFollowerIntroProtection() noexcept;

    Map<uint32_t, String> m_players;
    Map<uint32_t, uint64_t> m_invitations;
    uint64_t m_nextUpdate{0};

    bool m_inParty = false;
    bool m_isLeader = false;
    uint32_t m_leaderPlayerId{};
    Vector<uint32_t> m_partyMembers;
    Vector<uint32_t> m_readyPlayers;
    uint8_t m_campaignMode{};
    // Checkpoint the leader announced; written once this PC is in the world.
    String m_pendingCheckpoint{};
    // The cell this PC's player has been in since when (checkpoint timing).
    uint32_t m_checkpointCellId{};
    uint64_t m_checkpointCellSinceMs{};
    // When this follower's player last got 3D after a load, and whether its walking camera was cleared since.
    std::chrono::steady_clock::time_point m_player3DSince{};
    bool m_walkingCameraCleared{};
    uint8_t m_sessionState{};
    uint64_t m_startEpoch{};
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
    void LaunchCheckpoint(const String& acCheckpointId) noexcept;
};
