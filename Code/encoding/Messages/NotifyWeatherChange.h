#pragma once

#include "Message.h"

#include <Structs/GameId.h>

struct NotifyWeatherChange final : ServerMessage
{
    static constexpr ServerOpcode Opcode = kNotifyWeatherChange;

    NotifyWeatherChange()
        : ServerMessage(Opcode)
    {
    }

    void SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept override;
    void DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept override;

    bool operator==(const NotifyWeatherChange& acRhs) const noexcept { return GetOpcode() == acRhs.GetOpcode() && Id == acRhs.Id && HasSky == acRhs.HasSky && LastId == acRhs.LastId &&
            Percent == acRhs.Percent && WindSpeed == acRhs.WindSpeed && WindAngle == acRhs.WindAngle; }

    GameId Id{};
    // Sky state (host authority): see RequestWeatherChange.
    bool HasSky{};
    GameId LastId{};
    float Percent{1.f};
    float WindSpeed{};
    float WindAngle{};
};
