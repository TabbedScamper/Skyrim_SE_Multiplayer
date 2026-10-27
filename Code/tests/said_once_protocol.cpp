#include <TiltedCore/Stl.hpp>
#include <TiltedCore/Allocator.hpp>
#include <TiltedCore/Buffer.hpp>
#include <TiltedCore/Serialization.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <catch2/catch.hpp>

#include <Messages/ClientMessageFactory.h>
#include <Messages/ServerMessageFactory.h>
#include <Messages/RequestSaidOnce.h>
#include <Messages/NotifySaidOnce.h>

TEST_CASE("Said-once INFO identity survives both wire directions", "[encoding.said_once]")
{
    RequestSaidOnce request;
    request.Info = GameId{2, 0xA123};
    TiltedPhoques::Buffer requestBuffer(128);
    TiltedPhoques::Buffer::Writer requestWriter(&requestBuffer);
    request.Serialize(requestWriter);
    TiltedPhoques::Buffer::Reader requestReader(&requestBuffer);
    auto decodedRequest = ClientMessageFactory{}.Extract(requestReader);
    REQUIRE(decodedRequest);
    REQUIRE(decodedRequest->GetOpcode() == kRequestSaidOnce);
    CHECK(static_cast<RequestSaidOnce&>(*decodedRequest).Info == request.Info);

    NotifySaidOnce notify;
    notify.Info = request.Info;
    TiltedPhoques::Buffer notifyBuffer(128);
    TiltedPhoques::Buffer::Writer notifyWriter(&notifyBuffer);
    notify.Serialize(notifyWriter);
    TiltedPhoques::Buffer::Reader notifyReader(&notifyBuffer);
    auto decodedNotify = ServerMessageFactory{}.Extract(notifyReader);
    REQUIRE(decodedNotify);
    REQUIRE(decodedNotify->GetOpcode() == kNotifySaidOnce);
    CHECK(static_cast<NotifySaidOnce&>(*decodedNotify).Info == request.Info);
}
