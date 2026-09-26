#pragma once

#include <Messages/DoorVoteData.h>
#include <array>

struct World;
struct TransportService;
struct TESObjectREFR;
struct TESBoundObject;
struct BGSKeyword;
struct UpdateEvent;
struct DisconnectedEvent;
struct NotifyDoorVote;
struct NotifyLeaderControl;

struct DoorVoteService
{
    DoorVoteService(World& aWorld, entt::dispatcher& aDispatcher, TransportService& aTransport) noexcept;
    ~DoorVoteService();

    bool TryHold(TESObjectREFR* aDoor, TESObjectREFR* aActivator, uint8_t aUnk1,
        TESBoundObject* aObject, int32_t aCount, char aDefaultProcessing, const void* aCaller) noexcept;
    bool IsVoteDoor(TESObjectREFR* aDoor) noexcept;

private:
    void InitKeywords() noexcept;
    void OnUpdate(const UpdateEvent&) noexcept;
    void OnDisconnected(const DisconnectedEvent&) noexcept;
    void OnNotify(const NotifyDoorVote& aMessage) noexcept;
    void OnLeaderControl(const NotifyLeaderControl& aMessage) noexcept;
    void Reset() noexcept;
    void SendAction(DoorVoteAction aAction) noexcept;

    World& m_world;
    TransportService& m_transport;
    std::array<BGSKeyword*, 16> m_keywords{};
    bool m_keywordsReady{};
    bool m_leaderFree{true};
    uint32_t m_leader{};
    DoorVoteData m_state;
    GameId m_heldDoor{};
    uint32_t m_doorForm{};
    uint32_t m_objectForm{};
    uint8_t m_unk1{};
    int32_t m_count{};
    char m_defaultProcessing{};
    bool m_withdrawSent{};
    bool m_loading{};
    uint64_t m_activationTime{};
    bool m_gateHeld{};
    uint64_t m_nextLoadedSend{};
    uint64_t m_deadline{};
    uint64_t m_lastVoteId{};
    entt::scoped_connection m_updateConnection;
    entt::scoped_connection m_disconnectConnection;
    entt::scoped_connection m_notifyConnection;
    entt::scoped_connection m_controlConnection;
};
