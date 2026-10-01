#include <Structs/CharacterSnapshot.h>
#include <TiltedCore/Serialization.hpp>

using TiltedPhoques::Serialization;

namespace
{
// Bounds for a snapshot read from a file or the network: a count over these marks it corrupt.
constexpr uint64_t kMaxForms = 8192;
constexpr uint64_t kMaxValues = 256;

template <class T, class Write> void WriteList(Buffer::Writer& aWriter, const Vector<T>& acList, Write aWrite)
{
    Serialization::WriteVarInt(aWriter, acList.size());
    for (const auto& item : acList)
        aWrite(item);
}

template <class T, class Read> bool ReadList(Buffer::Reader& aReader, Vector<T>& aList, uint64_t aMax, Read aRead)
{
    const auto count = Serialization::ReadVarInt(aReader);
    if (count > aMax)
        return false;
    aList.clear();
    aList.reserve(static_cast<size_t>(count));
    for (uint64_t i = 0; i < count; ++i)
        aRead(aList.emplace_back());
    return true;
}
} // namespace

bool CharacterSnapshot::Skill::operator==(const Skill& acRhs) const noexcept
{
    return Level == acRhs.Level && Xp == acRhs.Xp && Threshold == acRhs.Threshold && Legendary == acRhs.Legendary;
}

bool CharacterSnapshot::Perk::operator==(const Perk& acRhs) const noexcept
{
    return Id == acRhs.Id && Rank == acRhs.Rank;
}

bool CharacterSnapshot::Word::operator==(const Word& acRhs) const noexcept
{
    return Id == acRhs.Id && Unlocked == acRhs.Unlocked;
}

bool CharacterSnapshot::BaseValue::operator==(const BaseValue& acRhs) const noexcept
{
    return ActorValue == acRhs.ActorValue && Value == acRhs.Value;
}

bool CharacterSnapshot::operator==(const CharacterSnapshot& acRhs) const noexcept
{
    return Version == acRhs.Version && Name == acRhs.Name && ChangeFlags == acRhs.ChangeFlags &&
           AppearanceBuffer == acRhs.AppearanceBuffer && FaceTints == acRhs.FaceTints && Level == acRhs.Level &&
           Xp == acRhs.Xp && LevelThreshold == acRhs.LevelThreshold && PerkPoints == acRhs.PerkPoints &&
           Skills == acRhs.Skills && BaseValues == acRhs.BaseValues && DragonSouls == acRhs.DragonSouls &&
           Perks == acRhs.Perks && Spells == acRhs.Spells && Shouts == acRhs.Shouts && Words == acRhs.Words &&
           Items == acRhs.Items && ExcludedQuestItems == acRhs.ExcludedQuestItems;
}

void CharacterSnapshot::Serialize(Buffer::Writer& aWriter) const noexcept
{
    Serialization::WriteVarInt(aWriter, Version);
    Serialization::WriteString(aWriter, Name);
    Serialization::WriteVarInt(aWriter, ChangeFlags);
    Serialization::WriteString(aWriter, AppearanceBuffer);
    FaceTints.Serialize(aWriter);

    Serialization::WriteVarInt(aWriter, Level);
    Serialization::WriteFloat(aWriter, Xp);
    Serialization::WriteFloat(aWriter, LevelThreshold);
    Serialization::WriteVarInt(aWriter, PerkPoints);
    WriteList(aWriter, Skills, [&](const Skill& acSkill) {
        Serialization::WriteFloat(aWriter, acSkill.Level);
        Serialization::WriteFloat(aWriter, acSkill.Xp);
        Serialization::WriteFloat(aWriter, acSkill.Threshold);
        Serialization::WriteVarInt(aWriter, acSkill.Legendary);
    });
    WriteList(aWriter, BaseValues, [&](const BaseValue& acValue) {
        Serialization::WriteVarInt(aWriter, acValue.ActorValue);
        Serialization::WriteFloat(aWriter, acValue.Value);
    });
    Serialization::WriteFloat(aWriter, DragonSouls);

    WriteList(aWriter, Perks, [&](const Perk& acPerk) {
        acPerk.Id.Serialize(aWriter);
        Serialization::WriteVarInt(aWriter, acPerk.Rank);
    });
    WriteList(aWriter, Spells, [&](const GameId& acId) { acId.Serialize(aWriter); });
    WriteList(aWriter, Shouts, [&](const GameId& acId) { acId.Serialize(aWriter); });
    WriteList(aWriter, Words, [&](const Word& acWord) {
        acWord.Id.Serialize(aWriter);
        Serialization::WriteBool(aWriter, acWord.Unlocked);
    });

    Items.Serialize(aWriter);
    WriteList(aWriter, ExcludedQuestItems, [&](const GameId& acId) { acId.Serialize(aWriter); });
}

bool CharacterSnapshot::Deserialize(Buffer::Reader& aReader) noexcept
{
    Version = static_cast<uint32_t>(Serialization::ReadVarInt(aReader));
    if (Version != kVersion)
        return false;
    Name = Serialization::ReadString(aReader);
    ChangeFlags = static_cast<uint32_t>(Serialization::ReadVarInt(aReader));
    AppearanceBuffer = Serialization::ReadString(aReader);
    FaceTints.Deserialize(aReader);

    Level = static_cast<uint16_t>(Serialization::ReadVarInt(aReader));
    Xp = Serialization::ReadFloat(aReader);
    LevelThreshold = Serialization::ReadFloat(aReader);
    PerkPoints = static_cast<uint8_t>(Serialization::ReadVarInt(aReader));
    if (!ReadList(aReader, Skills, kSkillCount, [&](Skill& aSkill) {
            aSkill.Level = Serialization::ReadFloat(aReader);
            aSkill.Xp = Serialization::ReadFloat(aReader);
            aSkill.Threshold = Serialization::ReadFloat(aReader);
            aSkill.Legendary = static_cast<uint32_t>(Serialization::ReadVarInt(aReader));
        }))
        return false;
    if (!ReadList(aReader, BaseValues, kMaxValues, [&](BaseValue& aValue) {
            aValue.ActorValue = static_cast<uint32_t>(Serialization::ReadVarInt(aReader));
            aValue.Value = Serialization::ReadFloat(aReader);
        }))
        return false;
    DragonSouls = Serialization::ReadFloat(aReader);

    if (!ReadList(aReader, Perks, kMaxForms, [&](Perk& aPerk) {
            aPerk.Id.Deserialize(aReader);
            aPerk.Rank = static_cast<uint8_t>(Serialization::ReadVarInt(aReader));
        }))
        return false;
    if (!ReadList(aReader, Spells, kMaxForms, [&](GameId& aId) { aId.Deserialize(aReader); }) ||
        !ReadList(aReader, Shouts, kMaxForms, [&](GameId& aId) { aId.Deserialize(aReader); }))
        return false;
    if (!ReadList(aReader, Words, kMaxForms, [&](Word& aWord) {
            aWord.Id.Deserialize(aReader);
            aWord.Unlocked = Serialization::ReadBool(aReader);
        }))
        return false;

    Items.Deserialize(aReader);
    return ReadList(aReader, ExcludedQuestItems, kMaxForms, [&](GameId& aId) { aId.Deserialize(aReader); });
}
