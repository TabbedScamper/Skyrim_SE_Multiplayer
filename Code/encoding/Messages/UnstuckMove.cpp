#include <Messages/UnstuckMove.h>
#include <TiltedCore/Serialization.hpp>
#include <Structs/CheckedRead.h>
#include <bit>
#include <cmath>

void UnstuckMove::Serialize(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    TiltedPhoques::Serialization::WriteVarInt(aWriter, Epoch);
    TiltedPhoques::Serialization::WriteVarInt(aWriter, Sequence);
    CellId.Serialize(aWriter);
    WorldSpaceId.Serialize(aWriter);
    Position.Serialize(aWriter);
    aWriter.WriteBits(std::bit_cast<uint32_t>(Heading), 32);
}

bool UnstuckMove::Deserialize(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    try
    {
        Epoch = CheckedRead::VarInt(aReader);
        Sequence = CheckedRead::VarInt(aReader);
        for (auto* pId : {&CellId, &WorldSpaceId})
        {
            const auto base = CheckedRead::VarInt(aReader);
            const auto mod = CheckedRead::VarInt(aReader);
            if (base > UINT32_MAX || mod > UINT32_MAX)
                return false;
            *pId = GameId(static_cast<uint32_t>(mod), static_cast<uint32_t>(base));
        }
        uint64_t value{};
        CheckedRead::Bits(aReader, value, 64);
        Position.Unpack(value);
        CheckedRead::Bits(aReader, value, 32);
        Heading = std::bit_cast<float>(static_cast<uint32_t>(value));
        return IsValid();
    }
    catch (...)
    {
        return false;
    }
}

bool UnstuckMove::IsValid() const noexcept
{
    return Sequence != 0 && static_cast<bool>(CellId) &&
        std::isfinite(Position.x) && std::isfinite(Position.y) &&
        std::isfinite(Position.z) && std::abs(Position.x) <= 1048575.f &&
        std::abs(Position.y) <= 1048575.f && std::abs(Position.z) <= 1048575.f &&
        std::isfinite(Heading) && std::abs(Heading) <= 6.284f;
}
