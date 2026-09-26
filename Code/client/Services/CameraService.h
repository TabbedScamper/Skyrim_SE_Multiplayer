#pragma once

#include <Structs/CameraStateSnapshot.h>
#include <Messages/ScriptedCameraState.h>
#include <array>
#include <atomic>
#include <mutex>

struct World;
struct TransportService;
struct UpdateEvent;
struct DisconnectedEvent;
struct NotifyCameraState;
struct NotifyScriptedCamera;
struct TESCameraState;

/**
 * Replicates the party leader's evaluated native camera during input-gated
 * cinematics. This is independent of actor and world-object authority.
 */
class CameraService
{
public:
    CameraService(World& aWorld, entt::dispatcher& aDispatcher, TransportService& aTransport) noexcept;

    TP_NOCOPYMOVE(CameraService);

    struct Diagnostic
    {
        bool InputGated{};
        bool HasHostSnapshot{};
        uint8_t LocalStateId{0xFF};
        uint8_t HostStateId{0xFF};
        uint64_t LastHostTick{};
        uint64_t ReceivedPackets{};
        uint64_t NativePostUpdates{};
        uint32_t LastNativeThreadId{};
        size_t HookedVtables{};
    };
    [[nodiscard]] Diagnostic GetDiagnostic() const noexcept;
    struct NativeUpdateSample
    {
        uint64_t TimeMs{};
        uint8_t StateId{0xFF};
        float Before[3]{};
        float After[3]{};
        float BeforeLocal[3]{};
        float AfterLocal[3]{};
        float BeforeParentWorld[3]{};
        float AfterParentWorld[3]{};
        bool ParentStable{};
        bool FirstPersonStateReadable{};
        float PitchBefore{};
        float PitchAfter{};
        float TargetPitchBefore{};
        float TargetPitchAfter{};
        bool FirstPersonObjectStable{};
        float ObjectBefore[3]{};
        float ObjectAfter[3]{};
        bool GraphPitchReadable{};
        float GraphPitchBefore{};
        float GraphPitchAfter{};
    };
    struct NativeUpdateTrace
    {
        std::array<NativeUpdateSample, 64> Samples{};
        size_t Count{};
    };
    [[nodiscard]] NativeUpdateTrace GetNativeUpdateTrace() const noexcept;
    void SetPositionProbeEnabled(bool aEnabled) noexcept;
    [[nodiscard]] bool IsPositionProbeEnabled() const noexcept;
    static void NoteWalkingCameraIdle(uint32_t aFormId, bool aStart) noexcept;

private:
    void OnUpdate(const UpdateEvent& acEvent) noexcept;
    void OnDisconnected(const DisconnectedEvent&) noexcept;
    void OnCameraState(const NotifyCameraState& acMessage) noexcept;
    void OnScriptedCamera(const NotifyScriptedCamera& acMessage) noexcept;
    void UpdateScriptedCamera() noexcept;
    void ApplyScriptedControls(const ScriptedCameraState& acState) noexcept;
    void ReleaseScriptedCamera() noexcept;
    void ApplyWalkingCamera(const ScriptedCameraState& acState) noexcept;
    bool HasScriptedCamera() const noexcept;

    [[nodiscard]] bool IsPresentationBlocked() const noexcept;
    [[nodiscard]] bool Capture(CameraStateSnapshot& aSnapshot) const noexcept;
    void Apply(const CameraStateSnapshot& acSnapshot) noexcept;
    void ApplyAfterNativeUpdate() noexcept;
    void InstallStateUpdateHooks() noexcept;
    void Clear() noexcept;

    using CameraUpdateFn = void (*)(TESCameraState*, void*);
    using CameraRotationFn = void (*)(TESCameraState*, float*);
    static void HookCameraRotation(TESCameraState* apState, float* apRotation) noexcept;
    static void HookCameraUpdate(TESCameraState* apState, void* apNextState) noexcept;
    static CameraService* s_instance;

    World& m_world;
    TransportService& m_transport;
    CameraStateSnapshot m_snapshot{};
    uint64_t m_lastReceivedTick{};
    uint64_t m_nextPublishTick{};
    uint8_t m_appliedStateId{0xFF};
    uint8_t m_lastStateMismatch{0xFF};
    bool m_hasSnapshot{};
    uint64_t m_receivedPackets{};
    std::atomic<uint64_t> m_nativePostUpdates{};
    std::atomic<uint32_t> m_lastNativeThreadId{};
    std::atomic<bool> m_positionProbeEnabled{};
    std::atomic<uint64_t> m_nextNativeTraceMs{};
    mutable std::mutex m_nativeTraceMutex;
    std::array<NativeUpdateSample, 64> m_nativeTrace{};
    size_t m_nativeTraceNext{};
    size_t m_nativeTraceCount{};
    std::array<void*, 13> m_hookedVtables{};
    std::array<CameraUpdateFn, 13> m_originalUpdates{};
    std::array<CameraRotationFn, 13> m_originalRotations{};
    size_t m_hookCount{};

    ScriptedCameraState m_scripted{};
    uint64_t m_scriptedReceivedMs{};
    uint64_t m_scriptedSequence{};
    uint64_t m_scriptedNextSendMs{};
    uint64_t m_scriptedEpoch{};
    uint32_t m_scriptedLeader{};
    bool m_scriptedWasActive{};
    bool m_scriptedControlsHeld{};
    bool m_scriptedReleasePending{};
    uint32_t m_savedCameraControls{};
    uint32_t m_appliedCameraControls{};
    bool m_savedLookHandler{};
    bool m_savedPovHandler{};
    bool m_savedPovScript{};
    bool m_appliedLookHandler{};
    bool m_appliedPovHandler{};
    bool m_appliedPovScript{};
    uint32_t m_walkingEndForm{};
    bool m_walkingCameraHeld{};
    uint64_t m_nextWalkingRetryMs{};
    uint8_t m_scriptedLogState{0xFF};
    uint8_t m_scriptedLogPolicy{0xFF};
    uint8_t m_scriptedLogLocalState{0xFF};

    entt::scoped_connection m_updateConnection;
    entt::scoped_connection m_disconnectedConnection;
    entt::scoped_connection m_cameraStateConnection;
    entt::scoped_connection m_scriptedCameraConnection;
};
