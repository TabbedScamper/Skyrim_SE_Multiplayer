#include <TiltedOnlinePCH.h>

#include "World.h"
#include "GameLoopDiagnostic.h"
#include <Utils.h>

#include <Services/DiscoveryService.h>
#include <Services/InputService.h>
#include <Services/TransportService.h>
#include <Services/RunnerService.h>
#include <Services/ImguiService.h>
#include <Services/PapyrusService.h>
#include <Services/DiscordService.h>
#include <Services/ObjectService.h>
#include <Services/WorldStateService.h>
#include <Services/QuestService.h>
#include <Services/Generic/QuestItemService.h>
#include <Services/ActorValueService.h>
#include <Services/InventoryService.h>
#include <Services/MagicService.h>
#include <Services/CommandService.h>
#include <Services/CalendarService.h>
#include <Services/StringCacheService.h>
#include <Services/PlayerService.h>
#include <Services/CombatService.h>
#include <Services/Generic/StealthService.h>
#include <Services/WeatherService.h>
#include <Services/MapService.h>
#include <Services/SteamLobbyService.h>
#include <Services/GameSettingsService.h>
#include <Services/GameTestService.h>
#include <Services/HarnessService.h>
#include <Services/CameraService.h>
#include <Services/SceneTimelineService.h>
#include <Services/Generic/SceneTurnsService.h>
#include <Services/CorpseRagdollService.h>
#include <Services/DoorVoteService.h>
#include <Services/DropInService.h>
#include <Services/UpdateService.h>
#include <Services/FurnitureGraphLink.h>
#include <Services/Generic/BusyLockService.h>
#include <Services/Generic/SharedDropService.h>
#include <Services/TriggerGate.h>
#include <Services/Generic/UnstuckReset.h>
#include <Services/Generic/HeadTrackService.h>
#include <Services/Generic/DialogueListenService.h>
#include <Services/Generic/NakedNpcGuard.h>
#include <Services/Generic/NpcLootService.h>
#include <Services/Generic/PlayerStateTrace.h>
#include <Services/Generic/BoundPoseKeeper.h>
#include <Services/Generic/OrphanTrace.h>

#include <Events/PreUpdateEvent.h>
#include <Events/UpdateEvent.h>

#include <ModCompat/BehaviorVar.h>  

namespace
{
std::atomic<uint64_t> s_vmHookCalls{};
std::atomic<uint64_t> s_vmActiveCalls{};
std::atomic<uint64_t> s_vmInactiveCalls{};
std::atomic<uint64_t> s_vmAppTotalUs{};
std::atomic<uint64_t> s_vmOriginalTotalUs{};
std::atomic<uint32_t> s_vmLastAppUs{};
std::atomic<uint32_t> s_vmLastOriginalUs{};
std::atomic<uint64_t> s_vmLastEntryNs{};
std::atomic<uint32_t> s_vmLastEntryGapUs{};
std::atomic<uint32_t> s_vmMaxEntryGapUs{};
std::atomic<uint32_t> s_vmMaxAppUs{};
std::atomic<uint32_t> s_vmMaxOriginalUs{};
std::atomic<uint64_t> s_worldCalls{};
std::atomic<uint64_t> s_worldPreUpdateTotalUs{};
std::atomic<uint64_t> s_worldRunnerTotalUs{};
std::atomic<uint64_t> s_worldDispatcherTotalUs{};
std::atomic<uint64_t> s_worldGameTestTotalUs{};
std::atomic<uint32_t> s_worldLastGameTestUs{};
std::atomic<uint32_t> s_worldMaxGameTestUs{};
std::atomic<uint64_t> s_worldLastEntryNs{};
std::atomic<uint32_t> s_worldLastEntryGapUs{};
std::atomic<uint32_t> s_worldMaxDispatcherUs{};
std::atomic<uint32_t> s_worldMaxEntryGapUs{};
std::atomic<uint64_t> s_worldGapsOver50Ms{};
std::atomic<uint64_t> s_worldGapsOver100Ms{};
std::atomic<uint64_t> s_worldGapsOver250Ms{};

uint32_t DurationUs(std::chrono::steady_clock::time_point aStart,
    std::chrono::steady_clock::time_point aEnd) noexcept
{
    return static_cast<uint32_t>((std::min)(int64_t{UINT32_MAX},
        std::chrono::duration_cast<std::chrono::microseconds>(
            aEnd - aStart).count()));
}

void RecordMaximum(std::atomic<uint32_t>& aMaximum, uint32_t aValue) noexcept
{
    auto previous = aMaximum.load(std::memory_order_relaxed);
    while (aValue > previous && !aMaximum.compare_exchange_weak(previous,
        aValue, std::memory_order_relaxed)) {}
}
}

void RecordGameVmHookEntry() noexcept
{
    const auto entry = std::chrono::steady_clock::now();
    const auto entryNs = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            entry.time_since_epoch()).count());
    const auto prior = s_vmLastEntryNs.exchange(entryNs, std::memory_order_relaxed);
    if (prior && entryNs >= prior)
    {
        const auto gapUs = static_cast<uint32_t>((std::min)(uint64_t{UINT32_MAX},
            (entryNs - prior) / 1000));
        s_vmLastEntryGapUs.store(gapUs, std::memory_order_relaxed);
        RecordMaximum(s_vmMaxEntryGapUs, gapUs);
    }
}

void RecordGameVmHookCall(bool aActive, uint32_t aAppDurationUs,
    uint32_t aOriginalDurationUs) noexcept
{
    s_vmHookCalls.fetch_add(1, std::memory_order_relaxed);
    (aActive ? s_vmActiveCalls : s_vmInactiveCalls).fetch_add(1,
        std::memory_order_relaxed);
    s_vmAppTotalUs.fetch_add(aAppDurationUs, std::memory_order_relaxed);
    s_vmOriginalTotalUs.fetch_add(aOriginalDurationUs,
        std::memory_order_relaxed);
    s_vmLastAppUs.store(aAppDurationUs, std::memory_order_relaxed);
    s_vmLastOriginalUs.store(aOriginalDurationUs, std::memory_order_relaxed);
    RecordMaximum(s_vmMaxAppUs, aAppDurationUs);
    RecordMaximum(s_vmMaxOriginalUs, aOriginalDurationUs);
}

GameLoopDiagnostic GetGameLoopDiagnostic() noexcept
{
    return {s_vmHookCalls.load(std::memory_order_relaxed),
        s_vmActiveCalls.load(std::memory_order_relaxed),
        s_vmInactiveCalls.load(std::memory_order_relaxed),
        s_vmAppTotalUs.load(std::memory_order_relaxed),
        s_vmOriginalTotalUs.load(std::memory_order_relaxed),
        s_vmLastAppUs.load(std::memory_order_relaxed),
        s_vmLastOriginalUs.load(std::memory_order_relaxed),
        s_vmLastEntryGapUs.load(std::memory_order_relaxed),
        s_vmMaxEntryGapUs.load(std::memory_order_relaxed),
        s_vmMaxAppUs.load(std::memory_order_relaxed),
        s_vmMaxOriginalUs.load(std::memory_order_relaxed),
        s_worldCalls.load(std::memory_order_relaxed),
        s_worldPreUpdateTotalUs.load(std::memory_order_relaxed),
        s_worldRunnerTotalUs.load(std::memory_order_relaxed),
        s_worldDispatcherTotalUs.load(std::memory_order_relaxed),
        s_worldGameTestTotalUs.load(std::memory_order_relaxed),
        s_worldLastGameTestUs.load(std::memory_order_relaxed),
        s_worldMaxGameTestUs.load(std::memory_order_relaxed),
        s_worldLastEntryGapUs.load(std::memory_order_relaxed),
        s_worldMaxDispatcherUs.load(std::memory_order_relaxed),
        s_worldMaxEntryGapUs.load(std::memory_order_relaxed),
        s_worldGapsOver50Ms.load(std::memory_order_relaxed),
        s_worldGapsOver100Ms.load(std::memory_order_relaxed),
        s_worldGapsOver250Ms.load(std::memory_order_relaxed)};
}

World::World()
    : m_runner(m_dispatcher)
    , m_transport(*this, m_dispatcher)
    , m_modSystem(m_dispatcher)
    , m_lastFrameTime{std::chrono::high_resolution_clock::now()}
{
    Utils::InitializeEntityIndex(*this);
    ctx().emplace<ImguiService>();
    ctx().emplace<HeadTrackService>(*this, m_dispatcher);
    ctx().emplace<DiscoveryService>(*this, m_dispatcher);
    ctx().emplace<OverlayService>(*this, m_transport, m_dispatcher);
    ctx().emplace<InputService>(ctx().at<OverlayService>());
    ctx().emplace<CharacterService>(*this, m_dispatcher, m_transport);
    ctx().emplace<DebugService>(m_dispatcher, *this, m_transport, ctx().at<ImguiService>());
    ctx().emplace<PapyrusService>(m_dispatcher);
    ctx().emplace<DiscordService>(m_dispatcher);
    ctx().emplace<ObjectService>(*this, m_dispatcher, m_transport);
    ctx().emplace<WorldStateService>(*this, m_dispatcher, m_transport);
    ctx().emplace<CalendarService>(*this, m_dispatcher, m_transport);
    ctx().emplace<QuestService>(*this, m_dispatcher);
    ctx().emplace<QuestItemService>(*this, m_dispatcher, m_transport);
    ctx().emplace<PartyService>(*this, m_dispatcher, m_transport);
    ctx().emplace<PlayerStateTrace>(*this, m_dispatcher);
    ctx().emplace<BoundPoseKeeper>(m_dispatcher);
    ctx().emplace<OrphanTrace>(*this, m_dispatcher);
    ctx().emplace<DialogueListenService>(*this, m_dispatcher, m_transport);
    ctx().emplace<DoorVoteService>(*this, m_dispatcher, m_transport);
    ctx().emplace<DropInService>(*this, m_dispatcher, m_transport);
    ctx().emplace<UpdateService>(*this, m_dispatcher);
    ctx().emplace<FurnitureGraphLink>(*this, m_dispatcher);
    ctx().emplace<BusyLockService>(*this, m_dispatcher, m_transport);
    ctx().emplace<SharedDropService>(*this, m_dispatcher, m_transport);
    ctx().emplace<TriggerGate>(*this, m_dispatcher, m_transport);
    ctx().emplace<CameraService>(*this, m_dispatcher, m_transport);
    ctx().emplace<UnstuckReset>(*this, m_dispatcher);
    ctx().emplace<SceneTimelineService>(*this, m_dispatcher, m_transport);
    ctx().emplace<SceneTurnsService>(*this, m_dispatcher, m_transport);
    ctx().emplace<CorpseRagdollService>(*this, m_dispatcher, m_transport);
    ctx().emplace<ActorValueService>(*this, m_dispatcher, m_transport);
    ctx().emplace<InventoryService>(*this, m_dispatcher, m_transport);
    ctx().emplace<NakedNpcGuard>(*this, m_dispatcher);
    ctx().emplace<NpcLootService>(*this, m_dispatcher);
    ctx().emplace<MagicService>(*this, m_dispatcher, m_transport);
    ctx().emplace<CommandService>(*this, m_transport, m_dispatcher);
    ctx().emplace<PlayerService>(*this, m_dispatcher, m_transport);
    ctx().emplace<ReviveService>(*this, m_dispatcher, m_transport);
    ctx().emplace<StringCacheService>(m_dispatcher);
    ctx().emplace<CombatService>(*this, m_transport, m_dispatcher);
    ctx().emplace<StealthService>(*this, m_dispatcher, m_transport);
    ctx().emplace<WeatherService>(*this, m_transport, m_dispatcher);
    ctx().emplace<MapService>(*this, m_dispatcher, m_transport);
    ctx().emplace<SteamLobbyService>(*this, m_dispatcher);
    ctx().emplace<GameSettingsService>(*this, m_dispatcher);
    ctx().emplace<GameTestService>(*this);
    ctx().emplace<HarnessService>(*this);

    BehaviorVar::Get()->Init();
}

World::~World() = default;

void World::Update() noexcept
{
    const Utils::EntityDispatchScope scaleDispatch;
    const auto entry = std::chrono::steady_clock::now();
    const auto entryNs = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            entry.time_since_epoch()).count());
    const auto priorEntryNs = s_worldLastEntryNs.exchange(entryNs,
        std::memory_order_relaxed);
    if (priorEntryNs && entryNs >= priorEntryNs)
    {
        const auto gapUs = static_cast<uint32_t>((std::min)(
            uint64_t{UINT32_MAX}, (entryNs - priorEntryNs) / 1000));
        s_worldLastEntryGapUs.store(gapUs, std::memory_order_relaxed);
        auto previousMax = s_worldMaxEntryGapUs.load(std::memory_order_relaxed);
        while (gapUs > previousMax &&
            !s_worldMaxEntryGapUs.compare_exchange_weak(previousMax, gapUs,
                std::memory_order_relaxed)) {}
        if (gapUs > 50000)
            s_worldGapsOver50Ms.fetch_add(1, std::memory_order_relaxed);
        if (gapUs > 100000)
            s_worldGapsOver100Ms.fetch_add(1, std::memory_order_relaxed);
        if (gapUs > 250000)
            s_worldGapsOver250Ms.fetch_add(1, std::memory_order_relaxed);
    }
    s_worldCalls.fetch_add(1, std::memory_order_relaxed);
    const auto cNow = std::chrono::high_resolution_clock::now();
    const auto cDelta = cNow - m_lastFrameTime;
    m_lastFrameTime = cNow;

    const auto cDeltaSeconds = std::chrono::duration_cast<std::chrono::duration<double>>(cDelta).count();

    m_dispatcher.trigger(PreUpdateEvent(cDeltaSeconds));
    const auto afterPreUpdate = std::chrono::steady_clock::now();

    // Force run this before so we get the tasks scheduled to run
    m_runner.OnUpdate(UpdateEvent(cDeltaSeconds));
    const auto afterRunner = std::chrono::steady_clock::now();
    m_dispatcher.trigger(UpdateEvent(cDeltaSeconds));
    const auto afterDispatcher = std::chrono::steady_clock::now();
    ctx().at<GameTestService>().OnGameThread();
    const auto afterGameTest = std::chrono::steady_clock::now();
    // Long update diagnostic: snapshots are sent from this update, so a slow phase here delays every NPC's stream
    // (periodic ~27 s bursts, 150-220 ms late on the other PC, with this PC's frames smooth).
    {
        const auto preMs = DurationUs(entry, afterPreUpdate) / 1000;
        const auto runnerMs = DurationUs(afterPreUpdate, afterRunner) / 1000;
        const auto dispatchMs = DurationUs(afterRunner, afterDispatcher) / 1000;
        const auto testMs = DurationUs(afterDispatcher, afterGameTest) / 1000;
        static std::atomic<uint32_t> s_longUpdateLogs{};
        if (preMs + runnerMs + dispatchMs + testMs > 80 && s_longUpdateLogs.fetch_add(1, std::memory_order_relaxed) < 400)
            spdlog::warn("Long world update: pre-update {} ms, runner {} ms, dispatch {} ms, test bridge {} ms", preMs, runnerMs,
                dispatchMs, testMs);
    }
    s_worldPreUpdateTotalUs.fetch_add(DurationUs(entry, afterPreUpdate),
        std::memory_order_relaxed);
    s_worldRunnerTotalUs.fetch_add(DurationUs(afterPreUpdate, afterRunner),
        std::memory_order_relaxed);
    const auto dispatcherUs = DurationUs(afterRunner, afterDispatcher);
    s_worldDispatcherTotalUs.fetch_add(dispatcherUs,
        std::memory_order_relaxed);
    auto previousMax = s_worldMaxDispatcherUs.load(std::memory_order_relaxed);
    while (dispatcherUs > previousMax &&
        !s_worldMaxDispatcherUs.compare_exchange_weak(previousMax,
            dispatcherUs, std::memory_order_relaxed)) {}
    const auto gameTestUs = DurationUs(afterDispatcher, afterGameTest);
    s_worldGameTestTotalUs.fetch_add(gameTestUs, std::memory_order_relaxed);
    s_worldLastGameTestUs.store(gameTestUs, std::memory_order_relaxed);
    RecordMaximum(s_worldMaxGameTestUs, gameTestUs);
}

RunnerService& World::GetRunner() noexcept
{
    return m_runner;
}

TransportService& World::GetTransport() noexcept
{
    return m_transport;
}

QuestService& World::GetQuestService() noexcept
{
    return ctx().at<QuestService>();
}

DropInService& World::GetDropInService() noexcept
{
    return ctx().at<DropInService>();
}

UpdateService& World::GetUpdateService() noexcept
{
    return ctx().at<UpdateService>();
}

DoorVoteService& World::GetDoorVoteService() noexcept
{
    return ctx().at<DoorVoteService>();
}

BusyLockService& World::GetBusyLockService() noexcept
{
    return ctx().at<BusyLockService>();
}

SharedDropService& World::GetSharedDropService() noexcept
{
    return ctx().at<SharedDropService>();
}

SceneTurnsService& World::GetSceneTurnsService() noexcept
{
    return ctx().at<SceneTurnsService>();
}

HeadTrackService& World::GetHeadTrackService() noexcept
{
    return ctx().at<HeadTrackService>();
}

DialogueListenService& World::GetDialogueListenService() noexcept
{
    return ctx().at<DialogueListenService>();
}

ModSystem& World::GetModSystem() noexcept
{
    return m_modSystem;
}

UnstuckReset& World::GetUnstuckReset() noexcept
{
    return ctx().at<UnstuckReset>();
}

NakedNpcGuard& World::GetNakedNpcGuard() noexcept
{
    return ctx().at<NakedNpcGuard>();
}

uint64_t World::GetTick() const noexcept
{
    return m_transport.GetClock().GetCurrentTick();
}

void World::Create() noexcept
{
    if (!entt::locator<World>::has_value())
    {
        entt::locator<World>::emplace();
    }
}

World& World::Get() noexcept
{
    return entt::locator<World>::value();
}

NpcLootService& World::GetNpcLootService() noexcept
{
    return ctx().at<NpcLootService>();
}
