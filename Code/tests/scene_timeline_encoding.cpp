#include <TiltedCore/Stl.hpp>
#include <TiltedCore/Allocator.hpp>
#include <TiltedCore/Buffer.hpp>
#include <TiltedCore/Serialization.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <catch2/catch.hpp>

#include <Messages/ClientMessageFactory.h>
#include <Messages/ServerMessageFactory.h>
#include <Messages/SceneTimelineRequest.h>
#include <Messages/NotifySceneTimeline.h>

using namespace TiltedPhoques;

TEST_CASE("Scene timeline request and notification round trip", "[encoding.scene_timeline]")
{
    SceneTimelineSnapshot snapshot{};
    snapshot.Tick = 123456;
    snapshot.AuthorityEpoch = 17;
    snapshot.TransactionId = 7788;
    snapshot.SceneId = GameId{1, 0xBECD4};
    snapshot.QuestId = GameId{1, 0x3372B};
    snapshot.RawPhaseWord = 4;
    snapshot.Playing = true;
    REQUIRE(snapshot.IsValid());

    SceneTimelineRequest request{};
    request.Snapshot = snapshot;
    Buffer requestBuffer(128);
    Buffer::Writer requestWriter(&requestBuffer);
    request.Serialize(requestWriter);
    Buffer::Reader requestReader(&requestBuffer);
    const ClientMessageFactory clientFactory;
    auto decodedRequest = clientFactory.Extract(requestReader);
    REQUIRE(decodedRequest);
    REQUIRE(decodedRequest->GetOpcode() == request.GetOpcode());
    REQUIRE(static_cast<SceneTimelineRequest&>(*decodedRequest).Snapshot == snapshot);

    NotifySceneTimeline notify{};
    notify.Snapshot = snapshot;
    notify.Snapshot.ServerSequence = 42;
    Buffer notifyBuffer(128);
    Buffer::Writer notifyWriter(&notifyBuffer);
    notify.Serialize(notifyWriter);
    Buffer::Reader notifyReader(&notifyBuffer);
    const ServerMessageFactory serverFactory;
    auto decodedNotify = serverFactory.Extract(notifyReader);
    REQUIRE(decodedNotify);
    REQUIRE(decodedNotify->GetOpcode() == notify.GetOpcode());
    REQUIRE(static_cast<NotifySceneTimeline&>(*decodedNotify).Snapshot == notify.Snapshot);
}

TEST_CASE("Scene timeline rejects missing authority fields", "[encoding.scene_timeline]")
{
    SceneTimelineSnapshot snapshot{};
    REQUIRE_FALSE(snapshot.IsValid());
}
