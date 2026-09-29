#pragma once

#include <Messages/ReviveData.h>
#include <Games/Primitives.h>
#include <map>

struct World;
struct TransportService;
struct Actor;
struct PlayerCharacter;
struct TESObjectCELL;
struct NotifyRevive;
struct DisconnectedEvent;

struct ReviveService
{
    ReviveService(World& aWorld, entt::dispatcher& aDispatcher, TransportService& aTransport) noexcept;
    // Called by PlayerService before its legacy solo respawn timer.
    bool Update(bool aEnabled) noexcept;
    // Test bridge: treat the Activate control as held (a revive can be tested without anyone at the keyboard).
    static void SetTestHold(bool aHeld) noexcept;
    static void SetTestBleed(float aBleed) noexcept;
    // Test bridge: treat the Shout key as held for the call-back ritual.
    static void SetTestShout(bool aHeld) noexcept;
    // The local player took a lethal hit (any thread): the blow as a share of max health (overkill from 0.75) and
    // the attacker (0 if none), which the fling pushes away from.
    static void NoteLethalHit(float aShareOfMax, uint32_t aAttacker) noexcept;
    // Test bridge: this player's fallen/spectate/ritual state as JSON (updated each Update).
    static std::string DescribeTest() noexcept;
    // A party player that has fallen (bled out, spectating), this one included. Main thread.
    [[nodiscard]] bool IsFallen(uint32_t aPlayerId) const noexcept;
    // The party wiped and is on its way back to the checkpoint.
    [[nodiscard]] bool IsWiped() const noexcept { return m_wiped; }

private:
    struct Peer
    {
        ReviveData Data;
        uint64_t Received{};
        uint32_t AppliedForm{};
        uint64_t AppliedRevision{};
        bool AppliedDown{};
        bool AppliedDead{};
        // The copy's graph states while it lay down (sampled 1.5 s in), and when to check that a stand-up took: a
        // stop that plays but leaves the graph in its down states shows the copy lying whenever the pose stream gaps.
        uint64_t DownDigest{};
        uint64_t DownSampleAt{};
        uint64_t StopCheckAt{};
    };

    void OnNotify(const NotifyRevive& aMessage) noexcept;
    void OnDisconnected(const DisconnectedEvent&) noexcept;
    // aRestorePlayer: undo the fallen state on the player (disconnect, leaving the party). Never during a session
    // restart: the checkpoint load is already replacing the world (touching the player then crashed the follower).
    void Reset(bool aRestorePlayer = true) noexcept;
    void SendState(PlayerCharacter* aPlayer, uint64_t aNow) noexcept;
    void SendHold(ReviveAction aAction) noexcept;
    void CancelHold() noexcept;
    void RestoreSpells(PlayerCharacter* aPlayer) noexcept;
    void ApplyPeers() noexcept;
    Actor* FindPlayer(uint32_t aId) const noexcept;
    // Player id -> local form of its character, rebuilt once per Update (one pass over the player entities instead of
    // one per lookup: lookups run per peer several times a frame).
    mutable std::vector<std::pair<uint32_t, uint32_t>> m_playerForms;
    mutable uint64_t m_playerFormsFrame{~0ull};
    uint64_t m_frame{};
    uint64_t m_nextTestState{};
    bool IsPartyMember(uint32_t aId) const noexcept;
    bool Near(PlayerCharacter* aPlayer, const Peer& aPeer, float aRadius) const noexcept;
    std::string Name(uint32_t aId) const;
    // Fallen (bled out): hidden, untargetable, spectating the living until an ally calls this player back.
    void EnterFallen(PlayerCharacter* aPlayer, const char* aReason) noexcept;
    void LeaveFallen(PlayerCharacter* aPlayer, const NotifyRevive* apRaise) noexcept;
    void UpdateFallen(PlayerCharacter* aPlayer, uint64_t aNow, bool aInput) noexcept;
    // The call-back ritual: hold Shout 2 s out of combat with full magicka. True when its prompt is shown.
    bool UpdateRitual(PlayerCharacter* aPlayer, uint64_t aNow, bool aInput) noexcept;
    TESObjectCELL* PeerCell(const ReviveData& aData, const NiPoint3& aPosition) const noexcept;

    World& m_world;
    TransportService& m_transport;
    std::map<uint32_t, Peer> m_peers;
    uint64_t m_epoch{};
    uint64_t m_revision{};
    uint64_t m_nextState{};
    uint64_t m_nextNotice{};
    uint32_t m_holdTarget{};
    uint64_t m_holdRevision{};
    uint64_t m_holdSince{};
    // Being revived (this player down): the reviver and when their hold started, from the server's Hold notices.
    // Bleedout meter while down: 1 -> 0 over kBleedoutMs, paused while someone is reviving this player.
    static constexpr uint64_t kBleedoutMs = 2 * 60 * 1000;
    float m_bleed{1.f};
    uint64_t m_bleedTick{};
    uint32_t m_revivedBy{};
    uint64_t m_revivedSince{};
    uint64_t m_revivedLast{};
    // Last overlay state sent, to push only changes (and progress at most every 50 ms).
    int m_uiMode{};
    std::string m_uiName, m_uiKey, m_uiHint, m_uiNote, m_uiButton, m_uiModel;
    float m_uiBleed{-1.f};
    bool m_uiGamepad{};
    double m_uiProgress{};
    uint64_t m_uiSent{};
    std::string m_model;
    uint64_t m_nextModel{};
    // Combat rule shared with the server: 6 s instead of 3 s while anyone involved is in combat or being targeted.
    bool CombatAround(PlayerCharacter* aPlayer, uint32_t aOther) const noexcept;
    void PushUi(int aMode, const std::string& acName, const std::string& acKey, double aProgress,
        const std::string& acHint, uint64_t aNow, const std::string& acNote = {}, const std::string& acButton = {},
        bool aGamepad = false, float aBleed = -1.f) noexcept;
    uint64_t m_nextHold{};
    uint32_t m_mainSpell{};
    uint32_t m_secondarySpell{};
    uint32_t m_power{};
    bool m_active{};
    bool m_down{};
    bool m_combat{};
    bool m_alive{};
    bool m_fallen{};
    // The party wiped: everyone collapsed and the checkpoint reload is on its way.
    bool m_wiped{};
    bool m_wasFirstPerson{};
    // Fallen to a fling or an overkill blow ("slain", for the notices; ReviveData::Flung on the wire). The body flew
    // and landed in the dying phase below, before this player became fallen; fallen bodies are hidden alike.
    bool m_slain{};
    // Between a lethal fling/overkill hit and landing: the engine's knockdown plays untouched and the body stays
    // visible (remote copies stream it) until it is nearly still or the player skips.
    uint64_t m_dyingSince{};
    bool m_dyingOverkill{};
    bool m_dyingKnocked{};
    bool m_dyingCollapsed{};
    uint64_t m_dyingKnockSince{};
    // Stillness of the flying body, sampled on its pelvis bone (the ragdoll moves the bones, not necessarily the
    // reference position), and the skip key's previous state.
    NiPoint3 m_dyingLastPos{};
    uint64_t m_dyingLastSample{};
    uint64_t m_dyingStillSince{};
    bool m_skipHeld{};
    uint32_t m_watch{};
    uint32_t m_cameraOn{};
    uint64_t m_nextCamera{};
    uint64_t m_nextWatchMove{};
    bool m_prevHeld{};
    bool m_nextHeld{};
    uint32_t m_ritualTarget{};
    uint64_t m_ritualSince{};
    bool m_ritualLatched{};
    uint64_t m_magickaLockUntil{};
    entt::scoped_connection m_notifyConnection;
    entt::scoped_connection m_disconnectConnection;
};
