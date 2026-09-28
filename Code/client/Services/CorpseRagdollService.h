#pragma once

#include <Messages/CorpseRagdollRequest.h>

#include <array>
#include <atomic>
#include <mutex>
#include <memory>
#include <deque>
#include <unordered_map>

struct World;
struct TransportService;
struct UpdateEvent;
struct DisconnectedEvent;
struct NotifyCorpseRagdoll;
struct NotifyDismember;

// Owner-authoritative ragdoll synchronization: deaths, knockdowns, shouts, explosions.
// Each PC used to simulate its own ragdoll for an actor (measured: 18 simulated bodies on both,
// different transforms; the prisoner shot in the intro fell and spun differently on the follower).
// The actor's owner streams every ragdoll body (relative to the actor) at nominal 30 Hz while physics owns
// the skeleton and the bodies move, and sends the settled pose afterwards (again every 5 s for
// late arrivals). Other PCs steer dynamic bodies at each physics step. The detached head
// uses the same steering and the reliable dismember event's tick.
class CorpseRagdollService
{
public:
    CorpseRagdollService(World& aWorld, entt::dispatcher& aDispatcher, TransportService& aTransport) noexcept;
    TP_NOCOPYMOVE(CorpseRagdollService);
    // Main::Update resolves graph/body bindings and publishes retained step snapshots.
    // Game-thread death/dismember transitions stay here; placement runs at the physics step.
    static void OnMainFrame() noexcept;
    // IDs 61410 and 61417: before/after the solver, without ECS/graph locks.
    static void OnHavokStep(void* apWorld, float aDeltaTime, bool aAfterStep) noexcept;
    static void InvalidateInstance(void* apInstance) noexcept;
    static void QueueActor(Actor* apActor) noexcept;
    // Bounded POD physics observations, consumed by the harness on the main thread.
    static std::string DrainRagdollCapture() noexcept;
    static void BeginRagdollCapture() noexcept;
    // True until the owner ends its ragdoll stream or authority is released:
    // nothing else may move it (the corpse cell correction reloaded it with MoveTo: naked, then
    // teleported to the owner's final position). Any thread.
    static bool IsFollowingOwner(uint32_t aFormId) noexcept;
    // Testing ground: the actor's ragdoll bodies as JSON (world positions, game units), "[]" if none.
    static std::string DescribeRagdollBodies(Actor* apActor) noexcept;
    static std::string DescribeRagdollRenderPose(Actor* apActor) noexcept;
    static void RecordDismember(Actor* apActor, bool aCreated) noexcept;
    static bool IsDismemberAuthorized(uint32_t aFormId) noexcept;

private:
    struct StepBinding;
    struct StepFrame;
    struct OwnedRagdoll
    {
        uint64_t SettledSinceMs{};
        uint64_t LastSentMs{};
        uint64_t LastTick{};
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
        uint64_t Revision{}, PublishedRevision{};
        // This copy was knocked into ragdoll to follow the owner's stream, and live placement logged.
        bool Knocked{};
        // The owner is dying (it reported the death), not only knocked down: set when this copy is
        // killed or knocked at the first sample.
        bool OwnerDying{};
        bool CountMismatchLogged{};
        const char* LastSkipReason{};
        // Times the local actor reference was moved onto the owner origin while following.
        uint32_t AnchorMoves{};
        uint64_t DismemberTick{};
        uint64_t EndTick{};
        uint64_t RetryTransitionMs{};
        const void* Root{};
        uint32_t LocalFormId{};
        Vector<uint32_t> BodyIds;
        Vector<const void*> BodyPointers;
        const void* PhysicsWorld{};
        Vector<uint8_t> MotionTypes;
        std::shared_ptr<StepBinding> Binding;
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
    size_t m_discoveryCursor{};
    std::deque<entt::entity> m_captureQueue;
    std::unordered_map<entt::entity, uint32_t> m_captureEntities;
    std::deque<uint64_t> m_remoteQueue;
    std::deque<uint32_t> m_dismemberQueue;
    Map<uint64_t, OwnedRagdoll> m_owned;
    Map<uint64_t, RemoteRagdoll> m_remote;
    Map<uint64_t, uint64_t> m_ended; // terminal tick survives binding/actor removal
    Map<uint32_t, Dismember> m_dismembers;
    Map<uint32_t, uint64_t> m_sentDismembers;
    std::recursive_mutex m_remoteLock;
    std::mutex m_stepLock;
    std::shared_ptr<StepFrame> m_stepFrame;
    std::atomic<bool> m_applyOnMainFrame{};
    std::atomic<bool> m_disconnectPending{};
    entt::scoped_connection m_updateConnection;
    entt::scoped_connection m_disconnectConnection;
    entt::scoped_connection m_ragdollConnection;
    entt::scoped_connection m_dismemberConnection;
};
