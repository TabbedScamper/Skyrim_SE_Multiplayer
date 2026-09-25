#pragma once

#include <cstdint>

// Process-local, read-only Papyrus native-dispatch telemetry. This does not
// enumerate VM stacks or imply that all Papyrus work passes through this seam.
struct NativeDispatchDiagnostic
{
    uint64_t Count{};
    uint64_t LastFunctionHash{};
    uint64_t LastTimeMs{};
    uint64_t VmUpdateCount{};
    uint64_t VmUpdateLastStartMs{};
    uint64_t VmUpdateLastDurationUs{};
    uint64_t VmTaskletCount{};
    uint64_t VmTaskletLastStartMs{};
    uint64_t VmTaskletLastDurationUs{};
    uint64_t DisablePlayerControlsCalls{};
    uint64_t EnablePlayerControlsCalls{};
    uint64_t LastControlCallTimeMs{};
    bool LastControlCallEnabled{};
};

[[nodiscard]] NativeDispatchDiagnostic GetNativeDispatchDiagnostic() noexcept;
void InstallVirtualMachineDiagnostic() noexcept;
