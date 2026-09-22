#include <TiltedOnlinePCH.h>

#include <Services/SteamLobbyService.h>
#include <Services/OverlayService.h>
#include <Services/TransportService.h>
#include <World.h>
#include <OverlayApp.hpp>

namespace
{
template <class T> T LoadSteamFunction(HMODULE aModule, const char* acName) noexcept
{
    return reinterpret_cast<T>(GetProcAddress(aModule, acName));
}

using GetInterface = void*(__cdecl*)();
using InitializeSteam = bool(__cdecl*)();
using RunCallbacks = void(__cdecl*)();
using CreateLobby = uint64_t(__cdecl*)(void*, int, int);
using JoinLobby = uint64_t(__cdecl*)(void*, uint64_t);
using LeaveLobby = void(__cdecl*)(void*, uint64_t);
using SetLobbyData = bool(__cdecl*)(void*, uint64_t, const char*, const char*);
using SetLobbyType = bool(__cdecl*)(void*, uint64_t, int);
using GetLobbyData = const char*(__cdecl*)(void*, uint64_t, const char*);
using IsApiCallCompleted = bool(__cdecl*)(void*, uint64_t, bool*);
using GetApiCallResult = bool(__cdecl*)(void*, uint64_t, void*, int, int, bool*);
using GetLobbyOwner = uint64_t(__cdecl*)(void*, uint64_t);
using GetNumLobbyMembers = int(__cdecl*)(void*, uint64_t);
using GetLobbyMemberByIndex = uint64_t(__cdecl*)(void*, uint64_t, int);
using GetFriendCount = int(__cdecl*)(void*, int);
using GetFriendByIndex = uint64_t(__cdecl*)(void*, int, int);
using GetFriendPersonaName = const char*(__cdecl*)(void*, uint64_t);
using GetFriendGamePlayed = bool(__cdecl*)(void*, uint64_t, void*);
using ActivateGameOverlayInviteDialog = void(__cdecl*)(void*, uint64_t);
using GetAppId = uint32_t(__cdecl*)(void*);

struct FriendGameInfo
{
    uint64_t GameId{};
    uint32_t GameIp{};
    uint16_t GamePort{};
    uint16_t QueryPort{};
    uint64_t LobbyId{};
};
static_assert(sizeof(FriendGameInfo) == 24);

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
    , m_connectedConnection(aDispatcher.sink<ConnectedEvent>().connect<&SteamLobbyService::OnConnected>(this))
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

    // The immersive launcher loads Steam's binaries and supplies the App ID,
    // but Skyrim Together historically never initialized the client API
    // because its direct-connect path did not need Steam interfaces. Lobby
    // matchmaking does: Valve requires SteamAPI_Init to succeed before any
    // ISteam* call.
    const auto initializeSteam = LoadSteamFunction<InitializeSteam>(m_steamModule, "SteamAPI_Init");
    if (!initializeSteam || !initializeSteam())
    {
        ShowMessage("Steam could not initialize. Make sure Steam is running and logged in.");
        return false;
    }

    const auto getMatchmaking = LoadSteamFunction<GetInterface>(m_steamModule, "SteamAPI_SteamMatchmaking_v009");
    const auto getUtils = LoadSteamFunction<GetInterface>(m_steamModule, "SteamAPI_SteamUtils_v010");
    auto getFriends = LoadSteamFunction<GetInterface>(m_steamModule, "SteamAPI_SteamFriends_v017");
    if (!getFriends)
        getFriends = LoadSteamFunction<GetInterface>(m_steamModule, "SteamAPI_SteamFriends_v016");
    m_matchmaking = getMatchmaking ? getMatchmaking() : nullptr;
    m_utils = getUtils ? getUtils() : nullptr;
    m_friends = getFriends ? getFriends() : nullptr;
    if (!m_matchmaking || !m_utils || !m_friends)
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
    if (m_lobbyId)
    {
        PublishLobbyState();
        return;
    }

    const auto createLobby = LoadSteamFunction<CreateLobby>(m_steamModule, "SteamAPI_ISteamMatchmaking_CreateLobby");
    if (!createLobby || !StartLocalServer())
        return;

    m_apiCall = createLobby(m_matchmaking, 0, 2); // private/invite-only by default
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

void SteamLobbyService::JoinFriend(const uint64_t aSteamId) noexcept
{
    if (!Initialize())
        return;
    const auto getFriendGamePlayed = LoadSteamFunction<GetFriendGamePlayed>(m_steamModule, "SteamAPI_ISteamFriends_GetFriendGamePlayed");
    FriendGameInfo game{};
    if (!getFriendGamePlayed || !getFriendGamePlayed(m_friends, aSteamId, &game) || !game.LobbyId)
    {
        ShowMessage("That friend is not currently in a joinable Skyrim SE Multiplayer lobby.");
        return;
    }
    spdlog::info("Steam friend join resolved friend {} to lobby {} (current {})", aSteamId, game.LobbyId, m_lobbyId);
    if (game.LobbyId == m_lobbyId)
        return;
    m_world.GetTransport().Close();
    LeaveSession();
    StopLocalServer();
    JoinSession(std::to_string(game.LobbyId).c_str());
}

void SteamLobbyService::InviteFriend() noexcept
{
    if (!Initialize() || !m_lobbyId)
    {
        ShowMessage("Create or join a lobby before inviting a friend.");
        return;
    }
    const auto activate = LoadSteamFunction<ActivateGameOverlayInviteDialog>(m_steamModule, "SteamAPI_ISteamFriends_ActivateGameOverlayInviteDialog");
    if (activate)
        activate(m_friends, m_lobbyId);
    else
        ShowMessage("Steam's friend invitation dialog is unavailable.");
}

void SteamLobbyService::RefreshLobbyState() noexcept
{
    if (Initialize())
        PublishLobbyState();
}

void SteamLobbyService::ApplyPartySettings(const bool aOpen, const bool aPasswordProtected) noexcept
{
    if (!m_isHost || !m_lobbyId || !Initialize())
        return;
    m_lobbyOpen = aOpen;
    m_passwordProtected = aOpen && aPasswordProtected;
    const auto setType = LoadSteamFunction<SetLobbyType>(m_steamModule, "SteamAPI_ISteamMatchmaking_SetLobbyType");
    const auto setData = LoadSteamFunction<SetLobbyData>(m_steamModule, "SteamAPI_ISteamMatchmaking_SetLobbyData");
    if (setType)
        setType(m_matchmaking, m_lobbyId, m_lobbyOpen ? 1 : 0); // friends-only when open, invite-only when private
    if (setData)
    {
        setData(m_matchmaking, m_lobbyId, "visibility", m_lobbyOpen ? "open" : "private");
        setData(m_matchmaking, m_lobbyId, "has_password", m_passwordProtected ? "1" : "0");
    }
    PublishLobbyState();
}

void SteamLobbyService::ConnectJoinedSession(const String& acPassword) noexcept
{
    if (!m_lobbyId || m_isHost || m_joinEndpoint.empty())
        return;
    m_world.GetTransport().SetServerPassword(acPassword.c_str());
    m_world.GetTransport().Connect(m_joinEndpoint.c_str());
    PublishLobbyState();
}

void SteamLobbyService::OnUpdate(const UpdateEvent&) noexcept
{
    PumpCallbacks();
}

void SteamLobbyService::PumpCallbacks() noexcept
{
    // World::Update does not reliably run on the title screen. This queue is
    // also drained by PollMainMenuOptions, unlike the gameplay RunnerService.
    m_titleScreenTasks.Drain();

    if (!m_autoHostAttempted && GetTickCount64() > 2000)
    {
        m_autoHostAttempted = true;
        HostSession();
    }
    const auto runCallbacks = LoadSteamFunction<RunCallbacks>(m_steamModule, "SteamAPI_RunCallbacks");
    if (runCallbacks)
        runCallbacks();

    const auto now = GetTickCount64();
    if (now >= m_nextLobbyRefresh)
    {
        m_nextLobbyRefresh = now + 1000;
        PublishLobbyState();
    }

    if (m_pending == PendingOperation::None || !m_apiCall)
        return;

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

void SteamLobbyService::QueueHostSession() noexcept
{
    m_titleScreenTasks.Add([this]() { HostSession(); });
}

void SteamLobbyService::QueueJoinSession(String aLobbyId) noexcept
{
    m_titleScreenTasks.Add([this, lobbyId = std::move(aLobbyId)]() { JoinSession(lobbyId); });
}

void SteamLobbyService::QueueLeaveSession() noexcept
{
    m_titleScreenTasks.Add([this]() { LeaveSession(); });
}

void SteamLobbyService::QueueJoinFriend(const uint64_t aSteamId) noexcept
{
    m_titleScreenTasks.Add([this, aSteamId]() { JoinFriend(aSteamId); });
}

void SteamLobbyService::QueueInviteFriend() noexcept
{
    m_titleScreenTasks.Add([this]() { InviteFriend(); });
}

void SteamLobbyService::QueueRefreshLobbyState() noexcept
{
    m_titleScreenTasks.Add([this]() { RefreshLobbyState(); });
}

void SteamLobbyService::QueueConnectJoinedSession(String aPassword) noexcept
{
    m_titleScreenTasks.Add([this, password = std::move(aPassword)]() { ConnectJoinedSession(password); });
}

void SteamLobbyService::OnConnected(const ConnectedEvent&) noexcept
{
    if (!m_isHost && m_waitingForPassword)
    {
        m_waitingForPassword = false;
        PublishLobbyState();
    }
    if (m_lobbyId && m_isHost && !m_world.GetPartyService().IsInParty())
        m_world.GetPartyService().CreateParty();
}

void SteamLobbyService::CompleteCreate() noexcept
{
    const auto getResult = LoadSteamFunction<GetApiCallResult>(m_steamModule, "SteamAPI_ISteamUtils_GetAPICallResult");
    LobbyCreatedResult result{};
    bool failed = false;
    const bool received = getResult && getResult(m_utils, m_apiCall, &result, sizeof(result), 513, &failed);
    spdlog::info("Steam LobbyCreated_t: received={}, ioFailed={}, result={}, lobbyId={}", received, failed, result.Result, result.LobbyId);
    if (!received || failed ||
        result.Result != 1 || !result.LobbyId)
    {
        const auto message = fmt::format("Steam could not create the lobby (result {}).", result.Result);
        ShowMessage(message.c_str());
    }
    else
    {
        m_lobbyId = result.LobbyId;
        m_isHost = true;
        const auto setData = LoadSteamFunction<SetLobbyData>(m_steamModule, "SteamAPI_ISteamMatchmaking_SetLobbyData");
        const auto endpoint = GetLanEndpoint();
        if (setData)
        {
            setData(m_matchmaking, m_lobbyId, "skyrim_se_multiplayer", "1");
            setData(m_matchmaking, m_lobbyId, "server_address", endpoint.c_str());
            setData(m_matchmaking, m_lobbyId, "build", BUILD_COMMIT);
            setData(m_matchmaking, m_lobbyId, "visibility", "private");
            setData(m_matchmaking, m_lobbyId, "has_password", "0");
        }

        const auto readyMessage = "Session ready. Lobby ID: " + std::to_string(m_lobbyId);
        ShowMessage(readyMessage.c_str());
        m_world.GetTransport().SetServerPassword("");
        m_world.GetTransport().Connect("127.0.0.1:10578");
        PublishLobbyState();
    }

    m_pending = PendingOperation::None;
    m_apiCall = 0;
    PublishLobbyState();
}

void SteamLobbyService::CompleteJoin() noexcept
{
    const auto getResult = LoadSteamFunction<GetApiCallResult>(m_steamModule, "SteamAPI_ISteamUtils_GetAPICallResult");
    LobbyEnterResult result{};
    bool failed = false;
    const bool received = getResult && getResult(m_utils, m_apiCall, &result, sizeof(result), 504, &failed);
    spdlog::info("Steam LobbyEnter_t: received={}, ioFailed={}, response={}, lobbyId={}", received, failed, result.EnterResponse, result.LobbyId);
    if (!received || failed ||
        result.EnterResponse != 1 || !result.LobbyId)
    {
        ShowMessage("Steam could not join the lobby.");
    }
    else
    {
        m_lobbyId = result.LobbyId;
        m_isHost = false;
        const auto getData = LoadSteamFunction<GetLobbyData>(m_steamModule, "SteamAPI_ISteamMatchmaking_GetLobbyData");
        const char* pMarker = getData ? getData(m_matchmaking, m_lobbyId, "skyrim_se_multiplayer") : nullptr;
        const char* pBuild = getData ? getData(m_matchmaking, m_lobbyId, "build") : nullptr;
        const char* pEndpoint = getData ? getData(m_matchmaking, m_lobbyId, "server_address") : nullptr;
        if (!pMarker || strcmp(pMarker, "1") != 0 || !pBuild || strcmp(pBuild, BUILD_COMMIT) != 0)
        {
            ShowMessage("That friend's session is not a compatible Skyrim SE Multiplayer build.");
            LeaveSession();
        }
        else if (!pEndpoint || !*pEndpoint)
            ShowMessage("The host has not published a server endpoint.");
        else
        {
            m_joinEndpoint = pEndpoint;
            const char* pHasPassword = getData(m_matchmaking, m_lobbyId, "has_password");
            m_passwordProtected = pHasPassword && strcmp(pHasPassword, "1") == 0;
            const char* pVisibility = getData(m_matchmaking, m_lobbyId, "visibility");
            m_lobbyOpen = pVisibility && strcmp(pVisibility, "open") == 0;
            m_waitingForPassword = m_passwordProtected;
            if (m_waitingForPassword)
                ShowMessage("Session joined. Enter its password to connect.");
            else
            {
                ShowMessage("Steam lobby joined. Connecting to host...");
                ConnectJoinedSession({});
            }
            PublishLobbyState();
        }
    }

    m_pending = PendingOperation::None;
    m_apiCall = 0;
    PublishLobbyState();
}

void SteamLobbyService::PublishLobbyState() noexcept
{
    if (!m_steamModule || !m_matchmaking || !m_friends)
        return;
    const auto getOwner = LoadSteamFunction<GetLobbyOwner>(m_steamModule, "SteamAPI_ISteamMatchmaking_GetLobbyOwner");
    const auto getMemberCount = LoadSteamFunction<GetNumLobbyMembers>(m_steamModule, "SteamAPI_ISteamMatchmaking_GetNumLobbyMembers");
    const auto getMember = LoadSteamFunction<GetLobbyMemberByIndex>(m_steamModule, "SteamAPI_ISteamMatchmaking_GetLobbyMemberByIndex");
    const auto getFriendCount = LoadSteamFunction<GetFriendCount>(m_steamModule, "SteamAPI_ISteamFriends_GetFriendCount");
    const auto getFriend = LoadSteamFunction<GetFriendByIndex>(m_steamModule, "SteamAPI_ISteamFriends_GetFriendByIndex");
    const auto getName = LoadSteamFunction<GetFriendPersonaName>(m_steamModule, "SteamAPI_ISteamFriends_GetFriendPersonaName");
    const auto getGame = LoadSteamFunction<GetFriendGamePlayed>(m_steamModule, "SteamAPI_ISteamFriends_GetFriendGamePlayed");
    const auto getAppId = LoadSteamFunction<GetAppId>(m_steamModule, "SteamAPI_ISteamUtils_GetAppID");
    if (!getOwner || !getMemberCount || !getMember || !getFriendCount || !getFriend || !getName || !getGame || !getAppId)
        return;

    auto arguments = CefListValue::Create();
    arguments->SetString(0, std::to_string(m_lobbyId));
    arguments->SetString(1, std::to_string(m_lobbyId ? getOwner(m_matchmaking, m_lobbyId) : 0));
    auto memberIds = CefListValue::Create();
    auto memberNames = CefListValue::Create();
    const int memberCount = m_lobbyId ? getMemberCount(m_matchmaking, m_lobbyId) : 0;
    for (int i = 0; i < memberCount; ++i)
    {
        const auto id = getMember(m_matchmaking, m_lobbyId, i);
        memberIds->SetString(i, std::to_string(id));
        const char* pName = getName(m_friends, id);
        memberNames->SetString(i, pName ? pName : "Steam player");
    }
    arguments->SetList(2, memberIds);
    arguments->SetList(3, memberNames);

    auto friendIds = CefListValue::Create();
    auto friendNames = CefListValue::Create();
    int joinableCount = 0;
    constexpr int kImmediateFriends = 0x04;
    const int friendCount = getFriendCount(m_friends, kImmediateFriends);
    for (int i = 0; i < friendCount; ++i)
    {
        const auto id = getFriend(m_friends, i, kImmediateFriends);
        FriendGameInfo game{};
        if (!getGame(m_friends, id, &game) || !game.LobbyId || (game.GameId & 0xFFFFFFu) != getAppId(m_utils))
            continue;
        friendIds->SetString(joinableCount, std::to_string(id));
        const char* pName = getName(m_friends, id);
        friendNames->SetString(joinableCount, pName ? pName : "Steam friend");
        ++joinableCount;
    }
    arguments->SetList(4, friendIds);
    arguments->SetList(5, friendNames);
    arguments->SetBool(6, m_lobbyOpen);
    arguments->SetBool(7, m_passwordProtected);
    arguments->SetBool(8, m_waitingForPassword);
    arguments->SetBool(9, m_isHost);
    if (auto* pApp = m_world.GetOverlayService().GetOverlayApp())
        pApp->ExecuteAsync("steamLobbyState", arguments);
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
    m_isHost = false;
    m_lobbyOpen = false;
    m_passwordProtected = false;
    m_waitingForPassword = false;
    m_joinEndpoint.clear();
    m_pending = PendingOperation::None;
    m_apiCall = 0;
    PublishLobbyState();
}

void SteamLobbyService::StopLocalServer() noexcept
{
    if (m_serverProcess && WaitForSingleObject(m_serverProcess, 0) == WAIT_TIMEOUT)
        TerminateProcess(m_serverProcess, 0);
    if (m_serverProcess)
        WaitForSingleObject(m_serverProcess, 3000);
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
