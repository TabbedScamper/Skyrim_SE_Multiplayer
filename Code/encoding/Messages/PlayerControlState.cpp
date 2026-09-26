#include <Messages/PlayerControlState.h>

bool PlayerControlState::IsValid() const noexcept
{
    return Epoch && Sequence && !(Controls & ~kChannels) && !(Handlers & ~7u) &&
        (!Free || (!Restrained && (Controls & 3) == 3 && (Handlers & 3) == 3));
}

void PlayerControlState::Serialize(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    aWriter.WriteBits(Epoch, 64);
    aWriter.WriteBits(Sequence, 64);
    aWriter.WriteBits(Controls, 16);
    aWriter.WriteBits(Handlers, 8);
    aWriter.WriteBits(Free, 1);
    aWriter.WriteBits(Restrained, 1);
    aWriter.WriteBits(PovScript, 1);
}

bool PlayerControlState::Deserialize(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    uint64_t value{};
    if (!aReader.ReadBits(Epoch, 64) || !aReader.ReadBits(Sequence, 64) || !aReader.ReadBits(value, 16))
        return false;
    Controls = static_cast<uint32_t>(value);
    if (!aReader.ReadBits(value, 8))
        return false;
    Handlers = static_cast<uint8_t>(value);
    if (!aReader.ReadBits(value, 1))
        return false;
    Free = value != 0;
    if (!aReader.ReadBits(value, 1))
        return false;
    Restrained = value != 0;
    if (!aReader.ReadBits(value, 1))
        return false;
    PovScript = value != 0;
    return IsValid();
}
