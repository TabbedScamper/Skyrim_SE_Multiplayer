#pragma once

#include <string>

// Plays a short sample from the mixer category a volume slider controls, through
// Skyrim's own audio engine, so the slider's effect is heard live.
namespace AudioPreview
{
// acChannel: "master", "effects", "footsteps", "voice". Music has no sound
// descriptors (it plays through the music system) and is ignored. Throttled
// per channel; main/window thread only.
void Play(const std::string& acChannel) noexcept;
} // namespace AudioPreview
