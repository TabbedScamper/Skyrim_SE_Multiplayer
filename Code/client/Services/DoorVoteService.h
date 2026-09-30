#pragma once

#include <Messages/DoorVoteData.h>
#include <array>
#include <cstddef>
#include <cstdint>

// Kept independent of engine objects so the input and follow policy can be tested offline.
namespace DoorVotePolicy
{
enum class Skip
{
    None, MissingReference, NotDoor, NotLoadDoor, NotPlayer, NotPlayerInput,
    Offline, NoParty, TooFewMembers, Locked, FreeDoor, Loading, MissingCell,
    MissingDestination, TooFar, UnmappedDoor, UnmappedCell, UnmappedDestination,
    UnmappedWorldSpace, SendFailed, Count
};

constexpr Skip Decide(bool aReference, bool aDoor, bool aTeleport, bool aPlayer,
    bool aInputCall, bool aOnline, bool aInParty, size_t aMembers) noexcept
{
    if (!aReference) return Skip::MissingReference;
    if (!aDoor) return Skip::NotDoor;
    if (!aTeleport) return Skip::NotLoadDoor;
    if (!aPlayer) return Skip::NotPlayer;
    if (!aInputCall) return Skip::NotPlayerInput;
    if (!aOnline) return Skip::Offline;
    if (!aInParty) return Skip::NoParty;
    if (aMembers < 2) return Skip::TooFewMembers;
    return Skip::None;
}

constexpr bool IsInputCall(uintptr_t aCaller, uintptr_t aPick, uintptr_t aChoice = 0) noexcept
{
    return (aPick != 0 && aCaller == aPick + 0x112) ||
        (aChoice != 0 && aCaller == aChoice + 0x73);
}

constexpr bool IsAutomaticEntry(uint32_t aDistanceBand, bool aEntering) noexcept
{
    return aDistanceBand == 1 && aEntering;
}

constexpr bool LostReadyVoter(bool aSameVote, uint32_t aPreviousReady, uint32_t aReady,
    bool aLocalReady, bool aLoading) noexcept
{
    return aSameVote && aLocalReady && !aLoading && aReady < aPreviousReady;
}

constexpr bool CanFollow(bool aPendingVote, uintptr_t aCell, uintptr_t aLeaderCell,
    uintptr_t aWorldSpace, uintptr_t aLeaderWorldSpace) noexcept
{
    return !aPendingVote && aCell && aLeaderCell &&
        (aCell == aLeaderCell || (aWorldSpace && aWorldSpace == aLeaderWorldSpace));
}
} // namespace DoorVotePolicy

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
        TESBoundObject* aObject, int32_t aCount, char aDefaultProcessing, const void* aCaller,
        bool aAtAutomaticDoor = false) noexcept;
    bool IsVoteDoor(TESObjectREFR* aDoor) noexcept;
    bool HasPendingVote() const noexcept { return m_deadline != 0 || m_state.VoteId != 0; }
    // Test harness, leader only: move the whole party into a cell through the door barrier. Returns an error or "".
    // By editor id (as the console's coc), which also finds exterior cells that are not loaded yet.
    const char* RequestTestCell(uint32_t aCellFormId, const char* apEditorId) noexcept;
    // Main thread (HookMainLoop): performs a queued test-cell load (CenterOnCell); never from the update thread.
    static void OnMainFrame() noexcept;

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
