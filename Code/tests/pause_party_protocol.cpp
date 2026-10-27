#include <TiltedCore/Stl.hpp>
#include <TiltedCore/Allocator.hpp>
#include <TiltedCore/Buffer.hpp>
#include <TiltedCore/Serialization.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <catch2/catch.hpp>

#include <Messages/ClientMessageFactory.h>
#include <Messages/ServerMessageFactory.h>
#include <Messages/PartyKickRequest.h>
#include <Messages/NotifyPartyLeft.h>
#include <Messages/PartySessionSettingsRequest.h>

TEST_CASE("END PARTY request and forced leave survive the wire", "[encoding.pause_party]")
{
    PartyKickRequest end;
    end.PartyMemberPlayerId = 0; // reserved END PARTY command, leader checked on server
    TiltedPhoques::Buffer requestBuffer(128);
    TiltedPhoques::Buffer::Writer requestWriter(&requestBuffer);
    end.Serialize(requestWriter);
    TiltedPhoques::Buffer::Reader requestReader(&requestBuffer);
    auto decodedRequest = ClientMessageFactory{}.Extract(requestReader);
    REQUIRE(decodedRequest);
    REQUIRE(decodedRequest->GetOpcode() == kPartyKickRequest);
    CHECK(static_cast<PartyKickRequest&>(*decodedRequest).PartyMemberPlayerId == 0);

    for (const bool forced : {false, true})
    {
        NotifyPartyLeft left;
        left.Forced = forced;
        TiltedPhoques::Buffer notifyBuffer(128);
        TiltedPhoques::Buffer::Writer notifyWriter(&notifyBuffer);
        left.Serialize(notifyWriter);
        TiltedPhoques::Buffer::Reader notifyReader(&notifyBuffer);
        auto decodedNotify = ServerMessageFactory{}.Extract(notifyReader);
        REQUIRE(decodedNotify);
        REQUIRE(decodedNotify->GetOpcode() == kNotifyPartyLeft);
        CHECK(static_cast<NotifyPartyLeft&>(*decodedNotify).Forced == forced);
    }
}

TEST_CASE("Native access toggle preserves the password intent on the wire", "[encoding.pause_party]")
{
    PartySessionSettingsRequest request;
    request.Open = false;
    request.KeepPassword = true;
    TiltedPhoques::Buffer buffer(128);
    TiltedPhoques::Buffer::Writer writer(&buffer);
    request.Serialize(writer);
    TiltedPhoques::Buffer::Reader reader(&buffer);
    auto decoded = ClientMessageFactory{}.Extract(reader);
    REQUIRE(decoded);
    REQUIRE(decoded->GetOpcode() == kPartySessionSettingsRequest);
    auto& settings = static_cast<PartySessionSettingsRequest&>(*decoded);
    CHECK_FALSE(settings.Open);
    CHECK(settings.KeepPassword);
    CHECK(settings.Password.empty());
}
