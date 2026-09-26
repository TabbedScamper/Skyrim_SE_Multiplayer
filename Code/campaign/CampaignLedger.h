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

// Plugin IDs refer to the campaign's pinned mod manifest, never a client's load order.
// Retired rows are retained so an old save cannot resurrect a handed-in item.
struct QuestItem
{
    uint32_t ModId{}, BaseId{}, QuestModId{}, QuestBaseId{}, AliasId{};
    uint32_t ReferenceModId{}, ReferenceBaseId{};
    uint32_t Count{1};
    bool QuestObject{};
    bool Active{true};
    uint64_t Revision{};
    uint32_t QuestInstance{};
    bool operator==(const QuestItem&) const = default;
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
    [[nodiscard]] std::vector<QuestItem> ReadQuestItems() const;
    // Compare-and-set and journal insertion share one SQLite transaction.
    [[nodiscard]] CommitResult SetQuestItem(const std::string& acTransactionId, const QuestItem& acItem, uint64_t aExpectedRevision);

private:
    void Initialize();
    void Execute(const char* acSql) const;
    CommitResult CommitLocked(const std::string& acTransactionId, const std::string& acKind, const std::string& acPayload);

    sqlite3* m_pDatabase{};
    mutable std::mutex m_mutex;
};
} // namespace Campaign
