#include <TiltedOnlinePCH.h>

#include <Services/LobbyService.h>
#include <atomic>

// Achievements with mods (owner 2026-10-02: "can we bypass th achievment message and keep achievements enabled?"). The
// game's "is this game modded" check (441528, 0x1401BFA00: true when the save's player was ever modded, PlayerCharacter
// +0xBED bit 3, or any loaded plugin is not an official one) gates:
//   - the "Mods are currently loaded. Note: Achievements are disabled. Do you wish to continue loading this save?" box
//     in the load handler (442580; sLoadVanillaSaveWithMods), which a Continue from the lobby waited on unseen under
//     the menu's black for 15 s,
//   - the New Game confirmation (52140; sConfirmNewMods),
//   - Papyrus Game.AddAchievement (55450), which grants nothing while it is true,
//   - marking saves as modded.
// Answering "not modded" removes both boxes and keeps achievements working, as SSE Engine Fixes'
// bEnableAchievementsWithMods does with the same function.
namespace
{
using TIsModded = bool(void*, bool);
TIsModded* RealIsModded{};

bool HookIsModded(void*, bool) noexcept
{
    return false;
}

// The Creations load order box (owner 2026-10-02: "got stuck on the load order dialog": "This save uses a different
// Creations Load Order than your current Load Order ... Save / Current"). The load handler (442580, 0x140627DE0) runs
// BGSSaveLoadGame::LoadGame (35600, 0x140618970); when the save's plugins differ from this game's, the load stops and
// the handler asks. Its "Current" answer (LoadGameDifferentLoadOrderCallBack, 0x14062C500, button 0) sets the save
// manager's +0x2B0 and queues the same load again as a confirmed one (4th argument), which then loads with this game's
// load order. A Continue from the lobby waited on that box unseen under the menu's black. Every load goes in that way
// from the start: loaded with the current load order, never asked. (Hiding the open menus from the handler instead
// took its in-game failure path back to the main menu, 2026-10-02 15:53.)
using TLoadHandler = void*(void*, void*, uint32_t, char, uint8_t, uint32_t);
TLoadHandler* RealLoadHandler{};
std::atomic<bool> s_loadGameSucceeded{false};

void* HookLoadHandler(void* apManager, void* apStorage, uint32_t aTask, char aConfirmed, uint8_t aFlag, uint32_t aOther) noexcept
{
    s_loadGameSucceeded = false;
    if (apManager)
        *(reinterpret_cast<uint8_t*>(apManager) + 0x2B0) = 1;
    if (!aConfirmed)
        spdlog::info("Save load: answered \"Current\" in advance (this game's load order, no Save/Current box)");
    // Both flags, as "Current" queues it (FUN_14062B950(storage, task, 1, 1)): confirmed (4th) passes the plugin check,
    // and the 5th skips LoadGame's Creations content check (0x140A69C40). With only the first, LoadGame refused every
    // save from the lobby without a word (2026-10-02 17:43: "LoadGame REFUSED the save (confirmed 1)").
    (void)aFlag;
    auto* pResult = RealLoadHandler(apManager, apStorage, aTask, 1, 1, aOther);
    LobbyService::OnSaveLoadHandled(s_loadGameSucceeded.load()); // the load's actual result, not only its handler return
    return pResult;
}

// Diagnostic: what BGSSaveLoadGame::LoadGame (35600) answers inside the handler (a false is a load that did not happen).
using TLoadGame = bool(void*, void*, char, uint64_t, void*, uint64_t);
TLoadGame* RealLoadGame{};

bool HookLoadGame(void* apGame, void* apStorage, char aConfirmed, uint64_t aArg4, void* apArg5, uint64_t aArg6) noexcept
{
    const bool loaded = RealLoadGame(apGame, apStorage, aConfirmed, aArg4, apArg5, aArg6);
    s_loadGameSucceeded = loaded;
    spdlog::info("Save load: LoadGame {} (confirmed {})", loaded ? "loaded the save" : "REFUSED the save", static_cast<int>(aConfirmed));
    return loaded;
}

TiltedPhoques::Initializer s_achievementsWithMods(
    []()
    {
        POINTER_SKYRIMSE(TIsModded, s_isModded, 441528);
        RealIsModded = s_isModded.Get();
        TP_HOOK(&RealIsModded, HookIsModded);
        POINTER_SKYRIMSE(TLoadHandler, s_loadHandler, 442580);
        RealLoadHandler = s_loadHandler.Get();
        TP_HOOK(&RealLoadHandler, HookLoadHandler);
        POINTER_SKYRIMSE(TLoadGame, s_loadGame, 35600);
        RealLoadGame = s_loadGame.Get();
        TP_HOOK(&RealLoadGame, HookLoadGame);
    });
} // namespace
