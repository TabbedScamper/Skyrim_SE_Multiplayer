#include <TiltedCore/Stl.hpp>
#include <TiltedCore/Buffer.hpp>
#include <TiltedCore/Serialization.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <catch2/catch.hpp>
#include <Messages/ServerMessageFactory.h>
#include <Messages/ServerTimeSettings.h>

TEST_CASE("Server clock sample preserves integer time and ordering fields", "[encoding.clock]")
{
    ServerTimeSettings sent;
    sent.timeModel.TimeScale = 20.f;
    sent.timeModel.Time = 6.5f;
    sent.timeModel.DaysPassed = 1234.2708f;
    sent.GameMs = 1234ull * 86400000 + 23400000;
    sent.SampleTick = 123456789;
    sent.Seq = 42;
    sent.ServerNonce = 0x1234567890abcdef;

    TiltedPhoques::Buffer buffer(256);
    TiltedPhoques::Buffer::Writer writer(&buffer);
    sent.Serialize(writer);
    TiltedPhoques::Buffer::Reader reader(&buffer);
    auto decoded = TiltedPhoques::CastUnique<ServerTimeSettings>(ServerMessageFactory().Extract(reader));
    REQUIRE(decoded);
    REQUIRE(*decoded == sent);
    REQUIRE(decoded->GameMs == sent.GameMs);
    REQUIRE(decoded->SampleTick == sent.SampleTick);
    REQUIRE(decoded->Seq == sent.Seq);
    REQUIRE(decoded->ServerNonce == sent.ServerNonce);
}
