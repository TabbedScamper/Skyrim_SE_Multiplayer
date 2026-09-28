#include <TiltedCore/Stl.hpp>
#include <TiltedCore/Buffer.hpp>
#include <TiltedCore/Serialization.hpp>

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include <catch2/catch.hpp>

#include <Messages/ClientMessageFactory.h>
#include <Messages/ServerMessageFactory.h>
#include <Messages/RequestWeatherChange.h>
#include <Messages/NotifyWeatherChange.h>

using namespace TiltedPhoques;

TEST_CASE("Weather change carries the host sky state (blend and wind)", "[encoding.weather]")
{
    Buffer buffer(256);
    {
        RequestWeatherChange request;
        request.Id = GameId(0, 0x12F89);
        request.HasSky = true;
        request.LastId = GameId(0, 0x10A242);
        request.Percent = 0.375f;
        request.WindSpeed = 0.6117647f;
        request.WindAngle = 2.8274333f;

        Buffer::Writer writer(&buffer);
        request.Serialize(writer);
        Buffer::Reader reader(&buffer);
        auto pMessage = ClientMessageFactory().Extract(reader);
        REQUIRE(pMessage);
        auto pRequest = CastUnique<RequestWeatherChange>(std::move(pMessage));
        REQUIRE(*pRequest == request);
    }
    {
        NotifyWeatherChange notify;
        notify.Id = GameId(0, 0x12F89);
        notify.HasSky = true;
        notify.LastId = GameId(0, 0x10A242);
        notify.Percent = 1.f;
        notify.WindSpeed = 0.2f;
        notify.WindAngle = -1.5f;

        Buffer::Writer writer(&buffer);
        notify.Serialize(writer);
        Buffer::Reader reader(&buffer);
        auto pMessage = ServerMessageFactory().Extract(reader);
        REQUIRE(pMessage);
        auto pNotify = CastUnique<NotifyWeatherChange>(std::move(pMessage));
        REQUIRE(*pNotify == notify);
    }
    {
        // Without sky state (older senders, map weather) the message stays the plain weather id.
        NotifyWeatherChange plain;
        plain.Id = GameId(0, 0x81A);

        Buffer::Writer writer(&buffer);
        plain.Serialize(writer);
        Buffer::Reader reader(&buffer);
        auto pMessage = ServerMessageFactory().Extract(reader);
        REQUIRE(pMessage);
        auto pNotify = CastUnique<NotifyWeatherChange>(std::move(pMessage));
        REQUIRE(*pNotify == plain);
        REQUIRE_FALSE(pNotify->HasSky);
    }
}
