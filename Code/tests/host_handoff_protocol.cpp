#include <TiltedCore/Stl.hpp>
#include <TiltedCore/Allocator.hpp>
#include <TiltedCore/Buffer.hpp>
#include <TiltedCore/ViewBuffer.hpp>
#include <TiltedCore/Serialization.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <catch2/catch.hpp>

#include <HandoffPolicy.h>
#include <Messages/HostHandoff.h>
#include <Messages/NotifyLedgerExtract.h>
#include <Messages/NotifyPartyInfo.h>
#include <Messages/ClientMessageFactory.h>
#include <Messages/ServerMessageFactory.h>

TEST_CASE("Handoff elects the next Steam account in party order", "[handoff]")
{
    const std::vector<uint64_t> order{101, 202, 303, 404, 505};
    CHECK(HandoffPolicy::Successor(order, 101) == 202);
    CHECK(HandoffPolicy::Successor(order, 303) == 404);
    CHECK(HandoffPolicy::Successor(order, 505) == 101);
    CHECK(HandoffPolicy::Successor(order, 999) == 0);
    CHECK(HandoffPolicy::Newer("00000001_1790000001", "0000000f_1790000000"));
    CHECK(HandoffPolicy::SameFamily("00000001_1790000000", "00000001_1790000001"));
    CHECK_FALSE(HandoffPolicy::SameFamily("00000002_1790000000", "00000001_1790000001"));
}

TEST_CASE("Handoff checkpoint acknowledgement survives the client wire", "[handoff][encoding]")
{
    RequestHandoffAck ack;
    ack.CheckpointId = "00000001_1790000000";
    TiltedPhoques::Buffer buffer(128);
    TiltedPhoques::Buffer::Writer writer(&buffer);
    ack.Serialize(writer);
    TiltedPhoques::Buffer::Reader reader(&buffer);
    auto decoded = ClientMessageFactory{}.Extract(reader);
    REQUIRE(decoded);
    CHECK(static_cast<RequestHandoffAck&>(*decoded).CheckpointId == ack.CheckpointId);
}

TEST_CASE("Five and ten members preserve Steam order, roster, and checkpoint availability", "[handoff][encoding]")
{
    for (const size_t count : {5u, 10u})
    {
        RequestHandoffSeed seed;
        seed.CheckpointId = "00000001_1790000000";
        NotifyPartyInfo party;
        party.LeaderPlayerId = 1;
        party.HandoffTargetSteamId = 76561198000000000ull;
        party.HandoffCheckpointId = seed.CheckpointId;
        party.HandoffReady = true;
        party.HandoffSeeded = true;
        for (size_t i = 0; i < count; ++i)
        {
            const auto steamId = 76561198000000000ull + i;
            seed.SteamIds.push_back(steamId);
            party.PlayerIds.push_back(static_cast<uint32_t>(i + 1));
            party.SteamIds.push_back(steamId);
            RosterPosition place;
            place.SteamId = steamId;
            place.CharacterKey = "A1B2C3D4";
            place.CharacterName = "Rex";
            place.CellId = {3, 0x1234};
            place.X = static_cast<float>(i * 100);
            seed.Entries.push_back(place);
        }
        TiltedPhoques::Buffer seedBuffer(8192);
        TiltedPhoques::Buffer::Writer seedWriter(&seedBuffer);
        seed.Serialize(seedWriter);
        TiltedPhoques::Buffer::Reader seedReader(&seedBuffer);
        auto decodedSeed = ClientMessageFactory{}.Extract(seedReader);
        REQUIRE(decodedSeed);
        auto& gotSeed = static_cast<RequestHandoffSeed&>(*decodedSeed);
        CHECK(gotSeed.SteamIds == seed.SteamIds);
        CHECK(gotSeed.Entries == seed.Entries);

        TiltedPhoques::Buffer infoBuffer(4096);
        TiltedPhoques::Buffer::Writer infoWriter(&infoBuffer);
        party.Serialize(infoWriter);
        TiltedPhoques::Buffer::Reader infoReader(&infoBuffer);
        auto decodedInfo = ServerMessageFactory{}.Extract(infoReader);
        REQUIRE(decodedInfo);
        CHECK(static_cast<NotifyPartyInfo&>(*decodedInfo) == party);

        RequestCheckpointAvailability available;
        available.Checkpoints = {"00000001_1790000000", "00000001_1780000000"};
        TiltedPhoques::Buffer availableBuffer(512);
        TiltedPhoques::Buffer::Writer availableWriter(&availableBuffer);
        available.Serialize(availableWriter);
        TiltedPhoques::Buffer::Reader availableReader(&availableBuffer);
        auto decodedAvailable = ClientMessageFactory{}.Extract(availableReader);
        REQUIRE(decodedAvailable);
        CHECK(static_cast<RequestCheckpointAvailability&>(*decodedAvailable).Checkpoints == available.Checkpoints);
    }
}

TEST_CASE("Ledger extract chunks survive the server wire and reject truncated data", "[handoff][encoding]")
{
    CHECK(kNotifyLedgerExtract == kNotifyContentStatus + 1);
    NotifyLedgerExtract extract;
    extract.CampaignId = 0x123456789abcdef0ull;
    extract.CheckpointId = "123456789abcdef0_2_1790000000";
    extract.ChunkIndex = 1;
    extract.ChunkCount = 2;
    extract.Data = String(4096, 'L');
    TiltedPhoques::Buffer buffer(8192);
    TiltedPhoques::Buffer::Writer writer(&buffer);
    extract.Serialize(writer);
    TiltedPhoques::Buffer::Reader reader(&buffer);
    auto decoded = ServerMessageFactory{}.Extract(reader);
    REQUIRE(decoded);
    auto& got = static_cast<NotifyLedgerExtract&>(*decoded);
    CHECK(got.CampaignId == extract.CampaignId);
    CHECK(got.CheckpointId == extract.CheckpointId);
    CHECK(got.ChunkIndex == extract.ChunkIndex);
    CHECK(got.ChunkCount == extract.ChunkCount);
    CHECK(got.Data == extract.Data);
    for (const size_t size : {size_t{0}, size_t{16}, writer.Size() - 1})
    {
        TiltedPhoques::ViewBuffer view(buffer.GetWriteData(), size);
        TiltedPhoques::Buffer::Reader shortReader(&view);
        CHECK_FALSE(ServerMessageFactory{}.Extract(shortReader));
    }
}
