#include <Messages/ReviveData.h>
#include <Structs/CheckedRead.h>
#include <algorithm>

namespace
{
uint32_t ReadU32(TiltedPhoques::Buffer::Reader& aReader)
{
    const auto value = CheckedRead::VarInt(aReader);
    if (value > UINT32_MAX)
        throw std::runtime_error("revive integer exceeds limit");
    return static_cast<uint32_t>(value);
}
}

void ReviveData::SerializeData(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    using TiltedPhoques::Serialization;
    Serialization::WriteVarInt(aWriter, static_cast<uint8_t>(Action));
    Serialization::WriteVarInt(aWriter, Epoch);
    Serialization::WriteVarInt(aWriter, Revision);
    Serialization::WriteVarInt(aWriter, PlayerId);
    Serialization::WriteVarInt(aWriter, ReviverId);
    Serialization::WriteBool(aWriter, Down);
    Serialization::WriteBool(aWriter, Alive);
    Serialization::WriteBool(aWriter, InCombat);
    Serialization::WriteBool(aWriter, Dead);
    Serialization::WriteBool(aWriter, Flung);
    Serialization::WriteVarInt(aWriter, static_cast<uint32_t>(std::clamp(Bleed, 0.f, 1.f) * 65535.f + 0.5f));
    Cell.Serialize(aWriter);
    WorldSpace.Serialize(aWriter);
    aWriter.WriteBits(Position.Pack(), 64);
}

void ReviveData::DeserializeData(TiltedPhoques::Buffer::Reader& aReader)
{
    const auto action = CheckedRead::VarInt(aReader);
    if (action > static_cast<uint8_t>(ReviveAction::Wipe))
        throw std::runtime_error("invalid revive action");
    Action = static_cast<ReviveAction>(action);
    Epoch = CheckedRead::VarInt(aReader);
    Revision = CheckedRead::VarInt(aReader);
    PlayerId = ReadU32(aReader);
    ReviverId = ReadU32(aReader);
    Down = CheckedRead::Bool(aReader);
    Alive = CheckedRead::Bool(aReader);
    InCombat = CheckedRead::Bool(aReader);
    Dead = CheckedRead::Bool(aReader);
    Flung = CheckedRead::Bool(aReader);
    const auto bleed = CheckedRead::VarInt(aReader);
    if (bleed > 65535)
        throw std::runtime_error("revive bleed exceeds limit");
    Bleed = static_cast<float>(bleed) / 65535.f;
    Cell.BaseId = ReadU32(aReader);
    Cell.ModId = ReadU32(aReader);
    WorldSpace.BaseId = ReadU32(aReader);
    WorldSpace.ModId = ReadU32(aReader);
    uint64_t position{};
    CheckedRead::Bits(aReader, position, 64);
    Position.Unpack(position);
}

bool ReviveData::operator==(const ReviveData& aOther) const noexcept
{
    return Action == aOther.Action && Epoch == aOther.Epoch && Revision == aOther.Revision &&
        PlayerId == aOther.PlayerId && ReviverId == aOther.ReviverId && Down == aOther.Down &&
        Alive == aOther.Alive && InCombat == aOther.InCombat && Dead == aOther.Dead && Flung == aOther.Flung && Bleed == aOther.Bleed && Cell == aOther.Cell &&
        WorldSpace == aOther.WorldSpace && Position == aOther.Position;
}
