#include <TiltedCore/Stl.hpp>
#include <TiltedCore/Serialization.hpp>
#include <TiltedCore/Buffer.hpp>
#include <Messages/RequestQuestAliasFills.h>
#include <Messages/NotifyQuestAliasFills.h>
#include <catch2/catch.hpp>

TEST_CASE("Quest stage run and replay wire", "[quest_stage_protocol]")
{
    RequestQuestAliasFills request;
    auto& run = request.Fills;
    run.Id = GameId(0, 0x3CEDA);
    run.AuthorityEpoch = 9;
    run.Sequence = 10;
    run.HasStage = run.HasSeq = run.HasRun = true;
    run.Stage = run.CurrentStageAfter = 10;
    run.Status = 0;
    run.TransactionId = 11;
    run.QuestSeq = 1;
    run.QuestFlags = 1;
    run.ItemCount = 6;
    run.ItemMask = 0x25;
    run.RunCount = 1;
    run.HasObjectives = true;
    run.Objectives = {{10, 1}, {20, 3}};
    run.Entries = {{1, GameId(0, 0x14)}};
    REQUIRE(run.IsValid());

    TiltedPhoques::Buffer requestBytes(4096);
    TiltedPhoques::Buffer::Writer requestWriter(&requestBytes);
    request.Serialize(requestWriter);
    TiltedPhoques::Buffer::Reader requestReader(&requestBytes);
    uint64_t opcode{};
    REQUIRE(requestReader.ReadBits(opcode, 8));
    RequestQuestAliasFills decoded;
    decoded.DeserializeRaw(requestReader);
    REQUIRE(decoded.IsValid());
    REQUIRE(decoded == request);

    NotifyQuestAliasFills notify;
    notify.Fills = run;
    notify.Fills.FillsIncluded = false;
    notify.Fills.Entries.clear();
    notify.Revision = 12;
    notify.LeaderPlayerId = 7;
    notify.Replay = true;
    TiltedPhoques::Buffer notifyBytes(4096);
    TiltedPhoques::Buffer::Writer notifyWriter(&notifyBytes);
    notify.Serialize(notifyWriter);
    TiltedPhoques::Buffer::Reader notifyReader(&notifyBytes);
    REQUIRE(notifyReader.ReadBits(opcode, 8));
    NotifyQuestAliasFills decodedNotify;
    decodedNotify.DeserializeRaw(notifyReader);
    REQUIRE(decodedNotify.IsValid());
    REQUIRE(decodedNotify == notify);
}

TEST_CASE("Quest stage wire refuses contradictory state", "[quest_stage_protocol]")
{
    QuestAliasFills record;
    record.Id = GameId(0, 0x3CEDA);
    record.AuthorityEpoch = 9;
    record.Sequence = 1;
    record.HasStage = record.HasSeq = true;
    record.TransactionId = 4;
    record.QuestSeq = 1;
    record.RecordKind = QuestAliasFills::State;
    REQUIRE(record.IsValid());
    record.HasRun = true;
    REQUIRE_FALSE(record.IsValid());
    record.HasRun = false;
    record.FillsIncluded = false;
    record.Entries.push_back({1, GameId(0, 0x14)});
    REQUIRE_FALSE(record.IsValid());
    record.Entries.clear();
    record.RecordKind = QuestAliasFills::SetStage;
    REQUIRE_FALSE(record.IsValid());
}
