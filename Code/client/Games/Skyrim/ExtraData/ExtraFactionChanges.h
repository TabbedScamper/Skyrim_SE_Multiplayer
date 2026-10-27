#pragma once

#include <ExtraData.h>

struct TESFaction;

struct ExtraFactionChanges : BSExtraData
{
    inline static constexpr auto eExtraData = ExtraDataType::Faction;

    virtual ~ExtraFactionChanges();

    struct Entry
    {
        TESFaction* faction;
        int8_t rank;
    };

    GameArray<Entry> entries;
    TESFaction* crimeFaction; // 0x28
    bool removeCrimeFaction; // 0x30
};

static_assert(sizeof(ExtraFactionChanges::Entry) == 0x10);
static_assert(offsetof(ExtraFactionChanges, entries) == 0x10);
static_assert(offsetof(ExtraFactionChanges, crimeFaction) == 0x28);
static_assert(offsetof(ExtraFactionChanges, removeCrimeFaction) == 0x30);
