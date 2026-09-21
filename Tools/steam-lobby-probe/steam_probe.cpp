#include <Windows.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <string>
#include <thread>

namespace
{
template <class T> T LoadFunction(HMODULE aModule, const char* acName)
{
    return reinterpret_cast<T>(GetProcAddress(aModule, acName));
}

void PrintPointer(const char* acName, const void* apValue)
{
    std::cout << acName << '=' << (apValue ? "available" : "missing") << '\n';
}
}

int wmain(int argc, wchar_t** argv)
{
    if (argc < 2 || argc > 3)
    {
        std::cerr << "usage: steam_probe.exe <path-to-steam_api64.dll> [--create-private-lobby]\n";
        return 2;
    }

    const std::filesystem::path libraryPath(argv[1]);
    const auto module = LoadLibraryW(libraryPath.c_str());
    if (!module)
    {
        std::cerr << "load_library_error=" << GetLastError() << '\n';
        return 3;
    }

    using Init = bool(__cdecl*)();
    using Shutdown = void(__cdecl*)();
    using GetHandle = int32_t(__cdecl*)();
    using GetInterface = void*(__cdecl*)();
    using GetSteamId = uint64_t(__cdecl*)(void*);
    using RunCallbacks = void(__cdecl*)();
    using CreateLobby = uint64_t(__cdecl*)(void*, int, int);
    using LeaveLobby = void(__cdecl*)(void*, uint64_t);
    using IsApiCallCompleted = bool(__cdecl*)(void*, uint64_t, bool*);
    using GetApiCallResult = bool(__cdecl*)(void*, uint64_t, void*, int, int, bool*);

    const auto init = LoadFunction<Init>(module, "SteamAPI_Init");
    const auto shutdown = LoadFunction<Shutdown>(module, "SteamAPI_Shutdown");
    const auto getUserHandle = LoadFunction<GetHandle>(module, "SteamAPI_GetHSteamUser");
    const auto getPipeHandle = LoadFunction<GetHandle>(module, "SteamAPI_GetHSteamPipe");
    const auto getUser = LoadFunction<GetInterface>(module, "SteamAPI_SteamUser_v021");
    const auto getSteamId = LoadFunction<GetSteamId>(module, "SteamAPI_ISteamUser_GetSteamID");
    const auto getMatchmaking = LoadFunction<GetInterface>(module, "SteamAPI_SteamMatchmaking_v009");
    const auto getSockets = LoadFunction<GetInterface>(module, "SteamAPI_SteamNetworkingSockets_SteamAPI_v012");
    const auto getUtils = LoadFunction<GetInterface>(module, "SteamAPI_SteamUtils_v010");
    const auto runCallbacks = LoadFunction<RunCallbacks>(module, "SteamAPI_RunCallbacks");
    const auto createLobby = LoadFunction<CreateLobby>(module, "SteamAPI_ISteamMatchmaking_CreateLobby");
    const auto leaveLobby = LoadFunction<LeaveLobby>(module, "SteamAPI_ISteamMatchmaking_LeaveLobby");
    const auto isApiCallCompleted =
        LoadFunction<IsApiCallCompleted>(module, "SteamAPI_ISteamUtils_IsAPICallCompleted");
    const auto getApiCallResult = LoadFunction<GetApiCallResult>(module, "SteamAPI_ISteamUtils_GetAPICallResult");

    if (!init || !shutdown)
    {
        std::cerr << "steam_api_core=missing\n";
        FreeLibrary(module);
        return 4;
    }

    const bool initialized = init();
    std::cout << "steam_api_initialized=" << (initialized ? "true" : "false") << '\n';
    if (!initialized)
    {
        FreeLibrary(module);
        return 5;
    }

    std::cout << "steam_user_handle=" << (getUserHandle ? getUserHandle() : 0) << '\n';
    std::cout << "steam_pipe_handle=" << (getPipeHandle ? getPipeHandle() : 0) << '\n';

    void* const user = getUser ? getUser() : nullptr;
    PrintPointer("steam_user", user);
    if (user && getSteamId)
        std::cout << "steam_id=" << getSteamId(user) << '\n';

    void* const matchmaking = getMatchmaking ? getMatchmaking() : nullptr;
    void* const utils = getUtils ? getUtils() : nullptr;
    PrintPointer("steam_matchmaking", matchmaking);
    PrintPointer("steam_networking_sockets", getSockets ? getSockets() : nullptr);
    PrintPointer("create_lobby_export", GetProcAddress(module, "SteamAPI_ISteamMatchmaking_CreateLobby"));
    PrintPointer("join_lobby_export", GetProcAddress(module, "SteamAPI_ISteamMatchmaking_JoinLobby"));
    PrintPointer("listen_p2p_export", GetProcAddress(module, "SteamAPI_ISteamNetworkingSockets_CreateListenSocketP2P"));
    PrintPointer("connect_p2p_export", GetProcAddress(module, "SteamAPI_ISteamNetworkingSockets_ConnectP2P"));

    if (argc == 3 && std::wstring_view(argv[2]) == L"--create-private-lobby")
    {
        if (!matchmaking || !utils || !runCallbacks || !createLobby || !leaveLobby || !isApiCallCompleted ||
            !getApiCallResult)
        {
            std::cerr << "private_lobby_test=missing_api\n";
            shutdown();
            FreeLibrary(module);
            return 6;
        }

        // k_ELobbyTypePrivate = 0. SteamAPICall_t is an unsigned 64-bit handle.
        const uint64_t apiCall = createLobby(matchmaking, 0, 2);
        std::cout << "create_lobby_call=" << apiCall << '\n';
        if (!apiCall)
        {
            shutdown();
            FreeLibrary(module);
            return 7;
        }

        bool failed = false;
        bool complete = false;
        for (int attempt = 0; attempt < 200 && !complete; ++attempt)
        {
            runCallbacks();
            complete = isApiCallCompleted(utils, apiCall, &failed);
            if (!complete)
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }

        struct LobbyCreatedResult
        {
            int32_t Result;
            uint32_t Padding;
            uint64_t LobbyId;
        } result{};

        bool resultFailed = false;
        constexpr int cLobbyCreatedCallback = 513;
        const bool received = complete && !failed &&
            getApiCallResult(utils, apiCall, &result, sizeof(result), cLobbyCreatedCallback, &resultFailed);
        std::cout << "private_lobby_complete=" << (complete ? "true" : "false") << '\n';
        std::cout << "private_lobby_result_received=" << (received && !resultFailed ? "true" : "false") << '\n';
        std::cout << "private_lobby_result=" << result.Result << '\n';
        std::cout << "private_lobby_id=" << result.LobbyId << '\n';

        if (received && !resultFailed && result.Result == 1 && result.LobbyId)
        {
            leaveLobby(matchmaking, result.LobbyId);
            for (int i = 0; i < 10; ++i)
            {
                runCallbacks();
                std::this_thread::sleep_for(std::chrono::milliseconds(25));
            }
            std::cout << "private_lobby_left=true\n";
        }
        else
        {
            shutdown();
            FreeLibrary(module);
            return 8;
        }
    }

    shutdown();
    FreeLibrary(module);
    return 0;
}
