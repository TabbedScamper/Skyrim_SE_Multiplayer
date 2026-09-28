#pragma once

struct Actor;

// Creator rebuild repair is independent of the explicitly armed read-only trace.
struct BoundPoseKeeper
{
    explicit BoundPoseKeeper(entt::dispatcher&) noexcept;
    ~BoundPoseKeeper() noexcept;
    TP_NOCOPYMOVE(BoundPoseKeeper);

    // Main-thread creator completion and separate diagnostic arm/drain.
    static void OnMainFrame(bool aCreatorOpen, bool aSharedCreation) noexcept;

    struct Pose
    {
        bool Known{}, Bound{};
        int32_t Left{-1}, Right{-1};
    };
    // Only at rebuild boundaries, on the main thread. No pose is cached.
    static Pose ReadPose(Actor*) noexcept;
    // Defer our appearance mutation before Reset3D can enqueue native work.
    static bool CanRebuildNow(Actor*) noexcept;
    // Native creator rebuilds establish intent themselves (52346). Other
    // appearance rebuilds must preserve an actually observed bound pose.
    static bool AfterRebuild(Actor*, Pose, const char* aReason, bool aCreatorRebuild = false) noexcept;
};
