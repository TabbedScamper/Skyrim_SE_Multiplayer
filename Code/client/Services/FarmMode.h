#pragma once
#include <Windows.h>
#include <filesystem>
#include <string>

// Harness builds only. The runner supplies a private root and a restricted token.
// An ordinary launch retains every production path and Steam behavior.
namespace FarmMode
{
#ifdef SEAMLESS_HARNESS
inline constexpr const char* BuildMarker = "SSC_FARM_ENABLED_V2";
#else
inline constexpr const char* BuildMarker = "SSC_FARM_DISABLED";
#endif
inline const std::string& Token()
{
    static const std::string token = [] {
#ifdef SEAMLESS_HARNESS
        char value[96]{};
        const auto size = GetEnvironmentVariableA("SSC_FARM_TOKEN", value, sizeof(value));
        if (size && size < sizeof(value))
        {
            std::string result(value);
            if (result.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_") == std::string::npos)
                return result;
        }
#endif
        return std::string{};
    }();
    return token;
}
inline const std::filesystem::path& Root()
{
    static const std::filesystem::path root = [] {
        wchar_t value[1024]{};
        const auto size = GetEnvironmentVariableW(L"SSC_FARM_ROOT", value, 1024);
        if (!Token().empty() && size && size < 1024 && std::filesystem::path(value).is_absolute())
            return std::filesystem::path(value);
        return std::filesystem::path{};
    }();
    return root;
}
inline bool Enabled() { return !Root().empty(); }
inline const std::string& SaveRelative()
{
    static const std::string path = "Saves\\Farm\\" + Token() + "\\";
    return path;
}
inline const wchar_t* Pipe()
{
    if (!Enabled()) return L"\\\\.\\pipe\\SkyrimSEMultiplayer.Test";
    static const std::wstring name = L"\\\\.\\pipe\\SkyrimSEMultiplayer.Farm." + std::to_wstring(GetCurrentProcessId());
    return name.c_str();
}
}
