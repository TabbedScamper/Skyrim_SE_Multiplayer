#include <Messages/NotifyDismember.h>
#include <TiltedCore/Serialization.hpp>
#include <Structs/CheckedRead.h>
#include <limits>

void NotifyDismember::SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    TiltedPhoques::Serialization::WriteVarInt(aWriter, ServerId);
    TiltedPhoques::Serialization::WriteVarInt(aWriter, Limb);
    aWriter.WriteBits(Tick, 64);
}

void NotifyDismember::DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    m_valid = false;
    try
    {
        const auto id = CheckedRead::VarInt(aReader);
        const auto limb = CheckedRead::VarInt(aReader);
        CheckedRead::Bits(aReader, Tick, 64);
        ServerId = static_cast<uint32_t>(id);
        Limb = static_cast<uint32_t>(limb);
        m_valid = id <= (std::numeric_limits<uint32_t>::max)() && limb == 1 && Tick != 0;
    }
    catch (...)
    {
        m_valid = false;
    }
}
