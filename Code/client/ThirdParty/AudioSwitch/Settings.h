#pragma once

// Skyrim Together shim for the vendored Auto Audio Output Switch settings.
// There is no separate INI: the options page owns the preferred device
// (Games/Skyrim/Audio/AudioDeviceSelection), and the switch behaviour is fixed
// to "follow the Windows default unless a preferred device is connected".

#include <atomic>
#include <cstdint>
#include <string>

namespace settings
{
namespace general
{
inline std::atomic<bool> enabled = true;
inline std::atomic<bool> switchOnDefaultChange = true;
inline std::atomic<bool> switchToNewDevice = true;
inline std::atomic<std::uint32_t> resetDelayMs = 500;
} // namespace general

std::string GetPreferredDevice();
} // namespace settings
