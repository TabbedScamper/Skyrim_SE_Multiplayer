#pragma once

#include <Events/UpdateEvent.h>

struct World;

struct SteamLobbyService
{
    SteamLobbyService(World& aWorld, entt::dispatcher& aDispatcher) noexcept;
    ~SteamLobbyService() noexcept;

    TP_NOCOPYMOVE(SteamLobbyService);

    void HostSession() noexcept;
    void JoinSession(const String& acLobbyId) noexcept;
    void LeaveSession() noexcept;

private:
    enum class PendingOperation
    {
        None,
        Create,
        Join
    };

    void OnUpdate(const UpdateEvent&) noexcept;
    bool Initialize() noexcept;
    bool StartLocalServer() noexcept;
    String GetLanEndpoint() const noexcept;
    void CompleteCreate() noexcept;
    void CompleteJoin() noexcept;
    void ShowMessage(const String& acMessage) const noexcept;

    World& m_world;
    entt::scoped_connection m_updateConnection;
    HMODULE m_steamModule{};
    void* m_matchmaking{};
    void* m_utils{};
    uint64_t m_apiCall{};
    uint64_t m_lobbyId{};
    PendingOperation m_pending{PendingOperation::None};
    HANDLE m_serverProcess{};
    HANDLE m_serverThread{};
    HANDLE m_serverJob{};
};
