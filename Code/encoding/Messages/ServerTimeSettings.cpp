#include <Messages/ServerTimeSettings.h>
#include <TiltedCore/Serialization.hpp>
#include <Structs/CheckedRead.h>

void ServerTimeSettings::SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    timeModel.Serialize(aWriter);
    TiltedPhoques::Serialization::WriteVarInt(aWriter, GameMs);
    TiltedPhoques::Serialization::WriteVarInt(aWriter, SampleTick);
    TiltedPhoques::Serialization::WriteVarInt(aWriter, Seq);
    aWriter.WriteBits(ServerNonce, 64);
}

void ServerTimeSettings::DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    timeModel.Deserialize(aReader);
    GameMs = TiltedPhoques::Serialization::ReadVarInt(aReader);
    SampleTick = TiltedPhoques::Serialization::ReadVarInt(aReader);
    const auto seq = TiltedPhoques::Serialization::ReadVarInt(aReader);
    Seq = static_cast<uint32_t>(seq);
    m_valid = CheckedRead::RemainingBits(aReader) >= 64 && aReader.ReadBits(ServerNonce, 64) &&
        ServerNonce != 0 && seq != 0 && seq <= UINT32_MAX;
}
