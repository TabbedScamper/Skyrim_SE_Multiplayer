#pragma once

#include <string>
#include <vector>

// Output device choice for the options page. Switching itself is done by the
// vendored Auto Audio Output Switch (ThirdParty/AudioSwitch), which reads the
// preference here: a connected preferred device wins, otherwise the game
// follows the Windows default and moves when it changes.
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
// Moves the running engine to the preference (no-op if already there).
void ApplyPreferredDevice() noexcept;

// The endpoint the running engine was created on (empty until init ran).
std::string GetActiveDevice() noexcept;

// JSON array [{"id":..,"name":..}] for the options UI.
std::string OutputsJson() noexcept;
} // namespace AudioDeviceSelection
