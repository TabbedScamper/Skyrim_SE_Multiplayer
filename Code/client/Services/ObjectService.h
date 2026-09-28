#pragma once

#include <Events/EventDispatcher.h>
#include <Games/Events.h>
#include <array>
#include <atomic>
#include <mutex>
#include <memory>
#include <vector>
#include <unordered_map>
#include <Messages/PhysicsReferencesMoveRequest.h>

struct ServerTimeSettings;
struct DisconnectedEvent;
struct World;
struct ActivateEvent;
struct TransportService;
struct NotifyActivate;
struct LockChangeEvent;
struct NotifyLockChange;
struct CellChangeEvent;
struct ScriptAnimationEvent;
struct AssignObjectsResponse;
struct NotifyScriptAnimation;
struct UpdateEvent;
struct NotifyPhysicsReferencesMove;
struct TESObjectREFR;

// Pure incremental membership; snapshots iterate contiguous IDs without allocating.
// BEGIN PHYSICS SCAN SET
namespace PhysicsScan
{
struct Set
{
    std::vector<uint32_t> Ids;
    std::unordered_map<uint32_t, size_t> Indices;

    void Insert(uint32_t aId)
    {
        if (Indices.try_emplace(aId, Ids.size()).second)
            Ids.push_back(aId);
    }
    void Erase(uint32_t aId)
    {
        const auto it = Indices.find(aId);
        if (it == Indices.end())
            return;
        const auto index = it->second;
        Ids[index] = Ids.back();
        Indices.find(Ids[index])->second = index;
        Ids.pop_back();
        Indices.erase(it);
    }
    void Clear() { Ids.clear(); Indices.clear(); }
};
struct PlaybackWindow
{
    uint32_t A{}, B{};
    float Fraction{};
    bool Available{}, Hold{};
};
template <class TickAt>
PlaybackWindow SelectPlaybackWindow(uint32_t aCount, double aTime, TickAt&& aTick)
{
    if (!aCount)
        return {};
    PlaybackWindow result{aCount - 1, aCount - 1, 0.f, true, aTime - static_cast<double>(aTick(aCount - 1)) > 300.0};
    if (aTime <= static_cast<double>(aTick(0)))
        result.A = result.B = 0;
    else
        for (uint32_t i = 1; i < aCount; ++i)
        {
            const auto end = aTick(i);
            if (aTime > static_cast<double>(end))
                continue;
            const auto start = aTick(i - 1);
            result.A = i - 1;
            result.B = i;
            result.Fraction = end > start ? static_cast<float>((aTime - static_cast<double>(start)) /
                static_cast<double>(end - start)) : 1.f;
            break;
        }
    return result;
}
inline bool MakeSnapshotRoom(size_t& aRead, size_t& aCount, size_t aCapacity) noexcept
{
    if (aCount != aCapacity)
        return false;
    aRead = (aRead + 1) % aCapacity;
    --aCount;
    return true;
}
template <class Visit>
void VisitRepair(const Set& aSet, size_t& aCursor, size_t aBudget, Visit&& aVisit)
{
    const auto count = aSet.Ids.size() < aBudget ? aSet.Ids.size() : aBudget;
    for (size_t i = 0; i < count; ++i)
        aVisit(aSet.Ids[aCursor++ % aSet.Ids.size()]);
}

// Fixed storage for coalesced native movement notifications. Drain cost depends only
// on occupied slots, not capacity or the total admitted/sleeping reference population.
template <size_t Capacity>
struct MovementQueue
{
    std::array<uint32_t, Capacity> Slots{};
    std::array<size_t, Capacity> Occupied{};
    size_t Count{};
    std::vector<uint32_t> Overflow; // rare growth preserves admission beyond fixed capacity
    bool Insert(uint32_t aId) noexcept
    {
        if (!aId)
            return true;
        size_t slot = (static_cast<uint64_t>(aId) * 2654435761u) % Capacity;
        for (size_t attempts = 0; attempts < Capacity; ++attempts, slot = (slot + 1) % Capacity)
        {
            if (Slots[slot] == aId)
                return true;
            if (!Slots[slot])
            {
                Slots[slot] = aId;
                Occupied[Count++] = slot;
                return true;
            }
        }
        Overflow.push_back(aId);
        return false; // report pressure, but do not lose this movement notification
    }
    template <class F> void Drain(F&& aVisit)
    {
        for (size_t i = 0; i < Count; ++i)
        {
            const auto slot = Occupied[i];
            aVisit(Slots[slot]);
            Slots[slot] = 0;
        }
        Count = 0;
        for (auto id : Overflow)
            aVisit(id);
        Overflow.clear();
    }
};

}
// END PHYSICS SCAN SET

/**
 * @brief Handles objects in the environment.
 */
class ObjectService final : public BSTEventSink<TESActivateEvent>,
    public BSTEventSink<TESObjectLoadedEvent>,
    public BSTEventSink<TESCellAttachDetachEvent>,
    public BSTEventSink<TESMoveAttachDetachEvent>
{
public:
    ObjectService(World&, entt::dispatcher&, TransportService&);
    // Test switch for the exterior-cell handoff of host-driven bodies.
    static void SetCellHandoffEnabled(bool aEnabled) noexcept;
    static void SetRootBodyWriteEnabled(bool aEnabled) noexcept;
    static void SetHostDrivenPlaybackEnabled(bool aEnabled) noexcept;
    static void SetMainFramePlaybackEnabled(bool aEnabled) noexcept;
    static void SetMainFrameCaptureEnabled(bool aEnabled) noexcept;
    static void SetPhysicsStampEnabled(bool aEnabled) noexcept;
    static void SetHermitePlaybackEnabled(bool aEnabled) noexcept;
    static void SetCartSmoothingEnabled(bool aEnabled) noexcept;
    static void SetBodyVelocityEnabled(bool aEnabled) noexcept;
    static void SetVisualLagFrameEnabled(bool aEnabled) noexcept;
    static void SetCartPhysicsEnabled(bool aEnabled) noexcept;
    // Follower cart assemblies: true = replay (bodies keyframed onto the owner pose each physics step),
    // false = the dynamic steer. Toggle for in-run A/B via the cart_replay bridge command.
    static void SetCartReplayEnabled(bool aEnabled) noexcept;
    // Follower cart replay: Hermite curve through owner samples on the unrounded clock (cart_curve switch).
    // Loose host-driven bodies land exactly on the host pose each step (exact_body_drive switch).
    static void SetExactBodyDrive(bool aEnabled) noexcept;
    // Impact hazard bodies (meteor dirt clods): the leader streams them, followers steer their own copies onto them.
    static void OnHazardCreated(uint32_t aFormId) noexcept;
    static void SetHazardSync(bool aEnabled) noexcept;
    static bool IsHazardSync() noexcept;
    static bool IsExactBodyDrive() noexcept;
    static void SetCartCurve(bool aEnabled) noexcept;
    static bool IsCartCurve() noexcept;
    // Per-frame rendered position/heading trace of chosen references (motion_trace). Ids start a trace; dump writes it.
    static std::string MotionTrace(const std::string& aIds, const std::string& aBone, const std::string& aDump) noexcept;
    static bool IsCartReplayEnabled() noexcept;
    // Lock-free: true for a local horse tethered to a cart. Its native position moves keep their Havok sync
    // (HookSetPosition), called from engine threads about once a frame per horse.
    static bool IsTetheredHorse(uint32_t aFormId) noexcept;
    // Expected reference z for a tethered horse (from its character controller), or NaN if unknown.
    static float TetheredHorseExpectedZ(uint32_t aFormId) noexcept;
    // Session ended (disconnect, left party, epoch change): forget published horse heights.
    static void ResetTetheredHorseState() noexcept;
    // Called from actor-process jobs: queue an owned NPC for a main-thread scene refresh (off-camera bones).
    static void QueueActorSceneUpdate(uint32_t aFormId) noexcept;
    // Off-camera refresh mode for live A/B: 0 off, 1 main thread (not seated or riders), 2 main thread (all),
    // 3 inside the actor job (all; deadlock risk, diagnosis only).
    static void SetSceneUpdateMode(uint32_t aMode) noexcept;
    // Horse controller->reference z writeback (float fix), live-switchable for paired in-ride A/B.
    static void SetHorseWriteback(bool aEnabled) noexcept;
    // Owned actors and carts always drawn/updated regardless of the camera (render_all switch, default on).
    static void SetRenderAll(bool aEnabled) noexcept;
    static bool IsRenderAll() noexcept;
    // Host cart scene-node refresh every frame (unproven; paired A/B switch).
    static void SetCartNodeRefresh(bool aEnabled) noexcept;
    static bool IsCartNodeRefresh() noexcept;
    static bool IsHorseWriteback() noexcept;
    static uint32_t GetSceneUpdateMode() noexcept;
    static void ArmRenderDiagnostics() noexcept;
    [[nodiscard]] static bool IsRenderDiagnosticsArmed() noexcept;
    // A remote actor at this host position (played-back timeline) that rides a host-driven
    // reference (a cart's driver or passenger) is placed with that reference on the main thread;
    // returns true when the caller must not place it itself. Any thread.
    static bool AttachRider(Actor* apActor, const NiPoint3& acHostPosition, float aHostHeading) noexcept;
    // Active tether assemblies share a frame clock, including the horse and SetVehicle riders.
    static bool VehiclePresentationTick(Actor* apActor, uint64_t& aTick) noexcept;
    static void QueueVehiclePose(Actor* apActor, uint64_t aTick, const NiPoint3& aPosition,
        const NiPoint3& aRotation, const NiPoint3& aVelocity, const NiPoint3& aAngular) noexcept;
    // Called by the Main::Update hook on the game's main thread, before the frame's jobs.
    static void OnMainFrame() noexcept;
    // After Main::Update (the frame is drawn): end-of-frame probe of host-driven bodies.
    static void OnMainFrameEnd() noexcept;

    struct RemotePhysicsDiagnostic
    {
        NiPoint3 Position{};
        uint64_t Tick{};
        uint64_t AuthorityEpoch{};
        uint64_t AgeMs{};
        bool BodyDriven{};
    };
    [[nodiscard]] bool GetRemotePhysicsDiagnostic(uint32_t aFormId,
        RemotePhysicsDiagnostic& arDiagnostic) const noexcept;

    struct BodyPlaybackDiagnostic
    {
        uint32_t SelectedFormId{};
        uint64_t Attempts{};
        uint64_t Succeeded{};
        uint64_t StaleSkips{};
        uint32_t LastSourceAgeMs{};
        uint32_t LastDurationUs{};
        float LastPreError{};
        float LastPostError{};
        float LastStep{};
    };
    static void SetBodyPlaybackProbe(uint32_t aFormId) noexcept;
    [[nodiscard]] static BodyPlaybackDiagnostic GetBodyPlaybackDiagnostic() noexcept;

    struct ReferencePhaseDiagnostic
    {
        uint32_t SelectedFormId{};
        bool HookInstalled{};
        uint64_t Update3DCalls{};
        uint64_t MoveHavokCalls{};
        uint32_t LastThreadId{};
        uint32_t LastMethod{};
        uint32_t LastDurationUs{};
        float LastReferenceDelta{};
        float LastNodeDelta{};
        float LastBodyDelta{};
        float LastRefBodyError{};
        float LastRefNodeError{};
        uint64_t SetPositionCalls{};
        uint64_t SetPositionRemoteSuppressedCalls{};
        uint64_t SetPositionRemoteOverrideCalls{};
        uint64_t SetPositionCallerRva{};
        uint32_t SetPositionThreadId{};
        uint32_t SetPositionFormType{};
        bool SetPositionSourceIsNodeWorld{};
        float SetPositionInputX{};
        float SetPositionInputY{};
        float SetPositionInputZ{};
        float SetPositionPreReferenceError{};
    };
    static void SetReferencePhaseProbe(uint32_t aFormId) noexcept;
    static void RecordNativeSetPosition(const TESObjectREFR* apReference,
        uint64_t aCallerRva, const NiPoint3* apInput,
        bool aRemote, bool aScopedOverride) noexcept;
    [[nodiscard]] static ReferencePhaseDiagnostic GetReferencePhaseDiagnostic() noexcept;

    struct RenderNodePhaseDiagnostic
    {
        bool HookInstalled{};
        uint64_t DownwardCalls{};
        uint64_t WorldDataCalls{};
        uint64_t TransformBoundsCalls{};
        uint32_t LastMethod{};
        uint32_t LastThreadId{};
        uint32_t LastDurationUs{};
        float LastLocalDelta{};
        float LastWorldDelta{};
        float LastReferenceDelta{};
        float LastRefNodeError{};
        uint64_t WorldDataCallerRva{};
        uint64_t TransformBoundsCallerRva{};
        float WorldDataReferenceDelta{};
        float TransformBoundsReferenceDelta{};
        uint64_t WorldDataTargetRva{};
        uint64_t TransformBoundsTargetRva{};
    };
    [[nodiscard]] static RenderNodePhaseDiagnostic GetRenderNodePhaseDiagnostic() noexcept;

    struct CollisionSyncDiagnostic
    {
        uint64_t SelectedCalls{};
        uint64_t LastCallerRva{};
        uint32_t LastThreadId{};
        uint32_t LastDurationUs{};
        float LastNodeDelta{};
        float LastReferenceDelta{};
        float LastBodyDelta{};
        float LastPreNodeReferenceError{};
        float LastPostNodeReferenceError{};
    };
    [[nodiscard]] static CollisionSyncDiagnostic GetCollisionSyncDiagnostic() noexcept;

    struct CollisionWorldDiagnostic
    {
        uint64_t SelectedCalls{};
        uint64_t LastCallerRva{};
        uint32_t LastThreadId{};
        uint32_t LastDurationUs{};
        float LastInputX{};
        float LastInputY{};
        float LastInputZ{};
        float LastInputBodyError{};
        float LastInputNodeError{};
        float LastPostNodeInputError{};
        float LastPostReferenceInputError{};
    };
    [[nodiscard]] static CollisionWorldDiagnostic GetCollisionWorldDiagnostic() noexcept;

    struct WorldUpdateDiagnostic
    {
        uint64_t Calls{};
        uint32_t LastThreadId{};
        uint32_t LastDurationUs{};
        uint64_t NativeStepCalls{};
        uint32_t NativeStepLastThreadId{};
        uint32_t NativeStepLastDurationUs{};
        uint64_t SelectedBodySteps{};
        uint64_t SelectedBodyChangedSteps{};
        float LastSelectedBodyStepDelta{};
        float PeakSelectedBodyStepDelta{};
        uint64_t SelectedBodyStepsOver75Units{};
        uint64_t PeakSelectedBodyStepTimeMs{};
        float PeakSelectedBodyStepDt{};
        float PeakSelectedBodyPreLinearSpeed{};
        float PeakSelectedBodyPostLinearSpeed{};
        float PeakSelectedBodyPreAngularSpeed{};
        float PeakSelectedBodyPostAngularSpeed{};
        uint32_t PeakSelectedBodyMotionType{};
        bool PeakSelectedBodyTargetApplied{};
        uint32_t PeakSelectedBodyTargetAgeMs{};
        float PeakSelectedBodyVelocityAfterWrite{};
        uint64_t SelectedBodyCacheRefreshes{};
        uint64_t SelectedStepWorldMatches{};
        uint64_t SelectedStepBodyReads{};
        uint64_t SelectedCollisionWorldDuringUpdate{};
        uint64_t SelectedCollisionWorldOutsideUpdate{};
        uint32_t LastSelectedCollisionWorldAfterUpdateUs{};
        uint32_t LastSelectedCollisionWorldAfterStepUs{};
    };
    [[nodiscard]] static WorldUpdateDiagnostic GetWorldUpdateDiagnostic() noexcept;

    struct PreStepPlaybackDiagnostic
    {
        uint32_t SelectedFormId{};
        uint32_t Mode{};
        uint64_t PublishedTargets{};
        uint64_t Attempts{};
        uint64_t Applied{};
        uint64_t StaleSkips{};
        uint32_t LastSourceAgeMs{};
        float LastPreError{};
        float LastPostError{};
        float LastVelocityCorrection{};
        uint64_t PoseWrites{};
        float LastPoseStep{};
        uint64_t HostScans{};
        uint64_t HostPacketsSent{};
        uint64_t HostUpdatesQueued{};
        uint64_t HostBodyOnlyUpdates{};
        uint64_t HostSelectedObserved{};
        uint64_t HostSelectedQueued{};
        uint64_t FollowerPacketsReceived{};
        uint64_t FollowerSelectedReceived{};
        uint32_t LastSelectedTransitAgeMs{};
        uint32_t LastHostScanDurationUs{};
        uint32_t LastHostReferencesVisited{};
        uint32_t LastHostUpdatesQueued{};
        uint64_t HostScanTotalUs{};
        uint32_t HostScanMaxUs{};
        uint32_t HostLastReferencesVisited{};
        uint32_t HostLastCandidateCount{};
        uint32_t HostKnownRefreshLastUs{};
        uint32_t HostKnownRefreshMaxUs{};
        uint64_t HostKnownRefreshTotalUs{};
        uint32_t HostCurrentDiscoveryLastUs{};
        uint32_t HostCurrentDiscoveryMaxUs{};
        uint64_t HostCurrentDiscoveryTotalUs{};
        uint32_t HostGridDiscoveryLastUs{};
        uint32_t HostGridDiscoveryMaxUs{};
        uint64_t HostGridDiscoveryTotalUs{};
        uint32_t HostPruneLastUs{};
        uint32_t HostPruneMaxUs{};
        uint64_t HostPruneTotalUs{};
    };
    static void SetPreStepBodyPlaybackProbe(uint32_t aFormId,
        uint32_t aMode = 1) noexcept;
    [[nodiscard]] static PreStepPlaybackDiagnostic GetPreStepPlaybackDiagnostic() noexcept;

private:
    void OnDisconnected(const DisconnectedEvent&) noexcept;
    void OnCellChange(const CellChangeEvent&) noexcept;
    void OnAssignObjectsResponse(const AssignObjectsResponse&) noexcept;
    void OnActivate(const ActivateEvent&) noexcept;
    void OnActivateNotify(const NotifyActivate&) noexcept;
    void OnLockChange(const LockChangeEvent&) noexcept;
    void OnLockChangeNotify(const NotifyLockChange&) noexcept;
    void OnScriptAnimationEvent(const ScriptAnimationEvent&) noexcept;
    void OnNotifyScriptAnimation(const NotifyScriptAnimation&) noexcept;
    void OnUpdate(const UpdateEvent&) noexcept;
    void OnPhysicsReferencesMove(const NotifyPhysicsReferencesMove&) noexcept;

    BSTEventResult OnEvent(const TESActivateEvent*, const EventDispatcher<TESActivateEvent>*) override;
    BSTEventResult OnEvent(const TESObjectLoadedEvent*, const EventDispatcher<TESObjectLoadedEvent>*) override;
    BSTEventResult OnEvent(const TESCellAttachDetachEvent*, const EventDispatcher<TESCellAttachDetachEvent>*) override;
    BSTEventResult OnEvent(const TESMoveAttachDetachEvent*, const EventDispatcher<TESMoveAttachDetachEvent>*) override;
    void QueuePhysicsRefresh(uint32_t aFormId) noexcept;
    void RefreshPhysicsCandidates() noexcept;
    void RefreshPhysicsDiagnostics() noexcept;
    void RefreshPhysicsReference(uint32_t aFormId) noexcept;
    void FlushPhysicsSnapshots() noexcept;
    void CaptureHazards() noexcept;
    void SendHazardPackets() noexcept;

    entt::entity CreateObjectEntity(const uint32_t acFormId, const uint32_t acServerId) noexcept;

    World& m_world;
    TransportService& m_transport;

    entt::scoped_connection m_disconnectedConnection;
    entt::scoped_connection m_cellChangeConnection;
    entt::scoped_connection m_onActivateConnection;
    entt::scoped_connection m_activateConnection;
    entt::scoped_connection m_lockChangeConnection;
    entt::scoped_connection m_lockChangeNotifyConnection;
    entt::scoped_connection m_assignObjectConnection;
    entt::scoped_connection m_scriptAnimationConnection;
    entt::scoped_connection m_scriptAnimationNotifyConnection;
    entt::scoped_connection m_updateConnection;
    entt::scoped_connection m_physicsMoveConnection;

    struct ReferencePose
    {
        NiPoint3 Position{};
        NiPoint3 Rotation{};
        std::array<float, 16> LastSentBodyTransform{};
        glm::vec3 LastSentBodyVelocity{};
        std::chrono::steady_clock::time_point LastSent{};
        std::chrono::steady_clock::time_point LastActive{};
        bool HasMoved{};
        bool HasBodyState{};
        // Sent as a simulated body before: the followers steer their copy to it, so it keeps
        // being sent when a scene makes it keyframed (the intro carts pulled into place on arrival).
        bool StreamedDynamic{};
        bool Shared{};
        uint8_t MissingChecks{};
        bool Promote{};
        bool Body{};
        bool Assembly{};
        uint32_t Generation{};
        // Reused packet scratch. Child storage is reserved on admission, never in capture.
        PhysicsReferenceUpdate Update;
    };
    struct RemoteReferencePose
    {
        NiPoint3 Position{};
        NiPoint3 PriorPosition{};
        NiPoint3 Rotation{};
        uint64_t Tick{};
        uint64_t PriorTick{};
        uint64_t AuthorityEpoch{};
        std::chrono::steady_clock::time_point LastApplied{};
        std::shared_ptr<void> BodyLifetime;
        uint32_t BodyUid{};
        bool Kinematic{};
        bool BodyDriven{};
        glm::vec3 LinearVelocity{};
        std::array<float, 16> BodyTransform{};
        std::chrono::steady_clock::time_point LastReceived{};

        // Host-driven playback of a moving dynamic body (see kHostDrivenMovingBodies).
        struct Sample
        {
            uint64_t Tick{};
            NiPoint3 Position{};
            NiPoint3 Rotation{};
            // The reference's other bodies (PhysicsReferenceUpdate::ChildBodies).
            std::vector<std::array<float, 7>> Children{};
            // The host body's linear velocity, game units per second.
            NiPoint3 Velocity{};
            // The host body's own pose, Havok units, and rotation (x, y, z, w).
            glm::vec3 BodyPosition{};
            glm::vec4 BodyRotation{0.f, 0.f, 0.f, 1.f};
        };
        // 800 ms of history at the owner frame rate (the main lane captures every frame).
        std::array<Sample, 48> Samples{};
        uint32_t SampleCount{};
        uint32_t SampleNext{};
        uint32_t HostMotionType{3};
        bool HostDriven{};
        // Newest sample tick whose resting pose has been written; later frames skip the write.
        uint64_t AppliedRestTick{};
        // End-of-frame probe: the root and first child as drawn, against what this frame wrote.
        NiMatrix3 ProbeWrittenRotate{};
        glm::vec3 ProbeChild0Written{};
        NiMatrix3 ProbeChild0Rotate{};
        bool ProbeEndArmed{};
        uint32_t ProbeEndFrames{};
        uint32_t ProbeEndMoved{};
        float ProbeEndMoveMax{};
        float ProbeEndTurnMax{};
        float ProbeEndChildMoveMax{};
        float ProbeEndChildTurnMax{};
        // The played-back target this frame before smoothing (riders are matched against it).
        // Dynamic follow (see s_cartPhysicsEnabled): this copy's body is simulated, steered to the host.
        bool DynamicFollow{};
        bool SettledAtFinalPose{};
        bool LoggedFinalPose{};
        // The previous frame's played-back transform: what the node shows (see kVisualLagFrame).
        glm::vec3 PreviousDrawnPosition{};
        glm::vec3 PreviousDrawnRotation{};
        bool HasPreviousDrawn{};
        glm::vec3 PlaybackTarget{};
        float PlaybackHeading{};
        bool HasPlaybackTarget{};
        // The played-back motion this frame (game units per second, radians per second).
        glm::vec3 RenderVelocity{};
        float LastFrameSeconds{};
        glm::vec3 RenderAngular{};
        glm::vec3 LastRendered{};
        glm::vec3 LastRenderedRotation{};
        std::chrono::steady_clock::time_point LastRenderedAt{};
        bool HasLastRendered{};
        // Smoothing of the played-back transform (see kCartSmoothingMs).
        bool SmoothHas{};
        glm::vec3 SmoothPosition{};
        glm::vec3 SmoothRotation{};
        std::chrono::steady_clock::time_point SmoothAt{};
        std::vector<std::array<float, 7>> SmoothChildren{};
        // Jitter probe: what this PC's node shows between two playback writes.
        glm::vec3 ProbeWritten{};
        bool ProbeHas{};
        std::chrono::steady_clock::time_point ProbeLastWrite{};
        std::chrono::steady_clock::time_point ProbeNextLog{};
        uint32_t ProbeFrames{};
        uint32_t ProbeMoved{};
        float ProbeDriftMax{};
        float ProbeSpeedSum{};
        float ProbeSpeedChangeSum{};
        float ProbeSpeedChangeMax{};
        float ProbeLastSpeed{};
        float ProbeDtMaxMs{};
        glm::vec3 ProbeLastRotation{};
        glm::vec3 ProbeLastAngularSpeed{};
        float ProbeAngularChangeSum{};
        float ProbeAngularChangeMax{};
        std::vector<glm::vec3> ProbeChildWritten{};
        uint32_t ProbeChildMoved{};
        float ProbeChildDriftMax{};
        std::string ProbeChildMotion{};
    };
    std::unordered_map<uint32_t, ReferencePose> m_referencePoses;
    std::unordered_map<uint32_t, RemoteReferencePose> m_remoteReferencePoses;
    std::unordered_map<uint32_t, uint32_t> m_ownedPhysicsGenerations;
    std::vector<uint32_t> m_physicsMovingScratch;
    PhysicsScan::Set m_physicsBodies;
    PhysicsScan::Set m_physicsUpdateRefs;
    std::vector<uint32_t> m_physicsPromotions;
    std::array<std::chrono::steady_clock::time_point, 2> m_nextPhysicsSnapshot{};
    struct PhysicsCellScan
    {
        uint32_t FormId{};
        uint32_t Cursor{};
        std::chrono::steady_clock::time_point NextSweep{};
    };
    std::vector<PhysicsCellScan> m_physicsCells;
    size_t m_physicsCellCursor{};
    size_t m_physicsMaintenanceCursor{};
    size_t m_physicsPassiveCursor{};
    uint64_t m_physicsSnapshotEvictions{}, m_physicsMaintenanceReportUs{}, m_physicsRepairReportUs{};
    std::chrono::steady_clock::time_point m_nextPhysicsCells{};
    std::chrono::steady_clock::time_point m_nextOwnedPhysics{};
    std::chrono::steady_clock::time_point m_nextPhysicsMaintenance{};
    uint64_t m_physicsEpoch{};
    bool m_physicsLeader{};
    std::mutex m_physicsEventsLock;
    PhysicsScan::Set m_physicsDirty;
    PhysicsScan::Set m_physicsRefresh;
    // Discovery publishes one reference at a time. Network serialization never holds this lock.
    std::recursive_mutex m_hostPhysicsLock;
    struct CapturedPhysics
    {
        uint32_t FormId{};
        uint32_t Generation{};
        GameId Id{};
        glm::vec3 Position{}, Rotation{}, LinearVelocity{};
        uint8_t MotionType{};
        std::array<float, 16> BodyTransform{};
        std::array<std::array<float, 7>, PhysicsReferenceUpdate::kMaxChildBodies> Children{};
        size_t ChildCount{};
        bool Shared{};
    };
    struct PhysicsSnapshot
    {
        uint64_t Tick{}, Epoch{};
        size_t Count{};
        std::vector<CapturedPhysics> Entries;
    };
    std::array<PhysicsSnapshot, 8> m_physicsSnapshots;
    PhysicsSnapshot m_physicsSending;
    size_t m_physicsSnapshotRead{}, m_physicsSnapshotCount{};
    std::chrono::steady_clock::time_point m_nextPhysicsScanReport{};
    uint64_t m_physicsScanReportTotalUs{}, m_physicsScanReportCount{};
    uint32_t m_physicsScanReportMaxUs{};
    void ApplyRemotePhysics() noexcept;
    // m_remoteReferencePoses is filled on the VM job thread and played back on the main thread.
    mutable std::recursive_mutex m_remotePhysicsLock;
    std::atomic<bool> m_applyOnMainFrame{};
    // Main owns scene/Havok capture: moving bodies plus bounded repair.
    // OnUpdate only serializes captured values; it never traverses native bodies.
    void CaptureHostPhysics(bool aUpdateThread) noexcept;
    std::atomic<bool> m_captureOnMainFrame{};
};
