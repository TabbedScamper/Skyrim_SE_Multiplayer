#include <TiltedCore/Buffer.hpp>
#include <catch2/catch.hpp>

#include <Structs/LoadingScreenPresentation.h>

using namespace TiltedPhoques;

TEST_CASE("Loading-screen presentation payload round trips", "[encoding.loading_screen]")
{
    const LoadingScreenPresentation expected{
        0xFEDCBA9876543210ULL,
        GameId{0x1234, 0x00ABCDEF},
    };

    Buffer buffer(64);
    Buffer::Writer writer(&buffer);
    expected.Serialize(writer);

    Buffer::Reader reader(&buffer);
    LoadingScreenPresentation actual{};
    actual.Deserialize(reader);

    REQUIRE(actual == expected);
}

TEST_CASE("Loading-screen presentation payload preserves the empty identity", "[encoding.loading_screen]")
{
    const LoadingScreenPresentation expected{};

    Buffer buffer(32);
    Buffer::Writer writer(&buffer);
    expected.Serialize(writer);

    Buffer::Reader reader(&buffer);
    LoadingScreenPresentation actual{};
    actual.Deserialize(reader);

    REQUIRE(actual == expected);
}
