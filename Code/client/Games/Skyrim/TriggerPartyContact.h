#pragma once

#include <cstdint>
#include <unordered_set>

namespace TriggerPartyContact
{
enum : uint8_t { Trigger, Enter, Leave };

struct Occupancy
{
    std::unordered_set<uint32_t> Members;
    uint64_t RepeatSample{};
    bool HasRepeated{};

    bool Observe(uint8_t aKind, uint32_t aHandle, uint64_t aSample)
    {
        if (aKind == Leave)
            return Members.erase(aHandle) && Members.empty();
        const bool occupied = !Members.empty();
        Members.insert(aHandle);
        if (aKind == Enter)
            return !occupied;
        if (HasRepeated && RepeatSample == aSample)
            return false;
        HasRepeated = true;
        RepeatSample = aSample;
        return true;
    }
};
}
