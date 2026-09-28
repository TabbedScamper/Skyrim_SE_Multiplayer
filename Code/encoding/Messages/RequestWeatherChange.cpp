#include <Messages/RequestWeatherChange.h>

#include <cstring>

void RequestWeatherChange::SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    Id.Serialize(aWriter);
    aWriter.WriteBits(HasSky ? 1 : 0, 8);
    if (!HasSky)
        return;
    LastId.Serialize(aWriter);
    for (float value : {Percent, WindSpeed, WindAngle})
    {
        uint32_t bits{};
        std::memcpy(&bits, &value, sizeof(bits));
        aWriter.WriteBits(bits, 32);
    }
}

void RequestWeatherChange::DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    ClientMessage::DeserializeRaw(aReader);

    Id.Deserialize(aReader);
    uint64_t flag{};
    aReader.ReadBits(flag, 8);
    HasSky = flag != 0;
    if (!HasSky)
        return;
    LastId.Deserialize(aReader);
    for (float* value : {&Percent, &WindSpeed, &WindAngle})
    {
        uint64_t bits{};
        aReader.ReadBits(bits, 32);
        const auto raw = static_cast<uint32_t>(bits);
        std::memcpy(value, &raw, sizeof(raw));
    }
}
