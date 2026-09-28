#include <Services/SmoothClock.h>
#include <Services/CorpseRagdollService.h>
#include <Services/Generic/HeadTrackService.h>
#include <Games/Skyrim/Havok/PoseCopyAuthority.h>

#include <World.h>
#include <Components.h>
#include <Games/Skyrim/Actor.h>
#include <Games/ActorExtension.h>
#include <Games/Skyrim/BSAnimationGraphManager.h>
#include <Games/Skyrim/Havok/ActorPoseDiagnosticViews.h>
#include <Games/Skyrim/NetImmerse/NiNode.h>

#include <array>
#include <atomic>
#include <bitset>
#include <chrono>
#include <cmath>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

namespace
{
using namespace ActorPoseDiagnosticViews;

constexpr size_t kMaxBones = EvaluatedPoseSnapshot::MaxBones;
constexpr size_t kRingSize = 12;
// A captured pose older than this is not sent (the actor is no longer being animated here).
constexpr uint64_t kCaptureFreshMs = 250;
// An unbracketed living pose must not freeze a distant actor between low-rate packets.
// One pose interval plus jitter (Muse diag-stutter cause 1): 75 ms was shorter than the measured 95 ms owner
// gap, so living copies kept falling back to the local graph and blending back: the visible stutter.
constexpr uint64_t kLivingHoldMs = 150;
constexpr uint64_t kLivingBlendMs = 150;

enum class Role : uint8_t
{
    Capture, // this PC owns the actor
    Apply,   // another PC owns it
};

struct RegistryEntry
{
    uint32_t FormId{};
    Role Kind{};
    const Actor* pActor{};
    std::bitset<kMaxBones> CameraLookBones;
};

// Dying, dead, knocked down or ragdolling (ActorState1 lifeState bits 21-24, knockState 25-27):
// physics owns the skeleton then. Forcing a living owner pose onto a ragdoll spun it around.
bool PhysicsOwnsSkeleton(const Actor* apActor) noexcept
{
    const uint32_t flags1 = apActor->actorState.flags1;
    return ((flags1 >> 21) & 0xF) != 0 || ((flags1 >> 25) & 0x7) != 0;
}

struct Sample
{
    uint64_t Tick{};
    uint32_t Count{};
    std::array<QsTransform, kMaxBones> Bones{};
};

struct ActorPose
{
    // Owner side. The engine can copy a pose onto an actor more than once a frame (graph finalize,
    // then the physics-step pass); only the last copy of a finished frame is published, so a read
    // in between never sends a half-updated pose.
    std::array<QsTransform, kMaxBones> Pending{};
    uint32_t PendingCount{};
    uint64_t PendingFrame{};
    uint64_t PendingTick{};
    uint64_t PendingAtMs{};
    bool PendingLiving{};
    std::array<QsTransform, kMaxBones> Captured{};
    uint32_t CapturedCount{};
    uint64_t CapturedAtMs{};
    uint64_t CapturedTick{};
    // Other PCs: legacy hysteresis for physics transitions; living samples blend back from
    // the local graph after a gap instead of snapping or holding an indefinitely stale pose.
    bool Overriding{};
    uint64_t FreshSinceMs{};
    uint64_t LivingBlendSinceMs{};
    // Other PCs. Retain the newest owner bone values for the physics transition paths.
    // Living actors use their local graph for bones missing from current owner samples.
    std::array<QsTransform, kMaxBones> Held{};
    uint32_t HeldCount{};
    std::array<Sample, kRingSize> Ring{};
    uint32_t RingCount{};
    uint32_t RingNext{};
};

std::mutex s_lock;
std::unordered_map<const void*, RegistryEntry> s_registry; // key: &graph->boneNodes
std::unordered_map<uint32_t, ActorPose> s_poses;
std::atomic<uint64_t> s_presentationTick{0};
std::atomic<uint32_t> s_presentationDelayMs{0};
std::atomic<uint32_t> s_localMirror{0};
std::mutex s_simulatingLock;
std::unordered_map<uint32_t, bool> s_ragdollSimulating;
std::unordered_map<uint32_t, bool> s_ragdollPending;
std::unordered_set<void*> s_controlledDrivers;
struct RagdollRenderPose
{
    void* Driver{};
    QsTransform WorldFromModel{};
    std::vector<QsTransform> Bones;
};
std::unordered_map<const void*, RagdollRenderPose> s_ragdollRenderPoses;
std::unordered_map<void*, const void*> s_ragdollRenderKeys;

// 58291 / 140AC6FB0 normally replaces controller targets and body motion types from the
// local graph. Streamed copies steer dynamic bodies at the native physics step instead.
using TDriveToPose = void(void*, float, void*, void*);
TDriveToPose* RealDriveToPose{};
using TReadRagdollPose = void(void*, void*, void*);
TReadRagdollPose* RealReadRagdollPose{};
using TSetWorldFromModel = void(void*, const QsTransform*);
TSetWorldFromModel* RealSetWorldFromModel{};

// PLANCK PostPostPhysicsHook retains BOTH tracks:
// https://github.com/adamhynek/activeragdoll/blob/master/src/main.cpp
// 58311 / 140ACA440 maps world bodies into locals relative to TRACK_WORLD_FROM_MODEL.
// 63563 / 140BCAB40 applies that root with 63569 / 140BCB640 before copying bones.
// 63581 / 140BCC7E0 can subsequently replace the root from local animation. Keeping
// just the bones then renders the right physics pose in the wrong coordinate frame.
// Keep the native root notification and its matching bone pose together. Do not
// write actor positions or use MoveTo (which can replace the corpse's entire 3D).
void HookSetWorldFromModel(void* apGraph, const QsTransform* apTransform)
{
    QsTransform root{};
    bool controlled = false;
    {
        std::lock_guard lock(s_simulatingLock);
        const auto it = s_ragdollRenderPoses.find(static_cast<uint8_t*>(apGraph) + offsetof(AnimationGraph, boneNodes));
        if (it != s_ragdollRenderPoses.end())
        {
            root = it->second.WorldFromModel;
            controlled = true;
        }
    }
    RealSetWorldFromModel(apGraph, controlled ? &root : apTransform);
}

bool ControlledDriver(void* apDriver)
{
    std::lock_guard lock(s_simulatingLock);
    return s_controlledDrivers.contains(apDriver);
}

void HookDriveToPose(void* apDriver, float aDeltaTime, void* apContext, void* apOutput)
{
    if (ControlledDriver(apDriver))
        return;
    RealDriveToPose(apDriver, aDeltaTime, apContext, apOutput);
}

void HookReadRagdollPose(void* apDriver, void* apContext, void* apOutput)
{
    if (!ControlledDriver(apDriver))
        return RealReadRagdollPose(apDriver, apContext, apOutput);
    // Readback can run inside solver listeners. Never mutate bodies in this phase.
    // 58293 / 140AC8B80 skips physics readback when +CB says every bone belongs to
    // animation, or +C7/+C8 report no controller. Our bodies are steered to the OWNER,
    // so run native mapping back to the rendered skeleton without the local blend-out.
    auto* bytes = static_cast<uint8_t*>(apDriver);
    uint8_t flags[6];
    float fractions[2];
    std::memcpy(flags, bytes + 0xC6, sizeof(flags));
    std::memcpy(fractions, bytes + 0xB4, sizeof(fractions));
    // 58308 / 140ACA000 chooses 64150 / 140BF3ED0 (swept-time sampling) when
    // +1D is set. Exact step placement must read the current body transforms instead.
    const auto asynchronous = bytes[0x1D];
    bytes[0x1D] = 0;
    bytes[0xC6] = 1;
    bytes[0xC7] = 0;
    bytes[0xC8] = 1;
    bytes[0xCA] = 0;
    bytes[0xCB] = 0;
    std::memset(bytes + 0xB4, 0, sizeof(fractions));
    RealReadRagdollPose(apDriver, apContext, apOutput);
    // 63563 / 140BCAB40 copies the mapped pose to the nodes after this call, but
    // 63589 / 140BCF480 can also copy local animation. Keep this physics pose as the
    // sole rendered pose while the stream owns the ragdoll. Otherwise the old
    // RagdollSimulating branch passed every competing animation copy through.
    auto* tracks = *static_cast<uint8_t**>(apOutput);
    auto* character = *reinterpret_cast<uint8_t**>(bytes + 0x80);
    if (tracks && character && bytes[0xCA] && *reinterpret_cast<int32_t*>(tracks + 4) > 2)
    {
        const auto count = *reinterpret_cast<int16_t*>(tracks + 0x32);
        const auto capacity = *reinterpret_cast<int16_t*>(tracks + 0x30);
        const auto offset = *reinterpret_cast<int16_t*>(tracks + 0x34);
        const auto rootOffset = *reinterpret_cast<int16_t*>(tracks + 0x14);
        const auto numBytes = *reinterpret_cast<int32_t*>(tracks);
        if (count > 0 && count <= capacity && count <= kMaxBones && offset >= 0 &&
            rootOffset >= 0 && rootOffset + sizeof(QsTransform) <= static_cast<size_t>((std::max)(numBytes, 0)) &&
            offset + count * sizeof(QsTransform) <= static_cast<size_t>((std::max)(numBytes, 0)))
        {
            const auto* pose = reinterpret_cast<const QsTransform*>(tracks + offset);
            std::lock_guard lock(s_simulatingLock);
            // Removal may invalidate authority while native readback runs.
            if (s_controlledDrivers.contains(apDriver))
            {
                s_ragdollRenderKeys[apDriver] = character + 0xA0;
                auto& rendered = s_ragdollRenderPoses[character + 0xA0]; // graph +160 boneNodes
                if (rendered.Driver != apDriver)
                {
                    spdlog::info("Ragdoll driver {}: current-transform physics readback, {} rendered bones paired with world-from-model (async was {})",
                        fmt::ptr(apDriver), count, asynchronous);
                    rendered.Bones.clear();
                }
                rendered.Driver = apDriver;
                std::memcpy(&rendered.WorldFromModel, tracks + rootOffset, sizeof(QsTransform));
                // Like the native node copy, a short LOD pass leaves other bones alone.
                if (rendered.Bones.size() < static_cast<size_t>(count))
                    rendered.Bones.resize(count);
                std::copy_n(pose, count, rendered.Bones.begin());
            }
        }
    }
    bytes[0x1D] = asynchronous;
    std::memcpy(bytes + 0xC6, flags, sizeof(flags));
    std::memcpy(bytes + 0xB4, fractions, sizeof(fractions));
}

bool RagdollPending(uint32_t aFormId) noexcept
{
    std::lock_guard lock(s_simulatingLock);
    return s_ragdollPending.contains(aFormId);
}

bool RagdollSimulating(uint32_t aFormId) noexcept
{
    std::lock_guard lock(s_simulatingLock);
    return s_ragdollSimulating.contains(aFormId);
}
// Actors whose owner sends fewer bones than this PC's skeleton copies (under s_lock): the bones
// past the owner's count come from its held values, or from the local graph if never sent.
struct ShortPose
{
    uint32_t Count{};
    uint32_t Driven{};
    uint32_t Held{};
    uint64_t Frames{};
};
std::unordered_map<uint32_t, ShortPose> s_shortPoses;
std::atomic<uint64_t> s_currentTick{0};
std::atomic<uint64_t> s_frame{0};
std::atomic<bool> s_enabled{true};
std::atomic<uint64_t> s_captured{0}, s_applied{0}, s_fallback{0}, s_countMismatch{0};
// Owner samples received, and the gap between consecutive samples of one actor (ms).
std::atomic<uint64_t> s_samples{0}, s_gapTotalMs{0}, s_gapCount{0}, s_gapMaxMs{0}, s_gapsOver150{0};

uint64_t NowMs() noexcept
{
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

// hkQsTransform array -> bone nodes (ID 63856).
using TCopyPoseToNodes = void(const QsTransform*, const void*, uint32_t);
TCopyPoseToNodes* RealCopyPoseToNodes = nullptr;

void Interpolate(const QsTransform* a, const QsTransform* b, const float t, const uint32_t aCount, QsTransform* apOut) noexcept
{
    for (uint32_t i = 0; i < aCount; ++i)
    {
        const auto& x = a[i];
        const auto& y = b[i];
        auto& o = apOut[i];
        for (int k = 0; k < 3; ++k)
            o.translation[k] = x.translation[k] + (y.translation[k] - x.translation[k]) * t;
        o.translation[3] = 0.f;
        // Shortest-arc normalized lerp; the native copy normalizes again.
        float dot = 0.f;
        for (int k = 0; k < 4; ++k)
            dot += x.rotation[k] * y.rotation[k];
        const float sign = dot < 0.f ? -1.f : 1.f;
        float norm = 0.f;
        for (int k = 0; k < 4; ++k)
        {
            o.rotation[k] = x.rotation[k] + (sign * y.rotation[k] - x.rotation[k]) * t;
            norm += o.rotation[k] * o.rotation[k];
        }
        norm = norm > 0.f ? 1.f / std::sqrt(norm) : 1.f;
        for (int k = 0; k < 4; ++k)
            o.rotation[k] *= norm;
        for (int k = 0; k < 4; ++k)
            o.scale[k] = y.scale[k];
    }
}

thread_local std::array<QsTransform, kMaxBones> t_override{};

// Bones [aDriven, aCount) the owner's samples do not carry.
void FillUndriven(const ActorPose& acPose, const QsTransform* apLocal, const uint32_t aDriven, const uint32_t aCount, const bool aLiving) noexcept
{
    // A living actor's culled bones must keep animating locally, not hold an old visible pose.
    const uint32_t held = aLiving ? 0u : (std::min)(acPose.HeldCount, aCount);
    for (uint32_t i = aDriven; i < aCount; ++i)
        t_override[i] = i < held ? acPose.Held[i] : apLocal[i];
}

void HookCopyPoseToNodes(const QsTransform* apPose, const void* apBoneNodes, uint32_t aCount)
{
    if (!apPose || !apBoneNodes)
        return RealCopyPoseToNodes(apPose, apBoneNodes, aCount);

    // The native clamps a negative or oversized count to the bone array length (+0x10).
    const auto length = *reinterpret_cast<const int32_t*>(static_cast<const uint8_t*>(apBoneNodes) + 0x10);
    const uint32_t count = (static_cast<int32_t>(aCount) < 0 || static_cast<int32_t>(aCount) > length) ? static_cast<uint32_t>((std::max)(length, 0)) : aCount;
    if (count == 0 || count > kMaxBones)
        return RealCopyPoseToNodes(apPose, apBoneNodes, aCount);

    // Physics bindings, unlike the animation registry, are refreshed every frame.
    // A newly spawned/reloaded corpse must not wait for a registry refresh, and
    // ApplyRemote's temporary simulating=false must not let animation overwrite it.
    bool physicsPose = false;
    {
        std::lock_guard lock(s_simulatingLock);
        const auto rendered = s_ragdollRenderPoses.find(apBoneNodes);
        if (rendered != s_ragdollRenderPoses.end() && !rendered->second.Bones.empty())
        {
            std::copy_n(apPose, count, t_override.begin());
            std::copy_n(rendered->second.Bones.begin(), (std::min)(static_cast<size_t>(count), rendered->second.Bones.size()), t_override.begin());
            physicsPose = true;
        }
    }
    if (physicsPose)
        return RealCopyPoseToNodes(t_override.data(), apBoneNodes, aCount);
    if (!s_enabled.load(std::memory_order_relaxed))
        return RealCopyPoseToNodes(apPose, apBoneNodes, aCount);

    bool useOverride = false;
    {
        std::lock_guard guard(s_lock);
        const auto it = s_registry.find(apBoneNodes);
        if (it == s_registry.end())
            return RealCopyPoseToNodes(apPose, apBoneNodes, aCount);
        // The owner keeps capturing through a ragdoll (the physics-step copy, ID 63563, hands
        // this hook the ragdoll pose), so a follower whose copy is still animated shows the
        // owner's knockdown. A follower whose copy is itself ragdolling leaves it to physics;
        // CorpseRagdollService drives those bodies.
        // Only once this copy's ragdoll is simulating: during a death animation the owner's pose
        // (its death animation) still drives it, so both PCs show the same death.
        // Until physics readback exists, retain the streamed owner skeleton below.
        auto& pose = s_poses[it->second.FormId];
        const bool living = it->second.pActor && !PhysicsOwnsSkeleton(it->second.pActor);
        if (it->second.Kind == Role::Capture)
        {
            const auto frame = s_frame.load(std::memory_order_relaxed);
            if (pose.PendingCount && pose.PendingFrame != frame)
            {
                std::copy_n(pose.Pending.begin(), pose.PendingCount, pose.Captured.begin());
                pose.CapturedCount = pose.PendingCount;
                pose.CapturedTick = pose.PendingTick;
                pose.CapturedAtMs = living ? pose.PendingAtMs : NowMs();
                // Merge partial passes within a living frame only. Otherwise a root-only
                // off-screen copy re-dated all the old full-body bones indefinitely.
                if (living)
                    pose.PendingCount = 0;
            }
            // Merge every copy of the frame bone by bone, as the skeleton itself ends up: some passes
            // copy only the first bone or few (measured: samples of 1 to 5 of 98 bones), and
            // publishing such a pass as the frame's pose left the other PC's bones stale.
            // Physics-owned skeletons also retain unwritten bones across frames.
            std::copy_n(apPose, count, pose.Pending.begin());
            pose.PendingCount = (std::max)(pose.PendingCount, count);
            pose.PendingFrame = frame;
            pose.PendingTick = s_currentTick.load(std::memory_order_relaxed);
            pose.PendingAtMs = NowMs();
            pose.PendingLiving = living;
            s_captured.fetch_add(1, std::memory_order_relaxed);
        }
        else
        {
            const bool pendingRagdoll = RagdollPending(it->second.FormId);
            const bool livingFallback = living && !pendingRagdoll;
            // Oldest-to-newest view of the ring; bracket the presentation tick.
            const auto size = static_cast<uint32_t>(pose.Ring.size());
            const auto sample = [&](uint32_t i) -> const Sample& { return pose.Ring[(pose.RingNext + size - pose.RingCount + i) % size]; };
            // Read the smooth clock at this copy (engine threads call it at their own point in the
            // frame), not the millisecond tick of the last update.
            const double time = PoseCopyAuthority::GetPresentationTimeMs();
            const auto tick = static_cast<uint64_t>(time);
            for (uint32_t i = 1; i < pose.RingCount && !useOverride; ++i)
            {
                const auto& a = sample(i - 1);
                const auto& b = sample(i);
                if (tick < a.Tick || tick > b.Tick)
                    continue;
                // A resumed sample must not interpolate across an entire off-screen interval.
                if (livingFallback && b.Tick - a.Tick > kCaptureFreshMs)
                    continue;
                // The owner may animate fewer bones (its LOD for this actor): drive the bones it
                // sent and leave the rest to the local graph.
                const uint32_t driven = (std::min)({count, a.Count, b.Count});
                if (driven < count)
                {
                    s_countMismatch.fetch_add(1, std::memory_order_relaxed);
                    FillUndriven(pose, apPose, driven, count, livingFallback);
                    auto& shortPose = s_shortPoses[it->second.FormId];
                    shortPose.Count = count;
                    shortPose.Driven = driven;
                    shortPose.Held = livingFallback ? 0u : pose.HeldCount;
                    ++shortPose.Frames;
                }
                const float t = b.Tick > a.Tick ? static_cast<float>((time - static_cast<double>(a.Tick)) /
                    static_cast<double>(b.Tick - a.Tick)) : 1.f;
                Interpolate(a.Bones.data(), b.Bones.data(), t, driven, t_override.data());
                useOverride = true;
            }
            // Briefly hold the newest sample beyond the buffered timeline.
            if (!useOverride && pose.RingCount)
            {
                const auto& newest = sample(pose.RingCount - 1);
                // Hold the owner pose through sample gaps rather than dropping back to the local
                // graph: alternating owner and local poses read as NPCs jittering to catch up.
                if (tick >= newest.Tick && (tick - newest.Tick <=
                    (livingFallback ? kLivingHoldMs : kCaptureFreshMs) || pendingRagdoll))
                {
                    const uint32_t driven = (std::min)(count, newest.Count);
                    if (driven < count)
                        FillUndriven(pose, apPose, driven, count, livingFallback);
                    std::copy_n(newest.Bones.begin(), driven, t_override.begin());
                    useOverride = true;
                }
            }
            // Living actors resume their local graph after the hold expires and blend back.
            // Keep the existing hysteresis for physics transitions.
            const auto nowMs = NowMs();
            if (livingFallback)
            {
                if (!useOverride)
                {
                    pose.Overriding = false;
                    pose.FreshSinceMs = 0;
                    pose.LivingBlendSinceMs = 0;
                }
                else
                {
                    if (!pose.LivingBlendSinceMs)
                        pose.LivingBlendSinceMs = nowMs;
                    const float blend = (std::min)(1.f, static_cast<float>(nowMs - pose.LivingBlendSinceMs) /
                        static_cast<float>(kLivingBlendMs));
                    if (blend < 1.f)
                        Interpolate(apPose, t_override.data(), blend, count, t_override.data());
                    pose.Overriding = true;
                }
            }
            else if (!useOverride)
            {
                pose.Overriding = false;
                pose.FreshSinceMs = 0;
            }
            else if (!pose.Overriding && !pendingRagdoll)
            {
                if (!pose.FreshSinceMs)
                    pose.FreshSinceMs = nowMs;
                if (nowMs - pose.FreshSinceMs >= 1000)
                    pose.Overriding = true;
                else
                    useOverride = false;
            }
            if (useOverride && livingFallback && it->second.CameraLookBones.any() &&
                HeadTrackService::IsCameraTracking(it->second.pActor))
            {
                // The receiving graph evaluated its own camera target. An
                // owner pose (especially first-person/older samples) must not
                // replace that head/neck result. All other bones stay owned.
                for (uint32_t i = 0; i < count; ++i)
                    if (it->second.CameraLookBones.test(i))
                        t_override[i] = apPose[i];
            }
            (useOverride ? s_applied : s_fallback).fetch_add(1, std::memory_order_relaxed);
        }
    }
    RealCopyPoseToNodes(useOverride ? t_override.data() : apPose, apBoneNodes, aCount);
}
} // namespace

namespace PoseCopyAuthority
{
void SetRagdollPending(uint32_t aFormId, bool aPending) noexcept
{
    std::lock_guard lock(s_simulatingLock);
    if (aPending)
        s_ragdollPending[aFormId] = true;
    else
        s_ragdollPending.erase(aFormId);
}

void SetControlledRagdollDrivers(const Vector<void*>& acDrivers) noexcept
{
    std::lock_guard lock(s_simulatingLock);
    s_controlledDrivers.clear();
    s_controlledDrivers.insert(acDrivers.begin(), acDrivers.end());
    for (auto it = s_ragdollRenderPoses.begin(); it != s_ragdollRenderPoses.end();)
        if (std::find(acDrivers.begin(), acDrivers.end(), it->second.Driver) == acDrivers.end())
            it = s_ragdollRenderPoses.erase(it);
        else
            ++it;
}

void SetControlledRagdollDriver(void* apDriver, bool aControlled) noexcept
{
    if (!apDriver) return;
    std::lock_guard lock(s_simulatingLock);
    if (aControlled)
        s_controlledDrivers.insert(apDriver);
    else
    {
        s_controlledDrivers.erase(apDriver);
        // A driver belongs to one graph: event invalidation is O(1), including
        // calls from native removal. Retire its root and bones together.
        if (const auto key = s_ragdollRenderKeys.find(apDriver); key != s_ragdollRenderKeys.end())
        {
            const auto pose = s_ragdollRenderPoses.find(key->second);
            if (pose != s_ragdollRenderPoses.end() && pose->second.Driver == apDriver)
                s_ragdollRenderPoses.erase(pose);
            s_ragdollRenderKeys.erase(key);
        }
    }
}

void ClearRagdollAuthority() noexcept
{
    std::lock_guard lock(s_simulatingLock);
    s_ragdollSimulating.clear();
    s_ragdollPending.clear();
    s_controlledDrivers.clear();
    s_ragdollRenderPoses.clear();
    s_ragdollRenderKeys.clear();
}

void RefreshRegistry(World& aWorld) noexcept
{
    std::unordered_map<const void*, RegistryEntry> registry;
    // aKeyFormId: whose pose this skeleton records or takes (itself, or the mirrored leader).
    const auto add = [&](const uint32_t aFormId, const Role aKind, const uint32_t aKeyFormId = 0)
    {
        auto* pActor = Cast<Actor>(TESForm::GetById(aFormId));
        if (!pActor || !pActor->GetNiNode())
            return;
        BSAnimationGraphManager* pManager{};
        if (!pActor->animationGraphHolder.GetBSAnimationGraph(&pManager) || !pManager)
            return;
        {
            BSScopedLock<BSRecursiveLock> lock(pManager->lock);
            const auto count = pManager->animationGraphs.size;
            // The active player graph can be first-person arms/camera. Remote players and the
            // cutscene body mirror use the third-person skeleton, like SaveAnimationVariables.
            const auto index = aFormId == 0x14 && !PhysicsOwnsSkeleton(pActor) ? 0u : pManager->animationGraphIndex;
            if (count && count <= 32 && index < count)
            {
                auto* pGraph = reinterpret_cast<const uint8_t*>(pManager->animationGraphs.Get(index));
                if (pGraph)
                {
                    registry[pGraph + offsetof(AnimationGraph, boneNodes)] = {aKeyFormId ? aKeyFormId : aFormId, aKind, pActor};
                    auto& entry = registry[pGraph + offsetof(AnimationGraph, boneNodes)];
                    if (aKind == Role::Apply && pActor->GetExtension()->IsRemotePlayer())
                    {
                        static BSFixedString s_head("NPC Head [Head]");
                        static BSFixedString s_neck("NPC Neck [Neck]");
                        auto* pHead = pActor->GetNiNode()->GetByName(s_head);
                        auto* pNeck = pActor->GetNiNode()->GetByName(s_neck);
                        const auto& nodes = reinterpret_cast<const AnimationGraph*>(pGraph)->boneNodes;
                        // ID 63856 / 140BDFA20 uses either a direct node, or
                        // a BSFlattenedBoneTree entry (+130, stride 80, node +70).
                        // Match node identity, never assume skeleton bone indices.
                        for (uint32_t i = 0; nodes.data && i < nodes.length && i < kMaxBones; ++i)
                        {
                            const auto& bone = nodes.data[i];
                            const void* pNode = bone.node;
                            const auto index = static_cast<int32_t>(bone.unk08);
                            if (pNode && index >= 0)
                            {
                                const auto* pEntries = *reinterpret_cast<const uint8_t* const*>(
                                    static_cast<const uint8_t*>(pNode) + 0x130);
                                pNode = pEntries ? *reinterpret_cast<void* const*>(pEntries +
                                    static_cast<size_t>(index) * 0x80 + 0x70) : nullptr;
                            }
                            if (pNode && (pNode == pHead || pNode == pNeck))
                                entry.CameraLookBones.set(i);
                        }
                    }
                }
            }
        }
        pManager->Release();
    };
    auto localView = aWorld.view<FormIdComponent, LocalComponent>();
    for (auto entity : localView)
        add(localView.get<FormIdComponent>(entity).Id, Role::Capture);
    auto remoteView = aWorld.view<FormIdComponent, RemoteComponent>();
    for (auto entity : remoteView)
        add(remoteView.get<FormIdComponent>(entity).Id, Role::Apply);
    // Cutscene follow: this player's skeleton takes the leader's pose (replaces its capture).
    if (const auto mirror = s_localMirror.load(std::memory_order_relaxed))
        add(0x14, Role::Apply, mirror);

    std::lock_guard guard(s_lock);
    s_registry = std::move(registry);
    // Drop poses of actors that left the registry.
    std::unordered_map<uint32_t, bool> live;
    for (const auto& [key, entry] : s_registry)
        live[entry.FormId] = true;
    for (auto it = s_poses.begin(); it != s_poses.end();)
        it = live.contains(it->first) ? std::next(it) : s_poses.erase(it);
}

void SetPresentationTick(const uint64_t aTick) noexcept
{
    s_presentationTick.store(aTick, std::memory_order_relaxed);
}

void SetCurrentTick(const uint64_t aTick) noexcept
{
    s_currentTick.store(aTick, std::memory_order_relaxed);
    s_frame.fetch_add(1, std::memory_order_relaxed);
}

double GetPresentationTimeMs() noexcept
{
    const double now = SmoothClock::NowMs();
    const auto delay = static_cast<double>(s_presentationDelayMs.load(std::memory_order_relaxed));
    if (now <= delay)
        return static_cast<double>(s_presentationTick.load(std::memory_order_relaxed));
    return now - delay;
}

void SetRagdollSimulating(const uint32_t aFormId, const bool aSimulating) noexcept
{
    std::lock_guard lock(s_simulatingLock);
    if (aSimulating)
        s_ragdollSimulating[aFormId] = true;
    else
        s_ragdollSimulating.erase(aFormId);
}

void SetLocalMirror(const uint32_t aSourceFormId) noexcept
{
    if (s_localMirror.exchange(aSourceFormId, std::memory_order_relaxed) == aSourceFormId)
        return;
    // Stop copying immediately on release/source change, including during the 250 ms until
    // the next registry refresh. Never let a stale mirror entry capture or drive this player.
    std::lock_guard guard(s_lock);
    std::erase_if(s_registry, [](const auto& entry) {
        return entry.second.pActor && entry.second.pActor->formID == 0x14;
    });
    s_poses.erase(0x14);
}

void SetPresentationDelayMs(const uint32_t aDelayMs) noexcept
{
    s_presentationDelayMs.store(aDelayMs, std::memory_order_relaxed);
}

uint64_t GetPresentationTick() noexcept
{
    return s_presentationTick.load(std::memory_order_relaxed);
}

uint64_t GetCurrentTick() noexcept
{
    return s_currentTick.load(std::memory_order_relaxed);
}

bool NeedsLocalGraph(const uint32_t aFormId) noexcept
{
    if (RagdollPending(aFormId) || RagdollSimulating(aFormId))
        return false;
    if (!s_enabled.load(std::memory_order_relaxed))
        return true;
    const auto tick = static_cast<uint64_t>(GetPresentationTimeMs());
    std::lock_guard guard(s_lock);
    const auto it = s_poses.find(aFormId);
    if (it == s_poses.end() || !it->second.RingCount)
        return true;
    const auto& pose = it->second;
    const auto sample = [&](uint32_t i) -> const Sample& {
        return pose.Ring[(pose.RingNext + kRingSize - pose.RingCount + i) % kRingSize];
    };
    for (uint32_t i = 1; i < pose.RingCount; ++i)
    {
        const auto& a = sample(i - 1);
        const auto& b = sample(i);
        if (a.Tick <= tick && tick <= b.Tick && b.Tick - a.Tick <= kCaptureFreshMs)
            return false;
    }
    const auto& newest = sample(pose.RingCount - 1);
    return tick < newest.Tick || tick - newest.Tick > kLivingHoldMs;
}

std::string DescribeOverride(const uint32_t aFormId) noexcept
{
    const auto tick = static_cast<uint64_t>(GetPresentationTimeMs());
    std::lock_guard guard(s_lock);
    const auto it = s_poses.find(aFormId);
    if (it == s_poses.end() || !it->second.RingCount)
        return "no owner samples";
    const auto& pose = it->second;
    const auto& newest = pose.Ring[(pose.RingNext + kRingSize - 1) % kRingSize];
    return fmt::format("overriding {} samples {} newest {} ms {} presentation, bones {}", pose.Overriding, pose.RingCount,
        tick >= newest.Tick ? tick - newest.Tick : newest.Tick - tick, tick >= newest.Tick ? "behind" : "ahead of",
        newest.Count);
}

bool GetCapturedPose(const uint32_t aFormId, EvaluatedPoseSnapshot& arPose) noexcept
{
    std::lock_guard guard(s_lock);
    const auto it = s_poses.find(aFormId);
    if (it == s_poses.end())
        return false;
    const auto& pose = it->second;
    // Publish a finished living frame even if culling means there is no subsequent copy to
    // retire it. Its age is measured from the copy, never from this read or the next copy.
    const bool pending = pose.PendingLiving && pose.PendingCount &&
        pose.PendingFrame != s_frame.load(std::memory_order_relaxed);
    const auto count = pending ? pose.PendingCount : pose.CapturedCount;
    const auto capturedAt = pending ? pose.PendingAtMs : pose.CapturedAtMs;
    if (!count || NowMs() - capturedAt > kCaptureFreshMs)
        return false;
    const auto& bones = pending ? pose.Pending : pose.Captured;
    arPose.SourceTick = pending ? pose.PendingTick : pose.CapturedTick;
    arPose.Bones.resize(count);
    for (uint32_t i = 0; i < count; ++i)
    {
        const auto& source = bones[i];
        auto& target = arPose.Bones[i];
        std::copy_n(source.translation, 3, target.Translation.begin());
        std::copy_n(source.rotation, 4, target.Rotation.begin());
        std::copy_n(source.scale, 3, target.Scale.begin());
    }
    return arPose.IsValid();
}

void PushOwnerSample(const uint32_t aFormId, const EvaluatedPoseSnapshot& acPose, const uint64_t aTick) noexcept
{
    if (acPose.Bones.empty() || acPose.Bones.size() > kMaxBones)
        return;
    std::lock_guard guard(s_lock);
    auto& pose = s_poses[aFormId];
    if (pose.RingCount)
    {
        const auto& newest = pose.Ring[(pose.RingNext + kRingSize - 1) % kRingSize];
        if (aTick <= newest.Tick)
            return;
        const uint64_t gap = aTick - newest.Tick;
        if (gap > kCaptureFreshMs)
            pose.LivingBlendSinceMs = 0;
        if (gap < 5000)
        {
            s_gapTotalMs += gap;
            ++s_gapCount;
            s_gapsOver150 += gap > 150 ? 1 : 0;
            uint64_t previous = s_gapMaxMs.load();
            while (gap > previous && !s_gapMaxMs.compare_exchange_weak(previous, gap)) {}
        }
    }
    ++s_samples;
    auto& sample = pose.Ring[pose.RingNext];
    sample.Tick = aTick;
    sample.Count = static_cast<uint32_t>(acPose.Bones.size());
    for (uint32_t i = 0; i < sample.Count; ++i)
    {
        const auto& source = acPose.Bones[i];
        auto& target = sample.Bones[i];
        std::copy(source.Translation.begin(), source.Translation.end(), target.translation);
        target.translation[3] = 0.f;
        std::copy(source.Rotation.begin(), source.Rotation.end(), target.rotation);
        std::copy(source.Scale.begin(), source.Scale.end(), target.scale);
        target.scale[3] = 0.f;
    }
    std::copy_n(sample.Bones.begin(), sample.Count, pose.Held.begin());
    pose.HeldCount = (std::max)(pose.HeldCount, sample.Count);
    pose.RingNext = (pose.RingNext + 1) % kRingSize;
    pose.RingCount = (std::min)(pose.RingCount + 1, static_cast<uint32_t>(kRingSize));
}

void SetEnabled(const bool aEnabled) noexcept
{
    s_enabled.store(aEnabled, std::memory_order_release);
}

bool IsEnabled() noexcept
{
    return s_enabled.load(std::memory_order_acquire);
}

std::string StatsJson() noexcept
{
    size_t registered = 0;
    std::string shortPoses;
    {
        std::lock_guard guard(s_lock);
        registered = s_registry.size();
        std::vector<std::pair<uint32_t, ShortPose>> sorted(s_shortPoses.begin(), s_shortPoses.end());
        std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) { return a.second.Frames > b.second.Frames; });
        for (size_t i = 0; i < sorted.size() && i < 10; ++i)
            shortPoses += fmt::format("{}\"{:X}\":\"{}/{} held {} x{}\"", i ? "," : "", sorted[i].first, sorted[i].second.Driven,
                sorted[i].second.Count, sorted[i].second.Held, sorted[i].second.Frames);
    }
    const auto gaps = s_gapCount.load();
    return fmt::format("\"enabled\":{},\"registered\":{},\"captured\":{},\"applied\":{},\"fallback\":{},\"countMismatch\":{},"
        "\"samples\":{},\"meanGapMs\":{},\"maxGapMs\":{},\"gapsOver150Ms\":{},\"shortPoses\":{{{}}}",
        s_enabled.load() ? "true" : "false", registered, s_captured.load(), s_applied.load(), s_fallback.load(),
        s_countMismatch.load(), s_samples.load(), gaps ? s_gapTotalMs.load() / gaps : 0, s_gapMaxMs.load(), s_gapsOver150.load(), shortPoses);
}
} // namespace PoseCopyAuthority

static TiltedPhoques::Initializer s_poseCopyAuthorityHooks(
    []()
    {
        POINTER_SKYRIMSE(TCopyPoseToNodes, copyPoseToNodes, 63856);
        RealCopyPoseToNodes = copyPoseToNodes.Get();
        TP_HOOK(&RealCopyPoseToNodes, HookCopyPoseToNodes);
        POINTER_SKYRIMSE(TDriveToPose, driveToPose, 58291);
        RealDriveToPose = driveToPose.Get();
        TP_HOOK(&RealDriveToPose, HookDriveToPose);
        POINTER_SKYRIMSE(TReadRagdollPose, readRagdollPose, 58293);
        RealReadRagdollPose = readRagdollPose.Get();
        TP_HOOK(&RealReadRagdollPose, HookReadRagdollPose);
        POINTER_SKYRIMSE(TSetWorldFromModel, setWorldFromModel, 63569);
        RealSetWorldFromModel = setWorldFromModel.Get();
        TP_HOOK(&RealSetWorldFromModel, HookSetWorldFromModel);
    });
