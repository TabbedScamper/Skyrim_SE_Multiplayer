#pragma once

#include <string>
#include <vector>

// Output device choice for Skyrim's XAudio2 2.7 engine.
//
// BSXAudio2Audio's init (ID 67952) enumerates XAudio2 devices and creates the
// mastering voice on the first one whose Role has DefaultGameDevice (0x8).
// We make that flag point at the player's chosen endpoint; an empty choice
// leaves Windows' own default untouched. The choice takes effect when the
// engine initializes (game start).
namespace AudioDeviceSelection
{
struct Endpoint
{
    std::string Id;   // MMDevice endpoint ID (UTF-8)
    std::string Name; // friendly name (UTF-8)
};

// Active render endpoints, Windows default first.
std::vector<Endpoint> EnumerateOutputs() noexcept;

// Empty = follow the Windows default.
std::string GetPreferredDevice() noexcept;
void SetPreferredDevice(const std::string& acId) noexcept;

// The endpoint the running engine was created on (empty until init ran).
std::string GetActiveDevice() noexcept;

// JSON array [{"id":..,"name":..}] for the options UI.
std::string OutputsJson() noexcept;
} // namespace AudioDeviceSelection
