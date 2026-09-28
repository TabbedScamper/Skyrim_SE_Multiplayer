#include <TiltedCore/Stl.hpp>
#include <TiltedCore/Allocator.hpp>
#include <TiltedCore/Buffer.hpp>
#include <TiltedCore/Serialization.hpp>
#include <TiltedCore/ViewBuffer.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <catch2/catch.hpp>
#include <Messages/AnimObjectRequest.h>
#include <Messages/NotifyAnimObject.h>
#include <Messages/ClientMessageFactory.h>
#include <Messages/ServerMessageFactory.h>

using namespace TiltedPhoques;
namespace
{
template <class T> T RoundTrip(const T& source)
{
    Buffer buffer(1024);
    Buffer::Writer writer(&buffer);
    source.SerializeRaw(writer);
    ViewBuffer view(buffer.GetWriteData(), writer.Size());
    Buffer::Reader reader(&view);
    T result;
    result.DeserializeRaw(reader);
    return result;
}
} // namespace

// The Helgen headsman's axe (AnimObjectExecutionerAxe) is attached by his idle clip's graph events on the owner only;
// the owner's props travel to the copies as the ANIO's server id.
TEST_CASE("Anim object prop events survive the owner request and the server relay", "[anim_object]")
{
    for (const uint8_t kind : {AnimObjectRequest::kLoad, AnimObjectRequest::kDraw, AnimObjectRequest::kDetach})
    {
        AnimObjectRequest request;
        request.Id = 0x2A;
        request.Kind = kind;
        request.AnimObject = GameId(0, 0x2E8E5); // AnimObjectExecutionerAxe
        const auto sent = RoundTrip(request);
        REQUIRE(sent == request);

        NotifyAnimObject relay;
        relay.Id = sent.Id;
        relay.Kind = sent.Kind;
        relay.AnimObject = sent.AnimObject;
        const auto received = RoundTrip(relay);
        REQUIRE(received == relay);
        REQUIRE(received.AnimObject == GameId(0, 0x2E8E5));
        REQUIRE(received.Kind == kind);

        // Through the opcode factories, as the transport decodes them.
        Buffer requestBuffer(256);
        Buffer::Writer requestWriter(&requestBuffer);
        request.Serialize(requestWriter);
        Buffer::Reader requestReader(&requestBuffer);
        const ClientMessageFactory clientFactory;
        auto decodedRequest = clientFactory.Extract(requestReader);
        REQUIRE(decodedRequest);
        REQUIRE(decodedRequest->GetOpcode() == request.GetOpcode());
        REQUIRE(static_cast<AnimObjectRequest&>(*decodedRequest) == request);
        Buffer notifyBuffer(256);
        Buffer::Writer notifyWriter(&notifyBuffer);
        relay.Serialize(notifyWriter);
        Buffer::Reader notifyReader(&notifyBuffer);
        const ServerMessageFactory serverFactory;
        auto decodedNotify = serverFactory.Extract(notifyReader);
        REQUIRE(decodedNotify);
        REQUIRE(decodedNotify->GetOpcode() == relay.GetOpcode());
        REQUIRE(static_cast<NotifyAnimObject&>(*decodedNotify) == relay);
    }
}
