#pragma once
#include <array>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace ReplayRecording
{
struct Item { uint32_t Mod{}, Base{}; uint64_t Slots{}; };
struct Case
{
    uint32_t Kind{};
    std::string Provenance;
    std::array<double, 3> Before{}, After{};
    std::array<double, 3> Observed{};
    uint64_t FirstTick{}, SecondTick{}, Tick{};
    std::array<float, 4> TargetRotation{}, ObservedRotation{};
    double Pitch{}, Intent{}, Tolerance{};
    bool Settled{};
    std::vector<Item> Owner, Local;
};
inline uint64_t Integer(std::istream& aInput, unsigned aBytes)
{
    uint64_t value{};
    for (unsigned i = 0; i < aBytes; ++i)
    {
        const int c = aInput.get();
        if (c == EOF) throw std::runtime_error("truncated replay recording");
        value |= uint64_t(uint8_t(c)) << (i * 8);
    }
    return value;
}
inline double Number(std::istream& aInput)
{
    const uint64_t bits = Integer(aInput, 8);
    double value;
    static_assert(sizeof(value) == sizeof(bits));
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}
inline std::vector<Case> Read(std::istream& aInput)
{
    if (Integer(aInput, 4) != 0x314C5052) throw std::runtime_error("unsupported replay version/magic");
    const auto count = Integer(aInput, 4);
    if (count == 0 || count > 1000000) throw std::runtime_error("invalid replay case count");
    std::vector<Case> result;
    for (uint64_t i = 0; i < count; ++i)
    {
        Case entry;
        entry.Kind = uint32_t(Integer(aInput, 4));
        const auto size = Integer(aInput, 4);
        if (size > 65536) throw std::runtime_error("oversized replay provenance");
        entry.Provenance.resize(size_t(size));
        if (!aInput.read(entry.Provenance.data(), std::streamsize(size))) throw std::runtime_error("truncated provenance");
        if (entry.Kind == 1)
        {
            for (auto& x : entry.Before) x = Number(aInput);
            for (auto& x : entry.After) x = Number(aInput);
        }
        else if (entry.Kind == 2)
        {
            for (auto* list : {&entry.Owner, &entry.Local})
            {
                const auto n = Integer(aInput, 4);
                if (n > 35) throw std::runtime_error("worn set exceeds protocol capacity");
                for (uint64_t k = 0; k < n; ++k)
                {
                    Item item;
                    item.Mod = uint32_t(Integer(aInput, 4));
                    item.Base = uint32_t(Integer(aInput, 4));
                    item.Slots = Integer(aInput, 8);
                    if (!item.Slots || item.Slots >> 35) throw std::runtime_error("invalid worn slots");
                    list->push_back(item);
                }
            }
        }
        else if (entry.Kind == 3 || entry.Kind == 4)
        {
            for (auto& x : entry.Before) x = Number(aInput);
            for (auto& x : entry.After) x = Number(aInput);
            for (auto& x : entry.Observed) x = Number(aInput);
            entry.FirstTick = Integer(aInput, 8);
            entry.SecondTick = Integer(aInput, 8);
            entry.Tick = Integer(aInput, 8);
            if (!(entry.FirstTick <= entry.Tick && entry.Tick <= entry.SecondTick && entry.FirstTick < entry.SecondTick))
                throw std::runtime_error("invalid gap sample timeline");
        }
        else if (entry.Kind == 5)
        {
            const auto mask = Integer(aInput, 1);
            if (mask > 7) throw std::runtime_error("invalid camera field mask");
            unsigned index{};
            for (auto* value : {&entry.Pitch, &entry.Intent, &entry.Tolerance})
            {
                const auto encoded = Number(aInput);
                *value = mask & (1ull << index++) ? encoded : std::numeric_limits<double>::quiet_NaN();
            }
        }
        else if (entry.Kind == 6 || entry.Kind == 7)
        {
            const auto settled = Integer(aInput, 1);
            if (settled > 1) throw std::runtime_error("invalid settled flag");
            entry.Settled = settled != 0;
            for (auto& x : entry.Before) x = Number(aInput);
            for (auto& x : entry.After) x = Number(aInput);
            if (entry.Kind == 6)
            {
                for (auto& x : entry.TargetRotation) x = float(Number(aInput));
                for (auto& x : entry.ObservedRotation) x = float(Number(aInput));
            }
        }
        else throw std::runtime_error("unknown replay case kind");
        result.push_back(std::move(entry));
    }
    if (aInput.peek() != EOF) throw std::runtime_error("trailing replay data");
    return result;
}
inline std::vector<Case> Load(const std::filesystem::path& aPath)
{
    std::ifstream file(aPath, std::ios::binary);
    if (!file) throw std::runtime_error("missing replay fixture: " + aPath.string());
    return Read(file);
}
} // namespace ReplayRecording
