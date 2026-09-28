#pragma once

#include "Message.h"

#include <Structs/GameId.h>

struct RequestWeatherChange final : ClientMessage
{
    static constexpr ClientOpcode Opcode = kRequestWeatherChange;

    RequestWeatherChange()
        : ClientMessage(Opcode)
    {
    }

    void SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept override;
    void DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept override;

    bool operator==(const RequestWeatherChange& acRhs) const noexcept { return GetOpcode() == acRhs.GetOpcode() && Id == acRhs.Id && HasSky == acRhs.HasSky && LastId == acRhs.LastId &&
            Percent == acRhs.Percent && WindSpeed == acRhs.WindSpeed && WindAngle == acRhs.WindAngle; }

    GameId Id;
    // Sky state (host authority): the weather being blended from, the blend percentage and the wind. The
    // engine picks each weather's wind angle with its own RNG (ID 26229), so each PC rolled a different one.
    bool HasSky{};
    GameId LastId{};
    float Percent{1.f};
    float WindSpeed{};
    float WindAngle{};
};
