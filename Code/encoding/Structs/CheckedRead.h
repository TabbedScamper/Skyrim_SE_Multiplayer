#pragma once

#include <TiltedCore/Buffer.hpp>
#include <TiltedCore/Serialization.hpp>
#include <TiltedCore/Stl.hpp>
#include <stdexcept>

namespace CheckedRead
{
inline size_t RemainingBits(const TiltedPhoques::Buffer::Reader& aReader)
{
    const auto size = aReader.GetBuffer()->GetSize() * 8;
    const auto position = aReader.GetBitPosition();
    return position <= size ? size - position : 0;
}

inline void Bits(TiltedPhoques::Buffer::Reader& aReader, uint64_t& aValue, size_t aCount)
{
    if (!aReader.ReadBits(aValue, aCount))
        throw std::runtime_error("truncated packet");
}

// Past the end of the buffer: the read ran into bytes that are not part of the message.
inline void CheckInBounds(const TiltedPhoques::Buffer::Reader& aReader)
{
    if (aReader.GetBitPosition() > aReader.GetBuffer()->GetSize() * 8)
        throw std::runtime_error("truncated packet");
}

// TiltedCore's encoding: 8-bit groups, 7 value bits and the continuation bit (0x80); rejects a
// missing final group and values over 64 bits.
inline uint64_t VarInt(TiltedPhoques::Buffer::Reader& aReader)
{
    uint64_t value{};
    for (unsigned shift = 0; shift < 64; shift += 7)
    {
        uint64_t group{};
        Bits(aReader, group, 8);
        const uint64_t bits = group & 0x7F;
        const bool more = (group & 0x80) != 0;
        if (shift == 63 && (bits > 1 || more))
            throw std::runtime_error("varint exceeds limit");
        value |= bits << shift;
        if (!more)
            return value;
    }
    throw std::runtime_error("invalid varint");
}

inline bool Bool(TiltedPhoques::Buffer::Reader& aReader)
{
    uint64_t value{};
    Bits(aReader, value, 1);
    return value != 0;
}

inline void Bytes(TiltedPhoques::Buffer::Reader& aReader, uint8_t* aData, size_t aCount)
{
    if (!aReader.ReadBytes(aData, aCount))
        throw std::runtime_error("truncated packet");
}

inline TiltedPhoques::String String(TiltedPhoques::Buffer::Reader& aReader)
{
    if (RemainingBits(aReader) < 8)
        throw std::runtime_error("truncated packet");
    TiltedPhoques::String value = TiltedPhoques::Serialization::ReadString(aReader);
    CheckInBounds(aReader);
    return value;
}
}
