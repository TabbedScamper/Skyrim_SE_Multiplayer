#pragma once

#include <cstdint>

struct Actor;
struct BGSScene;

namespace SceneTurnsNative
{
// Values, never native pointers, cross from engine callbacks to World::Update.
struct IdleStep
{
    uint32_t SceneId{};
    uint32_t QuestId{};
    uint32_t Phase{};
    uint32_t ActionIndex{};
    uint32_t ActorHandle{};
    uint32_t ActorId{};
    uint32_t IdleId{};
    uint32_t DefaultAction{};
    uint64_t CapturedMs{};
};

bool IsEnabled() noexcept;
bool IsCurrent(const IdleStep& aStep, bool& aActionComplete) noexcept;
bool Replay(const IdleStep& aStep, Actor* apActor, Actor* apTarget) noexcept;
bool Approach(Actor* apActor, Actor* apTarget) noexcept;
void ReleaseApproach(Actor* apActor) noexcept;
bool AnimationBusy(Actor* apActor, bool& aBusy) noexcept;
}
