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
#include <Structs/Factions.h>
#include <Structs/TimeModel.h>
#include <common/DateTime.h>

using namespace TiltedPhoques;

TEST_CASE("Weather change carries the host sky state (blend and wind)", "[encoding.weather]")
{
    Buffer buffer(256);
    {
        RequestWeatherChange request;
        request.Id = GameId(0, 0x12F89);
        request.HasSky = true;
        request.WorldSpaceId = GameId(0, 0x3C);
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
        notify.WorldSpaceId = GameId(0, 0x3C);
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

TEST_CASE("Crime faction and calendar day survive network serialization", "[encoding.weather]")
{
    Buffer buffer(256);
    Factions factions;
    factions.CrimeFaction = GameId(1, 0x1234);
    factions.RemoveCrimeFaction = true;
    factions.ExtraFactions.push_back(Faction{});
    factions.ExtraFactions.back().Id = GameId(1, 0x1234);
    factions.ExtraFactions.back().Rank = 1;
    {
        Buffer::Writer writer(&buffer);
        factions.Serialize(writer);
        Buffer::Reader reader(&buffer);
        Factions decoded;
        decoded.Deserialize(reader);
        CHECK(decoded == factions);
        CHECK(decoded.CrimeFaction == GameId(1, 0x1234));
        CHECK(decoded.RemoveCrimeFaction);
    }

    Buffer clockBuffer(128);
    TimeModel time;
    time.Time = 23.5f;
    time.DaysPassed = 109.979164f;
    {
        Buffer::Writer writer(&clockBuffer);
        time.Serialize(writer);
        Buffer::Reader reader(&clockBuffer);
        TimeModel decoded;
        decoded.Deserialize(reader);
        CHECK(decoded == time);
        CHECK(decoded.DaysPassed == time.DaysPassed);
    }
    DateTime running(time);
    running.Update(90000); // 90 real seconds at timescale 20 crosses midnight.
    CHECK(running.m_timeModel.Time < 0.01f);
    CHECK(running.m_timeModel.DaysPassed == Approx(110.f).margin(0.001f));
}
