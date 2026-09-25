#pragma once

#include <Games/Primitives.h>

// Native Gamebryo transform layout used by Skyrim SE/AE. Keeping this
// layout explicit lets multiplayer systems observe rendered scene-node poses
// instead of mistaking TESCamera input deltas for the final camera transform.
struct NiMatrix3
{
    float entry[3][3]{};
};

static_assert(sizeof(NiMatrix3) == 0x24);

struct NiTransform
{
    NiMatrix3 rotate{};
    NiPoint3 translate{};
    float scale{1.f};
};

static_assert(sizeof(NiTransform) == 0x34);

