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
#include <Messages/CheckpointRoster.h>
#include <Messages/DropIn.h>
#include <Messages/AuthenticationRequest.h>

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

TEST_CASE("Checkpoint roster and drop-in placement round trip", "[encoding.checkpoint_roster]")
{
    RosterPosition rex;
    rex.SteamId = 76561198000000123ull;
    rex.CharacterKey = "A1B2C3D4";
    rex.CharacterName = "Rex";
    rex.WorldSpaceId = {2, 0x37};
    rex.CellId = {2, 0x1234};
    rex.X = 1420.5f; rex.Y = -302.25f; rex.Z = 71.f; rex.Heading = 1.25f;
    rex.Downed = true;

    RequestCheckpointRoster report;
    report.CheckpointId = "00000001_1790000000";
    report.Position = rex;
    Buffer reportBuffer(512);
    Buffer::Writer reportWriter(&reportBuffer);
    report.Serialize(reportWriter);
    Buffer::Reader reportReader(&reportBuffer);
    auto decodedReport = ClientMessageFactory{}.Extract(reportReader);
    REQUIRE(decodedReport);
    const auto& gotReport = static_cast<const RequestCheckpointRoster&>(*decodedReport);
    REQUIRE(gotReport.CheckpointId == report.CheckpointId);
    REQUIRE(gotReport.Position == rex);

    NotifyCheckpointRoster roster;
    roster.CheckpointId = report.CheckpointId;
    roster.Entries.push_back(rex);
    Buffer rosterBuffer(512);
    Buffer::Writer rosterWriter(&rosterBuffer);
    roster.Serialize(rosterWriter);
    Buffer::Reader rosterReader(&rosterBuffer);
    auto decodedRoster = ServerMessageFactory{}.Extract(rosterReader);
    REQUIRE(decodedRoster);
    const auto& gotRoster = static_cast<const NotifyCheckpointRoster&>(*decodedRoster);
    REQUIRE(gotRoster.CheckpointId == roster.CheckpointId);
    REQUIRE(gotRoster.Entries.size() == 1);
    REQUIRE(gotRoster.Entries[0] == rex);

    NotifyDropIn done;
    done.Attempt = 0x1234;
    done.Op = DropInOp::Done;
    done.HasPosition = true;
    done.Position = rex;
    Buffer doneBuffer(512);
    Buffer::Writer doneWriter(&doneBuffer);
    static_cast<const ServerMessage&>(done).Serialize(doneWriter);
    Buffer::Reader doneReader(&doneBuffer);
    auto decodedDone = ServerMessageFactory{}.Extract(doneReader);
    REQUIRE(decodedDone);
    REQUIRE(static_cast<const NotifyDropIn&>(*decodedDone) == done);
}

TEST_CASE("Authentication binds the reported Steam account to the connection", "[encoding.checkpoint_roster]")
{
    AuthenticationRequest request;
    request.SteamId = 76561198000000123ull;
    request.Version = "test";
    request.Username = "Rex";
    Buffer buffer(1024);
    Buffer::Writer writer(&buffer);
    request.Serialize(writer);
    Buffer::Reader reader(&buffer);
    auto decoded = ClientMessageFactory{}.Extract(reader);
    REQUIRE(decoded);
    REQUIRE(static_cast<const AuthenticationRequest&>(*decoded).SteamId == request.SteamId);
}

TEST_CASE("Interior checkpoint rosters preserve five and ten distinct players", "[encoding.checkpoint_roster]")
{
    for (const size_t count : {5u, 10u})
    {
        NotifyCheckpointRoster roster;
        roster.CheckpointId = "interior";
        for (size_t i = 0; i < count; ++i)
        {
            RosterPosition player;
            player.SteamId = 76561198000000123ull + i;
            player.CharacterKey = "A1B2C3D4";
            player.CharacterName = "Renamed Rex";
            player.CellId = {3, 0x1234};
            player.X = static_cast<float>(i) * 100.f;
            player.Heading = static_cast<float>(i) * 0.5f;
            player.Downed = (i % 2) != 0;
            roster.Entries.push_back(player);
        }
        Buffer buffer(4096);
        Buffer::Writer writer(&buffer);
        roster.Serialize(writer);
        Buffer::Reader reader(&buffer);
        auto decoded = ServerMessageFactory{}.Extract(reader);
        REQUIRE(decoded);
        REQUIRE(static_cast<const NotifyCheckpointRoster&>(*decoded).Entries == roster.Entries);
    }
}
