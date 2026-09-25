#pragma once

#include <atomic>
#include <cstdint>

// Cheap process-local counters for identifying whether native menu input is
// reaching Skyrim's poll path. Read differences between two snapshots.
struct InputPollDiagnostic
{
    std::atomic_uint64_t Calls{0};
    std::atomic_uint64_t Forwarded{0};
    std::atomic_uint64_t Unfocused{0};
    std::atomic_uint64_t OverlayActive{0};
    std::atomic_uint64_t ResumeDelay{0};
};

inline InputPollDiagnostic g_inputPollDiagnostic;
