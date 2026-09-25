#include <TiltedOnlinePCH.h>

#include <Games/Skyrim/Audio/AudioDeviceSelection.h>
#include <ThirdParty/AudioSwitch/AudioSwitch.h>

#include <mmdeviceapi.h>
#include <propvarutil.h>

#include <mutex>

namespace
{
std::mutex s_lock;
std::string s_preferred;
bool s_preferredLoaded = false;

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

void ApplyPreferredDevice() noexcept
{
    // Rebuilds the engine on the audio thread if the target differs from the
    // device in use (the switcher re-reads GetPreferredDevice()).
    audioswitch::RequestReset("the options page changed the output device", false);
}

std::string GetActiveDevice() noexcept
{
    return audioswitch::CurrentDeviceId();
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
