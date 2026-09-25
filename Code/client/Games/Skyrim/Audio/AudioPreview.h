#pragma once

#include <cstdint>
#include <string>

// Live preview for the volume sliders. While a slider is being adjusted, the
// other mixer categories are muted and its own category plays continuously
// (random samples from a pool of Skyrim.esm sound descriptors), all through
// Skyrim's audio engine so the slider's level is exactly what is heard.
// Window/main thread only.
namespace AudioPreview
{
// Timer driving playback; handled in InputService::WndProc.
inline constexpr uintptr_t kTimerId = 0x5C1A;

// Starts or extends the preview for acChannel ("master", "effects",
// "footsteps", "voice", "music"). Stops by itself ~1.5 s after the last call.
void KeepAlive(const std::string& acChannel) noexcept;
// Stops playback and restores every muted category to its previous volume.
void Stop() noexcept;
// Timer callback.
void Tick() noexcept;
} // namespace AudioPreview
