#include <Messages/RequestOwnershipClaim.h>

void RequestOwnershipClaim::SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    Serialization::WriteVarInt(aWriter, ServerId);
    Serialization::WriteVarInt(aWriter, ExpectedOwnershipEpoch);
    Serialization::WriteVarInt(aWriter, Carry);
}

void RequestOwnershipClaim::DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    ClientMessage::DeserializeRaw(aReader);

    ServerId = Serialization::ReadVarInt(aReader) & 0xFFFFFFFF;
    ExpectedOwnershipEpoch = Serialization::ReadVarInt(aReader) & 0xFFFFFFFF;
    Carry = static_cast<uint8_t>(Serialization::ReadVarInt(aReader) & 0x3);
}
