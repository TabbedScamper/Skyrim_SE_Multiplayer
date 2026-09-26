#include <TiltedOnlinePCH.h>

#include <Havok/AnimationGraphUpdateTrace.h>
#include <Havok/VisualPoseMailbox.h>
#include <Games/Animation/IAnimationGraphManagerHolder.h>
#include <Games/Skyrim/BSAnimationGraphManager.h>
#include <Games/Skyrim/Havok/ActorPoseDiagnosticViews.h>
#include <Games/Skyrim/NetImmerse/NiAVObject.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <mutex>

namespace
{
struct TraceSlot
{
    std::atomic<uintptr_t> Holder{};
    std::atomic<uint64_t> LastPostCallMs{};
    std::atomic<uint64_t> Calls{};
    std::atomic<uint32_t> ThreadId{};
};

constexpr size_t cSlotCount = 4096;
std::array<TraceSlot, cSlotCount> s_slots{};
std::atomic<uint64_t> s_totalCalls{};
std::atomic<uint64_t> s_lastPostCallMs{};
std::atomic<bool> s_hookRegistered{};
std::atomic<uintptr_t> s_watchedHolder{};
std::atomic<uint64_t> s_watchedUntilMs{};
std::mutex s_watchedMutex;
std::atomic<uint32_t> s_watchedFormId{};
std::atomic<uint64_t> s_nextProbeMs{};
std::atomic<uint32_t> s_poseBoneCount{};
std::atomic<uint32_t> s_renderBoneCount{};
std::atomic<uint32_t> s_validRenderNodeCount{};
std::atomic<uint32_t> s_probeThreadId{};
std::atomic<uint64_t> s_probeLastMs{};
std::atomic<uint64_t> s_poseChecksum{};
std::atomic<uint64_t> s_renderChecksum{};
std::atomic<uint64_t> s_renderWorldChecksum{};
std::atomic<uint64_t> s_probeSamples{};
std::atomic<uint64_t> s_poseChanges{};
std::atomic<uint64_t> s_renderChanges{};
std::atomic<uint64_t> s_renderWorldChanges{};
std::atomic<uint64_t> s_probeDurationUs{};
std::atomic<uint64_t> s_playerWatchGeneration{};
std::atomic<uintptr_t> s_playerPhaseHolder{};
std::atomic<uintptr_t> s_playerFirstPersonState{};
std::atomic<uintptr_t> s_playerCameraObject{};
std::atomic<uint64_t> s_playerWatchUntilMs{};
std::atomic<uint64_t> s_nativeCameraSequence{};
std::mutex s_playerCameraObjectMutex;
std::array<AnimationGraphUpdateTrace::PlayerCameraObjectSample, 64>
    s_playerCameraObjectTrace{};
size_t s_playerCameraObjectNext{};
size_t s_playerCameraObjectCount{};

template <class T> bool ReadNative(const void* apAddress, T& arValue) noexcept
{
    SIZE_T read{};
    return apAddress && ReadProcessMemory(GetCurrentProcess(), apAddress,
        &arValue, sizeof(T), &read) && read == sizeof(T);
}

bool ReadCameraObjectTransform(const void* apState, const void* apObject,
    float (&aLocal)[3], float (&aWorld)[3]) noexcept
{
    const void* pCurrentObject{};
    if (!ReadNative(reinterpret_cast<const uint8_t*>(apState) + 0x58,
            pCurrentObject) || pCurrentObject != apObject)
        return false;
    std::array<NiTransform, 2> transforms{};
    if (!ReadNative(reinterpret_cast<const uint8_t*>(apObject) +
            offsetof(NiAVObject, local), transforms))
        return false;
    const auto& local = transforms[0].translate;
    const auto& world = transforms[1].translate;
    if (!std::isfinite(local.x) || !std::isfinite(local.y) ||
        !std::isfinite(local.z) || !std::isfinite(world.x) ||
        !std::isfinite(world.y) || !std::isfinite(world.z))
        return false;
    aLocal[0] = local.x;
    aLocal[1] = local.y;
    aLocal[2] = local.z;
    aWorld[0] = world.x;
    aWorld[1] = world.y;
    aWorld[2] = world.z;
    return true;
}

struct PendingPlayerCameraObjectSample
{
    AnimationGraphUpdateTrace::PlayerCameraObjectSample Sample{};
    uint64_t Generation{};
    const void* State{};
    const void* Object{};
    bool Active{};
};

PendingPlayerCameraObjectSample BeginPlayerCameraObjectSample(
    const IAnimationGraphManagerHolder* apHolder) noexcept
{
    PendingPlayerCameraObjectSample pending{};
    if (reinterpret_cast<uintptr_t>(apHolder) !=
        s_playerPhaseHolder.load(std::memory_order_acquire))
        return pending;
    pending.Generation = s_playerWatchGeneration.load(std::memory_order_acquire);
    if ((pending.Generation & 1) ||
        GetTickCount64() > s_playerWatchUntilMs.load(std::memory_order_acquire))
        return pending;
    pending.State = reinterpret_cast<const void*>(
        s_playerFirstPersonState.load(std::memory_order_acquire));
    pending.Object = reinterpret_cast<const void*>(
        s_playerCameraObject.load(std::memory_order_acquire));
    if (!pending.State || !pending.Object)
        return pending;
    pending.Sample.StartMs = GetTickCount64();
    pending.Sample.WatchGeneration = pending.Generation;
    pending.Sample.CameraSequenceBefore =
        s_nativeCameraSequence.load(std::memory_order_acquire);
    pending.Sample.ThreadId = GetCurrentThreadId();
    pending.Active = ReadCameraObjectTransform(pending.State, pending.Object,
        pending.Sample.LocalBefore, pending.Sample.WorldBefore) &&
        s_playerWatchGeneration.load(std::memory_order_acquire) ==
            pending.Generation;
    return pending;
}

void FinishPlayerCameraObjectSample(
    PendingPlayerCameraObjectSample& arPending) noexcept
{
    if (!arPending.Active ||
        s_playerWatchGeneration.load(std::memory_order_acquire) !=
            arPending.Generation)
        return;
    arPending.Sample.EndMs = GetTickCount64();
    arPending.Sample.CameraSequenceAfter =
        s_nativeCameraSequence.load(std::memory_order_acquire);
    arPending.Sample.Valid = ReadCameraObjectTransform(arPending.State,
        arPending.Object, arPending.Sample.LocalAfter,
        arPending.Sample.WorldAfter) &&
        s_playerWatchGeneration.load(std::memory_order_acquire) ==
            arPending.Generation;
    if (!arPending.Sample.Valid)
        return;
    std::lock_guard lock(s_playerCameraObjectMutex);
    if (s_playerWatchGeneration.load(std::memory_order_acquire) !=
        arPending.Generation)
        return;
    s_playerCameraObjectTrace[s_playerCameraObjectNext] = arPending.Sample;
    s_playerCameraObjectNext = (s_playerCameraObjectNext + 1) %
        s_playerCameraObjectTrace.size();
    s_playerCameraObjectCount = std::min(s_playerCameraObjectCount + 1,
        s_playerCameraObjectTrace.size());
}

void HashBytes(uint64_t& arHash, const void* apData, size_t aSize) noexcept
{
    const auto* bytes = static_cast<const uint8_t*>(apData);
    for (size_t i = 0; i < aSize; ++i)
    {
        arHash ^= bytes[i];
        arHash *= 1099511628211ULL;
    }
}

void ProbeWatchedHolder(IAnimationGraphManagerHolder* apHolder,
    uint64_t aNow) noexcept
{
    if (s_watchedHolder.load(std::memory_order_relaxed) !=
        reinterpret_cast<uintptr_t>(apHolder) ||
        aNow >= s_watchedUntilMs.load(std::memory_order_relaxed))
        return;
    // Serialize watch replacement with a sample, without stalling graph work
    // behind another diagnostic reader.
    std::unique_lock watchLock(s_watchedMutex, std::try_to_lock);
    if (!watchLock.owns_lock() ||
        s_watchedHolder.load(std::memory_order_relaxed) !=
            reinterpret_cast<uintptr_t>(apHolder) ||
        aNow >= s_watchedUntilMs.load(std::memory_order_relaxed))
        return;
    auto next = s_nextProbeMs.load(std::memory_order_relaxed);
    if (aNow < next || !s_nextProbeMs.compare_exchange_strong(next,
        aNow + 100, std::memory_order_relaxed))
        return;

    const auto started = std::chrono::steady_clock::now();
    BSAnimationGraphManager* pManager{};
    if (!apHolder->GetBSAnimationGraph(&pManager) || !pManager)
        return;

    uint64_t poseHash = 14695981039346656037ULL;
    uint64_t renderHash = 14695981039346656037ULL;
    uint64_t renderWorldHash = 14695981039346656037ULL;
    uint32_t poseCount{};
    uint32_t renderCount{};
    uint32_t validRenderCount{};
    {
        BSScopedLock<BSRecursiveLock> lock(pManager->lock);
        const auto graphCount = pManager->animationGraphs.size;
        const auto graphIndex = pManager->animationGraphIndex;
        if (graphCount > 0 && graphCount <= 32 && graphIndex < graphCount)
        {
            ActorPoseDiagnosticViews::AnimationGraph graph{};
            const auto* pGraph = pManager->animationGraphs.Get(graphIndex);
            if (ReadNative(pGraph, graph))
            {
                const auto count = graph.characterInstance.numPoseLocal;
                if (count > 0 && count <= 128 && graph.characterInstance.poseLocal)
                {
                    std::array<ActorPoseDiagnosticViews::QsTransform, 128> pose{};
                    const auto bytes = static_cast<size_t>(count) *
                        sizeof(ActorPoseDiagnosticViews::QsTransform);
                    SIZE_T read{};
                    if (ReadProcessMemory(GetCurrentProcess(),
                            graph.characterInstance.poseLocal, pose.data(),
                            bytes, &read) && read == bytes)
                    {
                        poseCount = static_cast<uint32_t>(count);
                        HashBytes(poseHash, pose.data(), bytes);
                    }
                }

                if (graph.boneNodes.length > 0 &&
                    graph.boneNodes.length <= 128 &&
                    graph.boneNodes.capacity >= graph.boneNodes.length &&
                    graph.boneNodes.data)
                {
                    std::array<ActorPoseDiagnosticViews::BoneNodeEntry, 128> nodes{};
                    const auto byteCount = static_cast<size_t>(graph.boneNodes.length) *
                        sizeof(ActorPoseDiagnosticViews::BoneNodeEntry);
                    SIZE_T bytesRead{};
                    if (ReadProcessMemory(GetCurrentProcess(), graph.boneNodes.data,
                            nodes.data(), byteCount, &bytesRead) && bytesRead == byteCount)
                    {
                        renderCount = graph.boneNodes.length;
                        for (uint32_t i = 0; i < renderCount; ++i)
                        {
                            const auto* pNode = nodes[i].node;
                            std::array<NiTransform, 2> transforms{};
                            if (!pNode || !ReadNative(
                                    reinterpret_cast<const uint8_t*>(pNode) +
                                        offsetof(NiAVObject, local), transforms))
                                continue;
                            HashBytes(renderHash, &i, sizeof(i));
                            HashBytes(renderHash, &transforms[0], sizeof(NiTransform));
                            HashBytes(renderWorldHash, &i, sizeof(i));
                            HashBytes(renderWorldHash, &transforms[1], sizeof(NiTransform));
                            ++validRenderCount;
                        }
                    }
                }
            }
        }
    }
    pManager->Release();
    if (!poseCount || !renderCount || validRenderCount < renderCount / 2)
        return;
    const auto previousPose = s_poseChecksum.exchange(poseHash,
        std::memory_order_relaxed);
    const auto previousRender = s_renderChecksum.exchange(renderHash,
        std::memory_order_relaxed);
    const auto previousRenderWorld = s_renderWorldChecksum.exchange(
        renderWorldHash, std::memory_order_relaxed);
    if (previousPose && previousPose != poseHash)
        s_poseChanges.fetch_add(1, std::memory_order_relaxed);
    if (previousRender && previousRender != renderHash)
        s_renderChanges.fetch_add(1, std::memory_order_relaxed);
    if (previousRenderWorld && previousRenderWorld != renderWorldHash)
        s_renderWorldChanges.fetch_add(1, std::memory_order_relaxed);
    s_poseBoneCount.store(poseCount, std::memory_order_relaxed);
    s_renderBoneCount.store(renderCount, std::memory_order_relaxed);
    s_validRenderNodeCount.store(validRenderCount, std::memory_order_relaxed);
    s_probeThreadId.store(GetCurrentThreadId(), std::memory_order_relaxed);
    s_probeLastMs.store(aNow, std::memory_order_relaxed);
    s_probeDurationUs.store(static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - started).count()),
        std::memory_order_relaxed);
    s_probeSamples.fetch_add(1, std::memory_order_relaxed);
}

size_t SlotFor(const void* apHolder) noexcept
{
    return (reinterpret_cast<uintptr_t>(apHolder) >> 4) & (cSlotCount - 1);
}

bool IsExecutableAddress(const void* apAddress) noexcept
{
    MEMORY_BASIC_INFORMATION memory{};
    if (!apAddress || !VirtualQuery(apAddress, &memory, sizeof(memory)) ||
        memory.State != MEM_COMMIT)
        return false;
    switch (memory.Protect & 0xFF)
    {
    case PAGE_EXECUTE:
    case PAGE_EXECUTE_READ:
    case PAGE_EXECUTE_READWRITE:
    case PAGE_EXECUTE_WRITECOPY:
        return true;
    default:
        return false;
    }
}

TP_THIS_FUNCTION(TUpdateAnimationGraphManager, bool,
    IAnimationGraphManagerHolder, const void*);
TUpdateAnimationGraphManager* s_realUpdateGraph{};

bool TP_MAKE_THISCALL(HookUpdateAnimationGraphManager,
    IAnimationGraphManagerHolder, const void* apUpdateData)
{
    auto playerCameraObjectSample = BeginPlayerCameraObjectSample(apThis);
    const bool result = TiltedPhoques::ThisCall(
        s_realUpdateGraph, apThis, apUpdateData);
    FinishPlayerCameraObjectSample(playerCameraObjectSample);
    // Observation only. A matching holder does not prove this is the final
    // render-pose update, nor that graph memory can be written here.
    const uint64_t now = GetTickCount64();
    auto& slot = s_slots[SlotFor(apThis)];
    slot.Holder.store(reinterpret_cast<uintptr_t>(apThis), std::memory_order_relaxed);
    slot.LastPostCallMs.store(now, std::memory_order_relaxed);
    slot.ThreadId.store(GetCurrentThreadId(), std::memory_order_relaxed);
    slot.Calls.fetch_add(1, std::memory_order_relaxed);
    s_lastPostCallMs.store(now, std::memory_order_relaxed);
    s_totalCalls.fetch_add(1, std::memory_order_relaxed);
    ProbeWatchedHolder(apThis, now);
    // The watched holder only limits the expensive diagnostic checksum probe.
    // Every remotely owned actor with a published frame needs the same native
    // post-graph opportunity; the mailbox returns immediately for other holders.
    VisualPoseMailbox::InspectPostGraph(apThis);
    return result;
}

TiltedPhoques::Initializer s_graphUpdateTraceInitializer([]()
{
    // CommonLibSSE-NG b93280e: AE UpdateAnimationGraphManager ID 32899.
    POINTER_SKYRIMSE(TUpdateAnimationGraphManager, updateGraph, 32899);
    const auto target = updateGraph.Get();
    if (!IsExecutableAddress(reinterpret_cast<const void*>(target)))
    {
        spdlog::warn("Read-only animation graph update trace not installed: AE ID 32899 did not resolve to executable memory");
        return;
    }
    s_realUpdateGraph = target;
    TP_HOOK(&s_realUpdateGraph, HookUpdateAnimationGraphManager);
    s_hookRegistered.store(true, std::memory_order_relaxed);
    spdlog::info("Registered read-only animation graph update trace at {}",
        reinterpret_cast<const void*>(target));
});
}

void AnimationGraphUpdateTrace::WatchHolder(
    const IAnimationGraphManagerHolder* apHolder, uint32_t aFormId) noexcept
{
    std::lock_guard lock(s_watchedMutex);
    const auto now = GetTickCount64();
    const auto holder = reinterpret_cast<uintptr_t>(apHolder);
    if (holder && aFormId &&
        now < s_watchedUntilMs.load(std::memory_order_relaxed))
    {
        // Preserve the first eligible actor within a capture window.
        if (holder == s_watchedHolder.load(std::memory_order_relaxed) &&
            aFormId == s_watchedFormId.load(std::memory_order_relaxed))
            s_watchedUntilMs.store(now + 5000, std::memory_order_relaxed);
        return;
    }
    s_watchedHolder.store(0, std::memory_order_relaxed);
    s_watchedUntilMs.store(0, std::memory_order_relaxed);
    s_watchedFormId.store(0, std::memory_order_relaxed);
    s_nextProbeMs.store(0, std::memory_order_relaxed);
    s_poseBoneCount.store(0, std::memory_order_relaxed);
    s_renderBoneCount.store(0, std::memory_order_relaxed);
    s_validRenderNodeCount.store(0, std::memory_order_relaxed);
    s_probeThreadId.store(0, std::memory_order_relaxed);
    s_probeLastMs.store(0, std::memory_order_relaxed);
    s_poseChecksum.store(0, std::memory_order_relaxed);
    s_renderChecksum.store(0, std::memory_order_relaxed);
    s_renderWorldChecksum.store(0, std::memory_order_relaxed);
    s_probeSamples.store(0, std::memory_order_relaxed);
    s_poseChanges.store(0, std::memory_order_relaxed);
    s_renderChanges.store(0, std::memory_order_relaxed);
    s_renderWorldChanges.store(0, std::memory_order_relaxed);
    s_probeDurationUs.store(0, std::memory_order_relaxed);
    if (holder && aFormId)
    {
        s_watchedFormId.store(aFormId, std::memory_order_relaxed);
        s_watchedUntilMs.store(now + 5000, std::memory_order_relaxed);
        s_watchedHolder.store(holder, std::memory_order_relaxed);
    }
}

void AnimationGraphUpdateTrace::WatchPlayerCameraObject(
    const IAnimationGraphManagerHolder* apHolder,
    const void* apFirstPersonState, const void* apCameraObject) noexcept
{
    if (!apHolder || !apFirstPersonState || !apCameraObject)
        return;
    const auto now = GetTickCount64();
    if (!(s_playerWatchGeneration.load(std::memory_order_acquire) & 1) &&
        s_playerPhaseHolder.load(std::memory_order_acquire) ==
            reinterpret_cast<uintptr_t>(apHolder) &&
        s_playerFirstPersonState.load(std::memory_order_acquire) ==
            reinterpret_cast<uintptr_t>(apFirstPersonState) &&
        s_playerCameraObject.load(std::memory_order_acquire) ==
            reinterpret_cast<uintptr_t>(apCameraObject) &&
        now <= s_playerWatchUntilMs.load(std::memory_order_acquire))
    {
        s_playerWatchUntilMs.store(now + 5000, std::memory_order_release);
        return;
    }
    s_playerWatchGeneration.fetch_add(1, std::memory_order_acq_rel);
    s_playerPhaseHolder.store(0, std::memory_order_release);
    {
        std::lock_guard lock(s_playerCameraObjectMutex);
        s_playerCameraObjectNext = 0;
        s_playerCameraObjectCount = 0;
    }
    s_playerFirstPersonState.store(
        reinterpret_cast<uintptr_t>(apFirstPersonState), std::memory_order_release);
    s_playerCameraObject.store(
        reinterpret_cast<uintptr_t>(apCameraObject), std::memory_order_release);
    s_playerWatchUntilMs.store(now + 5000,
        std::memory_order_release);
    s_playerPhaseHolder.store(reinterpret_cast<uintptr_t>(apHolder),
        std::memory_order_release);
    s_playerWatchGeneration.fetch_add(1, std::memory_order_acq_rel);
}

void AnimationGraphUpdateTrace::NoteNativeCameraUpdate() noexcept
{
    s_nativeCameraSequence.fetch_add(1, std::memory_order_release);
}

AnimationGraphUpdateTrace::PlayerCameraObjectTrace
AnimationGraphUpdateTrace::GetPlayerCameraObjectTrace() noexcept
{
    PlayerCameraObjectTrace result{};
    std::lock_guard lock(s_playerCameraObjectMutex);
    result.WatchGeneration = s_playerWatchGeneration.load(std::memory_order_acquire);
    result.Count = s_playerCameraObjectCount;
    const size_t start = (s_playerCameraObjectNext +
        s_playerCameraObjectTrace.size() - s_playerCameraObjectCount) %
        s_playerCameraObjectTrace.size();
    for (size_t i = 0; i < result.Count; ++i)
        result.Samples[i] = s_playerCameraObjectTrace[
            (start + i) % s_playerCameraObjectTrace.size()];
    return result;
}

AnimationGraphUpdateTrace::WatchedPoseSample
AnimationGraphUpdateTrace::GetWatchedPoseSample() noexcept
{
    std::lock_guard lock(s_watchedMutex);
    return {s_watchedFormId.load(std::memory_order_relaxed),
        s_poseBoneCount.load(std::memory_order_relaxed),
        s_renderBoneCount.load(std::memory_order_relaxed),
        s_validRenderNodeCount.load(std::memory_order_relaxed),
        s_probeThreadId.load(std::memory_order_relaxed),
        s_probeLastMs.load(std::memory_order_relaxed),
        s_poseChecksum.load(std::memory_order_relaxed),
        s_renderChecksum.load(std::memory_order_relaxed),
        s_renderWorldChecksum.load(std::memory_order_relaxed),
        s_probeSamples.load(std::memory_order_relaxed),
        s_poseChanges.load(std::memory_order_relaxed),
        s_renderChanges.load(std::memory_order_relaxed),
        s_renderWorldChanges.load(std::memory_order_relaxed),
        s_probeDurationUs.load(std::memory_order_relaxed)};
}

AnimationGraphUpdateTrace::HolderSample
AnimationGraphUpdateTrace::GetHolderSample(
    const IAnimationGraphManagerHolder* apHolder) noexcept
{
    if (!apHolder)
        return {};
    const auto& slot = s_slots[SlotFor(apHolder)];
    if (slot.Holder.load(std::memory_order_relaxed) !=
        reinterpret_cast<uintptr_t>(apHolder))
        return {};
    return {slot.LastPostCallMs.load(std::memory_order_relaxed),
        slot.Calls.load(std::memory_order_relaxed),
        slot.ThreadId.load(std::memory_order_relaxed)};
}

uint64_t AnimationGraphUpdateTrace::GetTotalCalls() noexcept
{
    return s_totalCalls.load(std::memory_order_relaxed);
}

uint64_t AnimationGraphUpdateTrace::GetLastPostCallMs() noexcept
{
    return s_lastPostCallMs.load(std::memory_order_relaxed);
}

bool AnimationGraphUpdateTrace::IsHookRegistered() noexcept
{
    return s_hookRegistered.load(std::memory_order_relaxed);
}
