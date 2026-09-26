#pragma once

#include <Messages/ReviveData.h>
#include <Games/Primitives.h>
#include <map>

struct World;
struct TransportService;
struct Actor;
struct PlayerCharacter;
struct NotifyRevive;
struct DisconnectedEvent;

struct ReviveService
{
    ReviveService(World& aWorld, entt::dispatcher& aDispatcher, TransportService& aTransport) noexcept;
    // Called by PlayerService before its legacy solo respawn timer.
    bool Update(bool aEnabled) noexcept;

private:
    struct Peer
    {
        ReviveData Data;
        uint64_t Received{};
        uint32_t AppliedForm{};
        uint64_t AppliedRevision{};
        bool AppliedDown{};
    };
    struct SafePosition
    {
        uint32_t Cell{};
        uint32_t WorldSpace{};
        NiPoint3 Position{};
        uint64_t Recorded{};
    };

    void OnNotify(const NotifyRevive& aMessage) noexcept;
    void OnDisconnected(const DisconnectedEvent&) noexcept;
    void Reset() noexcept;
    void SendState(PlayerCharacter* aPlayer, uint64_t aNow) noexcept;
    void SendHold(ReviveAction aAction) noexcept;
    void CancelHold() noexcept;
    void TrackSafePosition(PlayerCharacter* aPlayer, uint64_t aNow) noexcept;
    void Fallback(PlayerCharacter* aPlayer, const char* aReason) noexcept;
    void RestoreSpells(PlayerCharacter* aPlayer) noexcept;
    void ApplyPeers() noexcept;
    Actor* FindPlayer(uint32_t aId) const noexcept;
    bool IsPartyMember(uint32_t aId) const noexcept;
    bool Near(PlayerCharacter* aPlayer, const Peer& aPeer, float aRadius) const noexcept;
    std::string Name(uint32_t aId) const;

    World& m_world;
    TransportService& m_transport;
    std::map<uint32_t, Peer> m_peers;
    std::map<uint32_t, SafePosition> m_safePositions;
    uint64_t m_epoch{};
    uint64_t m_revision{};
    uint64_t m_nextState{};
    uint64_t m_nextNotice{};
    uint64_t m_noHelpSince{};
    uint64_t m_safeSince{};
    uint32_t m_safeCell{};
    NiPoint3 m_lastPosition{};
    uint64_t m_lastUpdate{};
    uint32_t m_holdTarget{};
    uint64_t m_holdRevision{};
    uint64_t m_holdSince{};
    uint64_t m_nextHold{};
    uint32_t m_mainSpell{};
    uint32_t m_secondarySpell{};
    uint32_t m_power{};
    bool m_active{};
    bool m_down{};
    bool m_combat{};
    bool m_alive{};
    bool m_giveUpHeld{};
    entt::scoped_connection m_notifyConnection;
    entt::scoped_connection m_disconnectConnection;
};
