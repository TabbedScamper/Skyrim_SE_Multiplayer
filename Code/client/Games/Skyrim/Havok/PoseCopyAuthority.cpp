#include <Games/Skyrim/Havok/PoseCopyAuthority.h>

#include <World.h>
#include <Components.h>
#include <Games/Skyrim/Actor.h>
#include <Games/Skyrim/BSAnimationGraphManager.h>
#include <Games/Skyrim/Havok/ActorPoseDiagnosticViews.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <mutex>
#include <unordered_map>

namespace
{
using namespace ActorPoseDiagnosticViews;

constexpr size_t kMaxBones = EvaluatedPoseSnapshot::MaxBones;
constexpr size_t kRingSize = 12;
// A captured pose older than this is not sent (the actor is no longer being animated here).
constexpr uint64_t kCaptureFreshMs = 250;

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
    // Owner side.
    std::array<QsTransform, kMaxBones> Captured{};
    uint32_t CapturedCount{};
    uint64_t CapturedAtMs{};
    uint64_t CapturedTick{};
    // Other PCs.
    std::array<Sample, kRingSize> Ring{};
    uint32_t RingCount{};
    uint32_t RingNext{};
};

std::mutex s_lock;
std::unordered_map<const void*, RegistryEntry> s_registry; // key: &graph->boneNodes
std::unordered_map<uint32_t, ActorPose> s_poses;
std::atomic<uint64_t> s_presentationTick{0};
std::atomic<uint64_t> s_currentTick{0};
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

void Interpolate(const Sample& a, const Sample& b, const float t, const uint32_t aCount, QsTransform* apOut) noexcept
{
    for (uint32_t i = 0; i < aCount; ++i)
    {
        const auto& x = a.Bones[i];
        const auto& y = b.Bones[i];
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

void HookCopyPoseToNodes(const QsTransform* apPose, const void* apBoneNodes, uint32_t aCount)
{
    if (!s_enabled.load(std::memory_order_relaxed) || !apPose || !apBoneNodes)
        return RealCopyPoseToNodes(apPose, apBoneNodes, aCount);

    // The native clamps a negative or oversized count to the bone array length (+0x10).
    const auto length = *reinterpret_cast<const int32_t*>(static_cast<const uint8_t*>(apBoneNodes) + 0x10);
    const uint32_t count = (static_cast<int32_t>(aCount) < 0 || static_cast<int32_t>(aCount) > length) ? static_cast<uint32_t>((std::max)(length, 0)) : aCount;
    if (count == 0 || count > kMaxBones)
        return RealCopyPoseToNodes(apPose, apBoneNodes, aCount);

    thread_local std::array<QsTransform, kMaxBones> t_override{};
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
        if (it->second.Kind == Role::Apply && it->second.pActor && PhysicsOwnsSkeleton(it->second.pActor))
            return RealCopyPoseToNodes(apPose, apBoneNodes, aCount);
        auto& pose = s_poses[it->second.FormId];
        if (it->second.Kind == Role::Capture)
        {
            std::copy_n(apPose, count, pose.Captured.begin());
            pose.CapturedCount = count;
            pose.CapturedAtMs = NowMs();
            pose.CapturedTick = s_currentTick.load(std::memory_order_relaxed);
            s_captured.fetch_add(1, std::memory_order_relaxed);
        }
        else
        {
            // Oldest-to-newest view of the ring; bracket the presentation tick.
            const auto size = static_cast<uint32_t>(pose.Ring.size());
            const auto sample = [&](uint32_t i) -> const Sample& { return pose.Ring[(pose.RingNext + size - pose.RingCount + i) % size]; };
            const auto tick = s_presentationTick.load(std::memory_order_relaxed);
            for (uint32_t i = 1; i < pose.RingCount && !useOverride; ++i)
            {
                const auto& a = sample(i - 1);
                const auto& b = sample(i);
                if (tick < a.Tick || tick > b.Tick)
                    continue;
                // The owner may animate fewer bones (its LOD for this actor): drive the bones it
                // sent and leave the rest to the local graph.
                const uint32_t driven = (std::min)({count, a.Count, b.Count});
                if (driven < count)
                {
                    s_countMismatch.fetch_add(1, std::memory_order_relaxed);
                    std::copy_n(apPose, count, t_override.begin());
                }
                const float t = b.Tick > a.Tick ? static_cast<float>(tick - a.Tick) / static_cast<float>(b.Tick - a.Tick) : 1.f;
                Interpolate(a, b, t, driven, t_override.data());
                useOverride = true;
            }
            // Past the newest sample by less than one interval: hold the newest pose.
            if (!useOverride && pose.RingCount)
            {
                const auto& newest = sample(pose.RingCount - 1);
                // Hold the owner pose through sample gaps rather than dropping back to the local
                // graph: alternating owner and local poses read as NPCs jittering to catch up.
                if (tick >= newest.Tick && tick - newest.Tick <= 250)
                {
                    const uint32_t driven = (std::min)(count, newest.Count);
                    if (driven < count)
                        std::copy_n(apPose, count, t_override.begin());
                    std::copy_n(newest.Bones.begin(), driven, t_override.begin());
                    useOverride = true;
                }
            }
            (useOverride ? s_applied : s_fallback).fetch_add(1, std::memory_order_relaxed);
        }
    }
    RealCopyPoseToNodes(useOverride ? t_override.data() : apPose, apBoneNodes, aCount);
}
} // namespace

namespace PoseCopyAuthority
{
void RefreshRegistry(World& aWorld) noexcept
{
    std::unordered_map<const void*, RegistryEntry> registry;
    const auto add = [&](const uint32_t aFormId, const Role aKind)
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
            const auto index = pManager->animationGraphIndex;
            if (count && count <= 32 && index < count)
            {
                auto* pGraph = reinterpret_cast<const uint8_t*>(pManager->animationGraphs.Get(index));
                if (pGraph)
                    registry[pGraph + offsetof(AnimationGraph, boneNodes)] = {aFormId, aKind, pActor};
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
}

uint64_t GetPresentationTick() noexcept
{
    return s_presentationTick.load(std::memory_order_relaxed);
}

uint64_t GetCurrentTick() noexcept
{
    return s_currentTick.load(std::memory_order_relaxed);
}

bool GetCapturedPose(const uint32_t aFormId, EvaluatedPoseSnapshot& arPose) noexcept
{
    std::lock_guard guard(s_lock);
    const auto it = s_poses.find(aFormId);
    if (it == s_poses.end() || !it->second.CapturedCount || NowMs() - it->second.CapturedAtMs > kCaptureFreshMs)
        return false;
    const auto& pose = it->second;
    arPose.SourceTick = pose.CapturedTick;
    arPose.Bones.resize(pose.CapturedCount);
    for (uint32_t i = 0; i < pose.CapturedCount; ++i)
    {
        const auto& source = pose.Captured[i];
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
    {
        std::lock_guard guard(s_lock);
        registered = s_registry.size();
    }
    const auto gaps = s_gapCount.load();
    return fmt::format("\"enabled\":{},\"registered\":{},\"captured\":{},\"applied\":{},\"fallback\":{},\"countMismatch\":{},"
        "\"samples\":{},\"meanGapMs\":{},\"maxGapMs\":{},\"gapsOver150Ms\":{}",
        s_enabled.load() ? "true" : "false", registered, s_captured.load(), s_applied.load(), s_fallback.load(),
        s_countMismatch.load(), s_samples.load(), gaps ? s_gapTotalMs.load() / gaps : 0, s_gapMaxMs.load(), s_gapsOver150.load());
}
} // namespace PoseCopyAuthority

static TiltedPhoques::Initializer s_poseCopyAuthorityHooks(
    []()
    {
        POINTER_SKYRIMSE(TCopyPoseToNodes, copyPoseToNodes, 63856);
        RealCopyPoseToNodes = copyPoseToNodes.Get();
        TP_HOOK(&RealCopyPoseToNodes, HookCopyPoseToNodes);
    });
