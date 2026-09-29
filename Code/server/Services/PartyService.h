#pragma once

#include <Events/PacketEvent.h>
#include <Structs/ServerSettings.h>

struct World;
struct UpdateEvent;
struct PlayerJoinEvent;
struct PlayerLeaveEvent;
struct PartyInviteRequest;
struct PartyAcceptInviteRequest;
struct PartyLeaveRequest;
struct NotifyPartyInfo;
struct PartyCreateRequest;
struct PartyChangeLeaderRequest;
struct PartyKickRequest;
struct PartyReadyRequest;
struct LeaderControlRequest;
struct PartyStartRequest;
struct PartySessionSettingsRequest;
struct PartyGameplaySettingsRequest;
struct CheckpointSaveRequest;

/**
 * @brief Manages every party in the server.
 */
struct PartyService
{
    struct Party
    {
        uint32_t LeaderPlayerId;
        Vector<Player*> Members;
        Vector<uint32_t> ReadyPlayerIds;
        Vector<uint32_t> LoadedPlayerIds;
        Vector<uint32_t> GameplayReadyPlayerIds;
        GameId CachedWeather{};
        // Host sky state (weather blend and wind), replayed to members who join or reload.
        bool CachedHasSky{};
        GameId CachedLastWeather{};
        float CachedWeatherPercent{1.f};
        float CachedWindSpeed{};
        float CachedWindAngle{};
        uint8_t CampaignMode{};
        uint8_t SessionState{};
        uint64_t StartEpoch{};
        String CheckpointId{};
        // The newest checkpoint the leader announced in this session (a wipe reloads it).
        String LatestCheckpointId{};
        bool LobbyOpen{};
        bool PasswordProtected{};
        ServerSettings GameplaySettings{};
        bool GameplayOverridden{};
    };

    PartyService(World& aWorld, entt::dispatcher& aDispatcher) noexcept;
    ~PartyService() noexcept = default;

    TP_NOCOPYMOVE(PartyService);

    const Party* GetById(uint32_t aId) const noexcept;
    // After a party wipe: every member reloads the leader's latest matched checkpoint (the Continue path, new epoch).
    // False when the session has no checkpoint to reload.
    bool RestartFromCheckpoint(uint32_t aPartyId) noexcept;
    bool IsPlayerInParty(Player* const apPlayer) const noexcept;
    bool IsPlayerLeader(const Player* const apPlayer) const noexcept;
    Party* GetPlayerParty(Player* const apPlayer) noexcept;
    ServerSettings GetSettingsForPlayer(const Player* apPlayer) const noexcept;

protected:
    void OnUpdate(const UpdateEvent& acEvent) noexcept;
    void OnPlayerJoin(const PlayerJoinEvent& acEvent) noexcept;
    void OnPlayerLeave(const PlayerLeaveEvent& acEvent) noexcept;
    void OnPartyInvite(const PacketEvent<PartyInviteRequest>& acPacket) noexcept;
    void OnPartyAcceptInvite(const PacketEvent<PartyAcceptInviteRequest>& acPacket) noexcept;
    void OnPartyLeave(const PacketEvent<PartyLeaveRequest>& acPacket) noexcept;
    void OnPartyCreate(const PacketEvent<PartyCreateRequest>& acPacket) noexcept;
    void OnPartyChangeLeader(const PacketEvent<PartyChangeLeaderRequest>& acPacket) noexcept;
    void OnPartyKick(const PacketEvent<PartyKickRequest>& acPacket) noexcept;
    void OnPartyReady(const PacketEvent<PartyReadyRequest>& acPacket) noexcept;
    void OnLeaderControl(const PacketEvent<LeaderControlRequest>& acPacket) noexcept;
    void OnPartyStart(const PacketEvent<PartyStartRequest>& acPacket) noexcept;
    void OnPartySessionSettings(const PacketEvent<PartySessionSettingsRequest>& acPacket) noexcept;
    void OnPartyGameplaySettings(const PacketEvent<PartyGameplaySettingsRequest>& acPacket) noexcept;
    void OnCheckpointSave(const PacketEvent<CheckpointSaveRequest>& acPacket) noexcept;
    void RemovePlayerFromParty(Player* apPlayer) noexcept;

    void BroadcastPlayerList(Player* apPlayer = nullptr) const noexcept;
    void BroadcastPartyInfo(uint32_t aPartyId) const noexcept;

private:
    World& m_world;

    TiltedPhoques::Map<uint32_t, Party> m_parties;
    uint32_t m_nextId{0};
    uint64_t m_nextInvitationExpire{0};

    entt::scoped_connection m_updateEvent;
    entt::scoped_connection m_playerJoinConnection;
    entt::scoped_connection m_playerLeaveConnection;
    entt::scoped_connection m_partyInviteConnection;
    entt::scoped_connection m_partyAcceptInviteConnection;
    entt::scoped_connection m_partyLeaveConnection;
    entt::scoped_connection m_partyCreateConnection;
    entt::scoped_connection m_partyChangeLeaderConnection;
    entt::scoped_connection m_partyKickConnection;
    entt::scoped_connection m_partyReadyConnection;
    entt::scoped_connection m_leaderControlConnection;
    entt::scoped_connection m_partyStartConnection;
    entt::scoped_connection m_partySessionSettingsConnection;
    entt::scoped_connection m_partyGameplaySettingsConnection;
    entt::scoped_connection m_checkpointSaveConnection;

    uint64_t m_nextStartEpoch{1};

    void SendPartyJoinedEvent(Party& aParty, Player* aPlayer) noexcept;
};
