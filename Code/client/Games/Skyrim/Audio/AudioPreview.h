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

// Tells the audio thread to re-apply category volumes to sounds already
// playing. Setting a BGSSoundCategory volume only stores it; the vanilla
// Journal OptionChange handler (ID 53310, default case) follows every
// category change with BSAudioManager message 0x10 (ID 67716). Without it a
// playing track (e.g. the menu music) keeps its old level.
void NotifyCategoryVolumesChanged() noexcept;

// Sets a BGSSoundCategory's volume through the vanilla Journal OptionChange
// handler (ID 53310): its default case takes the category form ID as the
// option number. Same path the vanilla Audio menu uses; master uses option 27.
void SetCategoryVolumeVanilla(uint32_t aCategoryFormId, float aValue) noexcept;
} // namespace AudioPreview
