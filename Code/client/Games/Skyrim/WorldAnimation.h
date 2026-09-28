#pragma once
#include <Messages/WorldState.h>
struct TESObjectREFR;

namespace WorldAnimation
{
bool Eligible(TESObjectREFR*) noexcept;
bool Capture(TESObjectREFR*, std::vector<uint8_t>&) noexcept;
bool Apply(TESObjectREFR*, const WorldState&) noexcept;
}
