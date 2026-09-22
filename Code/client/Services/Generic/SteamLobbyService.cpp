#include <TiltedOnlinePCH.h>

#include <Services/SteamLobbyService.h>
#include <Services/OverlayService.h>
#include <Services/TransportService.h>
#include <World.h>

namespace
{
template <class T> T LoadSteamFunction(HMODULE aModule, const char* acName) noexcept
{
    return reinterpret_cast<T>(GetProcAddress(aModule, acName));
}

using GetInterface = void*(__cdecl*)();
using RunCallbacks = void(__cdecl*)();
using CreateLobby = uint64_t(__cdecl*)(void*, int, int);
using JoinLobby = uint64_t(__cdecl*)(void*, uint64_t);
using LeaveLobby = void(__cdecl*)(void*, uint64_t);
using SetLobbyData = bool(__cdecl*)(void*, uint64_t, const char*, const char*);
using GetLobbyData = const char*(__cdecl*)(void*, uint64_t, const char*);
using IsApiCallCompleted = bool(__cdecl*)(void*, uint64_t, bool*);
using GetApiCallResult = bool(__cdecl*)(void*, uint64_t, void*, int, int, bool*);

struct LobbyCreatedResult
{
    int32_t Result;
    uint32_t Padding;
    uint64_t LobbyId;
};

struct LobbyEnterResult
{
    uint64_t LobbyId;
    uint32_t ChatPermissions;
    bool Locked;
    uint8_t Padding[3];
    uint32_t EnterResponse;
};
}

SteamLobbyService::SteamLobbyService(World& aWorld, entt::dispatcher& aDispatcher) noexcept
    : m_world(aWorld)
    , m_updateConnection(aDispatcher.sink<UpdateEvent>().connect<&SteamLobbyService::OnUpdate>(this))
{
}

SteamLobbyService::~SteamLobbyService() noexcept
{
    LeaveSession();
    if (m_serverJob)
        CloseHandle(m_serverJob);
    if (m_serverThread)
        CloseHandle(m_serverThread);
    if (m_serverProcess)
        CloseHandle(m_serverProcess);
}

bool SteamLobbyService::Initialize() noexcept
{
    if (m_matchmaking && m_utils)
        return true;

    m_steamModule = GetModuleHandleW(L"steam_api64.dll");
    if (!m_steamModule)
    {
        ShowMessage("Steam is unavailable. Launch Skyrim through Steam first.");
        return false;
    }

    const auto getMatchmaking = LoadSteamFunction<GetInterface>(m_steamModule, "SteamAPI_SteamMatchmaking_v009");
    const auto getUtils = LoadSteamFunction<GetInterface>(m_steamModule, "SteamAPI_SteamUtils_v010");
    m_matchmaking = getMatchmaking ? getMatchmaking() : nullptr;
    m_utils = getUtils ? getUtils() : nullptr;
    if (!m_matchmaking || !m_utils)
    {
        ShowMessage("Steam matchmaking could not be initialized.");
        return false;
    }
    return true;
}

void SteamLobbyService::HostSession() noexcept
{
    if (!Initialize() || m_pending != PendingOperation::None)
        return;

    const auto createLobby = LoadSteamFunction<CreateLobby>(m_steamModule, "SteamAPI_ISteamMatchmaking_CreateLobby");
    if (!createLobby || !StartLocalServer())
        return;

    m_apiCall = createLobby(m_matchmaking, 1, 2); // friends-only, two players
    if (!m_apiCall)
    {
        ShowMessage("Steam refused to create the session.");
        return;
    }
    m_pending = PendingOperation::Create;
    ShowMessage("Creating Skyrim SE Multiplayer session...");
}

void SteamLobbyService::JoinSession(const String& acLobbyId) noexcept
{
    if (!Initialize() || m_pending != PendingOperation::None)
        return;

    uint64_t lobbyId = 0;
    try
    {
        lobbyId = std::stoull(acLobbyId.c_str());
    }
    catch (...)
    {
        ShowMessage("The Steam lobby ID is invalid.");
        return;
    }

    const auto joinLobby = LoadSteamFunction<JoinLobby>(m_steamModule, "SteamAPI_ISteamMatchmaking_JoinLobby");
    m_apiCall = joinLobby ? joinLobby(m_matchmaking, lobbyId) : 0;
    if (!m_apiCall)
    {
        ShowMessage("Steam refused to join the session.");
        return;
    }
    m_pending = PendingOperation::Join;
    ShowMessage("Joining Skyrim SE Multiplayer session...");
}

void SteamLobbyService::OnUpdate(const UpdateEvent&) noexcept
{
    if (m_pending == PendingOperation::None || !m_apiCall)
        return;

    const auto runCallbacks = LoadSteamFunction<RunCallbacks>(m_steamModule, "SteamAPI_RunCallbacks");
    if (runCallbacks)
        runCallbacks();

    const auto isCompleted = LoadSteamFunction<IsApiCallCompleted>(m_steamModule, "SteamAPI_ISteamUtils_IsAPICallCompleted");
    bool failed = false;
    if (!isCompleted || !isCompleted(m_utils, m_apiCall, &failed))
        return;

    if (failed)
    {
        ShowMessage("Steam session request failed.");
        m_pending = PendingOperation::None;
        m_apiCall = 0;
        return;
    }

    if (m_pending == PendingOperation::Create)
        CompleteCreate();
    else
        CompleteJoin();
}

void SteamLobbyService::CompleteCreate() noexcept
{
    const auto getResult = LoadSteamFunction<GetApiCallResult>(m_steamModule, "SteamAPI_ISteamUtils_GetAPICallResult");
    LobbyCreatedResult result{};
    bool failed = false;
    if (!getResult || !getResult(m_utils, m_apiCall, &result, sizeof(result), 513, &failed) || failed ||
        result.Result != 1 || !result.LobbyId)
    {
        ShowMessage("Steam could not create the lobby.");
    }
    else
    {
        m_lobbyId = result.LobbyId;
        const auto setData = LoadSteamFunction<SetLobbyData>(m_steamModule, "SteamAPI_ISteamMatchmaking_SetLobbyData");
        const auto endpoint = GetLanEndpoint();
        if (setData)
        {
            setData(m_matchmaking, m_lobbyId, "skyrim_se_multiplayer", "1");
            setData(m_matchmaking, m_lobbyId, "server_address", endpoint.c_str());
            setData(m_matchmaking, m_lobbyId, "build", BUILD_COMMIT);
        }

        const auto readyMessage = "Session ready. Lobby ID: " + std::to_string(m_lobbyId);
        ShowMessage(readyMessage.c_str());
        m_world.GetTransport().SetServerPassword("");
        m_world.GetTransport().Connect("127.0.0.1:10578");
    }

    m_pending = PendingOperation::None;
    m_apiCall = 0;
}

void SteamLobbyService::CompleteJoin() noexcept
{
    const auto getResult = LoadSteamFunction<GetApiCallResult>(m_steamModule, "SteamAPI_ISteamUtils_GetAPICallResult");
    LobbyEnterResult result{};
    bool failed = false;
    if (!getResult || !getResult(m_utils, m_apiCall, &result, sizeof(result), 504, &failed) || failed ||
        result.EnterResponse != 1 || !result.LobbyId)
    {
        ShowMessage("Steam could not join the lobby.");
    }
    else
    {
        m_lobbyId = result.LobbyId;
        const auto getData = LoadSteamFunction<GetLobbyData>(m_steamModule, "SteamAPI_ISteamMatchmaking_GetLobbyData");
        const char* pEndpoint = getData ? getData(m_matchmaking, m_lobbyId, "server_address") : nullptr;
        if (!pEndpoint || !*pEndpoint)
            ShowMessage("The host has not published a server endpoint.");
        else
        {
            ShowMessage("Steam lobby joined. Connecting to host...");
            m_world.GetTransport().SetServerPassword("");
            m_world.GetTransport().Connect(pEndpoint);
        }
    }

    m_pending = PendingOperation::None;
    m_apiCall = 0;
}

void SteamLobbyService::LeaveSession() noexcept
{
    if (m_lobbyId && m_steamModule && m_matchmaking)
    {
        const auto leaveLobby = LoadSteamFunction<LeaveLobby>(m_steamModule, "SteamAPI_ISteamMatchmaking_LeaveLobby");
        if (leaveLobby)
            leaveLobby(m_matchmaking, m_lobbyId);
    }
    m_lobbyId = 0;
    m_pending = PendingOperation::None;
    m_apiCall = 0;
}

bool SteamLobbyService::StartLocalServer() noexcept
{
    if (m_serverProcess && WaitForSingleObject(m_serverProcess, 0) == WAIT_TIMEOUT)
        return true;

    wchar_t gamePath[MAX_PATH]{};
    if (!GetModuleFileNameW(nullptr, gamePath, MAX_PATH))
        return false;

    const auto serverDirectory = std::filesystem::path(gamePath).parent_path() / L"Data" / L"SkyrimTogetherReborn";
    const auto serverPath = serverDirectory / L"SkyrimTogetherServer.exe";
    if (!std::filesystem::exists(serverPath))
    {
        ShowMessage("Skyrim Together server executable was not found.");
        return false;
    }

    m_serverJob = CreateJobObjectW(nullptr, nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    SetInformationJobObject(m_serverJob, JobObjectExtendedLimitInformation, &limits, sizeof(limits));

    STARTUPINFOW startup{sizeof(startup)};
    PROCESS_INFORMATION process{};
    auto command = L"\"" + serverPath.wstring() + L"\"";
    if (!CreateProcessW(serverPath.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
            serverDirectory.c_str(), &startup, &process))
    {
        ShowMessage("The local Skyrim Together server could not start.");
        return false;
    }

    m_serverProcess = process.hProcess;
    m_serverThread = process.hThread;
    if (m_serverJob)
        AssignProcessToJobObject(m_serverJob, m_serverProcess);
    return true;
}

String SteamLobbyService::GetLanEndpoint() const noexcept
{
    char hostname[256]{};
    if (gethostname(hostname, sizeof(hostname)) == 0)
    {
        const auto* pHost = gethostbyname(hostname);
        if (pHost && pHost->h_addrtype == AF_INET)
        {
            for (auto** ppAddress = pHost->h_addr_list; ppAddress && *ppAddress; ++ppAddress)
            {
                in_addr ipv4{};
                memcpy(&ipv4, *ppAddress, sizeof(ipv4));
                const char* pAddress = inet_ntoa(ipv4);
                if (pAddress && strncmp(pAddress, "169.254.", 8) != 0 && strncmp(pAddress, "127.", 4) != 0)
                {
                    return String(pAddress) + ":10578";
                }
            }
        }
    }
    return "127.0.0.1:10578";
}

void SteamLobbyService::ShowMessage(const String& acMessage) const noexcept
{
    spdlog::info("Steam lobby: {}", acMessage);
    m_world.GetOverlayService().SendSystemMessage(acMessage.c_str());
}
