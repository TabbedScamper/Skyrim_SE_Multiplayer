#include <Structs/PhysicsReferenceUpdate.h>
#include <bit>
#include <Structs/CheckedRead.h>

namespace
{
void WriteFloat(TiltedPhoques::Buffer::Writer& aWriter, float aValue) noexcept
{
    aWriter.WriteBits(std::bit_cast<uint32_t>(aValue), 32);
}

float ReadFloat(TiltedPhoques::Buffer::Reader& aReader)
{
    uint64_t value{};
    CheckedRead::Bits(aReader, value, 32);
    return std::bit_cast<float>(static_cast<uint32_t>(value));
}
}

void PhysicsReferenceUpdate::Serialize(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    Id.Serialize(aWriter);
    WriteFloat(aWriter, Position.x);
    WriteFloat(aWriter, Position.y);
    WriteFloat(aWriter, Position.z);
    WriteFloat(aWriter, Rotation.x);
    WriteFloat(aWriter, Rotation.y);
    WriteFloat(aWriter, Rotation.z);
    aWriter.WriteBits(MotionType, 8);
    WriteFloat(aWriter, LinearVelocity.x);
    WriteFloat(aWriter, LinearVelocity.y);
    WriteFloat(aWriter, LinearVelocity.z);
    if (MotionType == 3)
    {
        for (const float value : BodyTransform)
            WriteFloat(aWriter, value);
        const auto count = (std::min)(ChildBodies.size(), kMaxChildBodies);
        aWriter.WriteBits(count, 8);
        for (size_t i = 0; i < count; ++i)
            for (const float value : ChildBodies[i])
                WriteFloat(aWriter, value);
    }
}

void PhysicsReferenceUpdate::Deserialize(TiltedPhoques::Buffer::Reader& aReader)
{
    Id.BaseId = static_cast<uint32_t>(CheckedRead::VarInt(aReader));
    Id.ModId = static_cast<uint32_t>(CheckedRead::VarInt(aReader));
    Position = {ReadFloat(aReader), ReadFloat(aReader), ReadFloat(aReader)};
    Rotation = {ReadFloat(aReader), ReadFloat(aReader), ReadFloat(aReader)};
    uint64_t motionType{};
    CheckedRead::Bits(aReader, motionType, 8);
    MotionType = static_cast<uint8_t>(motionType);
    LinearVelocity = {ReadFloat(aReader), ReadFloat(aReader), ReadFloat(aReader)};
    BodyTransform.fill(0.f);
    ChildBodies.clear();
    if (MotionType == 3)
    {
        for (float& value : BodyTransform)
            value = ReadFloat(aReader);
        uint64_t count{};
        CheckedRead::Bits(aReader, count, 8);
        if (count > kMaxChildBodies || count > CheckedRead::RemainingBits(aReader) / (7 * 32))
            throw std::runtime_error("physics child count exceeds limit");
        ChildBodies.resize(count);
        for (auto& body : ChildBodies)
            for (float& value : body)
                value = ReadFloat(aReader);
    }
}
