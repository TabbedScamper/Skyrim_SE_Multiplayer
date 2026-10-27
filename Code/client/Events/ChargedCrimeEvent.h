#pragma once

#include <cstdint>
#include <vector>

struct ChargedCrimeEvent
{
    uint64_t Epoch{};
    uint32_t Id{};
    uint32_t Type{};
    uint32_t Gold{};
    uint32_t CriminalHandle{};
    uint32_t FactionFormId{};
    std::vector<uint32_t> WitnessHandles;
};
