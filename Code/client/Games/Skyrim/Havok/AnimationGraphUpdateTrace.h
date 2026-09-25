#pragma once

#include <cstdint>
#include <array>
#include <cstddef>

struct IAnimationGraphManagerHolder;

namespace AnimationGraphUpdateTrace
{
struct HolderSample
{
    uint64_t LastPostCallMs{};
    uint64_t Calls{};
    uint32_t ThreadId{};
};

struct WatchedPoseSample
{
    uint32_t FormId{};
    uint32_t PoseBoneCount{};
    uint32_t RenderBoneCount{};
    uint32_t ValidRenderNodeCount{};
    uint32_t ThreadId{};
    uint64_t LastSampleMs{};
    uint64_t PoseChecksum{};
    uint64_t RenderLocalChecksum{};
    uint64_t RenderWorldChecksum{};
    uint64_t Samples{};
    uint64_t PoseChanges{};
    uint64_t RenderLocalChanges{};
    uint64_t RenderWorldChanges{};
    uint64_t DurationUs{};
};

struct PlayerCameraObjectSample
{
    uint64_t StartMs{};
    uint64_t EndMs{};
    uint64_t CameraSequenceBefore{};
    uint64_t CameraSequenceAfter{};
    uint64_t WatchGeneration{};
    uint32_t ThreadId{};
    bool Valid{};
    float LocalBefore[3]{};
    float LocalAfter[3]{};
    float WorldBefore[3]{};
    float WorldAfter[3]{};
};

struct PlayerCameraObjectTrace
{
    std::array<PlayerCameraObjectSample, 64> Samples{};
    size_t Count{};
    uint64_t WatchGeneration{};
};

void WatchHolder(const IAnimationGraphManagerHolder* apHolder,
    uint32_t aFormId) noexcept;
void WatchPlayerCameraObject(const IAnimationGraphManagerHolder* apHolder,
    const void* apFirstPersonState, const void* apCameraObject) noexcept;
void NoteNativeCameraUpdate() noexcept;
[[nodiscard]] PlayerCameraObjectTrace GetPlayerCameraObjectTrace() noexcept;
[[nodiscard]] WatchedPoseSample GetWatchedPoseSample() noexcept;
[[nodiscard]] HolderSample GetHolderSample(const IAnimationGraphManagerHolder* apHolder) noexcept;
[[nodiscard]] uint64_t GetTotalCalls() noexcept;
[[nodiscard]] uint64_t GetLastPostCallMs() noexcept;
[[nodiscard]] bool IsHookRegistered() noexcept;
}
