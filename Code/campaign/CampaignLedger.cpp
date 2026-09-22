#include "CampaignLedger.h"

#include <sqlite3.h>

#include <array>
#include <chrono>
#include <iomanip>
#include <limits>
#include <random>
#include <sstream>
#include <stdexcept>

namespace
{
class Statement final
{
public:
    Statement(sqlite3* apDatabase, const char* acSql)
    {
        const int result = sqlite3_prepare_v2(apDatabase, acSql, -1, &m_pStatement, nullptr);
        if (result != SQLITE_OK)
            throw std::runtime_error(sqlite3_errmsg(apDatabase));
    }

    ~Statement() noexcept
    {
        if (m_pStatement)
            sqlite3_finalize(m_pStatement);
    }

    sqlite3_stmt* Get() const noexcept { return m_pStatement; }

private:
    sqlite3_stmt* m_pStatement{};
};

class Transaction final
{
public:
    explicit Transaction(sqlite3* apDatabase)
        : m_pDatabase(apDatabase)
    {
        char* pError{};
        if (sqlite3_exec(m_pDatabase, "BEGIN IMMEDIATE", nullptr, nullptr, &pError) != SQLITE_OK)
        {
            const std::string error = pError ? pError : sqlite3_errmsg(m_pDatabase);
            sqlite3_free(pError);
            throw std::runtime_error(error);
        }
    }

    ~Transaction() noexcept
    {
        if (!m_committed)
            sqlite3_exec(m_pDatabase, "ROLLBACK", nullptr, nullptr, nullptr);
    }

    void Commit()
    {
        char* pError{};
        if (sqlite3_exec(m_pDatabase, "COMMIT", nullptr, nullptr, &pError) != SQLITE_OK)
        {
            const std::string error = pError ? pError : sqlite3_errmsg(m_pDatabase);
            sqlite3_free(pError);
            throw std::runtime_error(error);
        }
        m_committed = true;
    }

private:
    sqlite3* m_pDatabase{};
    bool m_committed{};
};

void Check(int aResult, sqlite3* apDatabase)
{
    if (aResult != SQLITE_OK && aResult != SQLITE_DONE && aResult != SQLITE_ROW)
        throw std::runtime_error(sqlite3_errmsg(apDatabase));
}

uint64_t ReadUnsigned(sqlite3_stmt* apStatement, int aColumn)
{
    const sqlite3_int64 value = sqlite3_column_int64(apStatement, aColumn);
    if (value < 0)
        throw std::runtime_error("Campaign database contains a negative unsigned value");
    return static_cast<uint64_t>(value);
}

sqlite3_int64 ToSqlInteger(uint64_t aValue)
{
    if (aValue > static_cast<uint64_t>(std::numeric_limits<sqlite3_int64>::max()))
        throw std::overflow_error("Campaign revision exceeds SQLite integer range");
    return static_cast<sqlite3_int64>(aValue);
}

std::string ReadText(sqlite3_stmt* apStatement, int aColumn)
{
    const auto* pText = sqlite3_column_text(apStatement, aColumn);
    const int size = sqlite3_column_bytes(apStatement, aColumn);
    return pText && size > 0 ? std::string(reinterpret_cast<const char*>(pText), static_cast<size_t>(size)) : std::string{};
}

std::string ReadBlob(sqlite3_stmt* apStatement, int aColumn)
{
    const auto* pData = static_cast<const char*>(sqlite3_column_blob(apStatement, aColumn));
    const int size = sqlite3_column_bytes(apStatement, aColumn);
    return pData && size > 0 ? std::string(pData, static_cast<size_t>(size)) : std::string{};
}

void BindText(sqlite3* apDatabase, sqlite3_stmt* apStatement, int aIndex, const std::string& acValue)
{
    Check(sqlite3_bind_text(apStatement, aIndex, acValue.data(), static_cast<int>(acValue.size()), SQLITE_TRANSIENT), apDatabase);
}

void BindBlob(sqlite3* apDatabase, sqlite3_stmt* apStatement, int aIndex, const std::string& acValue)
{
    Check(sqlite3_bind_blob(apStatement, aIndex, acValue.data(), static_cast<int>(acValue.size()), SQLITE_TRANSIENT), apDatabase);
}

int64_t NowMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

std::string GenerateCampaignId()
{
    std::random_device random;
    std::array<uint8_t, 16> bytes{};
    for (auto& byte : bytes)
        byte = static_cast<uint8_t>(random());

    // UUIDv4 variant/version bits make logs and tools recognize this as an ID.
    bytes[6] = static_cast<uint8_t>((bytes[6] & 0x0F) | 0x40);
    bytes[8] = static_cast<uint8_t>((bytes[8] & 0x3F) | 0x80);

    std::ostringstream stream;
    stream << std::hex << std::setfill('0');
    for (size_t i = 0; i < bytes.size(); ++i)
    {
        stream << std::setw(2) << static_cast<unsigned>(bytes[i]);
        if (i == 3 || i == 5 || i == 7 || i == 9)
            stream << '-';
    }
    return stream.str();
}

Campaign::JournalEntry ReadJournalEntry(sqlite3_stmt* apStatement)
{
    Campaign::JournalEntry entry;
    entry.Revision = ReadUnsigned(apStatement, 0);
    entry.AuthorityEpoch = ReadUnsigned(apStatement, 1);
    entry.TransactionId = ReadText(apStatement, 2);
    entry.Kind = ReadText(apStatement, 3);
    entry.Payload = ReadBlob(apStatement, 4);
    entry.CommittedAtMs = sqlite3_column_int64(apStatement, 5);
    return entry;
}
} // namespace

namespace Campaign
{
Ledger::Ledger(const std::filesystem::path& acPath)
{
    if (acPath.empty())
        throw std::invalid_argument("Campaign database path is empty");

    if (const auto parent = acPath.parent_path(); !parent.empty())
        std::filesystem::create_directories(parent);

    const auto path = acPath.u8string();
    const int result = sqlite3_open_v2(reinterpret_cast<const char*>(path.c_str()), &m_pDatabase,
        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX, nullptr);
    if (result != SQLITE_OK)
    {
        const std::string error = m_pDatabase ? sqlite3_errmsg(m_pDatabase) : "Unable to allocate SQLite connection";
        if (m_pDatabase)
            sqlite3_close(m_pDatabase);
        m_pDatabase = nullptr;
        throw std::runtime_error(error);
    }

    try
    {
        sqlite3_busy_timeout(m_pDatabase, 5000);
        Initialize();
    }
    catch (...)
    {
        sqlite3_close(m_pDatabase);
        m_pDatabase = nullptr;
        throw;
    }
}

Ledger::~Ledger() noexcept
{
    if (m_pDatabase)
        sqlite3_close(m_pDatabase);
}

void Ledger::Execute(const char* acSql) const
{
    char* pError{};
    if (sqlite3_exec(m_pDatabase, acSql, nullptr, nullptr, &pError) != SQLITE_OK)
    {
        const std::string error = pError ? pError : sqlite3_errmsg(m_pDatabase);
        sqlite3_free(pError);
        throw std::runtime_error(error);
    }
}

void Ledger::Initialize()
{
    Execute("PRAGMA journal_mode=WAL");
    Execute("PRAGMA synchronous=FULL");
    Execute("PRAGMA foreign_keys=ON");
    Execute("PRAGMA trusted_schema=OFF");

    Transaction transaction(m_pDatabase);
    Execute("CREATE TABLE IF NOT EXISTS campaign_meta ("
            "singleton INTEGER PRIMARY KEY CHECK(singleton = 1),"
            "campaign_id TEXT NOT NULL UNIQUE,"
            "authority_epoch INTEGER NOT NULL CHECK(authority_epoch >= 1),"
            "revision INTEGER NOT NULL CHECK(revision >= 0),"
            "checkpoint_revision INTEGER NOT NULL CHECK(checkpoint_revision >= 0),"
            "schema_version INTEGER NOT NULL)");
    Execute("CREATE TABLE IF NOT EXISTS journal ("
            "revision INTEGER PRIMARY KEY CHECK(revision >= 1),"
            "authority_epoch INTEGER NOT NULL CHECK(authority_epoch >= 1),"
            "transaction_id TEXT NOT NULL UNIQUE,"
            "kind TEXT NOT NULL,"
            "payload BLOB NOT NULL,"
            "committed_at_ms INTEGER NOT NULL)");
    Execute("CREATE TABLE IF NOT EXISTS checkpoints ("
            "revision INTEGER PRIMARY KEY CHECK(revision >= 0),"
            "snapshot BLOB NOT NULL,"
            "created_at_ms INTEGER NOT NULL)");

    {
        Statement query(m_pDatabase, "SELECT COUNT(*) FROM campaign_meta");
        Check(sqlite3_step(query.Get()), m_pDatabase);
        if (sqlite3_column_int64(query.Get(), 0) == 0)
        {
            Statement insert(m_pDatabase,
                "INSERT INTO campaign_meta(singleton,campaign_id,authority_epoch,revision,checkpoint_revision,schema_version) "
                "VALUES(1,?,1,0,0,1)");
            BindText(m_pDatabase, insert.Get(), 1, GenerateCampaignId());
            Check(sqlite3_step(insert.Get()), m_pDatabase);
        }
    }

    {
        Statement verify(m_pDatabase,
            "SELECT m.revision,m.checkpoint_revision,COALESCE(MAX(j.revision),0),m.schema_version "
            "FROM campaign_meta m LEFT JOIN journal j ON 1=1 WHERE m.singleton=1 GROUP BY m.singleton");
        Check(sqlite3_step(verify.Get()), m_pDatabase);
        const uint64_t revision = ReadUnsigned(verify.Get(), 0);
        const uint64_t checkpoint = ReadUnsigned(verify.Get(), 1);
        const uint64_t journalRevision = ReadUnsigned(verify.Get(), 2);
        const auto schemaVersion = sqlite3_column_int(verify.Get(), 3);
        if (schemaVersion != 1)
            throw std::runtime_error("Unsupported campaign database schema version");
        if (revision != journalRevision || checkpoint > revision)
            throw std::runtime_error("Campaign database watermark is inconsistent with its journal");
    }

    transaction.Commit();
}

Metadata Ledger::GetMetadata() const
{
    std::scoped_lock lock(m_mutex);
    Statement query(m_pDatabase,
        "SELECT campaign_id,authority_epoch,revision,checkpoint_revision FROM campaign_meta WHERE singleton=1");
    Check(sqlite3_step(query.Get()), m_pDatabase);

    Metadata metadata;
    metadata.CampaignId = ReadText(query.Get(), 0);
    metadata.AuthorityEpoch = ReadUnsigned(query.Get(), 1);
    metadata.Revision = ReadUnsigned(query.Get(), 2);
    metadata.CheckpointRevision = ReadUnsigned(query.Get(), 3);
    return metadata;
}

CommitResult Ledger::Commit(const std::string& acTransactionId, const std::string& acKind, const std::string& acPayload)
{
    if (acTransactionId.empty() || acKind.empty())
        throw std::invalid_argument("Campaign transaction id and kind are required");

    std::scoped_lock lock(m_mutex);
    Transaction transaction(m_pDatabase);

    {
        Statement existing(m_pDatabase,
            "SELECT revision,authority_epoch,transaction_id,kind,payload,committed_at_ms FROM journal WHERE transaction_id=?");
        BindText(m_pDatabase, existing.Get(), 1, acTransactionId);
        const int result = sqlite3_step(existing.Get());
        if (result == SQLITE_ROW)
        {
            auto entry = ReadJournalEntry(existing.Get());
            if (entry.Kind != acKind || entry.Payload != acPayload)
                throw std::runtime_error("Campaign transaction id was reused for different data");
            transaction.Commit();
            return {std::move(entry), false};
        }
        Check(result, m_pDatabase);
    }

    uint64_t currentRevision{};
    uint64_t authorityEpoch{};
    {
        Statement metadata(m_pDatabase, "SELECT revision,authority_epoch FROM campaign_meta WHERE singleton=1");
        Check(sqlite3_step(metadata.Get()), m_pDatabase);
        currentRevision = ReadUnsigned(metadata.Get(), 0);
        authorityEpoch = ReadUnsigned(metadata.Get(), 1);
    }

    if (currentRevision == static_cast<uint64_t>(std::numeric_limits<sqlite3_int64>::max()))
        throw std::overflow_error("Campaign revision is exhausted");

    JournalEntry entry;
    entry.Revision = currentRevision + 1;
    entry.AuthorityEpoch = authorityEpoch;
    entry.TransactionId = acTransactionId;
    entry.Kind = acKind;
    entry.Payload = acPayload;
    entry.CommittedAtMs = NowMs();

    {
        Statement insert(m_pDatabase,
            "INSERT INTO journal(revision,authority_epoch,transaction_id,kind,payload,committed_at_ms) VALUES(?,?,?,?,?,?)");
        Check(sqlite3_bind_int64(insert.Get(), 1, ToSqlInteger(entry.Revision)), m_pDatabase);
        Check(sqlite3_bind_int64(insert.Get(), 2, ToSqlInteger(entry.AuthorityEpoch)), m_pDatabase);
        BindText(m_pDatabase, insert.Get(), 3, entry.TransactionId);
        BindText(m_pDatabase, insert.Get(), 4, entry.Kind);
        BindBlob(m_pDatabase, insert.Get(), 5, entry.Payload);
        Check(sqlite3_bind_int64(insert.Get(), 6, entry.CommittedAtMs), m_pDatabase);
        Check(sqlite3_step(insert.Get()), m_pDatabase);
    }

    {
        Statement update(m_pDatabase, "UPDATE campaign_meta SET revision=? WHERE singleton=1 AND revision=?");
        Check(sqlite3_bind_int64(update.Get(), 1, ToSqlInteger(entry.Revision)), m_pDatabase);
        Check(sqlite3_bind_int64(update.Get(), 2, ToSqlInteger(currentRevision)), m_pDatabase);
        Check(sqlite3_step(update.Get()), m_pDatabase);
        if (sqlite3_changes(m_pDatabase) != 1)
            throw std::runtime_error("Campaign revision changed during commit");
    }

    transaction.Commit();
    return {std::move(entry), true};
}

std::vector<JournalEntry> Ledger::ReadAfter(uint64_t aRevision, size_t aLimit) const
{
    if (aLimit == 0)
        return {};
    if (aLimit > 100000)
        throw std::invalid_argument("Campaign journal read limit is too large");

    std::scoped_lock lock(m_mutex);
    Statement query(m_pDatabase,
        "SELECT revision,authority_epoch,transaction_id,kind,payload,committed_at_ms "
        "FROM journal WHERE revision>? ORDER BY revision ASC LIMIT ?");
    Check(sqlite3_bind_int64(query.Get(), 1, ToSqlInteger(aRevision)), m_pDatabase);
    Check(sqlite3_bind_int64(query.Get(), 2, static_cast<sqlite3_int64>(aLimit)), m_pDatabase);

    std::vector<JournalEntry> entries;
    while (true)
    {
        const int result = sqlite3_step(query.Get());
        if (result == SQLITE_DONE)
            break;
        Check(result, m_pDatabase);
        entries.emplace_back(ReadJournalEntry(query.Get()));
    }
    return entries;
}

uint64_t Ledger::CreateCheckpoint(const std::string& acSnapshot)
{
    std::scoped_lock lock(m_mutex);
    Transaction transaction(m_pDatabase);

    uint64_t revision{};
    {
        Statement metadata(m_pDatabase, "SELECT revision FROM campaign_meta WHERE singleton=1");
        Check(sqlite3_step(metadata.Get()), m_pDatabase);
        revision = ReadUnsigned(metadata.Get(), 0);
    }

    {
        Statement insert(m_pDatabase,
            "INSERT INTO checkpoints(revision,snapshot,created_at_ms) VALUES(?,?,?) "
            "ON CONFLICT(revision) DO UPDATE SET snapshot=excluded.snapshot,created_at_ms=excluded.created_at_ms");
        Check(sqlite3_bind_int64(insert.Get(), 1, ToSqlInteger(revision)), m_pDatabase);
        BindBlob(m_pDatabase, insert.Get(), 2, acSnapshot);
        Check(sqlite3_bind_int64(insert.Get(), 3, NowMs()), m_pDatabase);
        Check(sqlite3_step(insert.Get()), m_pDatabase);
    }

    {
        Statement update(m_pDatabase, "UPDATE campaign_meta SET checkpoint_revision=? WHERE singleton=1");
        Check(sqlite3_bind_int64(update.Get(), 1, ToSqlInteger(revision)), m_pDatabase);
        Check(sqlite3_step(update.Get()), m_pDatabase);
    }

    transaction.Commit();
    return revision;
}

Checkpoint Ledger::GetLatestCheckpoint() const
{
    std::scoped_lock lock(m_mutex);
    Statement query(m_pDatabase,
        "SELECT c.revision,c.snapshot,c.created_at_ms FROM checkpoints c "
        "JOIN campaign_meta m ON c.revision=m.checkpoint_revision WHERE m.singleton=1");
    const int result = sqlite3_step(query.Get());
    if (result == SQLITE_DONE)
        return {};
    Check(result, m_pDatabase);

    Checkpoint checkpoint;
    checkpoint.Revision = ReadUnsigned(query.Get(), 0);
    checkpoint.Snapshot = ReadBlob(query.Get(), 1);
    checkpoint.CreatedAtMs = sqlite3_column_int64(query.Get(), 2);
    return checkpoint;
}

uint64_t Ledger::AdvanceAuthorityEpoch(const std::string& acTransactionId, const std::string& acReason)
{
    if (acTransactionId.empty())
        throw std::invalid_argument("Authority transition transaction id is required");

    std::scoped_lock lock(m_mutex);
    Transaction transaction(m_pDatabase);

    {
        Statement existing(m_pDatabase,
            "SELECT authority_epoch,kind,payload FROM journal WHERE transaction_id=?");
        BindText(m_pDatabase, existing.Get(), 1, acTransactionId);
        const int result = sqlite3_step(existing.Get());
        if (result == SQLITE_ROW)
        {
            if (ReadText(existing.Get(), 1) != "authority_epoch" || ReadBlob(existing.Get(), 2) != acReason)
                throw std::runtime_error("Authority transaction id was reused for different data");
            const uint64_t epoch = ReadUnsigned(existing.Get(), 0);
            transaction.Commit();
            return epoch;
        }
        Check(result, m_pDatabase);
    }

    uint64_t revision{};
    uint64_t epoch{};
    {
        Statement metadata(m_pDatabase, "SELECT revision,authority_epoch FROM campaign_meta WHERE singleton=1");
        Check(sqlite3_step(metadata.Get()), m_pDatabase);
        revision = ReadUnsigned(metadata.Get(), 0);
        epoch = ReadUnsigned(metadata.Get(), 1);
    }
    if (revision == static_cast<uint64_t>(std::numeric_limits<sqlite3_int64>::max()) ||
        epoch == static_cast<uint64_t>(std::numeric_limits<sqlite3_int64>::max()))
        throw std::overflow_error("Campaign authority counters are exhausted");

    ++revision;
    ++epoch;
    const int64_t committedAt = NowMs();
    {
        Statement insert(m_pDatabase,
            "INSERT INTO journal(revision,authority_epoch,transaction_id,kind,payload,committed_at_ms) VALUES(?,?,?,?,?,?)");
        Check(sqlite3_bind_int64(insert.Get(), 1, ToSqlInteger(revision)), m_pDatabase);
        Check(sqlite3_bind_int64(insert.Get(), 2, ToSqlInteger(epoch)), m_pDatabase);
        BindText(m_pDatabase, insert.Get(), 3, acTransactionId);
        BindText(m_pDatabase, insert.Get(), 4, "authority_epoch");
        BindBlob(m_pDatabase, insert.Get(), 5, acReason);
        Check(sqlite3_bind_int64(insert.Get(), 6, committedAt), m_pDatabase);
        Check(sqlite3_step(insert.Get()), m_pDatabase);
    }
    {
        Statement update(m_pDatabase,
            "UPDATE campaign_meta SET revision=?,authority_epoch=? WHERE singleton=1 AND revision=?");
        Check(sqlite3_bind_int64(update.Get(), 1, ToSqlInteger(revision)), m_pDatabase);
        Check(sqlite3_bind_int64(update.Get(), 2, ToSqlInteger(epoch)), m_pDatabase);
        Check(sqlite3_bind_int64(update.Get(), 3, ToSqlInteger(revision - 1)), m_pDatabase);
        Check(sqlite3_step(update.Get()), m_pDatabase);
        if (sqlite3_changes(m_pDatabase) != 1)
            throw std::runtime_error("Campaign revision changed during authority transition");
    }

    transaction.Commit();
    return epoch;
}
} // namespace Campaign
