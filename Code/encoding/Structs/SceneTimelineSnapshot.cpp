#include <Structs/SceneTimelineSnapshot.h>

#include <TiltedCore/Serialization.hpp>

void SceneTimelineSnapshot::Serialize(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    TiltedPhoques::Serialization::WriteVarInt(aWriter, Tick);
    TiltedPhoques::Serialization::WriteVarInt(aWriter, AuthorityEpoch);
    TiltedPhoques::Serialization::WriteVarInt(aWriter, TransactionId);
    TiltedPhoques::Serialization::WriteVarInt(aWriter, ServerSequence);
    SceneId.Serialize(aWriter);
    QuestId.Serialize(aWriter);
    aWriter.WriteBits(RawPhaseWord, 32);
    aWriter.WriteBits(Playing ? 1 : 0, 1);
}

void SceneTimelineSnapshot::Deserialize(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    Tick = TiltedPhoques::Serialization::ReadVarInt(aReader);
    AuthorityEpoch = TiltedPhoques::Serialization::ReadVarInt(aReader);
    TransactionId = TiltedPhoques::Serialization::ReadVarInt(aReader);
    ServerSequence = TiltedPhoques::Serialization::ReadVarInt(aReader);
    SceneId.Deserialize(aReader);
    QuestId.Deserialize(aReader);
    uint64_t value{};
    aReader.ReadBits(value, 32);
    RawPhaseWord = static_cast<uint32_t>(value);
    aReader.ReadBits(value, 1);
    Playing = value != 0;
}

bool SceneTimelineSnapshot::IsValid() const noexcept
{
    return Tick != 0 && AuthorityEpoch != 0 && TransactionId != 0 &&
        static_cast<bool>(SceneId) && static_cast<bool>(QuestId) &&
        (RawPhaseWord == UINT32_MAX || RawPhaseWord < 65536);
}
