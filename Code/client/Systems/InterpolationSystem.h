#pragma once

struct World;
struct Actor;

/**
 * @brief Manages interpolation of movement and animations.
 */
struct InterpolationSystem
{
    static void Update(Actor* apActor, InterpolationComponent& aInterpolationComponent, uint64_t aTick) noexcept;
    // Main thread: applies the stale-seat releases queued by Update.
    static void OnMainFrame() noexcept;
    static void AddPoint(InterpolationComponent& aInterpolationComponent, const InterpolationComponent::TimePoint& acPoint) noexcept;
    static InterpolationComponent& Setup(World& aWorld, entt::entity aEntity) noexcept;
    static void Clean(World& aWorld, entt::entity aEntity) noexcept;
};
