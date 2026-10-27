#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <utility>

// The Script lane is SAM's ApplyQueue. Producers supply value records; the
// owner of each kind supplies its gate, apply and cancel actions at X.00.
namespace MainQueue
{
enum class Lane : uint8_t { Window, Ui, World, Script, Terminal, Count };
enum class Decision : uint8_t { Apply, Hold, Drop };

struct Record
{
    uint64_t OrderKey{};
    uint64_t AuthKey[2]{};
    uint32_t LoadSerial{};
    uint32_t ConnectionSerial{};
    uint32_t SnapGen{};
    uint32_t Deadline{};
    uint16_t Kind{};
    uint8_t AuthClass{};
    uint8_t Flags{};
    std::array<uint8_t, 48> Payload{};
    void* Heap{}; // App-lane producers only; never allocate in an engine hook.
    void (*DestroyHeap)(void*){};
};

// A bounded MPSC ring with one X.00 consumer. The outstanding reservation
// covers both ring and held records, so a closed gate cannot grow memory.
template<size_t Capacity>
class Queue
{
    static_assert(Capacity >= 2 && (Capacity & (Capacity - 1)) == 0);
    struct Cell
    {
        std::atomic<size_t> Sequence{};
        Record Value{};
    };

public:
    Queue() noexcept
    {
        for (size_t i = 0; i < Capacity; ++i)
            m_cells[i].Sequence.store(i, std::memory_order_relaxed);
    }

    Queue(const Queue&) = delete;
    Queue& operator=(const Queue&) = delete;

    bool Push(const Record& aRecord) noexcept
    {
        auto count = m_outstanding.load(std::memory_order_relaxed);
        do
        {
            if (count == Capacity)
            {
                m_poolFull.fetch_add(1, std::memory_order_relaxed);
                return false;
            }
        } while (!m_outstanding.compare_exchange_weak(count, count + 1,
                     std::memory_order_acq_rel, std::memory_order_relaxed));

        size_t pos = m_write.load(std::memory_order_relaxed);
        for (;;)
        {
            auto& cell = m_cells[pos & (Capacity - 1)];
            const auto seq = cell.Sequence.load(std::memory_order_acquire);
            if (seq == pos)
            {
                if (m_write.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed))
                {
                    cell.Value = aRecord;
                    cell.Sequence.store(pos + 1, std::memory_order_release);
                    return true;
                }
            }
            else
                pos = m_write.load(std::memory_order_relaxed);
        }
    }

    // The callback decides whether to apply, hold or drop a record and owns
    // any kind-specific cancel action. Records with a held key retain order;
    // other keys continue. Script records use OrderKey=0 for lane-wide order.
    template<class Action>
    size_t Drain(size_t aBudget, Action&& aAction) noexcept
    {
        while (m_pendingCount < Capacity)
        {
            const auto pos = m_read;
            auto& cell = m_cells[pos & (Capacity - 1)];
            if (cell.Sequence.load(std::memory_order_acquire) != pos + 1)
                break;
            m_pending[m_pendingCount++] = cell.Value;
            cell.Sequence.store(pos + Capacity, std::memory_order_release);
            m_read = pos + 1;
        }

        std::array<uint64_t, Capacity * 2> blockedKeys{};
        std::array<bool, Capacity * 2> blockedUsed{};
        auto blocked = [&](uint64_t key) noexcept -> bool {
            size_t slot = (key * 11400714819323198485ull) & (blockedKeys.size() - 1);
            while (blockedUsed[slot])
            {
                if (blockedKeys[slot] == key) return true;
                slot = (slot + 1) & (blockedKeys.size() - 1);
            }
            return false;
        };
        auto block = [&](uint64_t key) noexcept {
            size_t slot = (key * 11400714819323198485ull) & (blockedKeys.size() - 1);
            while (blockedUsed[slot]) slot = (slot + 1) & (blockedKeys.size() - 1);
            blockedUsed[slot] = true;
            blockedKeys[slot] = key;
        };

        size_t kept = 0, done = 0;
        for (size_t i = 0; i < m_pendingCount; ++i)
        {
            auto& record = m_pending[i];
            if (done == aBudget || blocked(record.OrderKey))
            {
                m_pending[kept++] = record;
                continue;
            }
            const auto decision = aAction(record);
            if (decision == Decision::Hold)
            {
                block(record.OrderKey);
                m_pending[kept++] = record;
                continue;
            }
            if (record.Heap && record.DestroyHeap)
                record.DestroyHeap(record.Heap);
            m_outstanding.fetch_sub(1, std::memory_order_release);
            ++done;
        }
        m_pendingCount = kept;
        if (kept) m_carry.fetch_add(1, std::memory_order_relaxed);
        return done;
    }

    size_t Outstanding() const noexcept { return m_outstanding.load(std::memory_order_acquire); }
    uint64_t PoolFull() const noexcept { return m_poolFull.load(std::memory_order_relaxed); }
    uint64_t Carry() const noexcept { return m_carry.load(std::memory_order_relaxed); }

private:
    std::array<Cell, Capacity> m_cells{};
    std::atomic<size_t> m_write{};
    size_t m_read{};
    std::atomic<size_t> m_outstanding{};
    std::array<Record, Capacity> m_pending{};
    size_t m_pendingCount{};
    std::atomic<uint64_t> m_poolFull{};
    std::atomic<uint64_t> m_carry{};
};

struct Lanes
{
    Queue<16> Window;
    Queue<64> Ui;
    Queue<512> World;
    Queue<256> Script;
    Queue<4> Terminal;
};

inline Lanes& Get() noexcept
{
    static Lanes lanes;
    return lanes;
}
}
