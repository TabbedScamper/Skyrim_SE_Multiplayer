#include <Messages/ScriptedCameraState.h>
#include <Structs/CheckedRead.h>
#include <bit>
#include <cmath>

void ScriptedCameraState::Serialize(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    TiltedPhoques::Serialization::WriteVarInt(aWriter, Epoch);
    TiltedPhoques::Serialization::WriteVarInt(aWriter, Sequence);
    aWriter.WriteBits(Active, 1);
    aWriter.WriteBits(FreeLook, 1);
    aWriter.WriteBits(FreePov, 1);
    aWriter.WriteBits(Controls, 8);
    aWriter.WriteBits(StateId, 8);
    aWriter.WriteBits(Walking, 8);
    WalkingStart.Serialize(aWriter);
    WalkingEnd.Serialize(aWriter);
    for (const float value : Rotation)
        aWriter.WriteBits(std::bit_cast<uint32_t>(value), 32);
    aWriter.WriteBits(std::bit_cast<uint32_t>(Pitch), 32);
    aWriter.WriteBits(std::bit_cast<uint32_t>(Heading), 32);
}

bool ScriptedCameraState::Deserialize(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    try
    {
        Epoch = CheckedRead::VarInt(aReader);
        Sequence = CheckedRead::VarInt(aReader);
        Active = CheckedRead::Bool(aReader);
        FreeLook = CheckedRead::Bool(aReader);
        FreePov = CheckedRead::Bool(aReader);
        uint64_t value{};
        CheckedRead::Bits(aReader, value, 8);
        Controls = static_cast<uint8_t>(value);
        CheckedRead::Bits(aReader, value, 8);
        StateId = static_cast<uint8_t>(value);
        CheckedRead::Bits(aReader, value, 8);
        Walking = static_cast<uint8_t>(value);
        for (auto* pId : {&WalkingStart, &WalkingEnd})
        {
            const auto base = CheckedRead::VarInt(aReader);
            const auto mod = CheckedRead::VarInt(aReader);
            if (base > UINT32_MAX || mod > UINT32_MAX)
                return false;
            *pId = GameId(static_cast<uint32_t>(mod), static_cast<uint32_t>(base));
        }
        for (float& item : Rotation)
        {
            CheckedRead::Bits(aReader, value, 32);
            item = std::bit_cast<float>(static_cast<uint32_t>(value));
        }
        CheckedRead::Bits(aReader, value, 32);
        Pitch = std::bit_cast<float>(static_cast<uint32_t>(value));
        CheckedRead::Bits(aReader, value, 32);
        Heading = std::bit_cast<float>(static_cast<uint32_t>(value));
        return IsValid();
    }
    catch (...)
    {
        return false;
    }
}

bool ScriptedCameraState::IsValid() const noexcept
{
    if (!Epoch || !Sequence || StateId >= 13 || Walking > 2 ||
        (Controls & ~kCameraControls) || (FreeLook && !(Controls & 2)) ||
        (FreePov && !(Controls & 0x20)) || !std::isfinite(Pitch) ||
        !std::isfinite(Heading) || std::abs(Pitch) > 6.284f || std::abs(Heading) > 6.284f)
        return false;
    float length{};
    for (const float value : Rotation)
    {
        if (!std::isfinite(value))
            return false;
        length += value * value;
    }
    return length >= 0.99f && length <= 1.01f;
}
