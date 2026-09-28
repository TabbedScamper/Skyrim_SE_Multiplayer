#pragma once
#include <Messages/WorldStateReplay.h>
#include <deque>
#include <map>
#include <set>
#include <mutex>
#include <string>

struct World;
struct TransportService;
struct TESObjectREFR;
struct UpdateEvent;
struct DisconnectedEvent;
struct NotifyWorldState;

struct WorldStateService
{
    WorldStateService(World&, entt::dispatcher&, TransportService&) noexcept;
    TP_NOCOPYMOVE(WorldStateService);
    static void MainThreadUpdate() noexcept;
    static bool IsMainThread() noexcept;
    static std::string Diagnostic(uint32_t) noexcept;
    static std::string AnimationDiagnostic(TESObjectREFR*) noexcept;
    static void TraceAnimation(uint32_t, const char*) noexcept;
    static bool Eligible(TESObjectREFR*) noexcept;
    // 0: no parent; UINT32_MAX: unresolved parent (fail closed).
    static uint32_t EnableParent(TESObjectREFR*) noexcept;
    // Hooks copy immutable IDs and arguments only; no engine queries.
    static void Observe(TESObjectREFR*, WorldStateKind, uint32_t = 0, float = 0, const char* = "") noexcept;
    static void ObserveId(uint32_t, WorldStateKind, uint32_t = 0, float = 0) noexcept;
    static void Attached(uint32_t) noexcept;
    static void AnimationEvent(uint32_t, const char*) noexcept;
    static void AnimationDirty(uint32_t) noexcept;
    static void AnimationKnown(uint32_t) noexcept;
    static void Sample(TESObjectREFR*) noexcept;
    static bool Apply(TESObjectREFR*, const WorldState&) noexcept;
    static bool Matches(TESObjectREFR*, const WorldState&) noexcept;
private:
    struct Authority
    {
        uint64_t Epoch{};
        uint32_t Leader{};
        bool IsLeader{};
        uint64_t Generation{}; // local connection identity, even if epoch repeats
        bool operator==(const Authority&) const = default;
    };
    void OnUpdate(const UpdateEvent&) noexcept;
    void OnDisconnected(const DisconnectedEvent&) noexcept;
    void OnState(const NotifyWorldState&) noexcept;
    void Pump() noexcept;
    void VisitCells() noexcept;
    void Baseline() noexcept;
    void Publish(WorldState) noexcept;
    void PumpAnimations() noexcept;
    World& m_world;
    TransportService& m_transport;
    std::mutex m_mailboxMutex;
    Authority m_requestedAuthority;
    uint64_t m_connectionGeneration{};
    std::deque<WorldState> m_incoming, m_outgoing;
    std::set<uint32_t> m_cellRequests;
    // Owned only by the native main-loop phase.
    Authority m_authority;
    std::set<uint32_t> m_cells, m_scopes;
    struct CellBaseline { uint32_t Cell{}, Cursor{}; std::set<uint32_t> Seen; std::deque<uint32_t> Pending; };
    std::deque<CellBaseline> m_baselines;
    struct Watch { uint64_t Until{}, Next{}; std::string Previous; };
    std::map<uint32_t, Watch> m_watches;
    uint32_t m_watchCursor{};
    uint32_t m_attachmentCursor{};
    uint32_t m_animationCursor{}, m_animationSeedCursor{};
    bool m_animationSeeding{};
    WorldStateTable::Key m_observationCursor{};
    uint64_t m_nextCells{}, m_watchdogStart{};
    bool m_watchdogReported{};
    std::map<uint32_t, uint64_t> m_sequences;
    std::map<WorldStateTable::Key, WorldState> m_latest;
    WorldStateReplay m_follower;
    entt::scoped_connection m_updateConnection, m_disconnectConnection, m_stateConnection;
};
