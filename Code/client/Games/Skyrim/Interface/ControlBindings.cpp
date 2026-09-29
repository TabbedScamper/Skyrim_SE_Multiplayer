#include <TiltedOnlinePCH.h>

#include <Games/Skyrim/Interface/ControlBindings.h>


namespace
{
// CommonLibSSE-NG ControlMap::UserEventMapping (0x18) and InputContext (four
// BSTArray<UserEventMapping>, one per INPUT_DEVICE). ControlMap::controlMap[]
// starts at +0x60; index 0 is the gameplay context.
struct UserEventMapping
{
    const char* EventId; // BSFixedString data
    uint16_t InputKey;
    uint16_t Modifier;
    int8_t IndexInContext;
    bool Remappable;
    bool Linked;
    uint8_t Pad0F;
    uint32_t GroupFlags;
    uint32_t Pad14;
};
static_assert(sizeof(UserEventMapping) == 0x18);

struct MappingArray
{
    UserEventMapping* Data;
    uint32_t Capacity;
    uint32_t Pad0C;
    uint32_t Size;
    uint32_t Pad14;
};
static_assert(sizeof(MappingArray) == 0x18);

constexpr size_t kControlMapContexts = 0x60;
constexpr uint32_t kDeviceCount = 3; // keyboard, mouse, gamepad

// Gamepad codes as Skyrim stores them (XInput masks; triggers are 9/10).
constexpr uint16_t kLeftTriggerCode = 0x0009;
constexpr uint16_t kRightTriggerCode = 0x000A;
constexpr uint16_t kEscapeScanCode = 0x01;

std::string s_captureEvent;
ControlBindings::Device s_captureDevice{};
bool s_capturing = false;
ControlBindings::BindingsChangedFn s_onChanged = nullptr;

uint8_t* ControlMapInstance() noexcept
{
    static VersionDbPtr<uint8_t*> s_singleton(400863);
    auto** pp = s_singleton.Get();
    return pp ? *pp : nullptr;
}

const MappingArray* GameplayMappings(ControlBindings::Device aDevice) noexcept
{
    auto* pMap = ControlMapInstance();
    if (!pMap)
        return nullptr;
    auto* pContext = *reinterpret_cast<uint8_t**>(pMap + kControlMapContexts);
    if (!pContext)
        return nullptr;
    return reinterpret_cast<const MappingArray*>(pContext + static_cast<uint32_t>(aDevice) * sizeof(MappingArray));
}

// Exact controller model from the USB vendor/product ID of the first physical
// HID game controller. Steam Input's virtual pad (28DE:11FF) is skipped so the
// real device underneath is drawn.
const char* ModelFor(DWORD aVendor, DWORD aProduct) noexcept
{
    switch (aVendor)
    {
    case 0x045E: // Microsoft
        switch (aProduct)
        {
        case 0x028E: case 0x028F: case 0x0291: case 0x02A1: case 0x0719: return "xbox-360";
        case 0x02D1: case 0x02DD: case 0x02E0: case 0x02E3: case 0x02EA: case 0x02FD: case 0x02FF:
        case 0x0B00: case 0x0B05: case 0x0B0A: return "xbox-one";
        default: return "xbox-series"; // 0B12/0B13/0B20/0B21/0B22 and newer
        }
    case 0x054C: // Sony
        switch (aProduct)
        {
        case 0x05C4: case 0x09CC: case 0x0BA0: return "dualshock4";
        default: return "dualsense"; // 0CE6, Edge 0DF2
        }
    case 0x057E: return "switch-pro"; // Nintendo (2009 Pro, Joy-Con pairs)
    case 0x28DE: // Valve
        switch (aProduct)
        {
        case 0x11FF: return nullptr; // Steam Input virtual gamepad
        case 0x1205: return "steam-deck";
        default: return "steam-controller";
        }
    default: return "generic";
    }
}

std::string ControllerModel() noexcept
{
    UINT count = 0;
    if (GetRawInputDeviceList(nullptr, &count, sizeof(RAWINPUTDEVICELIST)) != 0 || count == 0)
        return "xbox-series";
    std::vector<RAWINPUTDEVICELIST> devices(count);
    if (GetRawInputDeviceList(devices.data(), &count, sizeof(RAWINPUTDEVICELIST)) == static_cast<UINT>(-1))
        return "xbox-series";

    for (const auto& device : devices)
    {
        if (device.dwType != RIM_TYPEHID)
            continue;
        RID_DEVICE_INFO info{};
        info.cbSize = sizeof(info);
        UINT size = sizeof(info);
        if (GetRawInputDeviceInfoW(device.hDevice, RIDI_DEVICEINFO, &info, &size) == static_cast<UINT>(-1))
            continue;
        // Generic desktop page, joystick (4) or gamepad (5), or Valve's vendor page for the Deck.
        const bool gamepad = info.hid.usUsagePage == 0x01 && (info.hid.usUsage == 0x04 || info.hid.usUsage == 0x05);
        const bool valve = info.hid.dwVendorId == 0x28DE;
        if (!gamepad && !valve)
            continue;
        if (const char* pModel = ModelFor(info.hid.dwVendorId, info.hid.dwProductId))
            return pModel;
    }
    // Wired Xbox pads use the XUSB driver and are not HID devices.
    return "xbox-series";
}

std::string Escape(const char* acText)
{
    std::string out;
    for (const char* p = acText ? acText : ""; *p; ++p)
    {
        if (*p == '"' || *p == '\\')
            out += '\\';
        if (static_cast<unsigned char>(*p) >= 0x20)
            out += *p;
    }
    return out;
}

bool Remap(const std::string& acEvent, ControlBindings::Device aDevice, uint32_t aKey) noexcept
{
    auto* pMap = ControlMapInstance();
    if (!pMap)
        return false;

    using TRemap = bool (*)(void*, const BSFixedString&, uint32_t, uint32_t);
    using TSave = void (*)(void*);
    static VersionDbPtr<void> s_remap(68538);
    static VersionDbPtr<void> s_save(68539);
    auto* pRemap = reinterpret_cast<TRemap>(s_remap.GetPtr());
    auto* pSave = reinterpret_cast<TSave>(s_save.GetPtr());
    if (!pRemap || !pSave)
        return false;

    const BSFixedString eventName(acEvent.c_str());
    const bool remapped = pRemap(pMap, eventName, static_cast<uint32_t>(aDevice), aKey);
    if (remapped)
        pSave(pMap);
    spdlog::info("Remap {} on device {} to 0x{:X}: {}", acEvent, static_cast<uint32_t>(aDevice), aKey,
        remapped ? "saved" : "rejected by the game");
    return remapped;
}

// The vanilla Journal Menu's "ResetControlsToDefaults" GameDelegate callback
// (FUN_1409a8730, disassembled live): reload the default ControlMap for all
// devices (ID 68536 -> FUN_140cf17d0 with device 7), rebuild the derived
// mappings (ID 68554), and, only when the byte at ID 383626 is set, remap the
// user event at UserEvents (ID 402638) +0x28 to controller key 0x0C.
// Vanilla saves when its menu closes; this saves right away (ID 68539).
bool ResetBindingsNative() noexcept
{
    auto* pMap = ControlMapInstance();
    if (!pMap)
        return false;
    using TLoadDefaults = void (*)(void*);
    using TRebuild = void (*)(void*);
    using TRemap = bool (*)(void*, const void*, uint32_t, uint32_t);
    using TSave = void (*)(void*);
    static VersionDbPtr<void> s_loadDefaults(68536);
    static VersionDbPtr<void> s_rebuild(68554);
    static VersionDbPtr<uint8_t> s_extraMapping(383626);
    static VersionDbPtr<uint8_t*> s_userEvents(402638);
    static VersionDbPtr<void> s_remap(68538);
    static VersionDbPtr<void> s_save(68539);
    auto* pLoad = reinterpret_cast<TLoadDefaults>(s_loadDefaults.GetPtr());
    auto* pRebuild = reinterpret_cast<TRebuild>(s_rebuild.GetPtr());
    auto* pSave = reinterpret_cast<TSave>(s_save.GetPtr());
    if (!pLoad || !pRebuild || !pSave)
        return false;
    pLoad(pMap);
    pRebuild(pMap);
    if (s_extraMapping.Get() && *s_extraMapping.Get())
        if (auto* pUserEvents = s_userEvents.Get() ? *s_userEvents.Get() : nullptr)
            if (auto* pRemap = reinterpret_cast<TRemap>(s_remap.GetPtr()))
                pRemap(pMap, pUserEvents + 0x28, 2, 0x0C);
    pSave(pMap);
    return true;
}

bool FinishCapture(uint32_t aKey) noexcept
{
    const auto event = s_captureEvent;
    const auto device = s_captureDevice;
    s_capturing = false;
    s_captureEvent.clear();
    Remap(event, device, aKey);
    if (s_onChanged)
        s_onChanged(ControlBindings::BindingsJson());
    return true;
}
} // namespace

namespace ControlBindings
{
std::string BindingsJson() noexcept
{
    std::string json = "{\"controller\":\"" + ControllerModel() + "\",\"bindings\":[";
    bool first = true;
    for (uint32_t device = 0; device < kDeviceCount; ++device)
    {
        const auto* pArray = GameplayMappings(static_cast<Device>(device));
        if (!pArray || !pArray->Data || pArray->Size > 512)
            continue;
        for (uint32_t i = 0; i < pArray->Size; ++i)
        {
            const auto& mapping = pArray->Data[i];
            if (!mapping.EventId || !*mapping.EventId)
                continue;
            if (!first)
                json += ',';
            first = false;
            json += fmt::format("{{\"event\":\"{}\",\"device\":{},\"key\":{},\"remappable\":{}}}", Escape(mapping.EventId), device,
                mapping.InputKey, mapping.Remappable ? "true" : "false");
        }
    }
    return json + "]}";
}

void StartCapture(const std::string& acEvent, Device aDevice) noexcept
{
    s_captureEvent = acEvent;
    s_captureDevice = aDevice;
    s_capturing = true;
}

bool ResetToDefaults() noexcept
{
    CancelCapture();
    const bool reset = ResetBindingsNative();
    spdlog::info("Reset all key bindings to the game's defaults: {}", reset ? "saved" : "failed");
    if (s_onChanged)
        s_onChanged(BindingsJson());
    return reset;
}

void CancelCapture() noexcept
{
    s_capturing = false;
    s_captureEvent.clear();
    if (s_onChanged)
        s_onChanged(BindingsJson());
}

bool IsCapturing() noexcept
{
    return s_capturing;
}

bool OnKeyboardScanCode(uint16_t aMakeCode, bool aE0) noexcept
{
    if (!s_capturing)
        return false;
    if (aMakeCode == kEscapeScanCode && !aE0)
    {
        CancelCapture();
        return true;
    }
    if (s_captureDevice != Device::Keyboard)
        return true; // swallow keys while waiting for a mouse/gamepad button
    // DirectInput scan codes: E0-prefixed keys set the high bit.
    return FinishCapture(static_cast<uint32_t>((aMakeCode & 0x7F) | (aE0 ? 0x80 : 0)));
}

bool OnMouseButton(uint32_t aButtonIndex) noexcept
{
    if (!s_capturing || s_captureDevice != Device::Mouse)
        return false;
    return FinishCapture(aButtonIndex);
}

bool OnGamepadButtons(uint16_t aPressedMask, bool aLeftTrigger, bool aRightTrigger) noexcept
{
    if (!s_capturing || s_captureDevice != Device::Gamepad)
        return false;
    if (aLeftTrigger)
        return FinishCapture(kLeftTriggerCode);
    if (aRightTrigger)
        return FinishCapture(kRightTriggerCode);
    if (!aPressedMask)
        return false;
    // Lowest set bit: one button per capture.
    return FinishCapture(aPressedMask & static_cast<uint16_t>(-static_cast<int16_t>(aPressedMask)));
}

void SetBindingsChangedCallback(BindingsChangedFn apCallback) noexcept
{
    s_onChanged = apCallback;
}
} // namespace ControlBindings

std::string ControlBindings::ConnectedControllerModel() noexcept
{
    return ControllerModel();
}
