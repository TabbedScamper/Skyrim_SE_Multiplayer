#pragma once

#include <Events/EventDispatcher.h>
#include <Games/Events.h>
#include <array>

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
        };
        std::array<Sample, 12> Samples{};
        uint32_t SampleCount{};
        uint32_t SampleNext{};
        uint32_t HostMotionType{3};
        bool HostDriven{};
        // Newest sample tick whose resting pose has been written; later frames skip the write.
        uint64_t AppliedRestTick{};
    };
    std::unordered_map<uint32_t, ReferencePose> m_referencePoses;
    std::unordered_map<uint32_t, RemoteReferencePose> m_remoteReferencePoses;
    Set<uint32_t> m_physicsStreamCandidates;
    std::chrono::steady_clock::time_point m_nextPhysicsSnapshot{};
    std::chrono::steady_clock::time_point m_nextCurrentCellDiscovery{};
    std::chrono::steady_clock::time_point m_nextPhysicsPosePrune{};
    uint32_t m_gridDiscoveryCursor{};
    void ApplyRemotePhysics() noexcept;
};
