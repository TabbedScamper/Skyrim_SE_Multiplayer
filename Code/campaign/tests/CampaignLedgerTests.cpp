#define CATCH_CONFIG_MAIN
#include <catch2/catch.hpp>

#include "CampaignLedger.h"

#include <atomic>
#include <filesystem>
#include <mutex>
#include <thread>

namespace
{
class TemporaryDatabase final
{
public:
    TemporaryDatabase()
    {
        static std::atomic_uint64_t counter{};
        Path = std::filesystem::temp_directory_path() /
            ("skyrim-se-multiplayer-campaign-" + std::to_string(++counter) + ".sqlite3");
        std::error_code error;
        std::filesystem::remove(Path, error);
    }

    ~TemporaryDatabase()
    {
        std::error_code error;
        std::filesystem::remove(Path, error);
        std::filesystem::remove(Path.string() + "-wal", error);
        std::filesystem::remove(Path.string() + "-shm", error);
    }

    std::filesystem::path Path;
};
} // namespace

TEST_CASE("Campaign ledger survives reopen with a stable identity", "[campaign][persistence]")
{
    TemporaryDatabase database;
    std::string campaignId;

    {
        Campaign::Ledger ledger(database.Path);
        const auto metadata = ledger.GetMetadata();
        campaignId = metadata.CampaignId;
        REQUIRE_FALSE(campaignId.empty());
        REQUIRE(metadata.AuthorityEpoch == 1);
        REQUIRE(metadata.Revision == 0);

        const auto commit = ledger.Commit("quest-player1-1", "quest", "mq101:stage=10");
        REQUIRE(commit.Inserted);
        REQUIRE(commit.Entry.Revision == 1);
    }

    {
        Campaign::Ledger ledger(database.Path);
        const auto metadata = ledger.GetMetadata();
        REQUIRE(metadata.CampaignId == campaignId);
        REQUIRE(metadata.Revision == 1);
        const auto entries = ledger.ReadAfter(0);
        REQUIRE(entries.size() == 1);
        REQUIRE(entries.front().Payload == "mq101:stage=10");
    }
}

TEST_CASE("Campaign transactions are monotonic and idempotent", "[campaign][idempotency]")
{
    TemporaryDatabase database;
    Campaign::Ledger ledger(database.Path);

    const auto first = ledger.Commit("txn-a", "quest", "stage=10");
    const auto duplicate = ledger.Commit("txn-a", "quest", "stage=10");
    const auto second = ledger.Commit("txn-b", "death", "player=2;state=downed");

    REQUIRE(first.Inserted);
    REQUIRE_FALSE(duplicate.Inserted);
    REQUIRE(duplicate.Entry.Revision == first.Entry.Revision);
    REQUIRE(second.Entry.Revision == first.Entry.Revision + 1);
    REQUIRE(ledger.GetMetadata().Revision == 2);
    REQUIRE_THROWS(ledger.Commit("txn-a", "quest", "stage=20"));
}

TEST_CASE("Campaign checkpoints retain a journal watermark", "[campaign][checkpoint]")
{
    TemporaryDatabase database;
    {
        Campaign::Ledger ledger(database.Path);
        static_cast<void>(ledger.Commit("txn-a", "quest", "stage=10"));
        static_cast<void>(ledger.Commit("txn-b", "inventory", "item=42"));
        REQUIRE(ledger.CreateCheckpoint("snapshot-at-two") == 2);
        static_cast<void>(ledger.Commit("txn-c", "quest", "stage=20"));
    }

    Campaign::Ledger reopened(database.Path);
    const auto metadata = reopened.GetMetadata();
    const auto checkpoint = reopened.GetLatestCheckpoint();
    REQUIRE(metadata.Revision == 3);
    REQUIRE(metadata.CheckpointRevision == 2);
    REQUIRE(checkpoint.Revision == 2);
    REQUIRE(checkpoint.Snapshot == "snapshot-at-two");
    const auto tail = reopened.ReadAfter(checkpoint.Revision);
    REQUIRE(tail.size() == 1);
    REQUIRE(tail.front().TransactionId == "txn-c");
}

TEST_CASE("Authority transitions are durable and idempotent", "[campaign][authority]")
{
    TemporaryDatabase database;
    Campaign::Ledger ledger(database.Path);

    const auto epoch = ledger.AdvanceAuthorityEpoch("host-migration-1", "host disconnected");
    const auto duplicateEpoch = ledger.AdvanceAuthorityEpoch("host-migration-1", "host disconnected");
    REQUIRE(epoch == 2);
    REQUIRE(duplicateEpoch == epoch);
    REQUIRE(ledger.GetMetadata().AuthorityEpoch == 2);
    REQUIRE(ledger.GetMetadata().Revision == 1);
    REQUIRE_THROWS(ledger.AdvanceAuthorityEpoch("host-migration-1", "different reason"));
}

TEST_CASE("Concurrent campaign commits retain one gapless order", "[campaign][concurrency]")
{
    TemporaryDatabase database;
    Campaign::Ledger ledger(database.Path);
    constexpr size_t threadCount = 8;
    constexpr size_t commitsPerThread = 40;
    std::vector<std::thread> threads;
    std::vector<std::exception_ptr> failures;
    std::mutex failureMutex;

    for (size_t thread = 0; thread < threadCount; ++thread)
    {
        threads.emplace_back([&, thread]
        {
            try
            {
                for (size_t commit = 0; commit < commitsPerThread; ++commit)
                {
                    const auto id = "thread-" + std::to_string(thread) + "-commit-" + std::to_string(commit);
                    static_cast<void>(ledger.Commit(id, "test", id));
                }
            }
            catch (...)
            {
                std::scoped_lock lock(failureMutex);
                failures.emplace_back(std::current_exception());
            }
        });
    }
    for (auto& thread : threads)
        thread.join();

    REQUIRE(failures.empty());
    const auto entries = ledger.ReadAfter(0, threadCount * commitsPerThread);
    REQUIRE(entries.size() == threadCount * commitsPerThread);
    for (size_t i = 0; i < entries.size(); ++i)
        REQUIRE(entries[i].Revision == i + 1);
    REQUIRE(ledger.GetMetadata().Revision == threadCount * commitsPerThread);
}

TEST_CASE("Quest item acquisition and hand-in survive reopening", "[campaign][quest-items]")
{
    TemporaryDatabase database;
    Campaign::QuestItem item{1, 0x39647, 1, 0x39645, 11, 0, 0, 1, true, true, 0};
    uint64_t acquired{};
    {
        Campaign::Ledger ledger(database.Path);
        auto result = ledger.SetQuestItem("pickup-1", item, 0);
        acquired = result.Entry.Revision;
        REQUIRE(result.Inserted);
        REQUIRE_FALSE(ledger.SetQuestItem("pickup-1", item, 0).Inserted);
        REQUIRE(ledger.ReadQuestItems().size() == 1);
    }
    {
        Campaign::Ledger ledger(database.Path);
        auto loaded = ledger.ReadQuestItems().front();
        REQUIRE(loaded.Active);
        REQUIRE(loaded.Revision == acquired);
        loaded.Active = false;
        auto released = ledger.SetQuestItem("hand-in-1", loaded, acquired);
        REQUIRE(released.Entry.Revision > acquired);
        REQUIRE_FALSE(ledger.SetQuestItem("hand-in-1", loaded, acquired).Inserted);
        REQUIRE_THROWS(ledger.SetQuestItem("stale-save-pickup", item, 0));
    }
    Campaign::Ledger ledger(database.Path);
    const auto records = ledger.ReadQuestItems();
    REQUIRE(records.size() == 1);
    REQUIRE_FALSE(records.front().Active);
    REQUIRE(ledger.GetMetadata().Revision == 2);
    REQUIRE(ledger.ReadAfter(0).back().Kind == "quest_item");
}

TEST_CASE("Quest item compare-and-set rejects stale and contradictory writes atomically", "[campaign][quest-items]")
{
    TemporaryDatabase database;
    Campaign::Ledger ledger(database.Path);
    Campaign::QuestItem item{1, 100, 1, 200, 0, 1, 300, 1, true, true, 0};
    const auto result = ledger.SetQuestItem("pickup", item, 0);
    auto changed = item;
    changed.Count = 2;
    REQUIRE_THROWS(ledger.SetQuestItem("pickup", changed, 0));
    REQUIRE_THROWS(ledger.SetQuestItem("stale", changed, 0));
    changed.Count = 0;
    REQUIRE_THROWS(ledger.SetQuestItem("bad-count", changed, result.Entry.Revision));
    REQUIRE(ledger.ReadQuestItems().front().Count == 1);
    REQUIRE(ledger.ReadAfter(0).size() == 1);
    REQUIRE(ledger.GetMetadata().Revision == result.Entry.Revision);
    changed = item;
    changed.AliasId = 1;
    changed.Active = false;
    REQUIRE_THROWS(ledger.SetQuestItem("unknown-hand-in", changed, 0));
}

TEST_CASE("Five simultaneous discoveries create one durable entitlement", "[campaign][quest-items][concurrency]")
{
    TemporaryDatabase database;
    Campaign::Ledger ledger(database.Path);
    Campaign::QuestItem item{1, 100, 1, 200, 7, 1, 300, 1, true, true, 0};
    std::atomic_uint successes{};
    std::vector<std::thread> players;
    for (unsigned i = 0; i < 5; ++i)
        players.emplace_back([&, i]
        {
            try
            {
                if (ledger.SetQuestItem("player-" + std::to_string(i), item, 0).Inserted)
                    ++successes;
            }
            catch (const std::runtime_error&) {}
        });
    for (auto& player : players)
        player.join();
    REQUIRE(successes == 1);
    REQUIRE(ledger.ReadQuestItems().size() == 1);
    REQUIRE(ledger.ReadQuestItems().front().Count == 1);
    REQUIRE(ledger.GetMetadata().Revision == 1);
}

TEST_CASE("Campaign databases and different aliases retain separate quest item state", "[campaign][quest-items]")
{
    TemporaryDatabase first, second;
    Campaign::Ledger one(first.Path), two(second.Path);
    Campaign::QuestItem item{1, 100, 1, 200, 0, 1, 300, 1, false, true, 0};
    static_cast<void>(one.SetQuestItem("first", item, 0));
    item.AliasId = 1;
    static_cast<void>(one.SetQuestItem("second", item, 0));
    REQUIRE(one.ReadQuestItems().size() == 2);
    REQUIRE(two.ReadQuestItems().empty());
    REQUIRE(one.GetMetadata().CampaignId != two.GetMetadata().CampaignId);
}

TEST_CASE("A new quest instance does not overwrite the previous hand-in", "[campaign][quest-items]")
{
    TemporaryDatabase database;
    Campaign::Ledger ledger(database.Path);
    Campaign::QuestItem item{1, 100, 1, 200, 0, 0, 0, 1, true, true, 0};
    auto pickup = ledger.SetQuestItem("first-pickup", item, 0);
    item.Active = false;
    static_cast<void>(ledger.SetQuestItem("first-hand-in", item, pickup.Entry.Revision));
    item.Active = true;
    item.QuestInstance = 1;
    static_cast<void>(ledger.SetQuestItem("second-pickup", item, 0));
    const auto records = ledger.ReadQuestItems();
    REQUIRE(records.size() == 2);
    REQUIRE_FALSE(records[0].Active);
    REQUIRE(records[1].Active);
    REQUIRE(records[1].QuestInstance == 1);
}
