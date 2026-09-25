#include <TiltedOnlinePCH.h>

#include <Games/Skyrim/Audio/AudioDeviceSelection.h>

#include <mmdeviceapi.h>
#include <propvarutil.h>

#include <mutex>

namespace
{
// XAudio2 2.7 (XAudio2_7.dll), the version Skyrim loads. Its CLSID and the
// XAUDIO2_DEVICE_DETAILS layout (pack(1): DeviceID[256], DisplayName[256],
// Role at byte 1024) match the stack buffer Skyrim's init reads the Role from.
constexpr CLSID kClsidXAudio27{0x5a508685, 0xa254, 0x4fba, {0x9b, 0x82, 0x9a, 0x24, 0xb0, 0x03, 0x06, 0xaf}};
constexpr uint32_t kDefaultGameDevice = 0x8;
constexpr size_t kRoleOffset = 1024;
constexpr size_t kDeviceDetailsSize = 1024 + 4 + 40; // + WAVEFORMATEXTENSIBLE (40, packed)

// IXAudio2 2.7 vtable slots (Skyrim calls them at +0x18/+0x20/+0x50).
constexpr size_t kSlotGetDeviceCount = 3;
constexpr size_t kSlotGetDeviceDetails = 4;
constexpr size_t kSlotCreateMasteringVoice = 10;

using TGetDeviceCount = HRESULT(STDMETHODCALLTYPE*)(void*, uint32_t*);
using TGetDeviceDetails = HRESULT(STDMETHODCALLTYPE*)(void*, uint32_t, void*);
using TCreateMasteringVoice = HRESULT(STDMETHODCALLTYPE*)(void*, void**, uint32_t, uint32_t, uint32_t, uint32_t, const void*);

TGetDeviceCount s_realGetDeviceCount = nullptr;
TGetDeviceDetails s_realGetDeviceDetails = nullptr;
TCreateMasteringVoice s_realCreateMasteringVoice = nullptr;

std::mutex s_lock;
std::string s_preferred;
bool s_preferredLoaded = false;
std::string s_active;

// PKEY_Device_FriendlyName, spelled out to avoid needing INITGUID/propsys.
constexpr PROPERTYKEY kFriendlyName{{0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}}, 14};

std::string Narrow(const wchar_t* acText) noexcept
{
    if (!acText || !*acText)
        return {};
    const int length = WideCharToMultiByte(CP_UTF8, 0, acText, -1, nullptr, 0, nullptr, nullptr);
    std::string out(length > 1 ? length - 1 : 0, '\0');
    if (length > 1)
        WideCharToMultiByte(CP_UTF8, 0, acText, -1, out.data(), length, nullptr, nullptr);
    return out;
}

std::wstring Widen(const std::string& acText) noexcept
{
    if (acText.empty())
        return {};
    const int length = MultiByteToWideChar(CP_UTF8, 0, acText.c_str(), -1, nullptr, 0);
    std::wstring out(length > 1 ? length - 1 : 0, L'\0');
    if (length > 1)
        MultiByteToWideChar(CP_UTF8, 0, acText.c_str(), -1, out.data(), length);
    return out;
}

std::filesystem::path PrefsPath() noexcept
{
    wchar_t userProfile[MAX_PATH]{};
    if (GetEnvironmentVariableW(L"USERPROFILE", userProfile, MAX_PATH))
        return std::filesystem::path(userProfile) / L"Documents" / L"My Games" / L"Skyrim Special Edition" / L"SkyrimPrefs.ini";
    return L"SkyrimPrefs.ini";
}

// Caller holds s_lock.
const std::string& PreferredLocked() noexcept
{
    if (!s_preferredLoaded)
    {
        wchar_t value[512]{};
        GetPrivateProfileStringW(L"SkyrimTogether", L"sAudioDevice", L"", value, 512, PrefsPath().c_str());
        s_preferred = Narrow(value);
        s_preferredLoaded = true;
    }
    return s_preferred;
}

std::string DeviceIdOf(void* apDetails) noexcept
{
    return Narrow(static_cast<const wchar_t*>(apDetails));
}

// Index of the preferred endpoint in this engine's device list, or -1.
int FindPreferredIndex(void* apEngine, const std::string& acPreferred) noexcept
{
    uint32_t count = 0;
    if (acPreferred.empty() || FAILED(s_realGetDeviceCount(apEngine, &count)))
        return -1;
    alignas(8) uint8_t details[kDeviceDetailsSize]{};
    for (uint32_t i = 0; i < count; ++i)
    {
        if (SUCCEEDED(s_realGetDeviceDetails(apEngine, i, details)) &&
            _stricmp(DeviceIdOf(details).c_str(), acPreferred.c_str()) == 0)
            return static_cast<int>(i);
    }
    return -1;
}

HRESULT STDMETHODCALLTYPE HookGetDeviceDetails(void* apEngine, uint32_t aIndex, void* apDetails)
{
    const HRESULT result = s_realGetDeviceDetails(apEngine, aIndex, apDetails);
    if (FAILED(result) || !apDetails)
        return result;

    std::string preferred;
    {
        std::lock_guard lock(s_lock);
        preferred = PreferredLocked();
    }
    const int preferredIndex = FindPreferredIndex(apEngine, preferred);
    if (preferredIndex < 0)
        return result; // no choice, or the device is gone: keep Windows' default

    auto& role = *reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(apDetails) + kRoleOffset);
    if (static_cast<int>(aIndex) == preferredIndex)
        role |= kDefaultGameDevice;
    else
        role &= ~kDefaultGameDevice;
    return result;
}

HRESULT STDMETHODCALLTYPE HookCreateMasteringVoice(void* apEngine, void** appVoice, uint32_t aChannels, uint32_t aRate,
    uint32_t aFlags, uint32_t aDeviceIndex, const void* apChain)
{
    const HRESULT result = s_realCreateMasteringVoice(apEngine, appVoice, aChannels, aRate, aFlags, aDeviceIndex, apChain);
    alignas(8) uint8_t details[kDeviceDetailsSize]{};
    if (SUCCEEDED(result) && SUCCEEDED(s_realGetDeviceDetails(apEngine, aDeviceIndex, details)))
    {
        const auto id = DeviceIdOf(details);
        spdlog::info("Game audio output: {} ({})", Narrow(reinterpret_cast<const wchar_t*>(details + 512)), id);
        std::lock_guard lock(s_lock);
        s_active = id;
    }
    return result;
}

template <class T> void PatchSlot(void** apVtable, size_t aSlot, T apHook, T& arOriginal) noexcept
{
    if (apVtable[aSlot] == reinterpret_cast<void*>(apHook))
        return;
    DWORD oldProtect{};
    if (!VirtualProtect(&apVtable[aSlot], sizeof(void*), PAGE_READWRITE, &oldProtect))
        return;
    arOriginal = reinterpret_cast<T>(apVtable[aSlot]);
    apVtable[aSlot] = reinterpret_cast<void*>(apHook);
    VirtualProtect(&apVtable[aSlot], sizeof(void*), oldProtect, &oldProtect);
}

HRESULT WINAPI HookCreateXAudio2(REFCLSID acClsid, LPUNKNOWN apOuter, DWORD aContext, REFIID acIid, LPVOID* appOut)
{
    const HRESULT result = CoCreateInstance(acClsid, apOuter, aContext, acIid, appOut);
    if (SUCCEEDED(result) && appOut && *appOut && IsEqualCLSID(acClsid, kClsidXAudio27))
    {
        // The vtable lives in XAudio2_7.dll and is shared by every engine it creates.
        auto** pVtable = *static_cast<void***>(*appOut);
        s_realGetDeviceCount = reinterpret_cast<TGetDeviceCount>(pVtable[kSlotGetDeviceCount]);
        PatchSlot(pVtable, kSlotGetDeviceDetails, &HookGetDeviceDetails, s_realGetDeviceDetails);
        PatchSlot(pVtable, kSlotCreateMasteringVoice, &HookCreateMasteringVoice, s_realCreateMasteringVoice);
    }
    return result;
}
} // namespace

namespace AudioDeviceSelection
{
std::vector<Endpoint> EnumerateOutputs() noexcept
{
    std::vector<Endpoint> result;
    const HRESULT init = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool uninit = SUCCEEDED(init);

    IMMDeviceEnumerator* pEnumerator = nullptr;
    if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
            reinterpret_cast<void**>(&pEnumerator))))
    {
        std::string defaultId;
        IMMDevice* pDefault = nullptr;
        if (SUCCEEDED(pEnumerator->GetDefaultAudioEndpoint(eRender, eConsole, &pDefault)))
        {
            LPWSTR id = nullptr;
            if (SUCCEEDED(pDefault->GetId(&id)))
            {
                defaultId = Narrow(id);
                CoTaskMemFree(id);
            }
            pDefault->Release();
        }

        IMMDeviceCollection* pCollection = nullptr;
        if (SUCCEEDED(pEnumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &pCollection)))
        {
            UINT count = 0;
            pCollection->GetCount(&count);
            for (UINT i = 0; i < count; ++i)
            {
                IMMDevice* pDevice = nullptr;
                if (FAILED(pCollection->Item(i, &pDevice)))
                    continue;
                Endpoint endpoint;
                LPWSTR id = nullptr;
                if (SUCCEEDED(pDevice->GetId(&id)))
                {
                    endpoint.Id = Narrow(id);
                    CoTaskMemFree(id);
                }
                IPropertyStore* pStore = nullptr;
                if (SUCCEEDED(pDevice->OpenPropertyStore(STGM_READ, &pStore)))
                {
                    PROPVARIANT name;
                    PropVariantInit(&name);
                    if (SUCCEEDED(pStore->GetValue(kFriendlyName, &name)) && name.vt == VT_LPWSTR)
                        endpoint.Name = Narrow(name.pwszVal);
                    PropVariantClear(&name);
                    pStore->Release();
                }
                pDevice->Release();
                if (endpoint.Id.empty())
                    continue;
                if (endpoint.Name.empty())
                    endpoint.Name = endpoint.Id;
                if (endpoint.Id == defaultId)
                    result.insert(result.begin(), std::move(endpoint));
                else
                    result.push_back(std::move(endpoint));
            }
            pCollection->Release();
        }
        pEnumerator->Release();
    }

    if (uninit)
        CoUninitialize();
    return result;
}

std::string GetPreferredDevice() noexcept
{
    std::lock_guard lock(s_lock);
    return PreferredLocked();
}

void SetPreferredDevice(const std::string& acId) noexcept
{
    std::lock_guard lock(s_lock);
    s_preferred = acId;
    s_preferredLoaded = true;
    WritePrivateProfileStringW(L"SkyrimTogether", L"sAudioDevice", Widen(acId).c_str(), PrefsPath().c_str());
}

std::string GetActiveDevice() noexcept
{
    std::lock_guard lock(s_lock);
    return s_active;
}

std::string OutputsJson() noexcept
{
    auto escape = [](const std::string& acText)
    {
        std::string out;
        for (const char c : acText)
        {
            if (c == '"' || c == '\\')
                out += '\\';
            if (static_cast<unsigned char>(c) < 0x20)
                continue;
            out += c;
        }
        return out;
    };

    std::string json = "[";
    for (const auto& endpoint : EnumerateOutputs())
    {
        if (json.size() > 1)
            json += ',';
        json += "{\"id\":\"" + escape(endpoint.Id) + "\",\"name\":\"" + escape(endpoint.Name) + "\"}";
    }
    return json + "]";
}
} // namespace AudioDeviceSelection

static TiltedPhoques::Initializer s_audioDeviceHooks(
    []()
    {
        // BSXAudio2Audio init (ID 67952): `call [rip+disp] CoCreateInstance`
        // for CLSID_XAudio2 at +0xB7 on 1.7.104 (exe corpus 0x140cd7147).
        const VersionDbPtr<uint8_t> audioInit(67952);
        auto* pSite = audioInit.Get() ? audioInit.Get() + 0xB7 : nullptr;
        if (!pSite || pSite[0] != 0xFF || pSite[1] != 0x15)
        {
            spdlog::warn("Audio device selection disabled: unexpected XAudio2 creation site");
            return;
        }
        const auto displacement = reinterpret_cast<intptr_t>(&HookCreateXAudio2) - reinterpret_cast<intptr_t>(pSite + 5);
        if (displacement != static_cast<int32_t>(displacement))
        {
            spdlog::warn("Audio device selection disabled: hook out of rel32 range");
            return;
        }
        // 6-byte indirect call -> 5-byte direct call + nop.
        TiltedPhoques::Put<uint8_t>(mem::pointer(pSite), 0xE8);
        TiltedPhoques::Put<int32_t>(mem::pointer(pSite + 1), static_cast<int32_t>(displacement));
        TiltedPhoques::Put<uint8_t>(mem::pointer(pSite + 5), 0x90);
    });
