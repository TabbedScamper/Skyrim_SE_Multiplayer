#include <Structs/CameraStateSnapshot.h>

#include <TiltedCore/Serialization.hpp>

#include <bit>
#include <cmath>

namespace
{
void WriteFloat(TiltedPhoques::Buffer::Writer& aWriter, const float aValue) noexcept
{
    aWriter.WriteBits(std::bit_cast<uint32_t>(aValue), 32);
}

float ReadFloat(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    uint64_t value{};
    aReader.ReadBits(value, 32);
    return std::bit_cast<float>(static_cast<uint32_t>(value));
}

bool IsFinite(const float aValue) noexcept
{
    return std::isfinite(aValue);
}
}

void CameraStateSnapshot::Serialize(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    TiltedPhoques::Serialization::WriteVarInt(aWriter, Tick);
    TiltedPhoques::Serialization::WriteVarInt(aWriter, AuthorityEpoch);
    WriteFloat(aWriter, Position.x);
    WriteFloat(aWriter, Position.y);
    WriteFloat(aWriter, Position.z);
    for (const auto value : Rotation)
        WriteFloat(aWriter, value);
    WriteFloat(aWriter, Scale);
    WriteFloat(aWriter, Fov);
    aWriter.WriteBits(StateId, 8);
}

void CameraStateSnapshot::Deserialize(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    Tick = TiltedPhoques::Serialization::ReadVarInt(aReader);
    AuthorityEpoch = TiltedPhoques::Serialization::ReadVarInt(aReader);
    Position = {ReadFloat(aReader), ReadFloat(aReader), ReadFloat(aReader)};
    for (auto& value : Rotation)
        value = ReadFloat(aReader);
    Scale = ReadFloat(aReader);
    Fov = ReadFloat(aReader);
    uint64_t stateId{};
    aReader.ReadBits(stateId, 8);
    StateId = static_cast<uint8_t>(stateId);
}

bool CameraStateSnapshot::IsValid() const noexcept
{
    if (!IsFinite(Position.x) || !IsFinite(Position.y) || !IsFinite(Position.z) ||
        std::abs(Position.x) > 1.0e8f || std::abs(Position.y) > 1.0e8f ||
        std::abs(Position.z) > 1.0e8f || !IsFinite(Scale) || Scale < 0.001f ||
        Scale > 100.f || !IsFinite(Fov) || Fov < 1.f || Fov > 179.f ||
        StateId >= 13)
        return false;

    for (const auto value : Rotation)
    {
        if (!IsFinite(value) || std::abs(value) > 2.f)
            return false;
    }

    // Reject degenerate/non-transform matrices while tolerating the small
    // numerical drift found in evaluated Gamebryo camera nodes.
    for (size_t row = 0; row < 3; ++row)
    {
        const auto base = row * 3;
        const float lengthSquared = Rotation[base] * Rotation[base] +
            Rotation[base + 1] * Rotation[base + 1] +
            Rotation[base + 2] * Rotation[base + 2];
        if (lengthSquared < 0.25f || lengthSquared > 2.25f)
            return false;
    }

    return true;
}
