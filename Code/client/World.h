#pragma once

#include <Services/RunnerService.h>
#include <Services/TransportService.h>
#include <Services/PartyService.h>
#include <Services/ReviveService.h>
#include <Services/CharacterService.h>
#include <Services/OverlayService.h>
#include <Services/CharacterService.h>
#include <Services/MagicService.h>
#include <Services/DebugService.h>
#include <Services/SteamLobbyService.h>
#include <Services/GameSettingsService.h>
#include <Services/GameTestService.h>

#include <Systems/ModSystem.h>

#include <Structs/ServerSettings.h>

class QuestService;
struct QuestItemService;
struct DoorVoteService;
struct BusyLockService;
struct SharedDropService;
struct SceneTurnsService;
struct HeadTrackService;
struct DialogueListenService;
struct StealthService;
struct NakedNpcGuard;
struct UnstuckReset;

struct World : entt::registry
{
    World();
    ~World();

    void Update() noexcept;

    RunnerService& GetRunner() noexcept;
    TransportService& GetTransport() noexcept;
    ModSystem& GetModSystem() noexcept;

    PartyService& GetPartyService() noexcept { return ctx().at<PartyService>(); }
    const PartyService& GetPartyService() const noexcept { return ctx().at<const PartyService>(); }
    CharacterService& GetCharacterService() noexcept { return ctx().at<CharacterService>(); }
    const CharacterService& GetCharacterService() const noexcept { return ctx().at<const CharacterService>(); }
    OverlayService& GetOverlayService() noexcept { return ctx().at<OverlayService>(); }
    const OverlayService& GetOverlayService() const noexcept { return ctx().at<const OverlayService>(); }
    DebugService& GetDebugService() noexcept { return ctx().at<DebugService>(); }
    const DebugService& GetDebugService() const noexcept { return ctx().at<const DebugService>(); }
    MagicService& GetMagicService() noexcept { return ctx().at<MagicService>(); }
    const MagicService& GetMagicService() const noexcept { return ctx().at<const MagicService>(); }
    SteamLobbyService& GetSteamLobbyService() noexcept { return ctx().at<SteamLobbyService>(); }
    GameSettingsService& GetGameSettingsService() noexcept { return ctx().at<GameSettingsService>(); }
    GameTestService& GetGameTestService() noexcept { return ctx().at<GameTestService>(); }
    QuestService& GetQuestService() noexcept;
    DoorVoteService& GetDoorVoteService() noexcept;
    BusyLockService& GetBusyLockService() noexcept;
    SharedDropService& GetSharedDropService() noexcept;
    SceneTurnsService& GetSceneTurnsService() noexcept;
    HeadTrackService& GetHeadTrackService() noexcept;
    DialogueListenService& GetDialogueListenService() noexcept;
    NakedNpcGuard& GetNakedNpcGuard() noexcept;
    UnstuckReset& GetUnstuckReset() noexcept;
    ReviveService& GetReviveService() noexcept { return ctx().at<ReviveService>(); }

    auto& GetDispatcher() noexcept { return m_dispatcher; }

    const ServerSettings& GetServerSettings() const noexcept { return m_serverSettings; }
    void SetServerSettings(ServerSettings aServerSettings) noexcept { m_serverSettings = aServerSettings; }

    [[nodiscard]] uint64_t GetTick() const noexcept;

    static void Create() noexcept;
    [[nodiscard]] static World& Get() noexcept;

private:
    entt::dispatcher m_dispatcher;
    RunnerService m_runner;
    TransportService m_transport;
    ModSystem m_modSystem;
    ServerSettings m_serverSettings{};

    std::chrono::high_resolution_clock::time_point m_lastFrameTime;
};
