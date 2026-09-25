#pragma once

#include <Events/EventDispatcher.h>
#include <Games/Events.h>
#include <array>
#include <atomic>
#include <mutex>
#include <vector>
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

/**
 * @brief Handles objects in the environment.
 */
class ObjectService final : public BSTEventSink<TESActivateEvent>
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
    // A remote actor at this host position (played-back timeline) that rides a host-driven
    // reference (a cart's driver or passenger) is placed with that reference on the main thread;
    // returns true when the caller must not place it itself. Any thread.
    static bool AttachRider(Actor* apActor, const NiPoint3& acHostPosition, float aHostHeading) noexcept;
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
        bool HasMoved{};
        bool HasBodyState{};
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
        std::array<Sample, 12> Samples{};
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
    Set<uint32_t> m_physicsStreamCandidates;
    std::chrono::steady_clock::time_point m_nextPhysicsSnapshot{};
    std::chrono::steady_clock::time_point m_nextCurrentCellDiscovery{};
    std::chrono::steady_clock::time_point m_nextPhysicsPosePrune{};
    uint32_t m_gridDiscoveryCursor{};
    void ApplyRemotePhysics() noexcept;
    // m_remoteReferencePoses is filled on the VM job thread and played back on the main thread.
    mutable std::recursive_mutex m_remotePhysicsLock;
    std::atomic<bool> m_applyOnMainFrame{};
    // Host: the physics snapshot is read on the main thread (a consistent frame) and sent from
    // the update thread.
    void CaptureHostPhysics(bool aSendNow) noexcept;
    std::atomic<bool> m_captureOnMainFrame{};
    std::vector<PhysicsReferencesMoveRequest> m_pendingPhysicsRequests;
};
