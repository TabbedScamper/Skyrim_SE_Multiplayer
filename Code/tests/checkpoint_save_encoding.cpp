#include <TiltedCore/Stl.hpp>
#include <TiltedCore/Allocator.hpp>
#include <TiltedCore/Buffer.hpp>
#include <TiltedCore/Serialization.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <catch2/catch.hpp>

#include <Messages/ClientMessageFactory.h>
#include <Messages/ServerMessageFactory.h>
#include <Messages/CheckpointSaveRequest.h>
#include <Messages/NotifyCheckpointSave.h>

using namespace TiltedPhoques;

TEST_CASE("Checkpoint save request and notification round trip", "[encoding.checkpoint_save]")
{
    CheckpointSaveRequest request{};
    request.CheckpointId = "1a2b3c4d_1790000000";
    Buffer requestBuffer(128);
    Buffer::Writer requestWriter(&requestBuffer);
    request.Serialize(requestWriter);
    Buffer::Reader requestReader(&requestBuffer);
    const ClientMessageFactory clientFactory;
    auto decodedRequest = clientFactory.Extract(requestReader);
    REQUIRE(decodedRequest);
    REQUIRE(decodedRequest->GetOpcode() == request.GetOpcode());
    REQUIRE(static_cast<CheckpointSaveRequest&>(*decodedRequest) == request);

    NotifyCheckpointSave notify{};
    notify.CheckpointId = request.CheckpointId;
    notify.AuthorityEpoch = 0x0123456789ABCDEFull;
    Buffer notifyBuffer(128);
    Buffer::Writer notifyWriter(&notifyBuffer);
    notify.Serialize(notifyWriter);
    Buffer::Reader notifyReader(&notifyBuffer);
    const ServerMessageFactory serverFactory;
    auto decodedNotify = serverFactory.Extract(notifyReader);
    REQUIRE(decodedNotify);
    REQUIRE(decodedNotify->GetOpcode() == notify.GetOpcode());
    REQUIRE(static_cast<NotifyCheckpointSave&>(*decodedNotify) == notify);
}
