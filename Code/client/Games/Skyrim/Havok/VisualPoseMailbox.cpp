#include <TiltedOnlinePCH.h>

#include <Havok/VisualPoseMailbox.h>
#include <Games/Animation/IAnimationGraphManagerHolder.h>
#include <Games/Skyrim/BSAnimationGraphManager.h>
#include <Games/Skyrim/Havok/ActorPoseDiagnosticViews.h>
#include <Games/Skyrim/NetImmerse/NiAVObject.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <memory>
#include <glm/gtc/quaternion.hpp>

namespace
{
struct Frame
{
    uintptr_t Holder{};
    const void* Actor{};
    uint32_t FormId{};
    uint32_t OwnershipEpoch{};
    uint64_t ReceivedAtMs{};
    std::array<std::shared_ptr<const VisualBoneSnapshot>, 4> History{};
    mutable std::atomic<const void*> BoundGraph{};
    mutable std::atomic<const void*> BoundRoot{};
    mutable std::atomic<bool> Revoked{};
};

struct Slot
{
    std::atomic<uintptr_t> Holder{};
    std::atomic<std::shared_ptr<const Frame>> Latest{};
    std::atomic<uintptr_t> StatsHolder{};
    // Cumulative slot-level counters survive owner changes. Owner counters
    // reset on a new holder and are only reported while StatsHolder matches.
    std::atomic<uint64_t> OwnerEvictions{};
    std::atomic<uint64_t> InspectMisses{};
    std::atomic<uint64_t> OwnerPublishCount{};
    std::atomic<uint64_t> OwnerInspectCount{};
    std::atomic<uint64_t> OwnerApplyCount{};
    std::atomic<uint64_t> LastPublishedAtMs{};
    std::atomic<uint64_t> LastInspectedAtMs{};
    std::atomic<uint32_t> LastEligibleBones{};
    std::atomic<uint32_t> LastWrittenBones{};
    std::atomic<uint32_t> LastSkipReason{};
    std::atomic<uint64_t> LastAppliedSourceTick{};
};

constexpr size_t cSlotCount = 4096;
std::array<Slot, cSlotCount> s_slots{};
std::atomic<uint64_t> s_published{};
std::atomic<uint64_t> s_accepted{};
std::atomic<uint64_t> s_stale{};
std::atomic<uint64_t> s_rejectedGraph{};
std::atomic<uint64_t> s_inspected{};
std::atomic<uint32_t> s_lastFormId{};
std::atomic<uint32_t> s_lastEpoch{};
std::atomic<uint32_t> s_lastAgeMs{};
std::atomic<uint32_t> s_lastEligible{};
std::atomic<uint32_t> s_lastErrorMilli{};
std::atomic<uint32_t> s_lastDurationUs{};
std::atomic<bool> s_applyEnabled{};
std::atomic<uint32_t> s_applyFormId{};
std::atomic<uint64_t> s_appliedFrames{};
std::atomic<uint64_t> s_appliedBones{};
std::atomic<uint64_t> s_oldFrameApplySkips{};
std::atomic<uint64_t> s_timelineMisses{};
std::atomic<uint64_t> s_interpolatedFrames{};
std::atomic<uint32_t> s_lastInterpolationSpanMs{};
std::atomic<uint64_t> s_presentationTick{};
std::atomic<uint64_t> s_writeFailures{};
std::atomic<uint32_t> s_lastReadbackErrorMilli{};
std::atomic<uint64_t> s_rootSamples{};
std::atomic<uint32_t> s_lastRootLatestErrorMilli{};
std::atomic<uint32_t> s_lastRootPresentationErrorMilli{};
std::atomic<uint32_t> s_rootDiagnosticFormId{};

size_t SlotFor(const void* apHolder) noexcept
{
    return (reinterpret_cast<uintptr_t>(apHolder) >> 4) & (cSlotCount - 1);
}

template <class T> bool ReadNative(const void* apAddress, T& arValue) noexcept
{
    SIZE_T read{};
    return apAddress && ReadProcessMemory(GetCurrentProcess(), apAddress,
        &arValue, sizeof(T), &read) && read == sizeof(T);
}

// These are live scene nodes in our own process, protected by the graph lock.
// The native graph/root generation and parent checks run before this helper.
// Keep the SEH boundary in a trivial helper so a racing node teardown cannot
// terminate the game while this opt-in presenter is being validated.
bool WriteLocalTransform(NiAVObject* apNode,
    const NiTransform* apTransform) noexcept
{
    __try
    {
        apNode->local = *apTransform;
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

NiTransform InterpolateLocal(const VisualBoneSnapshot::Bone& acOlder,
    const VisualBoneSnapshot::Bone& acNewer, float aAlpha) noexcept
{
    NiTransform result{};
    const auto matrix = [](const VisualBoneSnapshot::Bone& bone)
    {
        return glm::mat3{glm::vec3{bone.Rotation[0], bone.Rotation[1], bone.Rotation[2]},
            glm::vec3{bone.Rotation[3], bone.Rotation[4], bone.Rotation[5]},
            glm::vec3{bone.Rotation[6], bone.Rotation[7], bone.Rotation[8]}};
    };
    const auto olderRotation = glm::normalize(glm::quat_cast(matrix(acOlder)));
    const auto newerRotation = glm::normalize(glm::quat_cast(matrix(acNewer)));
    const auto blended = glm::mat3_cast(glm::normalize(glm::slerp(
        olderRotation, newerRotation, aAlpha)));
    for (size_t row = 0; row < 3; ++row)
        for (size_t column = 0; column < 3; ++column)
            result.rotate.entry[row][column] = blended[row][column];
    result.translate.x = std::lerp(acOlder.Translation[0], acNewer.Translation[0], aAlpha);
    result.translate.y = std::lerp(acOlder.Translation[1], acNewer.Translation[1], aAlpha);
    result.translate.z = std::lerp(acOlder.Translation[2], acNewer.Translation[2], aAlpha);
    result.scale = std::lerp(acOlder.Scale, acNewer.Scale, aAlpha);
    return result;
}
}

void VisualPoseMailbox::SetApplyEnabled(bool aEnabled) noexcept
{
    s_applyEnabled.store(aEnabled, std::memory_order_release);
}

bool VisualPoseMailbox::IsApplyEnabled() noexcept
{
    return s_applyEnabled.load(std::memory_order_acquire);
}

void VisualPoseMailbox::SetApplyFormId(uint32_t aFormId) noexcept
{
    s_applyFormId.store(aFormId, std::memory_order_release);
}

uint32_t VisualPoseMailbox::GetApplyFormId() noexcept
{
    return s_applyFormId.load(std::memory_order_acquire);
}

void VisualPoseMailbox::SetPresentationTick(uint64_t aTick) noexcept
{
    s_presentationTick.store(aTick, std::memory_order_release);
}

void VisualPoseMailbox::SetRootDiagnosticFormId(uint32_t aFormId) noexcept
{
    s_rootSamples.store(0, std::memory_order_relaxed);
    s_lastRootLatestErrorMilli.store(0, std::memory_order_relaxed);
    s_lastRootPresentationErrorMilli.store(0, std::memory_order_relaxed);
    s_rootDiagnosticFormId.store(aFormId, std::memory_order_release);
}

void VisualPoseMailbox::Publish(const IAnimationGraphManagerHolder* apHolder,
    const void* apActor, uint32_t aFormId, uint32_t aOwnershipEpoch,
    uint64_t aLocalGraphDescriptor, const VisualBoneSnapshot& acState) noexcept
{
    if (!apHolder || !apActor || !aFormId || !aOwnershipEpoch ||
        acState.Bones.empty() || !acState.IsValid() ||
        (acState.GraphDescriptor && aLocalGraphDescriptor &&
            acState.GraphDescriptor != aLocalGraphDescriptor))
        return;
    auto frame = std::make_shared<Frame>();
    frame->Holder = reinterpret_cast<uintptr_t>(apHolder);
    frame->Actor = apActor;
    frame->FormId = aFormId;
    frame->OwnershipEpoch = aOwnershipEpoch;
    frame->ReceivedAtMs = GetTickCount64();
    frame->History[0] = std::make_shared<VisualBoneSnapshot>(acState);
    auto& slot = s_slots[SlotFor(apHolder)];
    if (slot.StatsHolder.load(std::memory_order_acquire) != frame->Holder)
    {
        if (slot.Holder.load(std::memory_order_acquire) != 0)
            slot.OwnerEvictions.fetch_add(1, std::memory_order_relaxed);
        slot.OwnerPublishCount.store(0, std::memory_order_relaxed);
        slot.OwnerInspectCount.store(0, std::memory_order_relaxed);
        slot.OwnerApplyCount.store(0, std::memory_order_relaxed);
        slot.LastPublishedAtMs.store(0, std::memory_order_relaxed);
        slot.LastInspectedAtMs.store(0, std::memory_order_relaxed);
        slot.LastEligibleBones.store(0, std::memory_order_relaxed);
        slot.LastWrittenBones.store(0, std::memory_order_relaxed);
        slot.LastSkipReason.store(0, std::memory_order_relaxed);
        slot.LastAppliedSourceTick.store(0, std::memory_order_relaxed);
        slot.StatsHolder.store(frame->Holder, std::memory_order_release);
    }
    if (const auto previous = slot.Latest.load(std::memory_order_acquire);
        previous && previous->Holder == frame->Holder &&
        previous->Actor == apActor && previous->OwnershipEpoch == aOwnershipEpoch &&
        previous->History[0] &&
        previous->History[0]->GraphDescriptor == acState.GraphDescriptor &&
        previous->History[0]->Bones.size() == acState.Bones.size() &&
        previous->History[0]->SourceTick < acState.SourceTick)
    {
        for (size_t i = 1; i < frame->History.size(); ++i)
            frame->History[i] = previous->History[i - 1];
    }
    slot.Holder.store(frame->Holder, std::memory_order_release);
    slot.LastPublishedAtMs.store(frame->ReceivedAtMs, std::memory_order_relaxed);
    slot.OwnerPublishCount.fetch_add(1, std::memory_order_relaxed);
    slot.Latest.store(std::move(frame), std::memory_order_release);
    s_published.fetch_add(1, std::memory_order_relaxed);
}

void VisualPoseMailbox::Clear(const IAnimationGraphManagerHolder* apHolder) noexcept
{
    if (!apHolder)
        return;
    auto& slot = s_slots[SlotFor(apHolder)];
    if (slot.Holder.load(std::memory_order_acquire) ==
        reinterpret_cast<uintptr_t>(apHolder))
    {
        if (const auto frame = slot.Latest.load(std::memory_order_acquire))
            frame->Revoked.store(true, std::memory_order_release);
        slot.Latest.store({}, std::memory_order_release);
        slot.Holder.store(0, std::memory_order_release);
        slot.StatsHolder.store(0, std::memory_order_release);
    }
}

void VisualPoseMailbox::InspectPostGraph(
    IAnimationGraphManagerHolder* apHolder) noexcept
{
    auto& slot = s_slots[SlotFor(apHolder)];
    if (slot.Holder.load(std::memory_order_acquire) !=
        reinterpret_cast<uintptr_t>(apHolder))
    {
        slot.InspectMisses.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    const auto frame = slot.Latest.load(std::memory_order_acquire);
    if (!frame || frame->Holder != reinterpret_cast<uintptr_t>(apHolder))
    {
        slot.InspectMisses.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    const auto selectedFormId = s_applyFormId.load(std::memory_order_acquire);
    const bool applyFreshFrame = s_applyEnabled.load(std::memory_order_acquire) &&
        selectedFormId && frame->FormId == selectedFormId;
    const bool diagnosticRootSelected = frame->FormId != 0 &&
        frame->FormId == s_rootDiagnosticFormId.load(std::memory_order_acquire);
    // Disabled playback must not inspect every remote skeleton on every
    // graph callback. Keep the explicit selected root diagnostic available.
    if (!applyFreshFrame && !diagnosticRootSelected)
        return;
    const auto started = std::chrono::steady_clock::now();

    const uint64_t now = GetTickCount64();
    const uint64_t age = now >= frame->ReceivedAtMs ?
        now - frame->ReceivedAtMs : 0;
    const auto record = [&](VisualPoseMailbox::SkipReason aReason,
                            uint32_t aEligible = 0, uint32_t aWritten = 0,
                            uint64_t aAppliedTick = 0)
    {
        if (slot.Holder.load(std::memory_order_acquire) != frame->Holder ||
            slot.StatsHolder.load(std::memory_order_acquire) != frame->Holder)
            return;
        slot.OwnerInspectCount.fetch_add(1, std::memory_order_relaxed);
        if (aReason == VisualPoseMailbox::SkipReason::Applied)
            slot.OwnerApplyCount.fetch_add(1, std::memory_order_relaxed);
        slot.LastInspectedAtMs.store(now, std::memory_order_relaxed);
        slot.LastEligibleBones.store(aEligible, std::memory_order_relaxed);
        slot.LastWrittenBones.store(aWritten, std::memory_order_relaxed);
        slot.LastSkipReason.store(static_cast<uint32_t>(aReason),
            std::memory_order_relaxed);
        if (aAppliedTick)
            slot.LastAppliedSourceTick.store(aAppliedTick,
                std::memory_order_relaxed);
    };
    if (age > 1500 && !s_applyEnabled.load(std::memory_order_acquire))
    {
        s_stale.fetch_add(1, std::memory_order_relaxed);
        record(VisualPoseMailbox::SkipReason::StaleFrame);
        return;
    }
    // Only the selected actor has host-pose ownership in this trial. A late
    // packet holds the last authoritative pose until an explicit clear or
    // ownership change instead of silently reverting to local animation.
    const auto presentationTick = s_presentationTick.load(std::memory_order_acquire);
    const VisualBoneSnapshot* pOlder = nullptr;
    const VisualBoneSnapshot* pNewer = nullptr;
    float alpha = 1.f;
    uint32_t interpolationSpanMs = 0;
    // Resolve host presentation without writes only for an explicitly
    // selected read-only root diagnostic.
    if ((applyFreshFrame || diagnosticRootSelected) && presentationTick &&
        frame->History[0])
    {
        const auto& newest = *frame->History[0];
        if (presentationTick >= newest.SourceTick)
            pOlder = pNewer = &newest;
        else
        {
            for (size_t i = 1; i < frame->History.size(); ++i)
            {
                const auto& older = frame->History[i];
                const auto& newer = frame->History[i - 1];
                if (!older || !newer || older->SourceTick > presentationTick ||
                    newer->SourceTick < presentationTick)
                    continue;
                const auto span = newer->SourceTick - older->SourceTick;
                if (!span || span > 500)
                    break;
                pOlder = older.get();
                pNewer = newer.get();
                alpha = std::clamp(static_cast<float>(presentationTick - older->SourceTick) /
                    static_cast<float>(span), 0.f, 1.f);
                interpolationSpanMs = static_cast<uint32_t>(span);
                break;
            }
        }
        if (!pOlder)
            s_timelineMisses.fetch_add(1, std::memory_order_relaxed);
        if (!pOlder && applyFreshFrame)
        {
            for (auto it = frame->History.rbegin(); it != frame->History.rend(); ++it)
            {
                if (*it)
                {
                    pOlder = pNewer = it->get();
                    break;
                }
            }
        }
    }
    const auto& inspectedState = *frame->History[0];

    BSAnimationGraphManager* pManager{};
    if (!apHolder->GetBSAnimationGraph(&pManager) || !pManager)
    {
        record(VisualPoseMailbox::SkipReason::GraphUnavailable);
        return;
    }

    uint32_t eligible{};
    uint32_t written{};
    float maxError{};
    float maxReadbackError{};
    bool graphValid = false;
    {
        BSScopedLock<BSRecursiveLock> lock(pManager->lock);
        const auto graphCount = pManager->animationGraphs.size;
        const auto graphIndex = pManager->animationGraphIndex;
        if (graphCount > 0 && graphCount <= 32 && graphIndex < graphCount)
        {
            ActorPoseDiagnosticViews::AnimationGraph graph{};
            const auto* pGraph = pManager->animationGraphs.Get(graphIndex);
            if (ReadNative(pGraph, graph) && graph.holder == frame->Actor &&
                graph.rootNode &&
                graph.boneNodes.length == inspectedState.Bones.size() &&
                graph.boneNodes.length <= VisualBoneSnapshot::MaxBones &&
                graph.boneNodes.capacity >= graph.boneNodes.length &&
                graph.boneNodes.data)
            {
                NiTransform localRootWorld{};
                if (diagnosticRootSelected && inspectedState.RootWorld.Present &&
                    ReadNative(reinterpret_cast<const uint8_t*>(graph.rootNode) +
                        offsetof(NiAVObject, world), localRootWorld))
                {
                    const auto rootErrorMilli = [&](const VisualBoneSnapshot::Bone& acRoot)
                    {
                        const auto dx = static_cast<double>(acRoot.Translation[0]) -
                            localRootWorld.translate.x;
                        const auto dy = static_cast<double>(acRoot.Translation[1]) -
                            localRootWorld.translate.y;
                        const auto dz = static_cast<double>(acRoot.Translation[2]) -
                            localRootWorld.translate.z;
                        return static_cast<uint32_t>((std::min)(
                            std::sqrt(dx * dx + dy * dy + dz * dz) * 1000.0,
                            static_cast<double>(UINT32_MAX)));
                    };
                    s_lastRootLatestErrorMilli.store(rootErrorMilli(
                        inspectedState.RootWorld), std::memory_order_relaxed);
                    if (pOlder && pNewer && pOlder->RootWorld.Present &&
                        pNewer->RootWorld.Present)
                    {
                        const auto target = InterpolateLocal(pOlder->RootWorld,
                            pNewer->RootWorld, alpha);
                        VisualBoneSnapshot::Bone presentedRoot{};
                        presentedRoot.Translation = {target.translate.x,
                            target.translate.y, target.translate.z};
                        s_lastRootPresentationErrorMilli.store(rootErrorMilli(
                            presentedRoot), std::memory_order_relaxed);
                    }
                    else
                        s_lastRootPresentationErrorMilli.store(0,
                            std::memory_order_relaxed);
                    s_rootSamples.fetch_add(1, std::memory_order_relaxed);
                }
                const void* expectedGraph{};
                frame->BoundGraph.compare_exchange_strong(expectedGraph, pGraph,
                    std::memory_order_relaxed);
                const void* expectedRoot{};
                frame->BoundRoot.compare_exchange_strong(expectedRoot, graph.rootNode,
                    std::memory_order_relaxed);
                const bool sameGeneration =
                    frame->BoundGraph.load(std::memory_order_relaxed) == pGraph &&
                    frame->BoundRoot.load(std::memory_order_relaxed) == graph.rootNode;
                std::array<ActorPoseDiagnosticViews::BoneNodeEntry,
                    VisualBoneSnapshot::MaxBones> nodes{};
                const auto byteCount = static_cast<size_t>(graph.boneNodes.length) *
                    sizeof(ActorPoseDiagnosticViews::BoneNodeEntry);
                SIZE_T read{};
                if (sameGeneration && ReadProcessMemory(GetCurrentProcess(), graph.boneNodes.data,
                        nodes.data(), byteCount, &read) && read == byteCount)
                {
                    graphValid = true;
                    for (uint32_t i = 0; i < graph.boneNodes.length; ++i)
                    {
                        const auto& source = inspectedState.Bones[i];
                        if (!source.Present || !nodes[i].node)
                            continue;
                        NiTransform local{};
                        if (!ReadNative(reinterpret_cast<const uint8_t*>(nodes[i].node) +
                                offsetof(NiAVObject, local), local))
                            continue;
                        for (size_t j = 0; j < 9; ++j)
                            maxError = (std::max)(maxError,
                                std::abs(source.Rotation[j] -
                                    (&local.rotate.entry[0][0])[j]));
                        const float localT[] = {local.translate.x,
                            local.translate.y, local.translate.z};
                        for (size_t j = 0; j < 3; ++j)
                            maxError = (std::max)(maxError,
                                std::abs(source.Translation[j] - localT[j]));
                        maxError = (std::max)(maxError,
                            std::abs(source.Scale - local.scale));
                        ++eligible;

                        if (!applyFreshFrame || !pOlder || !pNewer ||
                            frame->Revoked.load(std::memory_order_acquire))
                            continue;
                        auto* pNode = static_cast<NiAVObject*>(nodes[i].node);
                        NiAVObject* parent{};
                        if (!ReadNative(reinterpret_cast<const uint8_t*>(pNode) +
                                offsetof(NiAVObject, parent), parent) ||
                            (!parent && pNode != graph.rootNode))
                            continue;
                        const auto& olderBone = pOlder->Bones[i];
                        const auto& newerBone = pNewer->Bones[i];
                        if (!olderBone.Present || !newerBone.Present)
                            continue;
                        const NiTransform target = InterpolateLocal(
                            olderBone, newerBone, alpha);
                        const bool rotationFinite = std::all_of(
                            &target.rotate.entry[0][0],
                            &target.rotate.entry[0][0] + 9,
                            [](float value) { return std::isfinite(value); });
                        if (!rotationFinite ||
                            !std::isfinite(target.translate.x) ||
                            !std::isfinite(target.translate.y) ||
                            !std::isfinite(target.translate.z) ||
                            !std::isfinite(target.scale))
                            continue;
                        if (!WriteLocalTransform(pNode, &target))
                        {
                            s_writeFailures.fetch_add(1, std::memory_order_relaxed);
                            continue;
                        }
                        NiTransform readback{};
                        if (ReadNative(reinterpret_cast<const uint8_t*>(pNode) +
                                offsetof(NiAVObject, local), readback))
                        {
                            for (size_t j = 0; j < 9; ++j)
                                maxReadbackError = (std::max)(maxReadbackError,
                                    std::abs((&target.rotate.entry[0][0])[j] -
                                        (&readback.rotate.entry[0][0])[j]));
                        }
                        ++written;
                    }
                }
            }
        }
    }
    pManager->Release();
    if (!graphValid)
    {
        s_rejectedGraph.fetch_add(1, std::memory_order_relaxed);
        record(VisualPoseMailbox::SkipReason::GraphInvalid);
        return;
    }
    s_accepted.fetch_add(1, std::memory_order_relaxed);
    s_inspected.fetch_add(1, std::memory_order_relaxed);
    s_lastFormId.store(frame->FormId, std::memory_order_relaxed);
    s_lastEpoch.store(frame->OwnershipEpoch, std::memory_order_relaxed);
    s_lastAgeMs.store(static_cast<uint32_t>(age), std::memory_order_relaxed);
    s_lastEligible.store(eligible, std::memory_order_relaxed);
    s_lastErrorMilli.store(static_cast<uint32_t>(std::llround(
        (std::min)(maxError, 100000.f) * 1000.f)),
        std::memory_order_relaxed);
    s_lastDurationUs.store(static_cast<uint32_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - started).count()),
        std::memory_order_relaxed);
    if (written)
    {
        if (interpolationSpanMs)
            s_interpolatedFrames.fetch_add(1, std::memory_order_relaxed);
        s_lastInterpolationSpanMs.store(interpolationSpanMs,
            std::memory_order_relaxed);
        s_appliedFrames.fetch_add(1, std::memory_order_relaxed);
        s_appliedBones.fetch_add(written, std::memory_order_relaxed);
        s_lastReadbackErrorMilli.store(static_cast<uint32_t>(std::llround(
            (std::min)(maxReadbackError, 100000.f) * 1000.f)),
            std::memory_order_relaxed);
    }
    const auto reason = written ? VisualPoseMailbox::SkipReason::Applied :
        !s_applyEnabled.load(std::memory_order_relaxed) ?
            VisualPoseMailbox::SkipReason::ApplyDisabled :
        !applyFreshFrame ? VisualPoseMailbox::SkipReason::ReceiptTooOld :
        !pOlder || !pNewer ? VisualPoseMailbox::SkipReason::NoPresentationBracket :
        frame->Revoked.load(std::memory_order_acquire) ?
            VisualPoseMailbox::SkipReason::RevokedFrame :
        !eligible ? VisualPoseMailbox::SkipReason::NoEligibleBones :
            VisualPoseMailbox::SkipReason::NoWritableBones;
    record(reason, eligible, written,
        written && pNewer ? pNewer->SourceTick : 0);
}

VisualPoseMailbox::Inspection VisualPoseMailbox::GetInspection() noexcept
{
    return {s_published.load(std::memory_order_relaxed),
        s_accepted.load(std::memory_order_relaxed),
        s_stale.load(std::memory_order_relaxed),
        s_rejectedGraph.load(std::memory_order_relaxed),
        s_inspected.load(std::memory_order_relaxed),
        s_lastFormId.load(std::memory_order_relaxed),
        s_lastEpoch.load(std::memory_order_relaxed),
        s_lastAgeMs.load(std::memory_order_relaxed),
        s_lastEligible.load(std::memory_order_relaxed),
        s_lastErrorMilli.load(std::memory_order_relaxed),
        s_lastDurationUs.load(std::memory_order_relaxed),
        s_applyEnabled.load(std::memory_order_relaxed),
        s_applyFormId.load(std::memory_order_relaxed),
        s_appliedFrames.load(std::memory_order_relaxed),
        s_appliedBones.load(std::memory_order_relaxed),
        s_oldFrameApplySkips.load(std::memory_order_relaxed),
        s_timelineMisses.load(std::memory_order_relaxed),
        s_interpolatedFrames.load(std::memory_order_relaxed),
        s_lastInterpolationSpanMs.load(std::memory_order_relaxed),
        s_writeFailures.load(std::memory_order_relaxed),
        s_lastReadbackErrorMilli.load(std::memory_order_relaxed),
        s_rootSamples.load(std::memory_order_relaxed),
        s_lastRootLatestErrorMilli.load(std::memory_order_relaxed),
        s_lastRootPresentationErrorMilli.load(std::memory_order_relaxed),
        s_rootDiagnosticFormId.load(std::memory_order_relaxed)};
}

VisualPoseMailbox::HolderDiagnostics VisualPoseMailbox::GetHolderDiagnostics(
    const IAnimationGraphManagerHolder* apHolder, const void* apActor) noexcept
{
    HolderDiagnostics result{};
    if (!apHolder)
        return result;

    const uintptr_t holder = reinterpret_cast<uintptr_t>(apHolder);
    result.SlotIndex = static_cast<uint32_t>(SlotFor(apHolder));
    auto& slot = s_slots[result.SlotIndex];
    result.SlotOwnerEvictions = slot.OwnerEvictions.load(std::memory_order_relaxed);
    result.SlotInspectMisses = slot.InspectMisses.load(std::memory_order_relaxed);
    result.SlotOwnerMatches = slot.Holder.load(std::memory_order_acquire) == holder;
    result.StatsHolderMatches =
        slot.StatsHolder.load(std::memory_order_acquire) == holder;
    if (result.StatsHolderMatches)
    {
        result.OwnerPublishCount = slot.OwnerPublishCount.load(std::memory_order_relaxed);
        result.OwnerInspectCount = slot.OwnerInspectCount.load(std::memory_order_relaxed);
        result.OwnerApplyCount = slot.OwnerApplyCount.load(std::memory_order_relaxed);
        const auto lastPublish = slot.LastPublishedAtMs.load(std::memory_order_relaxed);
        const auto now = GetTickCount64();
        result.LastPublishAgeMs = lastPublish && now >= lastPublish ?
            now - lastPublish : 0;
    }
    if (!result.SlotOwnerMatches)
        return result;

    const auto frame = slot.Latest.load(std::memory_order_acquire);
    result.FrameMatchesHolder = frame && frame->Holder == holder;
    if (!result.FrameMatchesHolder)
        return result;
    result.FrameMatchesActor = frame->Actor == apActor;
    result.FormId = frame->FormId;
    result.OwnershipEpoch = frame->OwnershipEpoch;
    const uint64_t now = GetTickCount64();
    result.ReceiptAgeMs = now >= frame->ReceivedAtMs ?
        now - frame->ReceivedAtMs : 0;
    result.PresentationTick = s_presentationTick.load(std::memory_order_acquire);
    for (const auto& history : frame->History)
        if (history)
            ++result.HistoryCount;
    if (frame->History[0])
    {
        result.LatestSourceTick = frame->History[0]->SourceTick;
        if (result.PresentationTick >= result.LatestSourceTick)
            result.PresentationBracketed = true;
        else
        {
            for (size_t i = 1; i < frame->History.size(); ++i)
            {
                const auto& older = frame->History[i];
                const auto& newer = frame->History[i - 1];
                if (!older || !newer || older->SourceTick > result.PresentationTick ||
                    newer->SourceTick < result.PresentationTick)
                    continue;
                const auto span = newer->SourceTick - older->SourceTick;
                if (span > 0 && span <= 500)
                    result.PresentationBracketed = true;
                break;
            }
        }
    }
    if (slot.StatsHolder.load(std::memory_order_acquire) == holder)
    {
        const auto lastInspect = slot.LastInspectedAtMs.load(
            std::memory_order_relaxed);
        result.LastInspectAgeMs = lastInspect && now >= lastInspect ?
            now - lastInspect : 0;
        result.LastEligibleBones = slot.LastEligibleBones.load(
            std::memory_order_relaxed);
        result.LastWrittenBones = slot.LastWrittenBones.load(
            std::memory_order_relaxed);
        result.LastSkipReason = static_cast<VisualPoseMailbox::SkipReason>(
            slot.LastSkipReason.load(std::memory_order_relaxed));
        result.LastAppliedSourceTick = slot.LastAppliedSourceTick.load(
            std::memory_order_relaxed);
    }
    return result;
}
