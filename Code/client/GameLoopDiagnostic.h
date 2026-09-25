#pragma once

#include <cstdint>

// Read-only, cumulative process-local timing. Differences between two
// snapshots show where time was spent without per-frame logging or allocation.
struct GameLoopDiagnostic
{
    uint64_t VmHookCalls{};
    uint64_t VmActiveCalls{};
    uint64_t VmInactiveCalls{};
    uint64_t VmAppTotalUs{};
    uint64_t VmOriginalTotalUs{};
    uint32_t VmLastAppUs{};
    uint32_t VmLastOriginalUs{};
    uint32_t VmLastEntryGapUs{};
    uint32_t VmMaxEntryGapUs{};
    uint32_t VmMaxAppUs{};
    uint32_t VmMaxOriginalUs{};
    uint64_t WorldCalls{};
    uint64_t WorldPreUpdateTotalUs{};
    uint64_t WorldRunnerTotalUs{};
    uint64_t WorldDispatcherTotalUs{};
    uint64_t WorldGameTestTotalUs{};
    uint32_t WorldLastGameTestUs{};
    uint32_t WorldMaxGameTestUs{};
    uint32_t WorldLastEntryGapUs{};
    uint32_t WorldMaxDispatcherUs{};
    uint32_t WorldMaxEntryGapUs{};
    uint64_t WorldGapsOver50Ms{};
    uint64_t WorldGapsOver100Ms{};
    uint64_t WorldGapsOver250Ms{};
};

[[nodiscard]] GameLoopDiagnostic GetGameLoopDiagnostic() noexcept;
void RecordGameVmHookCall(bool aActive, uint32_t aAppDurationUs,
    uint32_t aOriginalDurationUs) noexcept;
void RecordGameVmHookEntry() noexcept;
