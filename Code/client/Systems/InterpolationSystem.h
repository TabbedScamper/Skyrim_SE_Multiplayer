#pragma once

struct World;
struct Actor;

/**
 * @brief Manages interpolation of movement and animations.
 */
struct InterpolationSystem
{
    // Other players' copies are left unseated on static furniture (unseat_remote_players switch).
    static void SetUnseatRemotePlayers(bool aEnabled) noexcept;
    static bool IsUnseatRemotePlayers() noexcept;

    static void Update(Actor* apActor, InterpolationComponent& aInterpolationComponent, uint64_t aTick) noexcept;
    // Main thread: applies the stale-seat releases queued by Update, and places remote actors (main_frame_placement).
    static void OnMainFrame() noexcept;
    // Remote actors are placed on the main frame at one presentation time per frame (default on). Off: placed from
    // the world update, which runs inside the Papyrus VM job at a varying point of the frame.
    static void SetMainFramePlacement(bool aEnabled) noexcept;
    static bool IsMainFramePlacement() noexcept;
    static void SetPresentationDelayMs(uint32_t aDelayMs) noexcept;
    static void AddPoint(InterpolationComponent& aInterpolationComponent, const InterpolationComponent::TimePoint& acPoint) noexcept;
    static InterpolationComponent& Setup(World& aWorld, entt::entity aEntity) noexcept;
    static void Clean(World& aWorld, entt::entity aEntity) noexcept;
};
