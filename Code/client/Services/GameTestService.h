#pragma once

#include <condition_variable>
#include <deque>
#include <memory>
#include <thread>
#include <atomic>
#include <array>
#include <Games/Events.h>

struct World;

inline constexpr UINT cGameTestWakeMessage = WM_APP + 0x51B;

// Local-only automation bridge. The pipe thread never touches game state;
// requests are executed on Skyrim's window thread through cGameTestWakeMessage.
struct GameTestService : BSTEventSink<TESTriggerEnterEvent>, BSTEventSink<TESTriggerLeaveEvent>
{
    explicit GameTestService(World& aWorld) noexcept;
    ~GameTestService() noexcept;

    TP_NOCOPYMOVE(GameTestService);

    void OnWindowThread() noexcept;
    void OnGameThread() noexcept;
    // Harness calls this only at the native main-loop tail. It uses the same
    // creator/path implementation without the pipe or window-thread executor.
    std::string HarnessDriverTick(const std::string& aRequest = {});
    [[nodiscard]] std::string GetCachedGameSnapshot() const noexcept;
    // Short, explicitly requested window for render bones and native camera tracing.
    [[nodiscard]] static bool IsDiagnosticCaptureArmed() noexcept;

    BSTEventResult OnEvent(const TESTriggerEnterEvent* apEvent,
        const EventDispatcher<TESTriggerEnterEvent>*) override;
    BSTEventResult OnEvent(const TESTriggerLeaveEvent* apEvent,
        const EventDispatcher<TESTriggerLeaveEvent>*) override;

private:
    struct Request
    {
        std::string Line;
        std::string Response;
        bool Complete{false};
        std::mutex Mutex;
        std::condition_variable Completed;
    };

    void PipeMain() noexcept;
    void RecordTriggerEvent(bool aEnter, uint32_t aTriggerFormId,
        uint32_t aActorFormId) noexcept;
    std::string Execute(const std::string& acLine) noexcept;
    void WakeWindowThread() noexcept;

    World& m_world;
    std::atomic_bool m_stopping{false};
    struct TriggerDiagnostic
    {
        uint64_t Sequence{};
        uint64_t TimeMs{};
        uint32_t TriggerFormId{};
        uint32_t ActorFormId{};
        bool Enter{};
    };
    mutable std::mutex m_triggerMutex;
    std::array<TriggerDiagnostic, 64> m_triggerEvents{};
    size_t m_triggerNext{};
    size_t m_triggerCount{};
    uint64_t m_triggerSequence{};
    std::mutex m_queueMutex;
    std::deque<std::shared_ptr<Request>> m_requests;
    std::thread m_pipeThread;
    mutable std::mutex m_snapshotMutex;
    std::string m_gameSnapshot{"null"};
    struct HitchSnapshotData;
    std::shared_ptr<const HitchSnapshotData> m_hitchSnapshot;
    [[nodiscard]] std::string GetHitchSnapshot() const;
    uint64_t m_nextHitchSnapshotMs{};
    std::string m_lastPoseSnapshot{"null"};
    struct TimedGameSnapshot
    {
        uint64_t WorldTick{};
        uint64_t SampleTimeMs{};
        std::string Json;
    };
    std::deque<TimedGameSnapshot> m_recentGameSnapshots;
    uint64_t m_gameSnapshotTimeMs{};
    // Full snapshots are expensive; zero means no pending one-shot capture.
    // Relative captures retain their shared-tick target for paired callers,
    // with a monotonic wall-time fallback across joins/clock rebases.
    // Explicit shared-tick captures are bound to an authority epoch.
    std::mutex m_snapshotScheduleMutex;
    uint64_t m_snapshotTargetTick{};
    uint64_t m_snapshotTargetAuthorityEpoch{};
    uint64_t m_snapshotRelativeDueWallMs{};
    // Zero disables the expensive bone/ragdoll scan. A nonzero shared world
    // tick arms exactly one capture at or after that tick.
    std::atomic<uint64_t> m_poseProbeTargetTick{0};
    // Zero captures the bounded ambient actor set; nonzero captures only the
    // selected actor for a low-skew, shared-tick paired comparison.
    std::atomic<uint32_t> m_poseProbeFormId{0};
    std::atomic<uint64_t> m_lastPoseSampleTick{0};
    std::atomic_bool m_nativeCreatorConfirmRequested{false};
    // Local bridge-only, one-shot fault injection for corpse reconciliation.
    // The pipe/window thread queues the ID; Skyrim's game thread performs it.
    std::atomic<uint32_t> m_testCorpseDisplaceFormId{0};
    bool m_nativeCreatorConfirmComplete{};
    bool m_nativeCreatorConfirmSucceeded{};
    Set<std::string> m_watchedQuests{"MQ101"};
    struct ReferenceMotionStats
    {
        float Position[3]{};
        uint64_t LastMs{};
        float PeakStep{};
        float PeakSpeed{};
        uint32_t LargeSteps{};
        uint32_t Samples{};
    };
    std::array<ReferenceMotionStats, 2> m_introCartMotionStats{};
    // Bounded, game-thread-only trace: relate large world-update gaps to
    // subsequent per-frame cart motion without logging every frame.
    struct HitchMotionEvent
    {
        uint64_t TimeMs{};
        uint64_t WorldTick{};
        uint32_t FormId{}; // zero denotes a gap without a cart step
        uint32_t WorldGapUs{};
        uint32_t VmGapUs{};
        uint32_t PriorVmAppUs{};
        uint32_t PriorVmOriginalUs{};
        uint32_t PriorGameTestUs{};
        uint32_t CartDeltaMs{};
        float CartStep{};
        float Position[3]{};
        bool HorsePresent{};
        float HorsePosition[3]{};
    };
    std::array<HitchMotionEvent, 64> m_hitchMotionEvents{};
    uint32_t m_hitchMotionNext{};
    uint32_t m_hitchMotionCount{};
    struct HitchCartSample
    {
        uint64_t WorldTick{};
        std::array<std::array<float, 3>, 2> Position{};
        std::array<bool, 2> Present{};
        std::array<std::array<float, 3>, 2> HorsePosition{};
        std::array<bool, 2> HorsePresent{};
        // Cart reference angles (radians): a flip shows as x/y leaving ~0.
        std::array<std::array<float, 3>, 2> Rotation{};
        // Horse AI process level (0 high .. 3 low, AIProcess +0x137) and whether it has a character
        // controller (middle-high process +0x250): a horse without one moves on the path with no ground.
        std::array<uint8_t, 2> HorseLevel{0xFF, 0xFF};
        std::array<bool, 2> HorseController{};
        // Controller flags (+0x218), current state (+0x200), supported state (+0x1A0).
        std::array<uint32_t, 2> HorseControllerFlags{};
        std::array<uint32_t, 2> HorseControllerState{0xFF, 0xFF};
        std::array<uint32_t, 2> HorseSupported{0xFF, 0xFF};
        // What the horse controller stands on (+0x2B0 supportBody): motion type (0xFF none), its z (game
        // units), and whether it is the cart's own root body. Owner: horses float when the host looks away.
        std::array<uint32_t, 2> SupportMotion{0xFF, 0xFF};
        std::array<float, 2> SupportZ{};
        std::array<bool, 2> SupportIsCart{};
        std::array<uint64_t, 2> SupportBody{};
        // Horse 3D root node world z and the last native SetPosition input z (reference-phase probe on horse 1),
        // to tell a frozen reference from a frozen scene node.
        std::array<float, 2> HorseNodeZ{};
        std::array<float, 2> HorseCharZ{};
        float ProbeInputZ{};
        // Host camera view: angle (degrees) between the camera's forward axis and each cart / horse, plus FOV.
        // Under ~FOV/2 is on screen. Owner repro: chaos in whatever the host is not looking at.
        std::array<float, 2> CartViewAngle{-1.f, -1.f};
        std::array<float, 2> HorseViewAngle{-1.f, -1.f};
        float CameraFov{};
        // Horse character controller (Muse diag-horsez stage 1): proxy position z (vslot 02, game units), fall
        // timer +0x244, fall start +0x240, step delta z +0x188. Frozen together = controller not stepped.
        std::array<float, 2> CtrlZ{};
        std::array<float, 2> CtrlFallTime{};
        std::array<float, 2> CtrlFallStart{};
        std::array<float, 2> CtrlDeltaZ{};
        uint64_t ProbeCalls{};
    };
    std::array<HitchCartSample, 64> m_hitchCartHistory{};
    uint32_t m_hitchCartNext{};
    uint32_t m_hitchCartCount{};
    bool m_titleSequenceMenuOpen{};
    uint32_t m_titleSequenceMenuTransitions{};
    uint64_t m_lastTitleSequenceTransitionMs{};
};
