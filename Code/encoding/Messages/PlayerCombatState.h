#pragma once

#include <Structs/GameId.h>
#include <array>

// One player per snapshot. IDs are server character IDs, never local form IDs.
struct PlayerCombatState
{
    enum Kind : uint8_t { Stealth, Detection, Damage, Threat };
    struct Perk
    {
        GameId Id{};
        uint16_t EntryIndex{};
        bool operator==(const Perk&) const noexcept = default;
    };
    static constexpr size_t MaxPerks = 512;
    static constexpr std::array<uint32_t, 5> ActorValues{15, 54, 92, 105, 144};

    uint8_t Type{Stealth};
    uint64_t Epoch{};
    uint64_t Sequence{};
    uint32_t ActorId{};
    uint32_t OwnershipEpoch{};
    uint32_t TargetId{};
    uint32_t TargetOwnershipEpoch{};
    bool Sneaking{};
    bool Moving{};
    bool Running{};
    bool KillMove{};
    float Speed{};
    float Light{};
    float ArmorWeight{};
    std::array<float, 5> Values{};
    TiltedPhoques::Vector<Perk> Perks;
    int32_t DetectionLevel{-1000};
    uint8_t MeterLevel{};
    uint32_t LOSCount{};
    float RawDamage{};

    void Serialize(TiltedPhoques::Buffer::Writer& aWriter) const noexcept;
    bool Deserialize(TiltedPhoques::Buffer::Reader& aReader) noexcept;
    bool IsValid() const noexcept;
    bool operator==(const PlayerCombatState&) const noexcept = default;
};
