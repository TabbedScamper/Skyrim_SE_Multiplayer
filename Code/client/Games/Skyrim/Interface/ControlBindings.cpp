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

std::string ControllerFamily() noexcept
{
    // USB vendor of the first HID game controller: Sony / Nintendo / anything
    // else XInput reports is drawn with Xbox labels.
    UINT count = 0;
    if (GetRawInputDeviceList(nullptr, &count, sizeof(RAWINPUTDEVICELIST)) != 0 || count == 0)
        return "none";
    std::vector<RAWINPUTDEVICELIST> devices(count);
    if (GetRawInputDeviceList(devices.data(), &count, sizeof(RAWINPUTDEVICELIST)) == static_cast<UINT>(-1))
        return "none";

    std::string family = "none";
    for (const auto& device : devices)
    {
        if (device.dwType != RIM_TYPEHID)
            continue;
        RID_DEVICE_INFO info{};
        info.cbSize = sizeof(info);
        UINT size = sizeof(info);
        if (GetRawInputDeviceInfoW(device.hDevice, RIDI_DEVICEINFO, &info, &size) == static_cast<UINT>(-1))
            continue;
        // Generic desktop page, joystick (4) or gamepad (5).
        if (info.hid.usUsagePage != 0x01 || (info.hid.usUsage != 0x04 && info.hid.usUsage != 0x05))
            continue;
        switch (info.hid.dwVendorId)
        {
        case 0x054C: return "playstation";
        case 0x057E: return "nintendo";
        default: family = "xbox"; break;
        }
    }
    // XInput pads (Xbox) are often not listed as HID joysticks.
    return family == "none" ? "xbox" : family;
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
    std::string json = "{\"controller\":\"" + ControllerFamily() + "\",\"bindings\":[";
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
