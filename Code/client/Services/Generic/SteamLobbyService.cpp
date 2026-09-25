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
using InviteUserToLobby = bool(__cdecl*)(void*, uint64_t, uint64_t);
using GetFriendPersonaState = int(__cdecl*)(void*, uint64_t);
using SetRichPresence = bool(__cdecl*)(void*, const char*, const char*);
using ClearRichPresence = void(__cdecl*)(void*);
using GetSmallFriendAvatar = int(__cdecl*)(void*, uint64_t);
using GetImageSize = bool(__cdecl*)(void*, int, uint32_t*, uint32_t*);
using GetImageRgba = bool(__cdecl*)(void*, int, uint8_t*, int);

// Callback ids (steam_api's k_iSteamFriendsCallbacks = 300, k_iSteamMatchmakingCallbacks = 500).
constexpr int kGameLobbyJoinRequested = 333;         // {CSteamID lobby, CSteamID friend}
constexpr int kGameRichPresenceJoinRequested = 337;  // {CSteamID friend, char connect[256]}
constexpr int kLobbyInvite = 503;                    // {uint64 user, uint64 lobby, uint64 gameId}

#pragma pack(push, 8)
struct GameLobbyJoinRequestedData
{
    uint64_t LobbyId;
    uint64_t FriendId;
};
struct GameRichPresenceJoinRequestedData
{
    uint64_t FriendId;
    char Connect[256];
};
struct LobbyInviteData
{
    uint64_t FriendId;
    uint64_t LobbyId;
    uint64_t GameId;
};
#pragma pack(pop)

/** "+connect_lobby <id>" as Steam passes it when the game is started from a join or invite. */
uint64_t LobbyFromConnectString(const char* acText) noexcept
{
    if (!acText)
        return 0;
    const char* pFound = strstr(acText, "+connect_lobby");
    if (!pFound)
        return 0;
    pFound += strlen("+connect_lobby");
    while (*pFound == ' ' || *pFound == '"')
        ++pFound;
    return strtoull(pFound, nullptr, 10);
}

std::string Base64(const uint8_t* apData, size_t aSize)
{
    static constexpr char cTable[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((aSize + 2) / 3 * 4);
    for (size_t i = 0; i < aSize; i += 3)
    {
        const uint32_t chunk = (apData[i] << 16) | ((i + 1 < aSize ? apData[i + 1] : 0) << 8) | (i + 2 < aSize ? apData[i + 2] : 0);
        out += cTable[(chunk >> 18) & 63];
        out += cTable[(chunk >> 12) & 63];
        out += i + 1 < aSize ? cTable[(chunk >> 6) & 63] : '=';
        out += i + 2 < aSize ? cTable[chunk & 63] : '=';
    }
    return out;
}

std::string JsonEscape(const char* acText)
{
    std::string out;
    for (const char* p = acText ? acText : ""; *p; ++p)
    {
        const auto c = static_cast<unsigned char>(*p);
        if (c == '"' || c == '\\')
        {
            out += '\\';
            out += *p;
        }
        else if (c >= 0x20)
            out += *p;
    }
    return out;
}

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

/**
 * A Steam CCallbackBase (steam_api.h) for SteamAPI_RegisterCallback. Declared
 * exactly like the SDK class so MSVC lays out the same vtable Steam calls into.
 */
class SteamCallbackBase
{
public:
    virtual void Run(void* apParam) = 0;
    virtual void Run(void* apParam, bool aIoFailure, uint64_t aApiCall) = 0;
    virtual int GetCallbackSizeBytes() = 0;

protected:
    uint8_t m_nCallbackFlags{};
    int m_iCallback{};
    friend struct SteamLobbyService::CallbackBridge;
};

struct SteamLobbyService::CallbackBridge final : SteamCallbackBase
{
    CallbackBridge(SteamLobbyService& aOwner, int aCallback, int aSize) noexcept
        : Owner(aOwner)
        , Size(aSize)
    {
        m_iCallback = aCallback;
    }

    void Run(void* apParam) override
    {
        if (!apParam)
            return;
        switch (m_iCallback)
        {
        case kGameLobbyJoinRequested:
        {
            const auto* pData = static_cast<const GameLobbyJoinRequestedData*>(apParam);
            Owner.OnJoinRequested(pData->LobbyId, pData->FriendId);
            break;
        }
        case kGameRichPresenceJoinRequested:
        {
            const auto* pData = static_cast<const GameRichPresenceJoinRequestedData*>(apParam);
            Owner.OnRichPresenceJoinRequested(pData->FriendId, pData->Connect);
            break;
        }
        case kLobbyInvite:
        {
            const auto* pData = static_cast<const LobbyInviteData*>(apParam);
            Owner.OnLobbyInvite(pData->FriendId, pData->LobbyId);
            break;
        }
        }
    }
    void Run(void* apParam, bool, uint64_t) override { Run(apParam); }
    int GetCallbackSizeBytes() override { return Size; }

    SteamLobbyService& Owner;
    int Size;
};

SteamLobbyService::SteamLobbyService(World& aWorld, entt::dispatcher& aDispatcher) noexcept
    : m_world(aWorld)
    , m_updateConnection(aDispatcher.sink<UpdateEvent>().connect<&SteamLobbyService::OnUpdate>(this))
    , m_connectedConnection(aDispatcher.sink<ConnectedEvent>().connect<&SteamLobbyService::OnConnected>(this))
{
}

SteamLobbyService::~SteamLobbyService() noexcept
{
    if (m_steamModule)
        if (const auto unregister = LoadSteamFunction<void(__cdecl*)(void*)>(m_steamModule, "SteamAPI_UnregisterCallback"))
            for (auto& pCallback : m_callbacks)
                unregister(pCallback.get());
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
    RegisterSteamCallbacks();
    // Started from a Steam "Join game" / accepted invite while the game was closed.
    m_launchLobby = LobbyFromConnectString(GetCommandLineA());
    if (m_launchLobby)
        spdlog::info("Steam lobby: launched with +connect_lobby {}", m_launchLobby);
    return true;
}

void SteamLobbyService::RegisterSteamCallbacks() noexcept
{
    if (!m_callbacks.empty())
        return;
    const auto registerCallback = LoadSteamFunction<void(__cdecl*)(void*, int)>(m_steamModule, "SteamAPI_RegisterCallback");
    if (!registerCallback)
    {
        spdlog::warn("Steam lobby: SteamAPI_RegisterCallback is unavailable; invites must be joined from the co-op menu");
        return;
    }
    for (const auto [id, size] : {std::pair{kGameLobbyJoinRequested, int(sizeof(GameLobbyJoinRequestedData))},
             std::pair{kGameRichPresenceJoinRequested, int(sizeof(GameRichPresenceJoinRequestedData))},
             std::pair{kLobbyInvite, int(sizeof(LobbyInviteData))}})
    {
        auto pCallback = std::make_unique<CallbackBridge>(*this, id, size);
        registerCallback(pCallback.get(), id);
        m_callbacks.push_back(std::move(pCallback));
    }
    spdlog::info("Steam lobby: listening for invites and join requests");
}

void SteamLobbyService::OnJoinRequested(const uint64_t aLobbyId, const uint64_t aFriendId) noexcept
{
    spdlog::info("Steam lobby: join requested for lobby {} (friend {})", aLobbyId, aFriendId);
    std::erase_if(m_invites, [aLobbyId](const Invite& acInvite) { return acInvite.LobbyId == aLobbyId; });
    if (!aLobbyId || aLobbyId == m_lobbyId)
        return;
    m_world.GetTransport().Close();
    LeaveSession();
    StopLocalServer();
    JoinSession(std::to_string(aLobbyId).c_str());
}

void SteamLobbyService::OnRichPresenceJoinRequested(const uint64_t aFriendId, const char* acConnect) noexcept
{
    spdlog::info("Steam lobby: rich presence join from friend {}: {}", aFriendId, acConnect ? acConnect : "");
    OnJoinRequested(LobbyFromConnectString(acConnect), aFriendId);
}

void SteamLobbyService::OnLobbyInvite(const uint64_t aFriendId, const uint64_t aLobbyId) noexcept
{
    if (!aLobbyId || aLobbyId == m_lobbyId)
        return;
    std::erase_if(m_invites, [aLobbyId](const Invite& acInvite) { return acInvite.LobbyId == aLobbyId; });
    m_invites.push_back({aFriendId, aLobbyId});
    const auto getName = LoadSteamFunction<GetFriendPersonaName>(m_steamModule, "SteamAPI_ISteamFriends_GetFriendPersonaName");
    const char* pName = getName ? getName(m_friends, aFriendId) : nullptr;
    spdlog::info("Steam lobby: invite from {} to lobby {}", aFriendId, aLobbyId);
    ShowMessage(String(pName ? pName : "A friend") + " invited you to their session. Open Co-op to accept.");
    PublishLobbyState();
}

void SteamLobbyService::AnswerInvite(const uint64_t aLobbyId, const bool aAccept) noexcept
{
    const bool known = std::any_of(m_invites.begin(), m_invites.end(), [aLobbyId](const Invite& acInvite) { return acInvite.LobbyId == aLobbyId; });
    std::erase_if(m_invites, [aLobbyId](const Invite& acInvite) { return acInvite.LobbyId == aLobbyId; });
    if (aAccept && known)
        OnJoinRequested(aLobbyId, 0);
    PublishLobbyState();
}

void SteamLobbyService::InviteFriendDirect(const uint64_t aSteamId) noexcept
{
    if (!Initialize() || !m_lobbyId)
    {
        ShowMessage("Create or join a lobby before inviting a friend.");
        return;
    }
    const auto invite = LoadSteamFunction<InviteUserToLobby>(m_steamModule, "SteamAPI_ISteamMatchmaking_InviteUserToLobby");
    const bool sent = invite && invite(m_matchmaking, m_lobbyId, aSteamId);
    spdlog::info("Steam lobby: invited friend {} to lobby {}: {}", aSteamId, m_lobbyId, sent);
    if (sent)
        m_invited.insert(aSteamId);
    else
        ShowMessage("Steam could not send the invite.");
    PublishLobbyState();
}

// Friends see "In a Skyrim co-op session" and a Join Game entry that sends
// "+connect_lobby <id>" (GameRichPresenceJoinRequested / launch argument).
void SteamLobbyService::UpdateRichPresence() noexcept
{
    if (m_richPresenceLobby == m_lobbyId)
        return;
    m_richPresenceLobby = m_lobbyId;
    const auto set = LoadSteamFunction<SetRichPresence>(m_steamModule, "SteamAPI_ISteamFriends_SetRichPresence");
    const auto clear = LoadSteamFunction<ClearRichPresence>(m_steamModule, "SteamAPI_ISteamFriends_ClearRichPresence");
    if (!set || !clear)
        return;
    clear(m_friends);
    if (!m_lobbyId)
        return;
    const auto lobby = std::to_string(m_lobbyId);
    set(m_friends, "status", "In a Skyrim co-op session");
    set(m_friends, "connect", ("+connect_lobby " + lobby).c_str());
    set(m_friends, "steam_player_group", lobby.c_str());
    set(m_friends, "steam_player_group_size", "2");
}

// 32x32 Steam avatar as an uncompressed BMP data URL, sent once per friend.
void SteamLobbyService::SendAvatar(const uint64_t aSteamId) noexcept
{
    if (m_avatarsSent.contains(aSteamId))
        return;
    const auto getAvatar = LoadSteamFunction<GetSmallFriendAvatar>(m_steamModule, "SteamAPI_ISteamFriends_GetSmallFriendAvatar");
    const auto getSize = LoadSteamFunction<GetImageSize>(m_steamModule, "SteamAPI_ISteamUtils_GetImageSize");
    const auto getRgba = LoadSteamFunction<GetImageRgba>(m_steamModule, "SteamAPI_ISteamUtils_GetImageRGBA");
    if (!getAvatar || !getSize || !getRgba)
        return;
    const int image = getAvatar(m_friends, aSteamId);
    uint32_t width = 0, height = 0;
    if (image <= 0 || !getSize(m_utils, image, &width, &height) || !width || !height || width > 64 || height > 64)
        return; // not downloaded yet; retried on the next refresh
    std::vector<uint8_t> rgba(width * height * 4);
    if (!getRgba(m_utils, image, rgba.data(), static_cast<int>(rgba.size())))
        return;

    const uint32_t pixelBytes = width * height * 4;
    std::vector<uint8_t> bmp(54 + pixelBytes);
    auto put32 = [&](size_t aOffset, uint32_t aValue) { memcpy(bmp.data() + aOffset, &aValue, 4); };
    bmp[0] = 'B';
    bmp[1] = 'M';
    put32(2, static_cast<uint32_t>(bmp.size()));
    put32(10, 54);
    put32(14, 40);
    put32(18, width);
    put32(22, static_cast<uint32_t>(-static_cast<int32_t>(height))); // top-down
    bmp[26] = 1;
    bmp[28] = 32;
    put32(34, pixelBytes);
    for (uint32_t i = 0; i < width * height; ++i)
    {
        bmp[54 + i * 4 + 0] = rgba[i * 4 + 2];
        bmp[54 + i * 4 + 1] = rgba[i * 4 + 1];
        bmp[54 + i * 4 + 2] = rgba[i * 4 + 0];
        bmp[54 + i * 4 + 3] = rgba[i * 4 + 3];
    }
    m_avatarsSent.insert(aSteamId);
    auto arguments = CefListValue::Create();
    arguments->SetString(0, std::to_string(aSteamId));
    arguments->SetString(1, "data:image/bmp;base64," + Base64(bmp.data(), bmp.size()));
    if (auto* pApp = m_world.GetOverlayService().GetOverlayApp())
        pApp->ExecuteAsync("steamAvatar", arguments);
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
        if (Initialize() && m_launchLobby)
        {
            const auto lobby = m_launchLobby;
            m_launchLobby = 0;
            JoinSession(std::to_string(lobby).c_str());
        }
        else
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

void SteamLobbyService::QueueInviteFriendDirect(const uint64_t aSteamId) noexcept
{
    m_titleScreenTasks.Add([this, aSteamId]() { InviteFriendDirect(aSteamId); });
}

void SteamLobbyService::QueueAnswerInvite(const uint64_t aLobbyId, const bool aAccept) noexcept
{
    m_titleScreenTasks.Add([this, aLobbyId, aAccept]() { AnswerInvite(aLobbyId, aAccept); });
}

std::string SteamLobbyService::TestStateJson() const noexcept
{
    std::lock_guard lock(m_testStateLock);
    return m_testState;
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

    // Every friend who is not offline, with what they are doing:
    // "coop" = in a joinable co-op lobby, "skyrim" = playing Skyrim SE, "online" / "away" / "busy".
    const auto getState = LoadSteamFunction<GetFriendPersonaState>(m_steamModule, "SteamAPI_ISteamFriends_GetFriendPersonaState");
    std::string friends = "[";
    int offline = 0;
    const uint32_t appId = getAppId(m_utils);
    for (int i = 0; i < friendCount; ++i)
    {
        const auto id = getFriend(m_friends, i, kImmediateFriends);
        const int persona = getState ? getState(m_friends, id) : 1;
        FriendGameInfo game{};
        const bool inGame = getGame(m_friends, id, &game);
        const bool sameApp = inGame && (game.GameId & 0xFFFFFFu) == appId;
        if (persona == 0 && !inGame)
        {
            ++offline;
            continue;
        }
        const char* pStatus = sameApp && game.LobbyId ? "coop" : sameApp ? "skyrim"
            : persona == 2 ? "busy" : (persona == 3 || persona == 4) ? "away" : "online";
        const char* pName = getName(m_friends, id);
        if (friends.size() > 1)
            friends += ',';
        friends += fmt::format(R"({{"id":"{}","name":"{}","status":"{}","lobby":"{}","inLobby":{},"invited":{}}})", id,
            JsonEscape(pName ? pName : "Steam friend"), pStatus, sameApp && game.LobbyId ? std::to_string(game.LobbyId) : "",
            m_lobbyId && game.LobbyId == m_lobbyId ? "true" : "false", m_invited.contains(id) ? "true" : "false");
        SendAvatar(id);
    }
    friends += "]";
    std::string invites = "[";
    for (const auto& invite : m_invites)
    {
        const char* pName = getName(m_friends, invite.FriendId);
        if (invites.size() > 1)
            invites += ',';
        invites += fmt::format(R"({{"friendId":"{}","name":"{}","lobby":"{}"}})", invite.FriendId,
            JsonEscape(pName ? pName : "A friend"), invite.LobbyId);
    }
    invites += "]";
    arguments->SetString(10, fmt::format(R"({{"friends":{},"offline":{},"invites":{}}})", friends, offline, invites));
    {
        std::string members = "[";
        for (int i = 0; i < memberCount; ++i)
            members += fmt::format("{}\"{}\"", i ? "," : "", getMember(m_matchmaking, m_lobbyId, i));
        members += "]";
        std::lock_guard lock(m_testStateLock);
        m_testState = fmt::format(R"({{"lobbyId":"{}","isHost":{},"open":{},"members":{},"friends":{},"offline":{},"invites":{}}})",
            m_lobbyId, m_isHost ? "true" : "false", m_lobbyOpen ? "true" : "false", members, friends, offline, invites);
    }
    for (int i = 0; i < memberCount; ++i)
        SendAvatar(getMember(m_matchmaking, m_lobbyId, i));
    UpdateRichPresence();
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
    m_invited.clear();
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

    const auto gameDirectory = std::filesystem::path(gamePath).parent_path();
    const auto serverDirectory = gameDirectory / L"Data" / L"SkyrimTogetherReborn";
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
    // STServer.ini and mandatory deployment manifests use paths relative to
    // Skyrim's root (for example, Data/Skyrim.esm). Starting in the server's
    // binary directory silently turns those into SkyrimTogetherReborn/Data/*
    // and makes the host reject its own otherwise-identical client.
    if (!CreateProcessW(serverPath.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
            gameDirectory.c_str(), &startup, &process))
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
