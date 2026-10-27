#pragma once

#include "TESForm.h"

struct TESTopicInfo : TESForm
{
    uint8_t pad20[0x1A];
    bool saidOnce;          // 0x3A
    uint8_t favorLevel;     // 0x3B
    uint16_t infoFlags;     // 0x3C (ENAM)
    uint16_t resetHours;    // 0x3E
};

static_assert(offsetof(TESTopicInfo, saidOnce) == 0x3A);
static_assert(offsetof(TESTopicInfo, infoFlags) == 0x3C);
