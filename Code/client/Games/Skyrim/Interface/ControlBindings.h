#pragma once

#include <cstdint>
#include <string>

// Skyrim's own key bindings (gameplay context), read and remapped through the
// same native functions the vanilla Controls menu uses:
//   ControlMap singleton  ID 400863
//   RemapButton           ID 68538  (swaps with the displaced event like vanilla)
//   SaveRemappings        ID 68539  (writes ControlMap_Custom.txt)
namespace ControlBindings
{
enum class Device : uint32_t
{
    Keyboard = 0,
    Mouse = 1,
    Gamepad = 2,
};

// JSON: {"controller":"xbox|playstation|nintendo|none","bindings":[{"event":..,"device":0,"key":..,"remappable":true}]}
std::string BindingsJson() noexcept;

// Arms capture: the next key / mouse button / gamepad button on that device
// is bound to acEvent. Esc cancels. Must run on the window thread.
void StartCapture(const std::string& acEvent, Device aDevice) noexcept;
void CancelCapture() noexcept;

// Restores every keyboard, mouse and controller binding to Interface/Controls/PC/ControlMap.txt,
// exactly as the vanilla Controls menu's "Defaults" does, then saves ControlMap_Custom.txt
// and reports the new bindings. Must run on the window thread.
bool ResetToDefaults() noexcept;
bool IsCapturing() noexcept;

// Fed from InputService (raw input) and the overlay's XInput poll. Return
// true when the input was consumed by an armed capture.
bool OnKeyboardScanCode(uint16_t aMakeCode, bool aE0) noexcept;
bool OnMouseButton(uint32_t aButtonIndex) noexcept;
bool OnGamepadButtons(uint16_t aPressedMask, bool aLeftTrigger, bool aRightTrigger) noexcept;

// Called with the refreshed JSON after a successful remap.
using BindingsChangedFn = void (*)(const std::string& acJson);
void SetBindingsChangedCallback(BindingsChangedFn apCallback) noexcept;
} // namespace ControlBindings
