#include <Messages/DoorVoteData.h>
#include <Structs/CheckedRead.h>
#include <limits>

namespace
{
uint32_t ReadU32(TiltedPhoques::Buffer::Reader& aReader)
{
    const auto value = CheckedRead::VarInt(aReader);
    if (value > UINT32_MAX)
        throw std::runtime_error("door vote integer exceeds limit");
    return static_cast<uint32_t>(value);
}

void ReadId(TiltedPhoques::Buffer::Reader& aReader, GameId& aId)
{
    aId.BaseId = ReadU32(aReader);
    aId.ModId = ReadU32(aReader);
}

void WriteText(TiltedPhoques::Buffer::Writer& aWriter, const TiltedPhoques::String& aText)
{
    TiltedPhoques::Serialization::WriteVarInt(aWriter, aText.size());
    aWriter.WriteBytes(reinterpret_cast<const uint8_t*>(aText.data()), aText.size());
}

void ReadText(TiltedPhoques::Buffer::Reader& aReader, TiltedPhoques::String& aText, size_t aLimit)
{
    const auto count = CheckedRead::VarInt(aReader);
    if (count > aLimit || count > CheckedRead::RemainingBits(aReader) / 8)
        throw std::runtime_error("door vote text exceeds limit");
    aText.resize(static_cast<size_t>(count));
    CheckedRead::Bytes(aReader, reinterpret_cast<uint8_t*>(aText.data()), aText.size());
}
}

void DoorVoteData::SerializeData(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    using TiltedPhoques::Serialization;
    Serialization::WriteVarInt(aWriter, static_cast<uint8_t>(Action));
    Serialization::WriteVarInt(aWriter, Epoch);
    Serialization::WriteVarInt(aWriter, VoteId);
    Serialization::WriteVarInt(aWriter, Tick);
    Door.Serialize(aWriter);
    Cell.Serialize(aWriter);
    WorldSpace.Serialize(aWriter);
    Destination.Serialize(aWriter);
    aWriter.WriteBits(Position.Pack(), 64);
    Serialization::WriteVarInt(aWriter, ReadyCount);
    Serialization::WriteVarInt(aWriter, TotalCount);
    Serialization::WriteBool(aWriter, Ready);
    WriteText(aWriter, Name);
    WriteText(aWriter, Notice);
}

void DoorVoteData::DeserializeData(TiltedPhoques::Buffer::Reader& aReader)
{
    const auto action = CheckedRead::VarInt(aReader);
    if (action > static_cast<uint8_t>(DoorVoteAction::Release))
        throw std::runtime_error("invalid door vote action");
    Action = static_cast<DoorVoteAction>(action);
    Epoch = CheckedRead::VarInt(aReader);
    VoteId = CheckedRead::VarInt(aReader);
    Tick = CheckedRead::VarInt(aReader);
    ReadId(aReader, Door);
    ReadId(aReader, Cell);
    ReadId(aReader, WorldSpace);
    ReadId(aReader, Destination);
    uint64_t position{};
    CheckedRead::Bits(aReader, position, 64);
    Position.Unpack(position);
    ReadyCount = ReadU32(aReader);
    TotalCount = ReadU32(aReader);
    Ready = CheckedRead::Bool(aReader);
    ReadText(aReader, Name, 160);
    ReadText(aReader, Notice, 512);
}

bool DoorVoteData::operator==(const DoorVoteData& aOther) const noexcept
{
    return Action == aOther.Action && Epoch == aOther.Epoch && VoteId == aOther.VoteId &&
        Tick == aOther.Tick && Door == aOther.Door && Cell == aOther.Cell &&
        WorldSpace == aOther.WorldSpace && Destination == aOther.Destination &&
        Position == aOther.Position && ReadyCount == aOther.ReadyCount &&
        TotalCount == aOther.TotalCount && Ready == aOther.Ready && Name == aOther.Name && Notice == aOther.Notice;
}
