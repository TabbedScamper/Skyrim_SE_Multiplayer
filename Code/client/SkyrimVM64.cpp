#include <TiltedOnlinePCH.h>
#include "TiltedOnlineApp.h"
#include "GameLoopDiagnostic.h"
#include "MainQueue.h"
#include "Utils/MainThread.h"
#include <Misc/GameVM.h>
#include <Services/ObjectService.h>
#include <Services/CharacterService.h>
#include <Services/CorpseRagdollService.h>
#include <Services/CreatorTogether.h>
#include <Services/HarnessService.h>
#include <Services/DoorVoteService.h>
#include <Services/CheckpointSaves.h>
#include <Services/PartyService.h>
#include <Services/CharacterSnapshots.h>
#include <Games/Skyrim/Actor.h>
#include <Systems/InterpolationSystem.h>
#include <World.h>
#include <Services/LobbyService.h>
#include <Services/Generic/BusyLockService.h>
#include <Services/TriggerGate.h>
#include <Services/WeatherService.h>
#include <Services/CalendarService.h>
#include <Services/QuestService.h>
#include <PlayerCharacter.h>
#include <BSGraphics/BSGraphicsRenderer.h>

extern std::unique_ptr<TiltedOnlineApp> g_appInstance;

struct Main;

TP_THIS_FUNCTION(TVMUpdate, int, GameVM, float);
TP_THIS_FUNCTION(TMainLoop, short, Main);
TP_THIS_FUNCTION(TVMDestructor, uintptr_t, void);

static TVMUpdate* VMUpdate = nullptr;
static TMainLoop* MainLoop = nullptr;
static TVMDestructor* VMDestructor = nullptr;

// N90256 surrounds the whole movie loop; N90293 can run as a nested step.
using TBinkPlay = uint64_t(void*, uint64_t, uint64_t, uint64_t, uint64_t,
    uint64_t, uint64_t, uint64_t, uint64_t);
using TBinkStep = uint64_t(void*);
static TBinkPlay* s_realBinkPlay{};
static TBinkStep* s_realBinkStep{};

uint64_t HookBinkPlay(void* aMovie, uint64_t a2, uint64_t a3, uint64_t a4,
    uint64_t a5, uint64_t a6, uint64_t a7, uint64_t a8, uint64_t a9)
{
    MT::BinkScope bink;
    return s_realBinkPlay(aMovie, a2, a3, a4, a5, a6, a7, a8, a9);
}

uint64_t HookBinkStep(void* aMovie)
{
    MT::BinkScope bink;
    return s_realBinkStep(aMovie);
}

int TP_MAKE_THISCALL(HookVMUpdate, GameVM, float a2)
{
    RecordGameVmHookEntry();
    const bool active = apThis->inactive == 0;
    // N39074+0x23 is the job entry, N36564+0x54A is M.19.
    static VersionDbPtr<void> jobVmUpdate(39074);
    static VersionDbPtr<void> mainUpdate(36564);
    const auto caller = reinterpret_cast<uintptr_t>(_ReturnAddress());
    const bool appEntry = caller == reinterpret_cast<uintptr_t>(jobVmUpdate.Get()) + 0x23 ||
        caller == reinterpret_cast<uintptr_t>(mainUpdate.Get()) + 0x54A;
    if (active && !appEntry)
        MT::AppLaneNested.fetch_add(1, std::memory_order_relaxed);
    const auto entry = std::chrono::steady_clock::now();
    if (active && appEntry)
    {
        MT::Scope appScope(MT::Ctx::AppLane);
        g_appInstance->Update();
    }
    const auto afterApp = std::chrono::steady_clock::now();
    const auto result = TiltedPhoques::ThisCall(VMUpdate, apThis, a2);
    const auto afterOriginal = std::chrono::steady_clock::now();
    const auto appUs = std::chrono::duration_cast<std::chrono::microseconds>(
        afterApp - entry).count();
    const auto originalUs = std::chrono::duration_cast<std::chrono::microseconds>(
        afterOriginal - afterApp).count();
    RecordGameVmHookCall(active && appEntry, static_cast<uint32_t>((std::min)(
        int64_t{UINT32_MAX}, appUs)), static_cast<uint32_t>((std::min)(
        int64_t{UINT32_MAX}, originalUs)));
    return result;
}

// X.00 slots are kept separate for each main-frame owner.
static void Slot0aBegin() noexcept
{
    MT::BeginX00();
    // Construct the bounded lanes before their first producer is installed.
    static_cast<void>(MainQueue::Get());
    CheckpointSaves::MarkMainThread();
}
static void Slot0bWindow() noexcept { BSGraphics::DrainDeferredWindowWorkOnMainFrame(); }
static void Slot0cTitlePump() noexcept {}
static void Slot0dOverlayPoll() noexcept {}
static void Slot1Clock() noexcept { CalendarService::OnMainFrame(); }
static void Slot2Authority() noexcept
{
    if (!MT::CutM6bEnabled() || !World::Exists())
        return;
    static int s_lastShared = -1;
    const bool shared = LobbyService::SharedWorld();
    if (!shared)
        MT::SharedWorldClosedFrames.fetch_add(1, std::memory_order_relaxed);
    if (s_lastShared != static_cast<int>(shared))
    {
        s_lastShared = shared;
        spdlog::info("shared_world={} world_generation={} held_looks={} held_spawns={} held_assignments={}",
            shared, MT::WorldGeneration.load(), MT::HeldLooks.load(), MT::HeldSpawns.load(),
            MT::HeldAssignments.load());
    }
}
static void Slot3_0PreUpdate() noexcept {}
static void Slot3_1Runner() noexcept {}
static void Slot3_2ExistingPumps() noexcept
{
    BusyLockService::OnMainFrame();
    TriggerGate::OnMainFrame();
    WeatherService::OnMainFrame();
    if (!MT::CutM6bEnabled() || LobbyService::SharedWorld())
        CharacterSnapshots::OnMainFrame();
    if (World::Exists() && MT::CutM6bEnabled())
        World::Get().GetCharacterService().DrainWorldHoldOnMainFrame();
    CharacterService::ApplyMovesOnMainFrame();
    DoorVoteService::OnMainFrame();
    DrainReplicaDoorCleanupOnMainFrame();
    DrainWorldOpenChecksOnMainFrame();
    if (!MT::CutM6bEnabled() || LobbyService::SharedWorld())
        ObjectService::OnMainFrame();
    CorpseRagdollService::OnMainFrame();
    if (!MT::CutM6bEnabled() || LobbyService::SharedWorld())
        CreatorTogether::OnMainFrame();
    Actor::FlushPendingReset3D();
    InterpolationSystem::OnMainFrame();
    LobbyService::OnMainFrame();
}
static void Slot3_3MigratedUpdates() noexcept
{
    if (!MT::Loading() && MT::CurrentLoadPhase.load(std::memory_order_acquire) == MT::LoadPhase::Idle)
        LobbyService::RunWorldContextStepOnMainFrame();
    if (MT::CutEnabled(6) && !MT::Loading() &&
        MT::CurrentLoadPhase.load(std::memory_order_acquire) == MT::LoadPhase::Idle)
    {
        if (World::Exists())
            World::Get().GetPartyService().DrainRestartFadeOnMainFrame();
        CheckpointSaves::DrainRespawnOnMainFrame();
    }
}
static void Slot4Script() noexcept {}
static void Slot4_5ExistingWorld() noexcept { World::Get().UpdateMainFrame(); }
static void Slot5Quest() noexcept { World::Get().GetQuestService().OnMainFrame(); }
static void Slot5sSave() noexcept
{
    if (MT::CutEnabled(6) && !MT::Loading() &&
        MT::CurrentLoadPhase.load(std::memory_order_acquire) == MT::LoadPhase::Idle)
        CheckpointSaves::DrainSaveWorkOnMainFrame();
}
static void Slot6TimeSkip() noexcept {}
static bool Slot7Terminal() noexcept
{
    const auto phase = MT::CurrentLoadPhase.load(std::memory_order_acquire);
    if (!MT::CutEnabled(6) || MT::Loading() ||
        (phase != MT::LoadPhase::Idle && phase != MT::LoadPhase::Settled))
        return false;
    if (CheckpointSaves::HasPendingQuit())
    {
        CheckpointSaves::DrainTerminalOnMainFrame();
        return true;
    }
    if (LobbyService::DrainTerminalQuitOnMainFrame())
        return true;
    if (World::Exists() && World::Get().GetPartyService().DrainPendingCheckpointLaunch())
        return true;
    if (CheckpointSaves::HasPendingTerminal())
    {
        CheckpointSaves::DrainTerminalOnMainFrame();
        return true;
    }
    return DoorVoteService::DrainTestCellOnMainFrame();
}
static void SlotX01Publish() noexcept {}
static void SlotX02PostFrame() noexcept
{
    ObjectService::OnMainFrameEnd();
    HarnessService::MainThreadUpdate();
}

template<class Fn>
static decltype(auto) RunSlot(MT::Slot aSlot, Fn&& aFunction) noexcept
{
    MT::SlotScope scope(aSlot);
    return aFunction();
}

short TP_MAKE_THISCALL(HookMainLoop, Main)
{
    RunSlot(MT::Slot::S0a, Slot0aBegin);
    RunSlot(MT::Slot::S0b, Slot0bWindow);
    RunSlot(MT::Slot::S0c, Slot0cTitlePump);
    RunSlot(MT::Slot::S0d, Slot0dOverlayPoll);
    RunSlot(MT::Slot::S1, Slot1Clock);
    RunSlot(MT::Slot::S2, Slot2Authority);
    RunSlot(MT::Slot::S3_0, Slot3_0PreUpdate);
    RunSlot(MT::Slot::S3_1, Slot3_1Runner);
    RunSlot(MT::Slot::S3_2, Slot3_2ExistingPumps);
    RunSlot(MT::Slot::S3_3, Slot3_3MigratedUpdates);
    // Same gate as the VM hook (VM active), plus a player in a cell: these commands never ran at the title screen.
    if (World::Exists() && !MT::Loading() && (!MT::CutM6bEnabled() || LobbyService::SharedWorld()))
    {
        auto* pVM = GameVM::Get();
        auto* pPlayer = PlayerCharacter::Get();
        if (pVM && pVM->inactive == 0 && pPlayer && pPlayer->parentCell)
        {
            RunSlot(MT::Slot::S4, Slot4Script);
            RunSlot(MT::Slot::S4_5, Slot4_5ExistingWorld);
        }
    }
    // Quest applies run outside the world gate: a New Game's first stage (MQ101 10) reaches a follower at the title, before
    // there is a shared world, and the follower's intro waits for it (2026-10-03: inside the gate no New Game ever started on
    // the follower). QuestService::OnMainFrame keeps its own loading and follower checks.
    if (World::Exists())
        RunSlot(MT::Slot::S5, Slot5Quest);
    RunSlot(MT::Slot::S5s, Slot5sSave);
    RunSlot(MT::Slot::S6, Slot6TimeSkip);
    if (!RunSlot(MT::Slot::S7, Slot7Terminal))
        RunSlot(MT::Slot::X01, SlotX01Publish);
    const auto result = TiltedPhoques::ThisCall(MainLoop, apThis);
    RunSlot(MT::Slot::X02, SlotX02PostFrame);
    return result;
}

uintptr_t TP_MAKE_THISCALL(HookVMDestructor, void)
{
    TP_EMPTY_HOOK_PLACEHOLDER

    return TiltedPhoques::ThisCall(VMDestructor, apThis);
}

static TiltedPhoques::Initializer s_mainHooks(
    []()
    {
        POINTER_SKYRIMSE(TMainLoop, cMainLoop, 36564);
        POINTER_SKYRIMSE(TVMUpdate, cVMUpdate, 53926);
        POINTER_SKYRIMSE(TVMDestructor, cVMDestructor, 40412);
        POINTER_SKYRIMSE(TBinkPlay, binkPlay, 90256);
        POINTER_SKYRIMSE(TBinkStep, binkStep, 90293);

        VMUpdate = cVMUpdate.Get();
        MainLoop = cMainLoop.Get();
        VMDestructor = cVMDestructor.Get();
        s_realBinkPlay = binkPlay.Get();
        s_realBinkStep = binkStep.Get();

        TP_HOOK(&VMUpdate, HookVMUpdate);
        TP_HOOK(&MainLoop, HookMainLoop);
        TP_HOOK(&VMDestructor, HookVMDestructor);
        TP_HOOK(&s_realBinkPlay, HookBinkPlay);
        TP_HOOK(&s_realBinkStep, HookBinkStep);
    });
