#pragma once

#include <Structs/VisualBoneSnapshot.h>

#include <cstdint>

struct IAnimationGraphManagerHolder;

namespace VisualPoseMailbox
{
struct Inspection
{
    uint64_t Published{};
    uint64_t Accepted{};
    uint64_t Stale{};
    uint64_t RejectedGraph{};
    uint64_t Inspected{};
    uint32_t LastFormId{};
    uint32_t LastEpoch{};
    uint32_t LastSourceAgeMs{};
    uint32_t LastEligibleBones{};
    uint32_t LastMaxElementErrorMilli{};
    uint32_t LastDurationUs{};
    bool ApplyEnabled{};
    uint32_t ApplyFormId{};
    uint64_t AppliedFrames{};
    uint64_t AppliedBones{};
    uint64_t OldFrameApplySkips{};
    uint64_t TimelineMisses{};
    uint64_t InterpolatedFrames{};
    uint32_t LastInterpolationSpanMs{};
    uint64_t WriteFailures{};
    uint32_t LastReadbackErrorMilli{};
    uint64_t RootSamples{};
    uint32_t LastRootLatestErrorMilli{};
    uint32_t LastRootPresentationErrorMilli{};
    uint32_t RootDiagnosticFormId{};
};

enum class SkipReason : uint32_t
{
    None,
    StaleFrame,
    GraphUnavailable,
    GraphInvalid,
    ApplyDisabled,
    ReceiptTooOld,
    NoPresentationBracket,
    RevokedFrame,
    NoEligibleBones,
    NoWritableBones,
    Applied
};

struct HolderDiagnostics
{
    uint32_t SlotIndex{};
    uint64_t SlotOwnerEvictions{};
    uint64_t SlotInspectMisses{};
    bool SlotOwnerMatches{};
    bool FrameMatchesHolder{};
    bool FrameMatchesActor{};
    bool StatsHolderMatches{};
    uint64_t OwnerPublishCount{};
    uint64_t OwnerInspectCount{};
    uint64_t OwnerApplyCount{};
    uint64_t LastPublishAgeMs{};
    uint32_t FormId{};
    uint32_t OwnershipEpoch{};
    uint64_t ReceiptAgeMs{};
    uint64_t LatestSourceTick{};
    uint32_t HistoryCount{};
    uint64_t PresentationTick{};
    bool PresentationBracketed{};
    uint64_t LastInspectAgeMs{};
    uint32_t LastEligibleBones{};
    uint32_t LastWrittenBones{};
    SkipReason LastSkipReason{};
    uint64_t LastAppliedSourceTick{};
};

void SetApplyEnabled(bool aEnabled) noexcept;
[[nodiscard]] bool IsApplyEnabled() noexcept;
void SetApplyFormId(uint32_t aFormId) noexcept;
[[nodiscard]] uint32_t GetApplyFormId() noexcept;
void SetPresentationTick(uint64_t aTick) noexcept;
void SetRootDiagnosticFormId(uint32_t aFormId) noexcept;
void Publish(const IAnimationGraphManagerHolder* apHolder,
    const void* apActor, uint32_t aFormId, uint32_t aOwnershipEpoch,
    uint64_t aLocalGraphDescriptor, const VisualBoneSnapshot& acState) noexcept;
void Clear(const IAnimationGraphManagerHolder* apHolder) noexcept;
void InspectPostGraph(IAnimationGraphManagerHolder* apHolder) noexcept;
[[nodiscard]] Inspection GetInspection() noexcept;
[[nodiscard]] HolderDiagnostics GetHolderDiagnostics(
    const IAnimationGraphManagerHolder* apHolder, const void* apActor) noexcept;
}
