#pragma once

#include <Structs/GameId.h>
#include <Structs/Inventory.h>
#include <Structs/Tints.h>

using TiltedPhoques::Buffer;
using TiltedPhoques::String;
using TiltedPhoques::Vector;

// A guest's character, separate from any world (owner design 2026-09-30: the host's world rules, the guest keeps its
// own character and takes its levels and loot home). Character fields only: never the native PlayerCharacter change
// form, which also carries the source world's objectives, location, factions and control state
// (C:\Tools\skyrim_re\agent\character-snapshot-design.md). Forms travel as GameId (plugin + local id).
struct CharacterSnapshot
{
    static constexpr uint32_t kVersion = 1;
    static constexpr uint32_t kSkillCount = 18;

    struct Skill
    {
        float Level{};
        float Xp{};
        float Threshold{};
        uint32_t Legendary{};

        bool operator==(const Skill& acRhs) const noexcept;
    };

    struct Perk
    {
        GameId Id{};
        uint8_t Rank{};

        bool operator==(const Perk& acRhs) const noexcept;
    };

    struct Word
    {
        GameId Id{};
        bool Unlocked{};

        bool operator==(const Word& acRhs) const noexcept;
    };

    struct BaseValue
    {
        uint32_t ActorValue{};
        float Value{};

        bool operator==(const BaseValue& acRhs) const noexcept;
    };

    bool operator==(const CharacterSnapshot& acRhs) const noexcept;

    void Serialize(Buffer::Writer& aWriter) const noexcept;
    // False on a version mismatch or a count over its bound; the snapshot is then unusable.
    bool Deserialize(Buffer::Reader& aReader) noexcept;

    uint32_t Version{kVersion};
    String Name{};
    // The native TESNPC appearance payload (TESNPC::Serialize: race, face, sex, head parts, morphs, weight trailer).
    uint32_t ChangeFlags{};
    String AppearanceBuffer{};
    Tints FaceTints{};

    uint16_t Level{};
    float Xp{};
    float LevelThreshold{};
    uint8_t PerkPoints{};
    Vector<Skill> Skills{};
    // Permanent (base) values: health, magicka, stamina and the like. Current damage is never carried.
    Vector<BaseValue> BaseValues{};
    float DragonSouls{};

    Vector<Perk> Perks{};
    Vector<GameId> Spells{};
    Vector<GameId> Shouts{};
    // Learned words of power; Unlocked when a soul was spent on it.
    Vector<Word> Words{};

    // Personal items only: quest-object instances of the source world are left out and listed below.
    Inventory Items{};
    Vector<GameId> ExcludedQuestItems{};
};
