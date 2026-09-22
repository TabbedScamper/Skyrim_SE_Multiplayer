#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

struct sqlite3;

namespace Campaign
{
struct Metadata
{
    std::string CampaignId;
    uint64_t AuthorityEpoch{};
    uint64_t Revision{};
    uint64_t CheckpointRevision{};
};

struct JournalEntry
{
    uint64_t Revision{};
    uint64_t AuthorityEpoch{};
    std::string TransactionId;
    std::string Kind;
    std::string Payload;
    int64_t CommittedAtMs{};
};

struct CommitResult
{
    JournalEntry Entry;
    bool Inserted{};
};

struct Checkpoint
{
    uint64_t Revision{};
    std::string Snapshot;
    int64_t CreatedAtMs{};
};

/**
 * Durable, server-authoritative ordering for shared campaign mutations.
 *
 * A transaction id is an idempotency key. Reusing it with the same kind and
 * payload returns the original commit. Reusing it for different data is an
 * error rather than silently applying a contradictory mutation.
 */
class Ledger final
{
public:
    explicit Ledger(const std::filesystem::path& acPath);
    ~Ledger() noexcept;

    Ledger(const Ledger&) = delete;
    Ledger& operator=(const Ledger&) = delete;
    Ledger(Ledger&&) = delete;
    Ledger& operator=(Ledger&&) = delete;

    [[nodiscard]] Metadata GetMetadata() const;
    [[nodiscard]] CommitResult Commit(const std::string& acTransactionId, const std::string& acKind, const std::string& acPayload);
    [[nodiscard]] std::vector<JournalEntry> ReadAfter(uint64_t aRevision, size_t aLimit = 1024) const;
    [[nodiscard]] uint64_t CreateCheckpoint(const std::string& acSnapshot);
    [[nodiscard]] Checkpoint GetLatestCheckpoint() const;
    [[nodiscard]] uint64_t AdvanceAuthorityEpoch(const std::string& acTransactionId, const std::string& acReason);

private:
    void Initialize();
    void Execute(const char* acSql) const;

    sqlite3* m_pDatabase{};
    mutable std::mutex m_mutex;
};
} // namespace Campaign
