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
