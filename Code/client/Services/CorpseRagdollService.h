#pragma once

#include <Messages/CorpseRagdollRequest.h>

#include <array>
#include <atomic>
#include <mutex>

struct World;
struct TransportService;
struct UpdateEvent;
struct DisconnectedEvent;
struct NotifyCorpseRagdoll;
struct NotifyDismember;

// Ragdolls look the same on every PC, in real time: deaths, knockdowns, shouts, explosions.
// Each PC used to simulate its own ragdoll for an actor (measured: 18 simulated bodies on both,
// different transforms; the prisoner shot in the intro fell and spun differently on the follower).
// The actor's owner streams every ragdoll body (relative to the actor) at 20 Hz while physics owns
// the skeleton and the bodies move, and sends the settled pose afterwards (again every 5 s for
// late arrivals). Other PCs keyframe ordered bodies through the native rigid-body controller's
// backend. The detached head uses the same controller and the reliable dismember event's tick.
class CorpseRagdollService
{
public:
    CorpseRagdollService(World& aWorld, entt::dispatcher& aDispatcher, TransportService& aTransport) noexcept;
    TP_NOCOPYMOVE(CorpseRagdollService);
    // Called by the Main::Update hook on the main thread: the remote ragdolls are placed there,
    // before the frame's physics step, not from the update job running beside it.
    static void OnMainFrame() noexcept;
    // True until the owner ends its ragdoll stream or authority is released:
    // nothing else may move it (the corpse cell correction reloaded it with MoveTo: naked, then
    // teleported to the owner's final position). Any thread.
    static bool IsFollowingOwner(uint32_t aFormId) noexcept;
    // Testing ground: the actor's ragdoll bodies as JSON (world positions, game units), "[]" if none.
    static std::string DescribeRagdollBodies(Actor* apActor) noexcept;
    static void RecordDismember(Actor* apActor, bool aCreated) noexcept;
    static bool IsDismemberAuthorized(uint32_t aFormId) noexcept;

private:
    struct OwnedRagdoll
    {
        uint64_t SettledSinceMs{};
        uint64_t LastSentMs{};
        bool SentSettled{};
        uint64_t DismemberTick{};
    };
    struct Sample
    {
        uint64_t Tick{};
        float Origin[3]{};
        Vector<CorpseRagdollBody> Bodies;
        bool Settled{};
        bool Dying{};
    };
    struct RemoteRagdoll
    {
        std::array<Sample, 12> Ring{};
        uint32_t RingCount{};
        uint32_t RingNext{};
        bool Asleep{};
        // This copy was knocked into ragdoll to follow the owner's stream, and live placement logged.
        bool Knocked{};
        // The owner is dying (it reported the death), not only knocked down: set when this copy is
        // killed or knocked at the first sample.
        bool OwnerDying{};
        bool LiveLogged{};
        bool CountMismatchLogged{};
        const char* LastSkipReason{};
        uint64_t DismemberTick{};
        uint64_t EndTick{};
        uint64_t LastAppliedMs{};
        uint32_t LocalFormId{};
        Vector<uint32_t> BodyIds;
        Vector<const void*> BodyPointers;
        const void* PhysicsWorld{};
        Vector<uint8_t> MotionTypes;
        // hkaKeyFrameHierarchyUtility::WorkElem, 0x40 bytes per body (IDs 65267/65268).
        struct WorkElem { float Values[16]{}; };
        std::array<WorkElem, CorpseRagdollRequest::kMaxBodies> ControllerState{};
    };
    struct Dismember
    {
        uint64_t Tick{};
        uint64_t RetryAtMs{};
        bool Applied{};
        bool Logged{};
        const void* Root{};
    };

    void OnUpdate(const UpdateEvent&) noexcept;
    void OnDisconnected(const DisconnectedEvent&) noexcept;
    void ResetOnMainFrame() noexcept;
    void OnCorpseRagdoll(const NotifyCorpseRagdoll&) noexcept;
    void OnDismember(const NotifyDismember&) noexcept;
    void ApplyDismembers(uint64_t aNowMs) noexcept;
    void CaptureOwned(uint64_t aNowMs) noexcept;
    void ApplyRemote(uint64_t aNowMs, bool aRelease = false) noexcept;

    World& m_world;
    TransportService& m_transport;
    uint64_t m_nextTickMs{};
    Map<uint64_t, OwnedRagdoll> m_owned;
    Map<uint64_t, RemoteRagdoll> m_remote;
    Map<uint32_t, Dismember> m_dismembers;
    Map<uint32_t, uint64_t> m_sentDismembers;
    std::recursive_mutex m_remoteLock;
    std::atomic<bool> m_applyOnMainFrame{};
    std::atomic<bool> m_disconnectPending{};
    entt::scoped_connection m_updateConnection;
    entt::scoped_connection m_disconnectConnection;
    entt::scoped_connection m_ragdollConnection;
    entt::scoped_connection m_dismemberConnection;
};
