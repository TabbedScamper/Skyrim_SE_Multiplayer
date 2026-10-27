#include <Services/LobbyService.h>
#include <Utils/MainThread.h>
#include <Games/Skyrim/AI/Movement/PlayerControls.h>

#include <Camera/PlayerCamera.h>
#include <Camera/TESCameraState.h>
#include <Interface/UI.h>
#include <Services/CharacterSnapshots.h>
#include <Services/SaveLook.h>
#include <Services/InventoryService.h>
#include <Messages/PlayerLevelRequest.h>
#include <Interface/IMenu.h>
#include <Misc/GameVM.h>
#include <PlayerCharacter.h>
#include <Forms/TESObjectCELL.h>
#include <Games/TES.h>
#include <Actor.h>
#include <NetImmerse/NiAVObject.h>
#include <NetImmerse/NiNode.h>
#include <Forms/TESNPC.h>
#include <Games/ActorExtension.h>
#include <Services/ImguiService.h>
#include <Services/OverlayService.h>
#include <Services/GameSettingsService.h>
#include <Games/References.h>
#include <Services/CheckpointSaves.h>
#include <Services/UpdateService.h>
#include <Services/PapyrusService.h>
#include <Utils/MainThread.h>
#include <Games/Skyrim/Interface/MainMenuIntegration.h>
#include <Games/Skyrim/Audio/AudioPreview.h>
#include <OverlayApp.hpp>
#include <World.h>
#include <Components.h>
#include <Services/PartyService.h>
#include <Services/SteamLobbyService.h>
#include <Messages/PartyStartRequest.h>

#include <imgui.h>
#include <thread>

#include <array>
#include <algorithm>
#include <random>
#include <atomic>
#include <cmath>
#include <mutex>
#include <ShlObj.h>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>

namespace
{
// Interesting Locations (DrainvolOFF) campfire in Tamriel cell 0000976F, grid (5, -14): a burning campfire with a tent
// and bedroll (the fire's position from the plugin's own placement). The marks and camera are the owner's editor layout
// (InitSlots, s_pose).
struct Point
{
    float X, Y, Z;
};
constexpr int kGridX = 5, kGridY = -14;
constexpr Point kFire{22405.2f, -56392.4f, 4507.9f};
// Clear night with the aurora (Update.esm SkyrimClearSN_A).
constexpr char kWeather[] = "10E1F0";
constexpr float kHour = 22.f;
constexpr uint32_t kFreeCamera = 3;

enum class Phase
{
    Idle,
    Enter,
    WaitWorld,
    Stage,
    Hold,
};

// Recursive: a lobby step runs engine code (cow's load) that pumps frames, and those frames come back in here on the
// same thread (host crash 2026-10-01 13:44:41: Status and DrawEditor re-locked it and std::mutex threw out of noexcept).
std::recursive_mutex s_lock;
std::atomic_bool s_fullLeaveRequested{false};
Phase s_phase{Phase::Idle};
uint64_t s_since{};
std::string s_status{"idle"};
bool s_menusHidden{};
enum class ReturnReason { None, QuitToMenu, PartyLeft, Disconnected, EngineTitle };
std::atomic<ReturnReason> s_returnRequested{ReturnReason::None};
ReturnReason s_lastReturnReason{ReturnReason::None};
std::atomic<bool> s_returnInProgress{};
std::atomic<bool> s_gameplaySeen{};
bool s_autoDone{};
uint64_t s_autoTitleSince{};
uint64_t s_autoSmokeFadeAt{};

const char* ReturnReasonName(ReturnReason aReason) noexcept
{
    switch (aReason)
    {
    case ReturnReason::QuitToMenu: return "quit_to_menu";
    case ReturnReason::PartyLeft: return "party_left";
    case ReturnReason::Disconnected: return "disconnected";
    case ReturnReason::EngineTitle: return "engine_title";
    default: return "none";
    }
}

uint64_t Now() noexcept
{
    return GetTickCount64();
}

void SetStatus(Phase aPhase, const std::string& acText) noexcept
{
    s_phase = aPhase;
    s_since = Now();
    s_status = acText;
    spdlog::info("Lobby: {}", acText);
}

// The engine's own console script path (as HarnessService), limited to the lobby's commands.
bool RunConsole(const std::string& acCommand) noexcept
{
    static constexpr std::array<std::string_view, 12> kAllowed{"cow ", "tfc", "tm", "fw ", "set gamehour to ",
                                                              "player.setpos ", "player.setangle z ", "load \"", "qqq",
                                                              "set Survival_PlayerHasBeenPrompted to 1",
                                                              "DisablePlayerControls", "EnablePlayerControls"};
    if (acCommand.empty() || acCommand.size() > 128 ||
        std::none_of(kAllowed.begin(), kAllowed.end(), [&](std::string_view p) { return acCommand.starts_with(p); }))
        return false;
    using Ctor = void*(void*);
    using Set = void(void*, const char*);
    using Run = void(void*, void*, int32_t, TESObjectREFR*);
    using Dtor = void(void*);
    POINTER_SKYRIMSE(Ctor, ctor, 441580);
    POINTER_SKYRIMSE(Set, set, 21883);
    POINTER_SKYRIMSE(Run, run, 441582);
    POINTER_SKYRIMSE(Dtor, dtor, 441581);
    alignas(8) std::array<uint8_t, 0x80> script{};
    uint8_t compiler{};
    ctor.Get()(script.data());
    set.Get()(script.data(), acCommand.c_str());
    run.Get()(script.data(), &compiler, 1, nullptr);
    dtor.Get()(script.data());
    return true;
}

// In the campfire's own cell with 3D and no load screen. The title screen's placeholder player already has a cell and
// 3D, so anything weaker passed before the load began and the scene was staged mid-load (host crash 2026-10-01 13:19:54,
// small-block allocator free list corrupted four seconds later).
constexpr uint32_t kCampfireCell = 0x0000976F;
bool PlayerInLobbyCell() noexcept
{
    auto* pPlayer = PlayerCharacter::Get();
    const auto* pUI = UI::Get();
    return pPlayer && pPlayer->parentCell && pPlayer->parentCell->formID == kCampfireCell && pPlayer->GetNiNode() &&
           pUI && !pUI->GetMenuOpen(BSFixedString("Loading Menu")) && !pUI->GetMenuOpen(BSFixedString("Main Menu"));
}

// Saving off while the lobby world exists (Game.SetInChargen, 55576), as the intro does before character creation.
void SetSavingDisabled(bool aDisabled) noexcept
{
    auto* pVM = GameVM::Get() ? GameVM::Get()->virtualMachine : nullptr;
    if (!pVM)
        return;
    using TSetInChargen = void(void*, uint32_t, void*, bool, bool, bool);
    POINTER_SKYRIMSE(TSetInChargen, setInChargen, 55576);
    setInChargen.Get()(pVM, 0, nullptr, aDisabled, aDisabled, false);
}

// Skyrim headings: 0 faces +Y (north), clockwise positive.
float HeadingTo(const Point& acFrom, const Point& acTo) noexcept
{
    return std::atan2(acTo.X - acFrom.X, acTo.Y - acFrom.Y);
}

// The lobby world holds the party and nobody else (owner 2026-10-01: a nearby NPC attacked the lobby player and knocked
// him into bleedout). Every actor other than the player is disabled, rechecked each second as cells stream in; the
// lobby world is never saved, so this never reaches a real game.
bool IsStandIn(const Actor* apActor) noexcept;

uint32_t ClearOtherActors() noexcept
{
    auto* pLists = ProcessLists::Get();
    auto* pPlayer = PlayerCharacter::Get();
    // Only ever around the lobby's own campfire: never in a world a save loaded.
    if (!pLists || !pPlayer || !pPlayer->parentCell || pPlayer->parentCell->formID != kCampfireCell)
        return 0;
    // Only actors loaded near the player (high and middle-high process): the low lists hold every actor in the world
    // (1,641 here), unloaded ones never report disabled, and they cannot reach the camp anyway.
    // Disable takes effect a little later, so an actor is left alone for 3 s after it was disabled; one still (or again)
    // enabled after that is disabled again, as an encounter's script can enable it once more.
    static std::map<uint32_t, uint64_t> s_disabledAt;
    // Other players' copies are party, never cleared: by the copy's own remote-player mark, as a fresh copy has no
    // FormIdComponent until its 3D is in (2026-10-02: Rex's copy was disabled on arrival and never drawn).
    uint32_t cleared = 0;
    for (auto* pHandles : {&pLists->highActorHandleArray, &pLists->middleHighActorHandleArray})
    {
        for (uint32_t i = 0; i < pHandles->length; ++i)
        {
            auto* pActor = Cast<Actor>(TESObjectREFR::GetByHandle((*pHandles)[i]));
            if (!pActor || pActor == pPlayer || IsStandIn(pActor) || pActor->IsDisabled() ||
                (pActor->GetExtension() && pActor->GetExtension()->IsRemotePlayer()))
                continue;
            auto& disabledAt = s_disabledAt[pActor->formID];
            if (disabledAt && Now() - disabledAt < 3000)
                continue;
            disabledAt = Now();
            pActor->Disable();
            ++cleared;
        }
    }
    return cleared;
}

// The lobby's characters (owner 2026-10-01: a CHARACTERS entry that turns the menu into a picker at the fire, always a
// fresh PRISONER, and Continue going straight into the chosen character's save): every character this PC has saves of,
// newest first, then PRISONER (the new game's default Nord, captured on arrival). The one shown is the last chosen
// (Data\SkyrimTogetherReborn\lobby_character.txt), else the newest. The list is read off the main thread at launch.
struct LobbyCharacter
{
    std::string Name, Detail; // as shown (upper case)
    std::string RawName;      // as saved
    std::string Key;          // the playthrough (CheckpointSaves::SavedCharacter::Key)
    uint32_t Id{};            // its ID, as the Load screen's character list entries carry it
    std::filesystem::path Save, Snap;
    bool Prisoner{};
    uint16_t Level{1};
    // No character file: the look read from the save itself (SaveLook), when it could be.
    std::shared_ptr<SaveLook::Parsed> SaveRecord;
};
std::mutex s_charactersLock;
std::vector<LobbyCharacter> s_characters;
std::atomic<bool> s_charactersReady{};
std::atomic<uint64_t> s_characterScanGeneration{};
int s_characterIndex{-1};
CharacterSnapshot s_prisonerLook;
bool s_havePrisonerLook{};
bool s_lookIsPrisoner{true};  // the body currently wears the default Nord's look
bool s_characterPending{};    // arrival: pick and apply once the list is read
bool s_characterApplying{};

std::filesystem::path CharacterChoiceFile() noexcept
{
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    return std::filesystem::path(path).parent_path() / "Data" / "SkyrimTogetherReborn" / "lobby_character.txt";
}

void ScanCharacters() noexcept
{
    const auto generation = ++s_characterScanGeneration;
    s_charactersReady = false;
    std::thread([generation] {
        std::vector<LobbyCharacter> list;
        for (auto& character : CheckpointSaves::SavedCharacters(200))
        {
            const auto upper = [](std::string aText) {
                for (auto& c : aText)
                    c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
                return aText;
            };
            std::string name = upper(character.Name);
            // Skyrim's own spacing for a detail line (as the Load menu and the level-up screen set it).
            auto detail = fmt::format("LEVEL {}", character.Level);
            if (!character.Location.empty())
                detail += "      " + upper(character.Location);
            if (character.Coop)
                detail += "      CO-OP";
            list.push_back({std::move(name), std::move(detail), character.Name, character.Key, character.Id, character.Save, character.Snap, false});
            list.back().Level = static_cast<uint16_t>(std::max<uint32_t>(1, character.Level));
            if (character.Snap.empty() && !character.Save.empty())
            {
                auto record = std::make_shared<SaveLook::Parsed>();
                std::string error;
                if (SaveLook::Parse(character.Save, *record, error))
                    list.back().SaveRecord = std::move(record);
                else
                    spdlog::warn("Lobby: {} has no character file and its save's look was not read ({})", character.Name.c_str(), error);
            }
        }
        list.push_back({"PRISONER", "A FRESH START", {}, "prisoner", 0, {}, {}, true});
        {
            std::lock_guard lock(s_charactersLock);
            if (generation != s_characterScanGeneration.load())
                return;
            s_characters = std::move(list);
        }
        s_charactersReady = true;
    }).detach();
}

void StartCharacterScan() noexcept
{
    static bool s_started{};
    if (s_started)
        return;
    s_started = true;
    ScanCharacters();
}

int InitialCharacterIndex() noexcept;
LobbyCharacter ChosenCharacter() noexcept
{
    // A party can start before the lobby applied its pick (2026-10-03: the harness started New Game Together within
    // seconds of launch and the follower's chosen character read as none): use the saved choice once the list is read.
    if (s_charactersReady)
    {
        int index{};
        {
            std::lock_guard lock(s_charactersLock);
            index = s_characterIndex;
        }
        if (index < 0)
        {
            const int initial = InitialCharacterIndex();
            std::lock_guard lock(s_charactersLock);
            if (s_characterIndex < 0)
                s_characterIndex = initial;
        }
    }
    std::lock_guard lock(s_charactersLock);
    if (s_characterIndex < 0 || s_characterIndex >= static_cast<int>(s_characters.size()))
        return {};
    return s_characters[s_characterIndex];
}

std::string ChosenOwnPlaythroughKey() noexcept
{
    const auto chosen = ChosenCharacter();
    return chosen.Prisoner || chosen.Save.empty() || chosen.Save.stem().string().rfind("SSC_", 0) == 0 ?
        std::string{} : chosen.Key;
}

std::filesystem::path ChosenOwnSnapshotPath() noexcept
{
    const auto chosen = ChosenCharacter();
    return chosen.Prisoner || chosen.Save.empty() || chosen.Save.stem().string().rfind("SSC_", 0) == 0 ?
        std::filesystem::path{} : chosen.Snap;
}

bool ReadChosenCharacterForNewGame(CharacterSnapshot& aOut, bool& aFullState, std::string& aChoice) noexcept
{
    const auto chosen = ChosenCharacter();
    aChoice = chosen.RawName.empty() ? chosen.Name : chosen.RawName;
    aFullState = false;
    if (chosen.Prisoner || chosen.Name.empty() || aChoice.empty())
        return false;
    if (!chosen.Snap.empty() && CharacterSnapshots::ReadFile(chosen.Snap, aOut) &&
        !aOut.AppearanceBuffer.empty() && aOut.Skills.size() == CharacterSnapshot::kSkillCount)
    {
        aOut.Name = aChoice;
        aFullState = true;
        return true;
    }
    if (chosen.SaveRecord)
    {
        std::string error;
        if (SaveLook::BuildLook(*chosen.SaveRecord, aOut, error))
        {
            aOut.Name = aChoice;
            return true;
        }
        spdlog::warn("New Game Together: chosen character {} could not be read: {}", aChoice, error);
    }
    return false;
}

int CharacterCount() noexcept
{
    std::lock_guard lock(s_charactersLock);
    return static_cast<int>(s_characters.size());
}

int InitialCharacterIndex() noexcept
{
    std::string wanted;
    std::ifstream file(CharacterChoiceFile());
    std::getline(file, wanted);
    std::lock_guard lock(s_charactersLock);
    for (size_t i = 0; i < s_characters.size(); ++i)
        if (!wanted.empty() && s_characters[i].Key == wanted)
            return static_cast<int>(i);
    // A choice saved before co-op checkpoints got their own key space ("name:X" then, "ssc:name:X" now).
    for (size_t i = 0; i < s_characters.size(); ++i)
        if (wanted.rfind("name:", 0) == 0 && s_characters[i].Key == "ssc:" + wanted)
            return static_cast<int>(i);
    for (size_t i = 0; i < s_characters.size(); ++i)
        if (!wanted.empty() && s_characters[i].Name == wanted)
            return static_cast<int>(i);
    return s_characters.empty() ? -1 : 0;
}

void SaveCharacterChoice() noexcept
{
    const auto chosen = ChosenCharacter();
    if (chosen.Name.empty())
        return;
    std::ofstream file(CharacterChoiceFile(), std::ios::trunc);
    file << chosen.Key << '\n';
}

// The lobby's display apply of the chosen character; New Game cancels it (2026-10-03: a slow one finished on the host's
// fresh New Game player and made it the saved character).
uint64_t s_lobbyLookOperation{};

// Puts the chosen character's look on the body. True when an apply was queued (poll CharacterSnapshots::ApplyStatus).
bool ApplyChosenLook() noexcept
{
    const auto chosen = ChosenCharacter();
    CharacterSnapshot snapshot;
    if (!chosen.Prisoner && !chosen.Snap.empty() && CharacterSnapshots::ReadFile(chosen.Snap, snapshot))
    {
        spdlog::info("Lobby: showing {} ({})", chosen.Name, chosen.Snap.filename().string());
        s_lobbyLookOperation = CharacterSnapshots::QueueApply(std::move(snapshot));
        s_lookIsPrisoner = false;
        return true;
    }
    // A save made before the mod wrote character files: its look read from the save (race, sex, face; cached by key).
    if (!chosen.Prisoner && chosen.SaveRecord)
    {
        static std::map<std::string, CharacterSnapshot> s_saveLooks;
        auto look = s_saveLooks.find(chosen.Key);
        if (look == s_saveLooks.end())
        {
            CharacterSnapshot built;
            std::string error;
            if (SaveLook::BuildLook(*chosen.SaveRecord, built, error))
                look = s_saveLooks.emplace(chosen.Key, std::move(built)).first;
            else
                spdlog::warn("Lobby: {}'s look from the save not built ({})", chosen.Name, error);
        }
        if (look != s_saveLooks.end())
        {
            spdlog::info("Lobby: showing {} with the look in its save ({})", chosen.Name, chosen.Save.filename().string());
            auto named = look->second;
            named.Name = chosen.RawName; // the character's name as saved (the party and the banner show it)
            named.Level = chosen.Level;  // and its level
            s_lobbyLookOperation = CharacterSnapshots::QueueApplyLook(std::move(named));
            s_lookIsPrisoner = false;
            return true;
        }
    }
    // PRISONER, or a character whose look could not be read: the default Nord.
    spdlog::info("Lobby: showing {} as the default Nord", chosen.Name.empty() ? "nobody" : chosen.Name);
    if (s_lookIsPrisoner || !s_havePrisonerLook)
        return false;
    s_lobbyLookOperation = CharacterSnapshots::QueueApply(s_prisonerLook);
    s_lookIsPrisoner = true;
    return true;
}

// Arrival: remember the default Nord's look, then pick the character once the list is in.
void BeginLobbyCharacter() noexcept
{
    std::string error;
    s_havePrisonerLook = CharacterSnapshots::Capture(s_prisonerLook, error);
    if (!s_havePrisonerLook)
        spdlog::warn("Lobby: the default Nord's look was not captured ({})", error);
    s_lookIsPrisoner = true;
    s_characterPending = true;
    StartCharacterScan();
}

// True while still waiting for the list (at most aGiveUpMs after arrival).
bool PollLobbyCharacter(uint64_t aSinceArrivalMs, uint64_t aGiveUpMs) noexcept
{
    if (!s_characterPending)
        return false;
    if (!s_charactersReady)
    {
        if (aSinceArrivalMs < aGiveUpMs)
            return true;
        spdlog::warn("Lobby: the saved characters were not read in time; showing the default Nord");
        s_characterPending = false;
        return false;
    }
    s_characterPending = false;
    s_characterIndex = InitialCharacterIndex();
    s_characterApplying = ApplyChosenLook();
    return false;
}

// The player fully drawn: entering the free camera from a close third-person camera keeps the camera's fade on the
// player (owner 2026-10-01: "i think my charcater is invisible"; screenshot: stand-ins drawn, the player not).
// Actor.SetAlpha(1, no fade) as a script would.
void ShowPlayer() noexcept
{
    if (auto* pPlayer = PlayerCharacter::Get())
    {
        PAPYRUS_FUNCTION(void, Actor, SetAlpha, float, bool);
        s_pSetAlpha(pPlayer, 1.f, false);
    }
}

// The lobby player stays put: no walking, jumping, fighting, sneaking, activating or view switching (owner
// 2026-10-01: "my space bar causes my player to jump"). ControlMap::ToggleControls (0x140CEFAA0), the call under
// Game.DisablePlayerControls; that native was not registered yet when the lobby staged, and the console form did
// not stop the jump either. Menus and looking stay on. UEFlag bits from CommonLibSSE-NG UserEvents.h.
void SetPlayerControlsLocked(bool aLocked) noexcept
{
    constexpr uint32_t kMovement = 1u << 0, kActivate = 1u << 2, kPOVSwitch = 1u << 5, kFighting = 1u << 6,
                       kSneaking = 1u << 7, kJumping = 1u << 10;
    using TToggle = void(void*, uint32_t, bool, bool);
    POINTER_SKYRIMSE(TToggle, toggleControls, 68545);
    static VersionDbPtr<void*> s_controlMap(400863);
    if (auto* pControlMap = s_controlMap.Get() ? *s_controlMap.Get() : nullptr)
        // storeState true: a lock taken or released without it is copied into the single stored snapshot by the next
        // StoreControls (chair, dialogue, bleedout, crafting, mount) and put back by its LoadStoredControls, so the
        // lobby's lock could come back in the real game (SkyrimAtlas ui-input-and-controls recipe, ToggleControls 68545).
        toggleControls.Get()(pControlMap, kMovement | kActivate | kPOVSwitch | kFighting | kSneaking | kJumping, !aLocked, true);
}

// The lobby's free camera (tfc) pushes the TFC input context (13) on ControlMap's context stack. A lobby that ends with a
// load under way skips its "tfc" toggle-off: the load replaces the camera but leaves the context on top, and in game only
// the buttons TFC mode maps work (owner 2026-10-02: move, look, jump, crouch, POV yes; attack, draw/sheathe, sprint, shout
// no; read live: stack [0, 13]). After that load, pop it with the engine's own ControlMap::PopInputContext (68544,
// 0x140CEF980; stack data +0x108, count +0x118) once the camera is no longer free.
std::atomic<bool> s_popTfcAfterLoad{};

// In a session the mod keeps menus from pausing (UI.cpp kAllowList), so their input also reached the player and camera (owner
// 2026-10-02: scrolling the pause menu zoomed the third-person camera far out). While such a menu is open, PlayerControls
// remapMode (+0x50) is set: PlayerControls::ProcessEvent (42338, 0x1407AD900) dispatches no gameplay input then, while the
// menus keep navigating (Atlas FRAME-17). Cleared only if this set it (the Journal's control remap writes +0x50 too), with
// +0x4F/+0x4E so held buttons release cleanly.
bool s_menuInputBlocked{};
void BlockGameplayInputUnderMenus() noexcept
{
    auto* pUI = UI::Get();
    auto* pControls = PlayerControls::GetInstance();
    if (!pUI || !pControls)
        return;
    bool open = false;
    if (entt::locator<World>::has_value() && World::Get().GetTransport().IsConnected() && !LobbyService::IsHolding())
        open = pUI->HasUnpausedMenuOpen();
    if (open && !s_menuInputBlocked)
    {
        if (!pControls->Data.remapMode)
        {
            pControls->Data.MoveInputVec = {};
            pControls->Data.LookInputVec = {};
            pControls->Data.remapMode = true;
            s_menuInputBlocked = true;
        }
    }
    else if (!open && s_menuInputBlocked)
    {
        s_menuInputBlocked = false;
        if (pControls->Data.remapMode)
        {
            pControls->Data.remapMode = false;
            pControls->MarkInputRelease();
        }
    }
}
void PopStaleFreeCameraContext() noexcept
{
    constexpr uint32_t kTfcContext = 13;
    static VersionDbPtr<void*> s_controlMap(400863);
    auto* pControlMap = s_controlMap.Get() ? static_cast<uint8_t*>(*s_controlMap.Get()) : nullptr;
    if (!pControlMap)
        return;
    using TPop = void(void*, uint32_t);
    POINTER_SKYRIMSE(TPop, popContext, 68544);
    for (int guard = 0; guard < 8; ++guard)
    {
        const auto* pData = *reinterpret_cast<const uint32_t* const*>(pControlMap + 0x108);
        const auto count = *reinterpret_cast<const uint32_t*>(pControlMap + 0x118);
        bool found = false;
        for (uint32_t i = 0; pData && i < count && !found; ++i)
            found = pData[i] == kTfcContext;
        if (!found)
            return;
        popContext.Get()(pControlMap, kTfcContext);
        spdlog::info("Lobby: removed the free camera input context left by the lobby");
    }
}

// The splash (owner 2026-10-01: "the SKYRIM text and smoke effects will stay on screen and only fade out when the menu
// is ready"; no dragon logo; must work on ultrawides): Skyrim's own pieces, which scale like the rest of its UI.
// - the intro's title menu ("TitleSequence Menu", Titles.swf) playing Sequence4, the SKYRIM wordmark of the cart
//   ride: its clip _root.sequence4 (TitleSequenceMenu::ProcessMessage 0x140984F90 plays "_root.<name lowercased>")
//   is stopped once fully in and played again to fade out;
// - the smoke (the "Mist Menu"), with its 3D model hidden at the title and no model on the lobby's load (the load
//   path 13363 takes a skipModel flag; with it the picker gives the Mist Menu no model, Atlas ANSWERS LS);
// - the vanilla title's list, the load screen's text and the load spinner hidden while it shows;
// - the smoke and title menus kept from closing (menu message queue, CreatorTogether's AddMessage hook) until the
//   lobby's menu is ready.
std::atomic<bool> s_splashActive{};
std::atomic<bool> s_noModelLoad{};
bool s_lobbyWorld{};          // the loaded world is the lobby's (its visits never count)
bool s_lobbyWorldReplaced{};  // a load other than the lobby's own started after it
bool s_titleBlackUp{};   // the black clip in the title movie is up (after the lobby's load)
uint64_t s_releaseAt{}; // the splash's release (dip through black) started
bool s_holdBlack{};       // Skyrim's fade held black under the splash while the lobby world is in
uint64_t s_nextBlack{};
uint64_t s_splashStartedAt{};
bool s_splashHeld{};
// New pressed in the lobby: the world reset that a new game needs always ends at the title's Main Menu (SkyrimAtlas MM5),
// which stays hidden like the launch title until the new game's load (owner 2026-10-02: "i hate that we go to the main
// menu before the launch").
bool s_newGameAfterMenu{};
double s_holdFrame{};              // _root.sequence4's frame when held
std::atomic<bool> s_restoreTitle{}; // the load closed the title: put it back on the held frame
bool s_splashWanted{};             // auto start on (no lobby_off.txt): hide the title's dragon from its first frame

// The splash runs at launch unless Data\SkyrimTogetherReborn\lobby_off.txt exists.
bool SplashWanted() noexcept
{
    static const bool s_wanted = [] {
        wchar_t path[MAX_PATH]{};
        GetModuleFileNameW(nullptr, path, MAX_PATH);
        return !std::filesystem::exists(std::filesystem::path(path).parent_path() / "Data" / "SkyrimTogetherReborn" / "lobby_off.txt");
    }();
    return s_wanted;
}

// Fades start once the frames come evenly (owner 2026-10-01: "the fade in was a bit stuttery"): the last frame longer
// than 34 ms, from the main loop.
uint64_t s_lastLongFrameAt{};

void NoteFrame() noexcept
{
    static uint64_t s_lastFrameAt{};
    const auto now = Now();
    if (s_lastFrameAt && now - s_lastFrameAt > 34)
        s_lastLongFrameAt = now;
    s_lastFrameAt = now;
}

bool FramesSteady(uint64_t aForMs) noexcept
{
    return Now() - s_lastLongFrameAt >= aForMs;
}

// The smoke's model (MistMenu::SetModel, 52313): no dragon while the splash is wanted at launch or showing. The Main
// Menu's show sets it after the launch hooks had cleared it (2026-10-01 capture: about 1.5 s of dragon).
using TSetMistModel = void(void*, float, float*, float*, void*, float, float);
TSetMistModel* s_realSetMistModel = nullptr;

void HookSetMistModel(void* apModel, float aScale, float* apA, float* apB, void* apC, float aMin, float aMax)
{
    if (apModel && SplashWanted() && ((s_phase == Phase::Idle && !s_splashHeld) || s_splashActive ||
        s_newGameAfterMenu || s_returnRequested.load() != ReturnReason::None || s_returnInProgress))
        apModel = nullptr;
    s_realSetMistModel(apModel, aScale, apA, apB, apC, aMin, aMax);
}

TiltedPhoques::Initializer s_setMistModelHook([]() {
    POINTER_SKYRIMSE(TSetMistModel, setMistModel, 52313);
    s_realSetMistModel = setMistModel.Get();
    TP_HOOK(&s_realSetMistModel, HookSetMistModel);
});

using TLoadScreenMode = uint64_t(char, int64_t, uint8_t, char);
TLoadScreenMode* s_realLoadScreenMode = nullptr;
std::atomic_bool s_reshowSplashMenus{};

void ShowNativeMenu(const char* apName) noexcept;

uint64_t HookLoadScreenMode(char aA, int64_t aLocation, uint8_t aSkipModel, char aD)
{
    MT::Scope loadScope(MT::Ctx::LoadHook);
    const auto loadBefore = MT::LoadByte();
    // Any load but the lobby's own (Enter, WaitWorld) replaces the lobby's world: a save, a new game, the reset to the
    // title. The lobby's visits stop being ignored once one has.
    if (s_lobbyWorld && s_phase != Phase::Enter && s_phase != Phase::WaitWorld)
        s_lobbyWorldReplaced = true;
    const bool splashLoad = s_noModelLoad.exchange(false);
    if (splashLoad)
        aSkipModel = 1;
    const auto result = s_realLoadScreenMode(aA, aLocation, aSkipModel, aD);
    MT::OnLoadModeReturn(aA != 0, loadBefore, MT::LoadByte(), MT::JlmState());
    // The load closes every menu. Restore them from the next main frame, after its caller has returned.
    if (splashLoad && s_splashActive)
    {
        // Black from the first frame of the world (the title menu takes seconds to be rebuilt after the load).
        s_holdBlack = true;
        s_nextBlack = 0;
        s_reshowSplashMenus.store(true, std::memory_order_release);
        s_restoreTitle = true;
    }
    return result;
}

TiltedPhoques::Initializer s_loadScreenModeHook([]() {
    POINTER_SKYRIMSE(TLoadScreenMode, loadScreenMode, 13363);
    s_realLoadScreenMode = loadScreenMode.Get();
    TP_HOOK(&s_realLoadScreenMode, HookLoadScreenMode);
});

void* MenuMovie(const char* apName) noexcept
{
    auto* pUI = UI::Get();
    auto* pMenu = pUI ? pUI->FindMenuByName(apName) : nullptr;
    return pMenu ? pMenu->uiMovie : nullptr;
}

void SetMovieVisible(const char* apMenu, const char* apPath, bool aVisible) noexcept
{
    auto* pMovie = MenuMovie(apMenu);
    if (!pMovie)
        return;
    struct Value
    {
        void* ObjectInterface{};
        uint32_t Type{3};
        uint32_t Pad{};
        double Number{};
    } value;
    value.Number = aVisible ? 1.0 : 0.0;
    using TSetVariable = bool(void*, const char*, const Value&, uint32_t);
    auto* pVtable = *reinterpret_cast<uintptr_t**>(pMovie);
    reinterpret_cast<TSetVariable*>(pVtable[0x10])(pMovie, apPath, value, 0);
}

void SetMovieNumber(const char* apMenu, const char* apPath, double aNumber) noexcept
{
    auto* pMovie = MenuMovie(apMenu);
    if (!pMovie)
        return;
    struct Value
    {
        void* ObjectInterface{};
        uint32_t Type{3};
        uint32_t Pad{};
        double Number{};
    } value;
    value.Number = aNumber;
    using TSetVariable = bool(void*, const char*, const Value&, uint32_t);
    auto* pVtable = *reinterpret_cast<uintptr_t**>(pMovie);
    reinterpret_cast<TSetVariable*>(pVtable[0x10])(pMovie, apPath, value, 0);
}

// A number written straight into a movie (the menu may not be on the UI's list yet).
void SetClipAlpha(void* apMovie, const char* apPath, double aValue) noexcept
{
    struct Value
    {
        void* ObjectInterface{};
        uint32_t Type{3};
        uint32_t Pad{};
        double Number{};
    } value;
    value.Number = aValue;
    using TSetVariable = bool(void*, const char*, const Value&, uint32_t);
    reinterpret_cast<TSetVariable*>((*reinterpret_cast<uintptr_t**>(apMovie))[0x10])(apMovie, apPath, value, 0);
}

// A black clip over a menu movie's whole stage (far past it on every side, so any aspect ratio), at alpha 0.
void AddBlackClip(void* apMovie, const char* apClip, int aDepth) noexcept
{
    auto* pMovie = apMovie;
    if (!pMovie)
        return;
    using TInvoke = bool(void*, const char*, const char*, ...);
    static VersionDbPtr<void> s_invoke(82664);
    auto* pInvoke = reinterpret_cast<TInvoke*>(s_invoke.GetPtr());
    const std::string clip = std::string("_root.") + apClip;
    pInvoke(pMovie, "_root.createEmptyMovieClip", "%s, %d", apClip, aDepth);
    pInvoke(pMovie, (clip + ".beginFill").c_str(), "%d, %d", 0, 100);
    pInvoke(pMovie, (clip + ".moveTo").c_str(), "%d, %d", -20000, -20000);
    pInvoke(pMovie, (clip + ".lineTo").c_str(), "%d, %d", 20000, -20000);
    pInvoke(pMovie, (clip + ".lineTo").c_str(), "%d, %d", 20000, 20000);
    pInvoke(pMovie, (clip + ".lineTo").c_str(), "%d, %d", -20000, 20000);
    pInvoke(pMovie, (clip + ".lineTo").c_str(), "%d, %d", -20000, -20000);
    pInvoke(pMovie, (clip + ".endFill").c_str(), nullptr);
    SetClipAlpha(pMovie, (clip + "._alpha").c_str(), 0.0);
}

// Leaving the lobby for a load faded the Main Menu to black (FadeWithMenuOut); set while that black must stay up.
uint64_t s_leaveBlackAt{};
void* s_leaveBlackMovie{};
std::atomic<bool> s_saveLoadHandled{};
// Any save load replaces the lobby's world: the lobby ends with it (owner 2026-10-02: a Continue Together through the
// checkpoint path left the lobby holding in the loaded world; it moved the player back to the campfire mark from
// 26,184 units away and re-seated the party there).
std::atomic<bool> s_loadEndsLobby{};

// The title's Main Menu while it is kept hidden (the launch, a lobby New Game's reset): see-through and unusable, so a
// stray click or key cannot press an entry nobody can see (owner 2026-10-02: "make sure when we are hiding menus that
// they cant be spam clicked"). The list holder invisible takes the mouse away (Flash sends no mouse events to invisible
// clips); StartMenu's own ShouldProcessInputs gate stops accept and cancel from the keyboard and pad. Set again every
// frame, as the menu's state animations turn both back on; a new Main Menu instance starts from its defaults.
void LockHiddenTitle(void* apMovie) noexcept
{
    if (!apMovie)
        return;
    struct Value
    {
        void* ObjectInterface{};
        uint32_t Type{3};
        uint32_t Pad{};
        double Number{};
    } zero;
    using TSetVariable = bool(void*, const char*, const Value&, uint32_t);
    auto* pSet = reinterpret_cast<TSetVariable*>((*reinterpret_cast<uintptr_t**>(apMovie))[0x10]);
    if (s_leaveBlackAt)
    {
        // The menu faded to black for this load: the black stays drawn (it is all that hides the bare scene, as the
        // smoke's menu draws the world over every menu below it); only the menu's own content goes.
        if (apMovie != s_leaveBlackMovie)
        {
            AddBlackClip(apMovie, "SSMLeaveBlack", 16300);
            s_leaveBlackMovie = apMovie;
        }
        SetClipAlpha(apMovie, "_root._alpha", 100.0);
    }
    else
        pSet(apMovie, "_root._alpha", zero, 0);
    pSet(apMovie, "_root.MenuHolder._visible", zero, 0);
    pSet(apMovie, "_root.MenuHolder.Menu_mc.ShouldProcessInputs", zero, 0);
}

bool InvokeTitle(const char* apPath) noexcept
{
    auto* pMovie = MenuMovie("TitleSequence Menu");
    if (!pMovie)
        return false;
    using TInvoke = bool(void*, const char*, const char*, ...);
    static VersionDbPtr<void> s_invoke(82664);
    return reinterpret_cast<TInvoke*>(s_invoke.GetPtr())(pMovie, apPath, nullptr);
}

// The smoke without its 3D model (the title's dragon): the call the Loading Menu's picker makes when a load has no
// model (51929's skip branch: 0x140965490, ID 52313, with no model form, scale 1, zero vectors and a +-180 range).
// MistMenu +0xC0 is the model's load request, not the model: walking it as a scene node crashed both PCs
// (2026-10-01 18:59), so it is never touched.
void ClearMistModel() noexcept
{
    auto* pUI = UI::Get();
    if (!pUI || !pUI->FindMenuByName("Mist Menu"))
        return;
    using TSetModel = void(void*, float, float*, float*, void*, float, float);
    POINTER_SKYRIMSE(TSetModel, setModel, 52313);
    float zero[3]{};
    float zero2[3]{};
    setModel.Get()(nullptr, 1.f, zero, zero2, nullptr, -180.f, 180.f);
}

void ShowNativeMenu(const char* apName) noexcept;
void HideNativeMenu(const char* apName) noexcept;

void StartSplash() noexcept
{
    s_splashActive = true;
    s_splashHeld = false;
    s_splashStartedAt = Now();
    SetMovieVisible("Main Menu", "_root.MenuHolder._visible", false);
    ClearMistModel();
    ShowNativeMenu("TitleSequence Menu");
    using TStart = void(BSFixedString*);
    POINTER_SKYRIMSE(TStart, startTitleSequence, 52654);
    BSFixedString name("Sequence4");
    startTitleSequence.Get()(&name);
    spdlog::info("Lobby: splash started");
}

// Once the wordmark is fully in (Sequence4 fades in over about 1.5 s), hold it.
void TickRelease() noexcept;
void AddTitleBlack() noexcept;
double Ramp(uint64_t aElapsed, uint64_t aFrom, uint64_t aTo) noexcept;

double MovieNumber(const char* apMenu, const char* apPath) noexcept
{
    auto* pMovie = MenuMovie(apMenu);
    if (!pMovie)
        return -1.0;
    struct Value
    {
        void* ObjectInterface{};
        uint32_t Type{};
        uint32_t Pad{};
        double Number{};
    } value;
    using TGetVariable = bool(void*, Value*, const char*);
    auto* pVtable = *reinterpret_cast<uintptr_t**>(pMovie);
    if (!reinterpret_cast<TGetVariable*>(pVtable[0x11])(pMovie, &value, apPath))
        return -2.0;
    if ((value.Type & 0x0F) == 3)
        return value.Number;
    if ((value.Type & 0x0F) == 2)
        return *reinterpret_cast<uint8_t*>(&value.Number) ? 1.0 : 0.0;
    return -3.0;
}

// The black clip in the title movie, made if the movie is new, at the given alpha.
void EnsureTitleBlack(double aAlpha) noexcept
{
    if (MovieNumber("TitleSequence Menu", "_root.SSMBlack._alpha") < 0.0)
        AddTitleBlack();
    SetMovieNumber("TitleSequence Menu", "_root.SSMBlack._alpha", aAlpha);
}

// TEMP diagnostics (2026-10-01): what the splash's menus do from launch through the load and the release.
void LogSplashState() noexcept
{
    static uint64_t s_next{};
    static uint64_t s_since{};
    if (!s_since)
        s_since = Now();
    if (Now() < s_next || Now() - s_since > 90000)
        return;
    s_next = Now() + 250;
    const auto* pUI = UI::Get();
    if (!pUI)
        return;
    const auto open = [&](const char* apName) { return pUI->GetMenuOpen(BSFixedString(apName)) ? 1 : 0; };
    std::string stack;
    for (uint32_t i = 0; i < pUI->menuStack.length; ++i)
    {
        auto* pName = const_cast<UI*>(pUI)->LookupMenuNameByInstance(pUI->menuStack.data[i]);
        auto* pMenu = reinterpret_cast<uint8_t*>(pUI->menuStack.data[i]);
        stack += pName && pName->AsAscii() ? pName->AsAscii() : "?";
        stack += fmt::format(":f{:X}:d{}:m{:X},", *reinterpret_cast<uint32_t*>(pMenu + 0x1C), static_cast<int>(*reinterpret_cast<int8_t*>(pMenu + 0x18)),
            reinterpret_cast<uintptr_t>(*reinterpret_cast<void**>(pMenu + 0x10)) & 0xFFFFFF);
    }
    spdlog::info("SPLASHDIAG stack=[{}]", stack);
    spdlog::info("SPLASHDIAG ph={} act={} rel={} blk={} | mist={} title={} fader={} load={} main={} hud={} | msv={} | "
                 "t.root vis={} a={} | s4 f={} vis={} a={} | blk a={} | main.root a={} holder vis={}",
                 static_cast<int>(s_phase), s_splashActive ? 1 : 0, s_releaseAt ? 1 : 0, s_holdBlack ? 1 : 0, open("Mist Menu"),
                 open("TitleSequence Menu"), open("Fader Menu"), open("Loading Menu"), open("Main Menu"), open("HUD Menu"),
                 *(reinterpret_cast<const uint8_t*>(pUI) + 0x1C0), MovieNumber("TitleSequence Menu", "_root._visible"),
                 MovieNumber("TitleSequence Menu", "_root._alpha"), MovieNumber("TitleSequence Menu", "_root.sequence4._currentframe"),
                 MovieNumber("TitleSequence Menu", "_root.sequence4._visible"), MovieNumber("TitleSequence Menu", "_root.sequence4._alpha"),
                 MovieNumber("TitleSequence Menu", "_root.SSMBlack._alpha"), MovieNumber("Main Menu", "_root._alpha"),
                 MovieNumber("Main Menu", "_root.MenuHolder._visible"));
}


void TickSplash() noexcept
{
    LogSplashState();
    TickRelease();
    // The smoke comes up before the title's Main Menu: no dragon from its first frame.
    if (s_splashWanted && !s_splashActive && !s_splashHeld && s_phase == Phase::Idle)
        ClearMistModel();
    // The hidden title stays unusable while it is hidden: the launch until the splash takes over, the splash itself, and a
    // lobby New Game's reset (a new Main Menu instance per show, so nothing needs restoring after).
    if (s_phase == Phase::Idle && (s_splashActive || s_newGameAfterMenu || (s_splashWanted && !s_splashHeld)))
        LockHiddenTitle(MenuMovie("Main Menu")); // the cursor is left alone: its menu may live on into the game
    if (!s_splashActive)
        return;
    // The Main Menu's show puts the dragon back (52316(1)): keep clearing it while the splash comes up.
    if (Now() - s_splashStartedAt < 2000)
        ClearMistModel();
    if (s_phase == Phase::Idle)
        SetMovieVisible("Main Menu", "_root.MenuHolder._visible", false); // the mod's title code shows it once
    if (!s_splashHeld && Now() - s_splashStartedAt > 2600)
    {
        s_splashHeld = InvokeTitle("_root.sequence4.stop");
        if (auto* pMovie = MenuMovie("TitleSequence Menu"))
        {
            struct Value
            {
                void* ObjectInterface{};
                uint32_t Type{};
                uint32_t Pad{};
                double Number{};
            } value;
            using TGetVariable = bool(void*, Value*, const char*);
            auto* pVtable = *reinterpret_cast<uintptr_t**>(pMovie);
            if (reinterpret_cast<TGetVariable*>(pVtable[0x11])(pMovie, &value, "_root.sequence4._currentframe") && (value.Type & 0x0F) == 3)
                s_holdFrame = value.Number;
        }
        spdlog::info("Lobby: splash held at frame {}", s_holdFrame);
    }
    // Black under the smoke and the title from the first frame after the load until the reveal: the fade asked for
    // inside the load call was undone by the load's own clean-up (2026-10-01 dump: Fader Menu not on the stack).
    if (s_holdBlack && Now() >= s_nextBlack)
    {
        s_nextBlack = Now() + 500;
        FadeOutGame(true, true, 0.f, true, 0.f);
    }
    if (s_restoreTitle && s_holdFrame > 0 && MenuMovie("TitleSequence Menu"))
    {
        using TInvoke = bool(void*, const char*, const char*, ...);
        static VersionDbPtr<void> s_invoke(82664);
        if (reinterpret_cast<TInvoke*>(s_invoke.GetPtr())(MenuMovie("TitleSequence Menu"), "_root.sequence4.gotoAndStop", "%d",
                static_cast<int>(s_holdFrame)))
        {
            s_restoreTitle = false;
            // Once the world is in, the smoke's menu draws the game's view over every menu below it (depth 8:
            // the Fader and the title at its own depth 4 vanished at load end, 2026-10-01 diagnostics), so the title
            // sits above it (OnSplashTitleQueued) and carries its own black.
            EnsureTitleBlack(100.0);
            s_titleBlackUp = true;
            const auto* pUI = UI::Get();
            const auto open = [&](const char* apName) { return pUI && pUI->GetMenuOpen(BSFixedString(apName)); };
            spdlog::info("Lobby: splash restored after the load (open: mist={} title={} fader={} loading={})",
                open("Mist Menu"), open("TitleSequence Menu"), open("Fader Menu"), open("Loading Menu"));
        }
    }
}

// The menu is ready: a dip through black (owner 2026-10-01: "it still doesnt fade out smooth"). The smoke is 3D and
// has no fade of its own, and the Fader's black draws under it, so the black is a clip in the title's own movie
// (drawn over the smoke, under the wordmark):
//   0 .. kDipDown        the wordmark fades (its own animation) and the clip rises to black over the smoke;
//   kSwap                under full black: smoke, Fader hold and the title go, the load-screen parts come back;
//   kSwap .. kDipEnd     the clip falls away over the lobby while the Main Menu fades in.
constexpr uint64_t kDipDown = 1500;
constexpr uint64_t kSwap = 2100;
constexpr uint64_t kDipEnd = 3600;

void AddTitleBlack() noexcept
{
    auto* pMovie = MenuMovie("TitleSequence Menu");
    if (!pMovie)
        return;
    using TInvoke = bool(void*, const char*, const char*, ...);
    static VersionDbPtr<void> s_invoke(82664);
    auto* pInvoke = reinterpret_cast<TInvoke*>(s_invoke.GetPtr());
    // Below every authored clip (they start at -16383), so the wordmark stays on top; far past the stage on every
    // side, so ultrawide and tall windows are covered too.
    pInvoke(pMovie, "_root.createEmptyMovieClip", "%s, %d", "SSMBlack", -16384);
    pInvoke(pMovie, "_root.SSMBlack.beginFill", "%d, %d", 0, 100);
    pInvoke(pMovie, "_root.SSMBlack.moveTo", "%d, %d", -20000, -20000);
    pInvoke(pMovie, "_root.SSMBlack.lineTo", "%d, %d", 20000, -20000);
    pInvoke(pMovie, "_root.SSMBlack.lineTo", "%d, %d", 20000, 20000);
    pInvoke(pMovie, "_root.SSMBlack.lineTo", "%d, %d", -20000, 20000);
    pInvoke(pMovie, "_root.SSMBlack.lineTo", "%d, %d", -20000, -20000);
    pInvoke(pMovie, "_root.SSMBlack.endFill", nullptr);
    SetMovieNumber("TitleSequence Menu", "_root.SSMBlack._alpha", 0.0);
}

// Smoothstep of the time between two marks, 0..100.
double Ramp(uint64_t aElapsed, uint64_t aFrom, uint64_t aTo) noexcept
{
    if (aElapsed <= aFrom)
        return 0.0;
    if (aElapsed >= aTo)
        return 100.0;
    const double t = static_cast<double>(aElapsed - aFrom) / static_cast<double>(aTo - aFrom);
    return 100.0 * t * t * (3.0 - 2.0 * t);
}

void SetWorldSoundsPaused(bool aPaused) noexcept;

void PutSplashAway() noexcept
{
    SetWorldSoundsPaused(false);
    s_splashActive = false;
    s_holdBlack = false;
    FadeOutGame(false, true, 0.f, true, 0.f);
    HideNativeMenu("Mist Menu");
    SetMovieVisible("Loading Menu", "_root.Menu_mc._visible", true);
    SetMovieVisible("LoadWaitSpinner", "_root.Menu_mc._visible", true);
}

void ReleaseSplash() noexcept
{
    if (!s_splashActive || s_releaseAt)
        return;
    s_releaseAt = Now();
    if (!s_titleBlackUp)
        AddTitleBlack();
    InvokeTitle("_root.sequence4.play");
    spdlog::info("Lobby: splash releasing");
}

// Every frame while the splash shows or releases. The Main Menu stays transparent until the dip's second half.
void TickRelease() noexcept
{
    if (!s_releaseAt)
    {
        if (s_splashActive)
        {
            SetMovieNumber("Main Menu", "_root._alpha", 0.0);
            SetMovieNumber("Cursor Menu", "_root._alpha", 0.0); // owner 2026-10-01: the cursor showed over the splash
        }
        return;
    }
    const auto elapsed = Now() - s_releaseAt;
    if (elapsed < kSwap)
    {
        SetMovieNumber("TitleSequence Menu", "_root.SSMBlack._alpha", s_titleBlackUp ? 100.0 : Ramp(elapsed, 0, kDipDown));
        SetMovieNumber("Main Menu", "_root._alpha", 0.0);
        SetMovieNumber("Cursor Menu", "_root._alpha", 0.0);
        return;
    }
    if (s_splashActive)
    {
        PutSplashAway();
        InvokeTitle("_root.sequence4.stop"); // its fade is done; nothing of it shows over the lobby
        SetMovieVisible("TitleSequence Menu", "_root.sequence4._visible", false);
        spdlog::info("Lobby: splash swapped out under black");
    }
    const double reveal = Ramp(elapsed, kSwap, kDipEnd);
    SetMovieNumber("TitleSequence Menu", "_root.SSMBlack._alpha", 100.0 - reveal);
    SetMovieNumber("Main Menu", "_root._alpha", reveal);
    SetMovieNumber("Cursor Menu", "_root._alpha", reveal);
    if (elapsed >= kDipEnd)
    {
        s_releaseAt = 0;
        s_titleBlackUp = false;
        SetMovieNumber("Main Menu", "_root._alpha", 100.0);
        SetMovieNumber("Cursor Menu", "_root._alpha", 100.0);
        HideNativeMenu("TitleSequence Menu");
        spdlog::info("Lobby: splash released");
    }
}

// Immediate end (the lobby ended or a load replaced it): no dip.
void EndSplash() noexcept
{
    const bool releasing = s_releaseAt != 0;
    s_releaseAt = 0;
    s_titleBlackUp = false;
    if (!s_splashActive && !releasing)
        return;
    if (s_splashActive)
        PutSplashAway();
    SetMovieNumber("Main Menu", "_root._alpha", 100.0);
    SetMovieNumber("Cursor Menu", "_root._alpha", 100.0);
    HideNativeMenu("TitleSequence Menu");
    spdlog::info("Lobby: splash ended");
}


// The world's music paused under the lobby's own soundtrack (Atlas ANSWERS 2026-10-01 "Music stack", item 5): the
// AudioCategoryMUS sound category paused as StatsMenu pauses its category (pause ID 67840, unpause 67841, fade in
// ms). The lobby's handles have no category and keep playing; the pause flag is not saved.
void SetWorldMusicPaused(bool aPaused) noexcept
{
    static bool s_paused{};
    if (s_paused == aPaused)
        return;
    auto* pCategory = TESForm::GetById(0x00071E64); // AudioCategoryMUS
    if (!pCategory)
        return;
    void* pInterface = reinterpret_cast<uint8_t*>(pCategory) + 0x30; // BSISoundCategory
    using TPause = void(void**, uint16_t);
    POINTER_SKYRIMSE(TPause, pause, 67840);
    POINTER_SKYRIMSE(TPause, unpause, 67841);
    (aPaused ? pause : unpause).Get()(&pInterface, 1000);
    s_paused = aPaused;
}

// The world's sound effects and voices paused while the splash covers the lobby's load-in (owner 2026-10-01: "I heard
// people attacking me while loading in"): AudioCategorySFX (0x172A1: weapons, impacts, footsteps, UI) and
// AudioCategoryVOCGeneral (0x876BD: combat shouts and all dialogue), by the same pause as the music, faded back in
// with the reveal.
void SetWorldSoundsPaused(bool aPaused) noexcept
{
    static bool s_paused{};
    if (s_paused == aPaused)
        return;
    using TPause = void(void**, uint16_t);
    POINTER_SKYRIMSE(TPause, pause, 67840);
    POINTER_SKYRIMSE(TPause, unpause, 67841);
    for (const uint32_t category : {0x000172A1u, 0x000876BDu})
    {
        auto* pCategory = TESForm::GetById(category);
        if (!pCategory)
            continue;
        void* pInterface = reinterpret_cast<uint8_t*>(pCategory) + 0x30; // BSISoundCategory
        (aPaused ? pause : unpause).Get()(&pInterface, aPaused ? 300 : 1500);
    }
    s_paused = aPaused;
}

// The player's body shown before the free camera starts (Atlas ANSWERS 2026-10-01 "First and third person after
// tfc", item 5): ForceThirdPerson changes the camera state at once but swaps the 3D a camera update later, and the
// free camera never swaps it. Ready when the camera is in its third-person state, that state is not still switching
// (+0xDE), the third-person root is not hidden, and the player's third-person flag (+0xBEB bit 0) is set.
NiAVObject* PlayerRoot(bool aFirstPerson) noexcept
{
    auto* pPlayer = PlayerCharacter::Get();
    if (!pPlayer)
        return nullptr;
    using TGet3D = NiAVObject*(PlayerCharacter*, bool);
    auto** pVtable = *reinterpret_cast<void***>(pPlayer);
    return reinterpret_cast<TGet3D*>(pVtable[0x378 / 8])(pPlayer, aFirstPerson);
}

bool ThirdPersonHidden() noexcept
{
    auto* pRoot = PlayerRoot(false);
    return pRoot && (*reinterpret_cast<uint32_t*>(reinterpret_cast<uint8_t*>(pRoot) + 0xF4) & 1) != 0;
}

// The body's fade (BSFadeNode currentFade, +0x130 of the third-person root, advanced by the culling jobs). The free
// camera entered before it reached 1 left it at 0, so the character showed on some launches and not others
// (2026-10-01: four launches, fade 0 in the two without him; a per-frame write of 1 was undone by the culling).
float PlayerFade() noexcept
{
    auto* pRoot = PlayerRoot(false);
    return pRoot ? *reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(pRoot) + 0x130) : 0.f;
}

// Fading off for the lobby's scene: with the fade switch (0x1420D663C, ID 390865) at 0, BSFadeNode::OnVisible (105485)
// sets every visited node fully drawn at once, as the game does for 20 frames after a teleport (SkyrimAtlas
// render.fade_node). The player stayed faded when the third-person camera met the tree beside his mark (2026-10-01:
// fade stuck at 0.10 in third person). The game turns fading back on by itself once the lobby stops holding it off.
void HoldFadesOff() noexcept
{
    POINTER_SKYRIMSE(uint8_t, s_fadingEnabled, 390865);
    // Both switches: BSFadeNode::OnVisible (105485) tests the second byte, 0x1420D663D, to skip fade work and draw the
    // node; the first, 0x1420D663C, only drives the frame counter that turns both back on (SkyrimAtlas rendering 10).
    // Clearing only the first left fading on: the player's body sometimes sat at fade 0, unseen on its own PC, with
    // a per-frame write of 1 undone by the culling jobs (owner 2026-10-02: "my character is on her screen but not mine").
    s_fadingEnabled.Get()[0] = 0;
    s_fadingEnabled.Get()[1] = 0;
    // A body rebuilt with a new skeleton (a race from a save, 2026-10-02: the Argonian stayed at fade 0.10 behind the
    // tree, then 0 in the free camera, invisible) keeps the fade it had when fading was switched off: drawn in full.
    if (auto* pRoot = PlayerRoot(false))
    {
        auto& fade = *reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(pRoot) + 0x130);
        if (fade < 1.f)
        {
            static uint64_t s_lastLog{};
            if (GetTickCount64() - s_lastLog > 5000)
            {
                s_lastLog = GetTickCount64();
                spdlog::info("Lobby: the player's body was at fade {:.2f}; drawn in full", fade);
            }
            fade = 1.f;
        }
    }
}

bool BodyReadyForFreeCamera() noexcept
{
    auto* pCamera = PlayerCamera::Get();
    auto* pPlayer = PlayerCharacter::Get();
    if (!pCamera || !pPlayer || !PlayerRoot(false) || !PlayerRoot(true))
        return false;
    auto* pBase = reinterpret_cast<uint8_t*>(pCamera);
    auto* pCurrent = *reinterpret_cast<uint8_t**>(pBase + 0x28);
    auto* pThirdPerson = *reinterpret_cast<uint8_t**>(pBase + 0x100);
    if (!pCurrent || pCurrent != pThirdPerson)
        return false;
    if (*(pThirdPerson + 0xDE) != 0)
        return false;
    if (ThirdPersonHidden())
        return false;
    if (PlayerFade() < 0.99f)
        return false;
    return (*(reinterpret_cast<uint8_t*>(pPlayer) + 0xBEB) & 1) != 0;
}


// Third person if the body is hidden (first person or iron sights); never from the free camera itself.
void ShowBodyForFreeCamera() noexcept
{
    auto* pCamera = PlayerCamera::Get();
    if (!pCamera || !pCamera->state || pCamera->state->id == kFreeCamera)
        return;
    const auto id = pCamera->state->id;
    if (ThirdPersonHidden() && (id == 0 || id == 4))
        pCamera->ForceThirdPerson();
}

// God mode for the lobby player (40500, as the harness uses it).
void SetGodMode(bool aEnabled) noexcept
{
    using TSet = void(bool);
    POINTER_SKYRIMSE(TSet, set, 40500);
    set.Get()(aEnabled);
}

// FreeCameraState (CommonLib layout, read back in game): translation at +0x30, rotation (pitch, yaw) at +0x3C.
// The held camera pose: computed from the plugin's placements at first, replaced by a hand-framed one (framing tool).
struct CameraPose
{
    float X, Y, Z, Pitch, Yaw;
};
// The shipped camera of the first campfire (the owner's editor layout, 2026-10-01; pitch + looks down).
CameraPose s_pose{22862.3f, -56449.3f, 4583.2f, 0.0631f, -1.4937f};
bool s_cameraReleased{};

float* FreeCameraFields() noexcept
{
    auto* pCamera = PlayerCamera::Get();
    auto* pState = pCamera ? pCamera->state : nullptr;
    if (!pState || pState->id != kFreeCamera)
        return nullptr;
    return reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(pState) + 0x30); // translation xyz, then pitch, yaw
}

bool HoldCamera() noexcept
{
    auto* pFields = FreeCameraFields();
    if (!pFields)
        return false;
    if (s_cameraReleased)
        return true; // being framed by hand
    pFields[0] = s_pose.X;
    pFields[1] = s_pose.Y;
    pFields[2] = s_pose.Z;
    pFields[3] = s_pose.Pitch;
    pFields[4] = s_pose.Yaw;
    return true;
}

// The party layout: mark 0 is the local player, marks 1-4 are filled by stand-in copies of the player while the
// editor is open. Edited by the ImGui panel (render thread), applied on the main frame.
struct Idle
{
    const char* Name;
    const char* Event;
};
// Animation events of Skyrim.esm idles (each IDLE record's ENAM, read from the installed Skyrim.esm), sent straight
// to the character's graph as posing mods do (Debug.SendAnimationEvent): Actor.PlayIdle returned true for idles
// that never showed and refused the seated ones (owner 2026-10-01: "none of the animations presets are working").
constexpr std::array<Idle, 23> kIdles{{{"Stand", "IdleForceDefaultState"},
                                       {"Warm hands, crouched", "IdleWarmHandsCrouched"},
                                       {"Warm hands, standing", "IdleWarmHandsStanding"},
                                       {"Warm arms", "IdleWarmArms"},
                                       {"Sit cross-legged", "IdleSitCrossLeggedEnter"},
                                       {"Sit on a ledge", "IdleSitLedgeEnter"},
                                       {"Kneel (blessing)", "IdleBlessingKneelEnter"},
                                       {"Meditate", "IdleGreybeardMeditateEnter"},
                                       {"Pray, crouched", "IdlePrayCrouchedEnter"},
                                       {"Pray", "IdlePray"},
                                       {"Read a book", "IdleBook_Read"},
                                       {"Hands behind back", "IdleHandsBehindBack"},
                                       {"Look far", "IdleLookFar"},
                                       {"Nervous", "IdleNervous"},
                                       {"Study", "IdleStudy"},
                                       {"Examine", "IdleExamine"},
                                       {"Wipe brow", "IdleWipeBrow"},
                                       {"Drink", "IdleDrink"},
                                       {"Eat soup", "IdleEatSoup"},
                                       {"Play the lute", "IdleLuteStart"},
                                       {"Laugh", "IdleLaugh"},
                                       {"Applaud", "IdleApplaud2"},
                                       {"Clean sword", "IdleCleanSword"}}};
constexpr char kResetEvent[] = "IdleForceDefaultState";

struct Slot
{
    float X, Y, Z, HeadingDeg;
    int Idle;
};
std::array<Slot, 5> s_slots{};
bool s_slotsReady{};
uint32_t s_dirtyPlace{}, s_dirtyIdle{}; // bit per slot
std::atomic<bool> s_editorOpen{}; // read by InputService without the lock
bool s_saveRequested{};
std::string s_editorMessage;
std::array<GamePtr<Actor>, 5> s_standIns{}; // [0] unused (the player)

int IdleIndex(std::string_view aName) noexcept;

void InitSlots() noexcept
{
    if (s_slotsReady)
        return;
    // The shipped layout of the first campfire: the owner's editor layout (2026-10-01). A lobby_layout.json beside the
    // mod (the editor's Save) overrides it.
    struct DefaultSlot
    {
        float X, Y, Z, HeadingDeg;
        const char* Idle;
    };
    constexpr std::array<DefaultSlot, 5> kDefaultSlots{{
        {22691.3f, -56491.5f, 4508.4f, 32.f, "IdleCleanSword"},
        {22589.0f, -56464.0f, 4551.0f, 13.f, "IdleSitLedgeEnter"},
        {22494.4f, -56380.2f, 4504.0f, 58.f, "IdleForceDefaultState"},
        {22367.4f, -56277.5f, 4508.5f, 164.f, "IdleSitCrossLeggedEnter"},
        {22562.6f, -56301.9f, 4503.6f, 118.f, "IdleForceDefaultState"},
    }};
    for (size_t i = 0; i < s_slots.size(); ++i)
    {
        const auto& slot = kDefaultSlots[i];
        s_slots[i] = {slot.X, slot.Y, slot.Z, slot.HeadingDeg, IdleIndex(slot.Idle)};
    }
    s_slotsReady = true;
}

std::filesystem::path LayoutPath()
{
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    return std::filesystem::path(path).parent_path() / "Data" / "SkyrimTogetherReborn" / "lobby_layout.json";
}

int IdleIndex(std::string_view aName) noexcept
{
    for (size_t i = 0; i < kIdles.size(); ++i)
        if (aName == kIdles[i].Name || aName == kIdles[i].Event) // the file stores the event
            return static_cast<int>(i);
    return 0;
}

// One line per entry, so the file reads back without a JSON library.
bool SaveLayout(std::string& aMessage) noexcept
{
    try
    {
        const auto path = LayoutPath();
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out << "{\n \"location\": \"Interesting Locations campfire, cell 0000976F grid (5,-14)\",\n";
        out << fmt::format(" \"camera\": {{\"x\": {:.1f}, \"y\": {:.1f}, \"z\": {:.1f}, \"pitch\": {:.4f}, \"yaw\": {:.4f}}},\n",
            s_pose.X, s_pose.Y, s_pose.Z, s_pose.Pitch, s_pose.Yaw);
        out << " \"slots\": [\n";
        for (size_t i = 0; i < s_slots.size(); ++i)
        {
            const auto& slot = s_slots[i];
            out << fmt::format("  {{\"slot\": {}, \"x\": {:.1f}, \"y\": {:.1f}, \"z\": {:.1f}, \"heading\": {:.1f}, \"idle\": \"{}\"}}{}\n",
                i, slot.X, slot.Y, slot.Z, slot.HeadingDeg, kIdles[slot.Idle].Event, i + 1 < s_slots.size() ? "," : "");
        }
        out << " ]\n}\n";
        out.close();
        if (!out)
            throw std::runtime_error("write failed");
        aMessage = "saved " + path.string();
        spdlog::info("Lobby: layout {}", aMessage);
        return true;
    }
    catch (const std::exception& e)
    {
        aMessage = std::string("save failed: ") + e.what();
        spdlog::warn("Lobby: layout {}", aMessage);
        return false;
    }
}

void LoadLayout() noexcept
{
    try
    {
        std::ifstream in(LayoutPath(), std::ios::binary);
        if (!in)
            return;
        std::string line;
        int loaded = 0;
        while (std::getline(in, line) && line.size() < 512)
        {
            CameraPose pose{};
            if (sscanf_s(line.c_str(), " \"camera\": {\"x\": %f, \"y\": %f, \"z\": %f, \"pitch\": %f, \"yaw\": %f",
                    &pose.X, &pose.Y, &pose.Z, &pose.Pitch, &pose.Yaw) == 5)
            {
                s_pose = pose;
                ++loaded;
                continue;
            }
            int index{};
            Slot slot{};
            char idle[64]{};
            if (sscanf_s(line.c_str(), " {\"slot\": %d, \"x\": %f, \"y\": %f, \"z\": %f, \"heading\": %f, \"idle\": \"%63[^\"]",
                    &index, &slot.X, &slot.Y, &slot.Z, &slot.HeadingDeg, idle, static_cast<unsigned>(sizeof(idle))) == 6 &&
                index >= 0 && index < static_cast<int>(s_slots.size()))
            {
                slot.Idle = IdleIndex(idle);
                s_slots[index] = slot;
                ++loaded;
            }
        }
        if (loaded)
            spdlog::info("Lobby: loaded the saved layout ({} entries)", loaded);
    }
    catch (...)
    {
    }
}

// Party members at the fire (owner 2026-10-02: "when she joins shes supposed to take the slot of the setup i did in
// debug mode ... sitting down next to me"): every other player's copy takes the next mark, 1 to 4 in player order, in
// that mark's pose, on every PC (each sees itself on mark 0). The lobby holds a seated copy there: its owner's own
// movement and actions (standing on their own mark 0) are not applied (HoldsRemote, read on the systems' threads).
struct Seat
{
    uint32_t FormId{};
    void* Root{};
};
// By mark (0-4). The party leader holds mark 0 on every PC (owner 2026-10-02: "Always the host"; a promotion swaps
// them); this PC's own player sits wherever its place in the party puts it (s_playerMark).
std::array<Seat, 5> s_seats{};
std::array<std::atomic<uint32_t>, 5> s_seated{};
size_t s_playerMark{};
// This player's look goes to the party when it changes here (a character swap) and when someone new sits down: a copy
// is spawned with the look its owner had at connect, which can be from before the lobby put the chosen character on.
bool s_sendLook{};
std::array<uint64_t, 2> s_announceWornAt{};

// A character changing marks, or rebuilt with a new look, was moved and re-posed in view (owner 2026-10-02: "the
// animations are a bit jumpy when changing characters or swapping"): it fades out (or, rebuilt, is hidden at once), is
// placed and posed while unseen, and fades back in once the pose's enter animation has played.
struct MarkTransition
{
    uint64_t PlaceAt{};  // placing waits for the fade-out
    bool Revealing{};    // fade back in after the pose starts
    uint64_t RevealAt{};
    uint64_t DoneAt{};   // when the fade back in started
    size_t Worn{};       // worn items when last counted, and since when unchanged: dressed before it shows
    uint64_t WornSince{};
};
std::array<MarkTransition, 5> s_transitions{};

void SetActorAlpha(Actor* apActor, float aAlpha, bool aFade) noexcept
{
    if (!apActor)
        return;
    static uint32_t s_logged{};
    if (aAlpha < 1.f || s_logged < 40)
    {
        ++s_logged;
        spdlog::info("Lobby: alpha {:.1f} (fade {}) on {:X}{}", aAlpha, aFade, apActor->formID,
            apActor == PlayerCharacter::Get() ? " (this player)" : "");
    }
    PAPYRUS_FUNCTION(void, Actor, SetAlpha, float, bool);
    s_pSetAlpha(apActor, aAlpha, aFade);
}

void BeginMarkTransition(size_t aMark, bool aFadeOut) noexcept;

Actor* SeatedGuest(size_t aSlot) noexcept
{
    if (aSlot >= s_seats.size() || !s_seats[aSlot].FormId)
        return nullptr;
    return Cast<Actor>(TESForm::GetById(s_seats[aSlot].FormId));
}

void ReleaseSeats() noexcept
{
    for (size_t i = 0; i < s_seats.size(); ++i)
    {
        s_seats[i] = {};
        s_seated[i] = 0;
    }
    s_playerMark = 0;
}

Actor* SlotActor(size_t aSlot) noexcept
{
    if (aSlot == s_playerMark)
        return PlayerCharacter::Get();
    if (auto* pGuest = SeatedGuest(aSlot))
        return pGuest;
    return aSlot < s_standIns.size() ? static_cast<Actor*>(s_standIns[aSlot]) : nullptr;
}

bool IsStandIn(const Actor* apActor) noexcept
{
    return std::any_of(s_standIns.begin(), s_standIns.end(), [&](GamePtr<Actor>& p) { return static_cast<Actor*>(p) == apActor; });
}

void PlaceSlot(size_t aSlot) noexcept
{
    auto* pActor = SlotActor(aSlot);
    if (!pActor)
        return;
    const auto& slot = s_slots[aSlot];
    if (pActor == PlayerCharacter::Get())
    {
        RunConsole(fmt::format("player.setpos x {:.1f}", slot.X));
        RunConsole(fmt::format("player.setpos y {:.1f}", slot.Y));
        RunConsole(fmt::format("player.setpos z {:.1f}", slot.Z));
        RunConsole(fmt::format("player.setangle z {:.1f}", slot.HeadingDeg));
        return;
    }
    NiPoint3 position;
    position.x = slot.X;
    position.y = slot.Y;
    position.z = slot.Z;
    pActor->ForcePosition(position);
    pActor->SetRotation(0.f, 0.f, slot.HeadingDeg / 57.29578f);
}

} // namespace
extern thread_local bool g_forceAnimation;
namespace
{

// Back to the default state first, so a looping or seated idle can be switched; then the idle's own event.
void PlaySlotIdle(size_t aSlot) noexcept
{
    auto* pActor = SlotActor(aSlot);
    if (!pActor)
        return;
    const auto& idle = kIdles[s_slots[aSlot].Idle];
    BSFixedString reset(kResetEvent);
    BSFixedString event(idle.Event);
    g_forceAnimation = true;
    const bool resetAccepted = pActor->SendAnimationEvent(&reset);
    const bool accepted = idle.Event == std::string_view(kResetEvent) ? resetAccepted : pActor->SendAnimationEvent(&event);
    g_forceAnimation = false;
    // Heads follow the pose alone: tracking aims at this PC's own camera or player, so the same character looked
    // somewhere else on each screen (owner 2026-10-02: "their look at target doesnt match compared to both").
    PAPYRUS_FUNCTION(void, Actor, SetHeadTracking, bool);
    PAPYRUS_FUNCTION(void, Actor, ClearLookAt);
    s_pSetHeadTracking(pActor, false);
    s_pClearLookAt(pActor);
    spdlog::info("Lobby: slot {} pose '{}' event {} accepted={} (reset accepted={})", aSlot, idle.Name, idle.Event, accepted,
        resetAccepted);
}

void BeginMarkTransition(size_t aMark, bool aFadeOut) noexcept
{
    if (aMark >= s_transitions.size())
        return;
    SetActorAlpha(SlotActor(aMark), 0.f, aFadeOut);
    s_transitions[aMark] = {Now() + (aFadeOut ? 450 : 0), false, 0, 0, 0, 0};
    s_dirtyPlace |= 1u << aMark;
    s_dirtyIdle |= 1u << aMark;
}

// The party's characters seated here, in mark order (the leader's first).
std::vector<uint32_t> PartyViewList() noexcept
{
    std::vector<uint32_t> forms;
    for (size_t mark = 0; mark < s_seats.size(); ++mark)
        if (mark != s_playerMark && s_seats[mark].FormId && s_seats[mark].Root)
            forms.push_back(s_seats[mark].FormId);
    return forms;
}

size_t MarkOf(uint32_t aForm) noexcept
{
    for (size_t mark = 0; mark < s_seats.size(); ++mark)
        if (s_seats[mark].FormId == aForm)
            return mark;
    return s_playerMark;
}

// Seats the party's copies (see Seat) and keeps them on their marks. A copy arrives disabled-proof but without 3D;
// once its body is in it is placed and posed, and again whenever its 3D is rebuilt (a look change respawns parts).
void TickGuests() noexcept
{
    std::vector<std::pair<uint32_t, uint32_t>> guests; // player id, copy
    auto view = World::Get().view<RemoteComponent, PlayerComponent>();
    for (auto entity : view)
    {
        auto* pActor = Cast<Actor>(TESForm::GetById(view.get<RemoteComponent>(entity).CachedRefId));
        if (pActor && pActor->GetExtension() && pActor->GetExtension()->IsRemotePlayer())
            guests.emplace_back(view.get<PlayerComponent>(entity).Id, pActor->formID);
    }
    // Party order (owner 2026-10-02: "filled out in importance order and when one drops they all move up"): the leader
    // first, then the members as they joined (the server's list); anyone not on it after them, by player id.
    const auto& party = World::Get().GetPartyService();
    const auto& members = party.GetPartyMembers();
    const auto rank = [&](uint32_t aPlayerId) -> uint64_t {
        if (aPlayerId == party.GetLeaderPlayerId())
            return 0;
        const auto it = std::find(members.begin(), members.end(), aPlayerId);
        return it != members.end() ? 1 + static_cast<uint64_t>(it - members.begin()) : (uint64_t{1} << 32) + aPlayerId;
    };
    // This PC's own player takes its place in the same order (form 0 marks it).
    const bool inParty = !guests.empty();
    if (inParty)
        guests.emplace_back(World::Get().GetTransport().GetLocalPlayerId(), 0u);
    std::sort(guests.begin(), guests.end(), [&](const auto& a, const auto& b) { return rank(a.first) < rank(b.first); });
    // The marks in importance order: nearest the main character's mark first (the layout's own numbering is the order
    // they were placed in the editor).
    std::array<size_t, 4> order{1, 2, 3, 4};
    const auto distance = [](size_t aMark) {
        const float dx = s_slots[aMark].X - s_slots[0].X, dy = s_slots[aMark].Y - s_slots[0].Y;
        return dx * dx + dy * dy;
    };
    std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) { return distance(a) < distance(b); });
    // First in the party on mark 0, the rest on the marks nearest it; everyone behind a player who left moves up a mark.
    std::array<uint32_t, 5> wanted{};
    size_t playerMark = 0;
    for (size_t k = 0; k < guests.size() && k <= order.size(); ++k)
    {
        const size_t mark = k == 0 ? 0 : order[k - 1];
        if (guests[k].second == 0)
            playerMark = mark;
        else
            wanted[mark] = guests[k].second;
    }
    if (playerMark != s_playerMark)
    {
        spdlog::info("Lobby: this player moves from mark {} to mark {}", s_playerMark, playerMark);
        s_playerMark = playerMark;
        // At once: the party member taking this player's old mark arrives there now (owner 2026-10-02: on the joining
        // PC the host's copy spawned onto her and pushed her back during a fade-out).
        BeginMarkTransition(playerMark, false);
    }
    static uint64_t s_nextDriftCheck{};
    const bool driftCheck = Now() >= s_nextDriftCheck;
    if (driftCheck)
        s_nextDriftCheck = Now() + 1000;
    for (size_t k = 0; k < s_seats.size(); ++k)
    {
        const size_t mark = k;
        auto& seat = s_seats[k];
        const uint32_t form = wanted[k];
        if (seat.FormId != form)
        {
            if (form)
            {
                // The mark's stand-in (layout editor) makes way.
                if (mark < s_standIns.size() && s_standIns[mark])
                {
                    s_standIns[mark]->Delete();
                    s_standIns[mark] = nullptr;
                }
                spdlog::info("Lobby: party member's character {:X} takes mark {}", form, mark);
                s_sendLook = true;
            }
            else
                spdlog::info("Lobby: mark {} is free again", mark);
            seat = {form, nullptr};
            s_seated[k] = form;
            if (form)
            {
                // A copy that just spawned stands where its owner's player stands (often mark 0, on this player): placed
                // at once, unseen, or it shoves whoever is there (owner 2026-10-02: "the host get scooted over when
                // someone joins"). One already shown here moves marks: it fades out first.
                auto* pArrival = Cast<Actor>(TESForm::GetById(form));
                BeginMarkTransition(mark, pArrival && pArrival->GetNiNode() && !(pArrival->GetNiNode()->flags & 1));
            }
        }
        if (!form)
            continue;
        auto* pActor = Cast<Actor>(TESForm::GetById(form));
        if (!pActor)
            continue;
        if (pActor->IsDisabled())
        {
            pActor->Enable();
            continue;
        }
        auto* pRoot = pActor->GetNiNode();
        if (!pRoot)
            continue;
        if (pRoot != seat.Root)
        {
            // First body after taking the mark: its transition is already under way. A later one is a rebuild (a new
            // look): hidden at once, posed, faded back in.
            const bool rebuilt = seat.Root != nullptr;
            seat.Root = pRoot;
            if (rebuilt || !s_transitions[mark].PlaceAt)
                BeginMarkTransition(mark, false);
            continue;
        }
        // Held on the mark: a seated idle keeps it there, a standing one can be nudged by physics.
        if (driftCheck)
        {
            const auto& slot = s_slots[mark];
            const float dx = pActor->position.x - slot.X, dy = pActor->position.y - slot.Y;
            if (dx * dx + dy * dy > 25.f * 25.f)
                s_dirtyPlace |= 1u << mark;
        }
    }
    if (driftCheck)
    {
        // This player too: an arrival or a copy's physics can push it off its mark. Back on it, pose again.
        if (auto* pPlayer = PlayerCharacter::Get(); pPlayer && pPlayer->parentCell && pPlayer->parentCell->formID == kCampfireCell &&
                                                    !s_transitions[s_playerMark].PlaceAt && !s_editorOpen)
        {
            const auto& slot = s_slots[s_playerMark];
            const float dx = pPlayer->position.x - slot.X, dy = pPlayer->position.y - slot.Y;
            if (dx * dx + dy * dy > 25.f * 25.f)
            {
                spdlog::info("Lobby: this player was pushed {:.0f} units off mark {}; put back", std::sqrt(dx * dx + dy * dy), s_playerMark);
                s_dirtyPlace |= 1u << s_playerMark;
                s_dirtyIdle |= 1u << s_playerMark;
            }
        }
        // Whoever is on a mark whose fade finished is drawn in full (owner 2026-10-02: the guest's own character stayed
        // partly see-through after moving to its seat): an interrupted fade-in leaves the alpha short of 1.
        for (size_t mark = 0; mark < s_transitions.size(); ++mark)
        {
            const auto& transition = s_transitions[mark];
            if (transition.PlaceAt || transition.Revealing || Now() - transition.DoneAt < 2000 || !transition.DoneAt)
                continue;
            if (auto* pActor = SlotActor(mark); pActor && pActor->GetNiNode())
                SetActorAlpha(pActor, 1.f, false);
        }
    }
}

void SpawnStandIns() noexcept
{
    auto* pPlayer = PlayerCharacter::Get();
    auto* pBase = pPlayer ? Cast<TESNPC>(pPlayer->baseForm) : nullptr;
    if (!pBase)
        return;
    const auto inventory = pPlayer->GetActorInventory();
    for (size_t i = 1; i < s_standIns.size(); ++i)
    {
        if (s_standIns[i] || SeatedGuest(i) || i == s_playerMark)
            continue;
        s_standIns[i] = Actor::Create(pBase);
        if (!s_standIns[i])
            continue;
        s_standIns[i]->SetActorInventory(inventory);
        s_dirtyPlace |= 1u << i;
        s_dirtyIdle |= 1u << i;
    }
    spdlog::info("Lobby: stand-ins placed on marks 1-4");
}

void RemoveStandIns() noexcept
{
    for (auto& pActor : s_standIns)
    {
        if (pActor)
            pActor->Delete();
        pActor = nullptr;
    }
}

// Presentation: our loading screen over the vanilla one (progress follows the real steps), the title theme instead
// of the world's music, and the lobby menu once the scene is set.
std::atomic<bool> s_menuShown{}; // read by InputService
std::atomic<bool> s_holding{};   // read by the UI menu queue hook
bool s_nativeMenuOpened{};
uint64_t s_primeMenuAt{};
uint64_t s_sizeVignetteUntil{};
// The loading screen stays up until the menu is filled and the vignette fitted, then all of it appears at once
// (owner 2026-10-01: "i also see the vignette pop in").
uint64_t s_revealAt{};
uint64_t s_holdSince{};

void PushPresentation(bool aLoading, float aProgress, bool aMenu) noexcept
{
    // The UI starts on the loading screen (the vanilla title is never seen at launch), so "loading" is the
    // starting state: the first push that ends it (lobby_off.txt, a failure) must not be skipped as unchanged.
    static bool s_loading{true}, s_menu{};
    static float s_progress{0.02f};
    if (aLoading == s_loading && aMenu == s_menu && std::abs(aProgress - s_progress) < 0.01f)
        return;
    s_loading = aLoading;
    s_menu = aMenu;
    s_progress = aProgress;
    World::Get().GetOverlayService().SetLobbyState(aLoading, aProgress, aMenu);
}

// The shuffled soundtrack: advances tracks and shows the playing one's official title.
void KeepMusic() noexcept
{
    static uint64_t s_next{};
    static std::string s_shown;
    if (Now() < s_next)
        return;
    s_next = Now() + 500;
    const auto title = AudioPreview::TickLobbyPlaylist();
    if (title != s_shown)
    {
        s_shown = title;
        World::Get().GetOverlayService().SetLobbyMusic(title);
    }
}

void SetEditorCursor(bool aVisible) noexcept;

// The real Main Menu's New over the lobby (Atlas ANSWERS 2026-10-01 "Main menu actions in a running world"):
// StartNewGame (ID 52118) must never run in a live world. Pressed in the lobby, it is held back and replaced by a reset
// (Game.QuitToMainMenu, ID 55490) and, once the reset is over, the real New Game at the title. The hook only raises a
// flag: the reset is started from the main frame.
using TStartNewGame = void();
TStartNewGame* s_realStartNewGame = nullptr;
std::atomic<bool> s_newGamePressed{};

void HookStartNewGame()
{
    if (LobbyService::IsHolding())
    {
        s_newGamePressed = true;
        return;
    }
    MT::WorldGeneration.fetch_add(1, std::memory_order_acq_rel);
    s_realStartNewGame();
}

TiltedPhoques::Initializer s_startNewGameHook([]() {
    POINTER_SKYRIMSE(TStartNewGame, startNewGame, 52118);
    s_realStartNewGame = startNewGame.Get();
    TP_HOOK(&s_realStartNewGame, HookStartNewGame);
});

using TResetWorld = void(void*);
TResetWorld* s_realResetWorld{};
void HookResetWorld(void* main)
{
    MT::WorldGeneration.fetch_add(1, std::memory_order_acq_rel);
    s_realResetWorld(main);
}
TiltedPhoques::Initializer s_resetWorldHook([]() {
    POINTER_SKYRIMSE(TResetWorld, resetWorld, 36601);
    s_realResetWorld = resetWorld.Get();
    TP_HOOK(&s_realResetWorld, HookResetWorld);
});

// Engine flags that say a reset is over (Atlas MM3/MM6): Main+0x11 (reset pending), Main+0x12 (full reload),
// 0x143230523 (a game started since the last reset, ID 403446) and 0x14219E5F6 (load-screen mode, ID 400336).
uint8_t* MainSingleton() noexcept
{
    static VersionDbPtr<uint8_t*> s_main(403449);
    return s_main.Get() ? *s_main.Get() : nullptr;
}

bool ResetFinished() noexcept
{
    auto* pMain = MainSingleton();
    static VersionDbPtr<uint8_t> s_gameStarted(403446);
    static VersionDbPtr<uint8_t> s_loadScreenMode(400336);
    const auto* pUI = UI::Get();
    return pMain && pMain[0x11] == 0 && pMain[0x12] == 0 && s_gameStarted.Get() && *s_gameStarted.Get() == 0 &&
           s_loadScreenMode.Get() && *s_loadScreenMode.Get() == 0 && pUI &&
           pUI->GetMenuOpen(BSFixedString("Main Menu")) && !pUI->GetMenuOpen(BSFixedString("Loading Menu"));
}

// The real Main Menu's Continue over the lobby (ContinueLastSavedGame 0x1409590F0, ID 52117): safe while the lobby's
// Main Menu instance exists (Atlas MM2), but the lobby must be put away first, or its god mode, saving block and
// control lock would follow the player into the save. The hook raises a flag; the main frame runs the original and
// then ends the lobby. After the save is in, a character who has discovered the lobby's area arrives at the campfire
// (owner 2026-10-01: "continue directly from the main menu ... if the player has discovered that location").
using TContinue = void();
TContinue* s_realContinue = nullptr;
std::atomic<bool> s_continuePressed{};
bool s_continueUnderWay{};

// ContinueLastSavedGame (52117) starts by writing through the live MainMenu instance (406330, 0x143258220) with no null
// check (SkyrimAtlas ANSWERS "Main menu actions in a running world"): never called without one.
bool MainMenuInstanceAlive() noexcept
{
    static VersionDbPtr<void*> s_mainMenu(406330);
    return s_mainMenu.Get() && *s_mainMenu.Get();
}

void HookContinue()
{
    if (LobbyService::IsHolding())
    {
        s_continuePressed = true;
        return;
    }
    if (MainMenuInstanceAlive())
        s_realContinue();
}

TiltedPhoques::Initializer s_continueHook([]() {
    POINTER_SKYRIMSE(TContinue, continueGame, 52117);
    s_realContinue = continueGame.Get();
    TP_HOOK(&s_realContinue, HookContinue);
});

// Lobby spots unlock by being visited in the character's own game (owner 2026-10-01: "the player cant continue from
// the camp unless they have unlocked it by visiting it"). The mod records each visit per character name in
// Data\SkyrimTogetherReborn\lobby_unlocks.txt, one "<location id>\t<character name>" per line. The lobby's own world puts
// the player at the fire too, so it never counts: from the lobby's load until another game has loaded after it.
constexpr const char* kCampfireLocationId = "il-campfire-976F";
constexpr float kVisitRadius = 1500.f;

std::filesystem::path UnlocksPath() noexcept
{
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    return std::filesystem::path(path).parent_path() / "Data" / "SkyrimTogetherReborn" / "lobby_unlocks.txt";
}

std::string PlayerName() noexcept
{
    auto* pPlayer = PlayerCharacter::Get();
    auto* pNpc = pPlayer ? Cast<TESNPC>(pPlayer->baseForm) : nullptr;
    return pNpc && pNpc->fullName.value.data ? std::string(pNpc->fullName.value.AsAscii()) : std::string{};
}

bool HasVisited(const std::string& acLocation, const std::string& acName) noexcept
{
    if (acName.empty())
        return false;
    std::ifstream in(UnlocksPath());
    const auto wanted = acLocation + '\t' + acName;
    for (std::string line; std::getline(in, line);)
        if (line == wanted)
            return true;
    return false;
}

void RecordVisit(const std::string& acLocation, const std::string& acName) noexcept
{
    if (acName.empty() || HasVisited(acLocation, acName))
        return;
    std::ofstream out(UnlocksPath(), std::ios::app);
    out << acLocation << '\t' << acName << '\n';
    spdlog::info("Lobby: {} visited {}: unlocked", acName, acLocation);
}

// Main frame, outside the lobby, once a second: a character standing at a lobby spot unlocks it.
void TrackVisits() noexcept
{
    static uint64_t s_next{};
    if (Now() < s_next)
        return;
    s_next = Now() + 1000;
    if (s_lobbyWorld)
    {
        const auto* pLoadUI = UI::Get();
        auto* pLoaded = PlayerCharacter::Get();
        if (s_phase != Phase::Idle || !s_lobbyWorldReplaced || !pLoadUI || pLoadUI->GetMenuOpen(BSFixedString("Loading Menu")) ||
            !pLoaded || !pLoaded->parentCell)
            return;
        s_lobbyWorld = s_lobbyWorldReplaced = false;
        spdlog::info("Lobby: another game replaced the lobby's world; visits count again");
    }
    const auto* pUI = UI::Get();
    auto* pPlayer = PlayerCharacter::Get();
    if (s_phase != Phase::Idle || !pUI || !pPlayer || !pPlayer->parentCell || pUI->GetMenuOpen(BSFixedString("Loading Menu")) ||
        pUI->GetMenuOpen(BSFixedString("Main Menu")))
        return;
    if (pPlayer->parentCell->formID != kCampfireCell ||
        std::hypot(pPlayer->position.x - kFire.X, pPlayer->position.y - kFire.Y) > kVisitRadius)
        return;
    RecordVisit(kCampfireLocationId, PlayerName());
}

enum class Arrival
{
    None,
    WaitSave,     // the save is loading
    WaitCampfire, // moved; waiting to be in the campfire cell
};
Arrival s_arrival{Arrival::None};
uint64_t s_arrivalSince{};

uint64_t s_newGameRequestedAt{};
bool s_sawResetPending{};
bool s_coverUntilLoaded{};
bool s_sawLoadAfterNew{};
uint64_t s_coverSince{};

// Menu actions from the UI (CEF thread), run on the main frame.
std::mutex s_actionLock;
std::string s_pendingAction;
uint64_t s_mainMenuSince{};

// The newest .ess in the saves folder that is not one of the mod's own (checkpoints and drop-ins, "SSC_" prefix).
std::string NewestSave() noexcept
{
    const auto chosen = ChosenCharacter();
    std::error_code error;
    return chosen.Prisoner || chosen.Save.empty() || chosen.Save.stem().string().rfind("SSC_", 0) == 0 ||
        !std::filesystem::exists(chosen.Save, error) ? std::string{} : chosen.Save.stem().string();
}

// UIMessageQueue::AddMessage (ID 13631) on the queue singleton (ID 400445): show a native menu.
void SendMenuMessage(const char* apName, uint32_t aType) noexcept
{
    using TAddMessage = void(void*, const BSFixedString*, uint32_t, void*);
    POINTER_SKYRIMSE(TAddMessage, addMessage, 13631);
    static VersionDbPtr<void*> s_queue(400445);
    if (auto* pQueue = s_queue.Get() ? *s_queue.Get() : nullptr)
    {
        BSFixedString name(apName);
        addMessage.Get()(pQueue, &name, aType, nullptr);
    }
}

// The Main Menu sets itself up on "show" (fills its list, its state machine; MainMenu::ProcessMessage case 1,
// 0x140959F90) only when the message carries a BSUIMessageData; its bool (+0x20) is stored at menu +0x6C/+0x6E and,
// when false, runs the first-launch platform steps. Shown with no data the menu came up with an empty list
// (Launch watch 2026-10-01: entries=0, no state). Data from UIMessageQueue::CreateUIMessageData (ID 82169).
void ShowMainMenuWithData() noexcept
{
    using TCreate = void*(void*, const BSFixedString*);
    using TAddMessage = void(void*, const BSFixedString*, uint32_t, void*);
    POINTER_SKYRIMSE(TCreate, createData, 82169);
    POINTER_SKYRIMSE(TAddMessage, addMessage, 13631);
    static VersionDbPtr<void*> s_queue(400445);
    auto* pQueue = s_queue.Get() ? *s_queue.Get() : nullptr;
    if (!pQueue)
        return;
    BSFixedString kind("BSUIMessageData");
    auto* pData = createData.Get()(pQueue, &kind);
    if (pData)
        *(reinterpret_cast<uint8_t*>(pData) + 0x20) = 1; // as returning from a game: skip the first-launch steps
    BSFixedString name("Main Menu");
    addMessage.Get()(pQueue, &name, 1 /* show */, pData);
    spdlog::info("Lobby: Main Menu shown (message data {})", pData ? "set" : "missing");
}

// A second after the show: the menu's script fills its list only from "sendMenuProperties", which it listens for
// once StartMenu.InitExtensions has run (vanilla StartMenu.as: GameDelegate "sendMenuProperties" -> setupMainMenu).
// Re-run InitExtensions, then the game's own sender (0x14095B5E0, ID 442717, called from the show handler).
void ApplyMotd() noexcept;
extern std::string s_motdBody;
extern uint64_t s_motdKeepUntil;

// The menu's save list (LOAD, and the character screen's SAVES) is filled by the UISaveLoadManager (0x1431E0140, ID
// 401336): asked through GameDelegate "PopulateCharacterList" (0x140990D80), it answers with a message to the menu
// named in its +0x48. MainMenu's show (0x140959F90, ID 52137) sets that name and registers the manager's event sink
// only when the show carries message data, which the lobby's show has none of (its data request returns nothing), so
// the list never came and the menu sat in "CharacterLoad" (owner 2026-10-01: "the saves button just puts me in a soft
// lock"). The same two calls as the show: name = InterfaceStrings (400446) +0xA8 "Main Menu" through 52885, and the
// sink (manager +8) added to the source at 403340 +0x18 through 34011 (0x14095A002 and 0x14095A024).
void RegisterMenuSaveList() noexcept
{
    static VersionDbPtr<uint8_t*> s_manager(401336);
    static VersionDbPtr<uint8_t*> s_strings(400446);
    static VersionDbPtr<uint8_t*> s_source(403340);
    auto* pManager = s_manager.Get() ? *s_manager.Get() : nullptr;
    auto* pStrings = s_strings.Get() ? *s_strings.Get() : nullptr;
    auto* pSource = s_source.Get() ? *s_source.Get() : nullptr;
    if (!pManager || !pStrings || !pSource)
    {
        spdlog::warn("Lobby: the save list could not be registered (manager {}, strings {}, source {})",
            fmt::ptr(pManager), fmt::ptr(pStrings), fmt::ptr(pSource));
        return;
    }
    using TSetName = void(void*, void*);
    POINTER_SKYRIMSE(TSetName, setName, 52885);
    setName.Get()(pManager, pStrings + 0xA8);
    using TAddSink = void(void*, void*);
    POINTER_SKYRIMSE(TAddSink, addSink, 34011);
    addSink.Get()(pSource + 0x18, pManager + 8);
    spdlog::info("Lobby: the menu's save list registered with the save manager");
}

void PrimeMainMenu() noexcept
{
    RegisterMenuSaveList();
    auto* pUI = UI::Get();
    auto* pMenu = pUI ? pUI->FindMenuByName("Main Menu") : nullptr;
    if (!pMenu || !pMenu->uiMovie)
    {
        spdlog::warn("Lobby: Main Menu not found to prime");
        return;
    }
    auto* pVtable = *reinterpret_cast<uintptr_t**>(pMenu->uiMovie);
    using TIsAvailable = bool(void*, const char*);
    using TInvoke = bool(void*, const char*, const char*, ...);
    static VersionDbPtr<void> s_invoke(82664);
    auto* pInvoke = reinterpret_cast<TInvoke*>(s_invoke.GetPtr());
    for (const char* pPath : {"_root.MenuHolder.Menu_mc.InitExtensions", "_root.InitExtensions"})
    {
        if (pInvoke && reinterpret_cast<TIsAvailable*>(pVtable[0x0A])(pMenu->uiMovie, pPath))
        {
            pInvoke(pMenu->uiMovie, pPath, nullptr);
            spdlog::info("Lobby: Main Menu {} re-run", pPath);
        }
    }
    using TSendProperties = void(IMenu*);
    POINTER_SKYRIMSE(TSendProperties, sendProperties, 442717);
    sendProperties.Get()(pMenu);
    spdlog::info("Lobby: Main Menu properties sent");
    // White menu text over a bright sky (owner 2026-10-01): a vignette under the menu, over the world.
    AddMainMenuVignette(pMenu->uiMovie);
    s_sizeVignetteUntil = Now() + 5000;
    s_motdBody.clear(); // a new lore entry each time the lobby opens
    ApplyMotd();
    s_motdKeepUntil = Now() + 15000;
}

void ShowNativeMenu(const char* apName) noexcept
{
    SendMenuMessage(apName, 1); // kShow
}

void HideNativeMenu(const char* apName) noexcept
{
    SendMenuMessage(apName, 3); // kHide
}

void RunAction(const std::string& acAction) noexcept
{
    spdlog::info("Lobby: menu action {}", acAction);
    if (acAction == "continue")
    {
        const auto save = NewestSave();
        if (save.empty() || save.find('"') != std::string::npos)
        {
            spdlog::warn("Lobby: no save to continue");
            return;
        }
        LobbyService::End(); // s_lock is recursive: safe from inside OnMainFrame
        RunConsole(fmt::format("load \"{}\"", save));
    }
    else if (acAction == "load")
        ShowNativeMenu("Journal Menu");
    else if (acAction == "newgame")
    {
        // The main menu's New Game (StartNewGame, ID 52118) starts the intro in an empty game: go back to the main
        // menu first (Game.QuitToMainMenu, ID 55490), then press it there once the menu has settled.
        s_newGamePressed = true; // the same reset-then-New flow as the real menu's New
    }
    else if (acAction == "quit")
    {
        LobbyService::End(); // s_lock is recursive: safe from inside OnMainFrame
        RunConsole("qqq");
    }
}

// After New: wait for the engine's own "reset over" flags, then press the real New Game at the title. If the reset
// was never seen starting (a fade dropped), set Main+0x11 ourselves once, as the fade callback would.
void PollNewGame() noexcept
{
    if (s_coverUntilLoaded)
    {
        // Our loading screen stays up through the reset and the intro's load, then goes.
        const auto* pUI = UI::Get();
        const bool loading = pUI && pUI->GetMenuOpen(BSFixedString("Loading Menu"));
        if (loading)
            s_sawLoadAfterNew = true;
        if ((s_sawLoadAfterNew && !loading && !s_newGameAfterMenu) || Now() - s_coverSince > 90000)
        {
            s_coverUntilLoaded = false;
            PushPresentation(false, 0.f, false);
        }
    }
    if (!s_newGameAfterMenu)
        return;
    if (auto* pMain = MainSingleton(); pMain && pMain[0x11])
        s_sawResetPending = true;
    float fadeSeconds = 1.f;
    if (auto* pSettings = INISettingCollection::Get())
        if (auto* pFade = pSettings->GetSetting("fLoadGameFadeSecs:General"))
            std::memcpy(&fadeSeconds, &pFade->data, sizeof(fadeSeconds));
    const auto fallbackMs = static_cast<uint64_t>((std::clamp(fadeSeconds, 0.f, 60.f) + 2.f) * 1000.f);
    // FRAME-34's menu and fade conditions are withdrawn: in the lobby the Main Menu is still open here and the fade never
    // starts the reset, so the fallback never fired and the party's New Game sat at the title (2026-10-02 07:16 two-PC
    // test). The timer is the configured fade plus two seconds, never under the 6 s that worked before.
    if (!s_sawResetPending && Now() - s_newGameRequestedAt > std::max<uint64_t>(6000, fallbackMs))
    {
        if (auto* pMain = MainSingleton())
        {
            pMain[0x11] = 1;
            s_sawResetPending = true;
            spdlog::warn("Lobby: the reset did not start from the fade; requested it directly");
        }
    }
    if (!s_sawResetPending || !ResetFinished())
        return;
    s_newGameAfterMenu = false;
    s_sawLoadAfterNew = false;
    spdlog::info("Lobby: reset over; starting the main menu's New Game");
    MT::WorldGeneration.fetch_add(1, std::memory_order_acq_rel);
    s_realStartNewGame();
}

// After Continue: once the save is in, a character who has discovered the lobby's area is moved to the campfire
// (cow to its cell, then onto the first mark facing the fire); otherwise the game is shown where it was saved. Our
// loading screen covers both loads.
void PollArrival() noexcept
{
    if (s_arrival == Arrival::None)
        return;
    const auto* pUI = UI::Get();
    auto* pPlayer = PlayerCharacter::Get();
    const bool inWorld = pUI && pPlayer && pPlayer->parentCell && pPlayer->GetNiNode() &&
                         !pUI->GetMenuOpen(BSFixedString("Loading Menu")) && !pUI->GetMenuOpen(BSFixedString("Main Menu"));
    if (Now() - s_arrivalSince > 120000)
    {
        spdlog::warn("Lobby: arrival gave up waiting");
        s_arrival = Arrival::None;
        PushPresentation(false, 0.f, false);
        return;
    }
    static uint64_t s_inWorldSince{};
    if (!inWorld)
    {
        s_inWorldSince = 0;
        return;
    }
    if (!s_inWorldSince)
        s_inWorldSince = Now();
    if (Now() - s_inWorldSince < 1000) // settled
        return;
    s_inWorldSince = 0;
    if (s_arrival == Arrival::WaitSave)
    {
        const auto name = PlayerName();
        if (HasVisited(kCampfireLocationId, name))
        {
            spdlog::info("Lobby: {} has been to the campfire; arriving there", name);
            PushPresentation(true, 0.5f, false);
            RunConsole(fmt::format("cow Tamriel {} {}", kGridX, kGridY));
            s_arrival = Arrival::WaitCampfire;
            s_arrivalSince = Now();
            return;
        }
        spdlog::info("Lobby: {} has not been to the campfire yet; continuing where the save was made", name.empty() ? "this character" : name);
        s_arrival = Arrival::None;
        PushPresentation(false, 1.f, false);
        return;
    }
    // WaitCampfire: in the campfire cell, onto the player's mark facing the fire.
    if (pPlayer->parentCell->formID != kCampfireCell)
        return;
    const auto& slot = s_slots[0];
    RunConsole(fmt::format("player.setpos x {:.1f}", slot.X));
    RunConsole(fmt::format("player.setpos y {:.1f}", slot.Y));
    RunConsole(fmt::format("player.setpos z {:.1f}", slot.Z));
    RunConsole(fmt::format("player.setangle z {:.1f}", HeadingTo(Point{slot.X, slot.Y, slot.Z}, kFire) * 57.29578f));
    s_arrival = Arrival::None;
    PushPresentation(false, 1.f, false);
    spdlog::info("Lobby: arrived at the campfire");
}

// Test: the intro's title menu ("TitleSequence Menu", Titles.swf) with one of its sequences (mq101questscript plays
// Sequence1..4, then Game.HideTitleSequenceMenu). StartTitleSequence's body is 0x140985190 (ID 52654), taking the
// sequence name as a BSFixedString passed by reference.
std::mutex s_titleLock;
std::string s_titleRequest;

void RunTitleRequest() noexcept
{
    std::string request;
    {
        std::lock_guard lock(s_titleLock);
        request.swap(s_titleRequest);
    }
    if (request.empty())
        return;
    if (request == "hide")
    {
        HideNativeMenu("TitleSequence Menu");
        spdlog::info("Lobby test: title menu hidden");
        return;
    }
    if (request == "stop" || request == "play")
    {
        // Freeze or resume the title movie's main timeline.
        auto* pUI = UI::Get();
        auto* pMenu = pUI ? pUI->FindMenuByName("TitleSequence Menu") : nullptr;
        if (pMenu && pMenu->uiMovie)
        {
            using TInvoke = bool(void*, const char*, const char*, ...);
            static VersionDbPtr<void> s_invoke(82664);
            const bool ok = reinterpret_cast<TInvoke*>(s_invoke.GetPtr())(pMenu->uiMovie, request == "stop" ? "_root.sequence4.stop" : "_root.sequence4.play", nullptr);
            spdlog::info("Lobby test: title movie {} invoked={} {}", request, ok, ProbeTitleMovie(pMenu->uiMovie));
        }
        return;
    }
    ShowNativeMenu("TitleSequence Menu");
    using TStart = void(BSFixedString*);
    POINTER_SKYRIMSE(TStart, startTitleSequence, 52654);
    BSFixedString name(request.c_str());
    startTitleSequence.Get()(&name);
    spdlog::info("Lobby test: title sequence {}", request);
}

// The CHARACTERS screen (the patched menu's character mode): the camera eases in on the character, left and right swap
// the character at the fire behind a short fade, and the choice is kept for the next launch and for Continue.
enum class Swap
{
    None,
    FadeOut,
    Applying,
    Settle,
};
Swap s_swap{Swap::None};
uint64_t s_swapAt{};
int s_swapQueued{-1};
bool s_charactersOpen{};
CameraPose s_menuPose{}, s_camFrom{}, s_camTo{};
uint64_t s_camMoveAt{};
std::string s_publishedName, s_publishedDetail;
int s_publishedCount{-1};
int s_publishedParty{-1};
uint32_t s_publishedView{};
int s_publishedIndex{-1};
int s_publishedRole{-1};
bool s_publishedReady{};
bool s_publishedOthersReady{};
double s_roleStamp{};

void SetMovieString(const char* apMenu, const char* apPath, const std::string& acText) noexcept
{
    auto* pMovie = MenuMovie(apMenu);
    if (!pMovie)
        return;
    struct Value
    {
        void* ObjectInterface{};
        uint32_t Type{4};
        uint32_t Pad{};
        const char* Text{};
    } value;
    value.Text = acText.c_str();
    using TSetVariable = bool(void*, const char*, const Value&, uint32_t);
    auto* pVtable = *reinterpret_cast<uintptr_t**>(pMovie);
    reinterpret_cast<TSetVariable*>(pVtable[0x10])(pMovie, apPath, value, 0);
}

// CHARACTERS also steps through the party's characters, after this player's own (owner 2026-10-02: "when on the
// character section i would like to be able to view other players characters too"): the camera goes to that player's
// mark and the banner names their character; nothing to choose, browse or delete. If they leave while looked at, the
// view goes back to this player's own character.
uint32_t s_viewForm{};
std::vector<uint32_t> PartyViewList() noexcept;
size_t MarkOf(uint32_t aForm) noexcept;

void PublishCharacter(bool aForce) noexcept
{
    auto chosen = ChosenCharacter();
    const auto party = PartyViewList();
    const int own = CharacterCount();
    int count = own;
    int index = s_characterIndex;
    if (s_viewForm)
    {
        count = static_cast<int>(party.size());
        const auto it = std::find(party.begin(), party.end(), s_viewForm);
        auto* pActor = Cast<Actor>(TESForm::GetById(s_viewForm));
        auto* pNpc = pActor ? Cast<TESNPC>(pActor->baseForm) : nullptr;
        chosen = {};
        chosen.Name = pNpc && pNpc->fullName.value.data ? pNpc->fullName.value.AsAscii() : "PARTY MEMBER";
        for (auto& c : chosen.Name)
            c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        chosen.Detail = fmt::format("LEVEL {}      IN YOUR PARTY", pActor ? pActor->GetLevel() : 1);
        index = static_cast<int>(it - party.begin());
    }
    if (static_cast<int>(party.size()) != s_publishedParty)
    {
        s_publishedParty = static_cast<int>(party.size());
        SetMovieNumber("Main Menu", "_root.SSMPartyCount", static_cast<double>(party.size()));
        aForce = true; // the bar gains or loses PARTY
    }
    if (!aForce && chosen.Name == s_publishedName && chosen.Detail == s_publishedDetail && count == s_publishedCount &&
        index == s_publishedIndex && s_viewForm == s_publishedView)
        return;
    s_publishedIndex = index;
    s_publishedView = s_viewForm;
    SetMovieNumber("Main Menu", "_root.SSMCharRemote", s_viewForm ? 1.0 : 0.0);
    s_publishedName = chosen.Name;
    s_publishedDetail = chosen.Detail;
    s_publishedCount = count;
    SetMovieString("Main Menu", "_root.SSMCharName", chosen.Name);
    SetMovieString("Main Menu", "_root.SSMCharDetail", chosen.Detail);
    SetMovieString("Main Menu", "_root.SSMCharRawName", chosen.RawName);
    SetMovieNumber("Main Menu", "_root.SSMCharHasSaves", chosen.Save.empty() ? 0.0 : 1.0);
    SetMovieNumber("Main Menu", "_root.SSMCharId", static_cast<double>(chosen.Id));
    // SAVES opens Skyrim's Load screen on this playthrough's entry, which needs its ID: co-op checkpoints have none
    // (the Load screen files them all under one entry), so they get no SAVES.
    SetMovieNumber("Main Menu", "_root.SSMCharCanBrowse", chosen.Id != 0 ? 1.0 : 0.0);
    SetMovieNumber("Main Menu", "_root.SSMCharRefresh", 1.0); // the menu rebuilds its button bar (SAVES, DELETE)
    SetMovieNumber("Main Menu", "_root.SSMCharCount", static_cast<double>(count));
    SetMovieNumber("Main Menu", "_root.SSMCharIndex", static_cast<double>(index + 1)); // the "3 / 64" counter
}

// Square in front of the character on a mark, level with their waist (owner 2026-10-02: Rex seated on the ledge was
// framed badly by a three-quarter, chest-height view): along the mark's facing, at the height of the character's pelvis
// bone as it is drawn (seated or standing), looking straight at it.
CameraPose CharacterCloseUp(size_t aMark = SIZE_MAX) noexcept
{
    const size_t mark = aMark < s_slots.size() ? aMark : s_playerMark;
    const auto& slot = s_slots[mark];
    float waist = slot.Z + 95.f;
    float heading = slot.HeadingDeg / 57.29578f;
    if (auto* pActor = SlotActor(mark); pActor && pActor->GetNiNode())
    {
        static BSFixedString s_pelvis("NPC Pelvis [Pelv]");
        if (auto* pPelvis = static_cast<NiNode*>(pActor->GetNiNode())->GetByName(s_pelvis))
            waist = pPelvis->world.translate.z;
        heading = pActor->rotation.z; // as the body faces (a seated idle can turn it)
    }
    constexpr float kDistance = 170.f;
    const Point eye{slot.X + std::sin(heading) * kDistance, slot.Y + std::cos(heading) * kDistance, waist};
    const Point target{slot.X, slot.Y, waist};
    return {eye.X, eye.Y, eye.Z, 0.f, HeadingTo(eye, target)};
}

void MoveCamera(const CameraPose& acTo) noexcept
{
    s_camFrom = s_pose;
    s_camTo = acTo;
    // The short way round.
    while (s_camTo.Yaw - s_camFrom.Yaw > 3.14159265f)
        s_camTo.Yaw -= 6.2831853f;
    while (s_camTo.Yaw - s_camFrom.Yaw < -3.14159265f)
        s_camTo.Yaw += 6.2831853f;
    s_camMoveAt = Now();
}

void BeginSwap(int aTarget) noexcept
{
    {
        std::lock_guard lock(s_charactersLock);
        s_characterIndex = aTarget;
    }
    PublishCharacter(false);
    if (s_swap == Swap::FadeOut)
    {
        s_swapAt = Now(); // still black-out: just aim at the newer choice
        return;
    }
    if (s_swap != Swap::None)
    {
        s_swapQueued = aTarget;
        return;
    }
    FadeOutGame(true, true, 0.25f, true, 0.f);
    s_swap = Swap::FadeOut;
    s_swapAt = Now();
}

void TickCharacters() noexcept
{
    PublishCharacter(false);
    // Open and close: the camera eases in on the character and back to the menu's view.
    const bool open = MovieNumber("Main Menu", "_root.SkyrimSeamlessCharactersOpen") == 1.0;
    if (open != s_charactersOpen)
    {
        s_charactersOpen = open;
        s_viewForm = 0;
        if (open)
        {
            s_menuPose = s_pose;
            MoveCamera(CharacterCloseUp());
        }
        else
        {
            MoveCamera(s_menuPose);
            SaveCharacterChoice();
            spdlog::info("Lobby: character chosen: {} ({})", ChosenCharacter().Name, ChosenCharacter().Key);
        }
    }
    // The party member being looked at left: back to this player's own character (owner 2026-10-02). Moved marks (someone
    // ahead of them left, or a promotion): the camera follows.
    static size_t s_viewMark{SIZE_MAX};
    if (!s_viewForm)
        s_viewMark = SIZE_MAX;
    else if (s_charactersOpen)
    {
        const auto party = PartyViewList();
        if (std::find(party.begin(), party.end(), s_viewForm) == party.end())
        {
            spdlog::info("Lobby: the party member being looked at left; back to this player's character");
            s_viewForm = 0;
            s_viewMark = SIZE_MAX;
            MoveCamera(CharacterCloseUp());
            PublishCharacter(true);
        }
        else if (const auto mark = MarkOf(s_viewForm); mark != s_viewMark)
        {
            if (s_viewMark != SIZE_MAX)
                MoveCamera(CharacterCloseUp(mark));
            s_viewMark = mark;
        }
    }
    if (s_camMoveAt)
    {
        constexpr uint64_t kMove = 900;
        const auto elapsed = Now() - s_camMoveAt;
        const float t = std::min(1.f, static_cast<float>(elapsed) / kMove);
        const float k = t * t * (3.f - 2.f * t);
        s_pose.X = s_camFrom.X + (s_camTo.X - s_camFrom.X) * k;
        s_pose.Y = s_camFrom.Y + (s_camTo.Y - s_camFrom.Y) * k;
        s_pose.Z = s_camFrom.Z + (s_camTo.Z - s_camFrom.Z) * k;
        s_pose.Pitch = s_camFrom.Pitch + (s_camTo.Pitch - s_camFrom.Pitch) * k;
        s_pose.Yaw = s_camFrom.Yaw + (s_camTo.Yaw - s_camFrom.Yaw) * k;
        if (elapsed >= kMove)
            s_camMoveAt = 0;
    }
    // DELETE answered Yes in the menu's confirmation: that character's saves go, the list is read again, and the
    // character in the same place in it (the next one) is put on.
    static bool s_rescanAfterDelete{};
    if (MovieNumber("Main Menu", "_root.SkyrimSeamlessDeleteRequested") == 1.0)
    {
        SetMovieNumber("Main Menu", "_root.SkyrimSeamlessDeleteRequested", 0.0);
        const auto chosen = ChosenCharacter();
        if (!chosen.Prisoner && !chosen.Key.empty())
        {
            const auto deleted = CheckpointSaves::DeleteCharacterSaves(chosen.Key);
            spdlog::info("Lobby: deleted {} saves of {} ({})", deleted, chosen.RawName, chosen.Key);
            ScanCharacters();
            s_rescanAfterDelete = true;
        }
    }
    if (s_rescanAfterDelete && s_charactersReady)
    {
        s_rescanAfterDelete = false;
        const int count = CharacterCount();
        BeginSwap(std::clamp(s_characterIndex, 0, std::max(0, count - 1)));
        PublishCharacter(true);
    }
    // Left and right (the menu's own navigation events, so keyboard, mouse and gamepad alike).
    const double step = MovieNumber("Main Menu", "_root.SkyrimSeamlessCharacterStep");
    if (step == 1.0 || step == -1.0)
    {
        SetMovieNumber("Main Menu", "_root.SkyrimSeamlessCharacterStep", 0.0);
        const int own = CharacterCount();
        if (s_charactersOpen && s_viewForm)
        {
            // Looking at a party member: left and right come back to this player's own character first.
            s_viewForm = 0;
            MoveCamera(CharacterCloseUp());
            PublishCharacter(true);
        }
        else if (own > 1 && s_charactersOpen)
        {
            const int current = s_swapQueued >= 0 ? s_swapQueued : s_characterIndex;
            BeginSwap(((current < 0 ? 0 : current) + static_cast<int>(step) + own) % own);
        }
    }
    // The party's characters on their own key (owner 2026-10-02): each press shows the next one; after the last, this
    // player's own character again.
    if (MovieNumber("Main Menu", "_root.SkyrimSeamlessPartyStep") == 1.0)
    {
        SetMovieNumber("Main Menu", "_root.SkyrimSeamlessPartyStep", 0.0);
        const auto party = PartyViewList();
        if (s_charactersOpen && !party.empty())
        {
            const auto it = std::find(party.begin(), party.end(), s_viewForm);
            const size_t next = s_viewForm && it != party.end() ? static_cast<size_t>(it - party.begin()) + 1 : 0;
            s_viewForm = next < party.size() ? party[next] : 0;
            MoveCamera(s_viewForm ? CharacterCloseUp(MarkOf(s_viewForm)) : CharacterCloseUp());
            PublishCharacter(true);
        }
    }
    switch (s_swap)
    {
    case Swap::None:
        break;
    case Swap::FadeOut:
        if (Now() - s_swapAt >= 300)
        {
            s_characterApplying = ApplyChosenLook();
            s_swap = Swap::Applying;
            s_swapAt = Now();
        }
        break;
    case Swap::Applying:
    {
        const auto status = CharacterSnapshots::ApplyStatus();
        const bool finished = !s_characterApplying || status.rfind("done", 0) == 0 || status.rfind("failed", 0) == 0 || status == "idle";
        if (finished || Now() - s_swapAt > 8000)
        {
            if (s_characterApplying)
                spdlog::info("Lobby: character swap {}", finished ? status.substr(0, 200) : "still applying after 8 s");
            s_characterApplying = false;
            PlaceSlot(s_playerMark);
            s_dirtyIdle |= 1u << s_playerMark;
            s_sendLook = true;
            s_swap = Swap::Settle;
            s_swapAt = Now();
        }
        break;
    }
    case Swap::Settle:
        // The rebuilt body faded in before the world comes back.
        // The rebuilt body faded in and its pose's enter animation played (owner 2026-10-02: jumpy) before the world.
        if ((PlayerFade() >= 0.99f && Now() - s_swapAt >= 1000) || Now() - s_swapAt > 2500)
        {
            if (s_swapQueued >= 0)
            {
                s_swapQueued = -1;
                s_swap = Swap::FadeOut; // still black: straight to the next one
                s_swapAt = Now() - 300;
                break;
            }
            FadeOutGame(false, true, 0.35f, true, 0.f);
            s_swap = Swap::None;
        }
        break;
    }
}

// The menu follows this player's place in a party (owner 2026-10-01: "her main menu adjust to not interrupt the host";
// "consistency across the menu and not so many duplicates"): solo, host (CONTINUE and NEW start the party together) or
// guest (no CONTINUE, NEW or LOAD; READY and LEAVE PARTY instead). The patched menu rebuilds its list when the stamp
// changes. _root.SkyrimSeamlessRole: 0 solo, 1 host in a party, 2 guest.
// Leaving the lobby is one fade to black (owner 2026-10-02: the campfire showed alone between the menu going and the black).
// The menu's own fade-out (StartMenu.FadeOutAndCall, before StartNewGame or ContinueLastSavedGame) is joined by a black
// clip in the Main Menu movie easing in over the scene; the Fader cannot do it, as the smoke's menu draws the world over
// every menu below it. The lobby then leaves its Main Menu up (black) for the reset or load to close; if neither comes
// within 15 s it is put away as usual.
void FadeWithMenuOut() noexcept
{
    auto* pMovie = MenuMovie("Main Menu");
    if (!pMovie || s_leaveBlackAt)
        return;
    struct Value
    {
        void* ObjectInterface{};
        uint32_t Type{};
        uint32_t Pad{};
        const char* Text{};
    } value;
    using TGetVariable = bool(void*, Value*, const char*);
    auto* pGet = reinterpret_cast<TGetVariable*>((*reinterpret_cast<uintptr_t**>(pMovie))[0x11]);
    if (!pGet(pMovie, &value, "_root.MenuHolder.Menu_mc.strFadeOutCallback") || (value.Type & 0x0F) != 4 || !value.Text)
        return;
    const char* text = (value.Type & 0x40) ? *reinterpret_cast<const char* const*>(value.Text) : value.Text;
    if (!text || !*text)
        return;
    AddBlackClip(pMovie, "SSMLeaveBlack", 16300);
    s_leaveBlackMovie = pMovie;
    s_leaveBlackAt = Now();
    spdlog::info("Lobby: the menu is fading out ({}); the scene fades to black with it", text);
}

// Every main frame, any phase: the black eases in, and is let go once the menu it lives in is gone.
void TickLeaveBlack() noexcept
{
    if (!s_leaveBlackAt)
        return;
    if (!MenuMovie("Main Menu"))
    {
        s_leaveBlackAt = 0;
        s_leaveBlackMovie = nullptr;
        return;
    }
    SetMovieNumber("Main Menu", "_root.SSMLeaveBlack._alpha", Ramp(Now() - s_leaveBlackAt, 0, 400));
    const auto* pUI = UI::Get();
    // A save loaded (from the lobby there is no loading screen): a load, unlike New Game's reset, never closes the menu
    // the lobby opened itself (2026-10-02: Continue sat on the black for the 15 s fallback with the world already in).
    if (s_phase == Phase::Idle && (s_saveLoadHandled.exchange(false) || (pUI && pUI->GetMenuOpen(BSFixedString("Loading Menu")))))
    {
        s_leaveBlackAt = 0;
        s_leaveBlackMovie = nullptr;
        HideNativeMenu("Main Menu");
        ShowNativeMenu("HUD Menu");
        spdlog::info("Lobby: the loading screen is up; the lobby's black menu put away");
        return;
    }
    // Never while a New Game is under way (reset, or its load not yet up; the cover gives up at 90 s): hiding the Main
    // Menu then showed the half-reset world, whose 3D loads deadlocked the frame jobs (2026-10-03 15:23 and 15:35, a slow
    // follower whose New Game took longer than this timer).
    const bool newGamePending = s_newGameAfterMenu || (s_coverUntilLoaded && !s_sawLoadAfterNew);
    if (s_phase == Phase::Idle && !newGamePending && Now() - s_leaveBlackAt > 15000 && pUI &&
        !pUI->GetMenuOpen(BSFixedString("Loading Menu")))
    {
        s_leaveBlackAt = 0;
        s_leaveBlackMovie = nullptr;
        HideNativeMenu("Main Menu");
        ShowNativeMenu("HUD Menu");
        spdlog::warn("Lobby: no load followed the menu's fade-out within 15 s; the lobby's menu put away");
    }
}

void TickPartyRole() noexcept
{
    FadeWithMenuOut();
    auto& party = World::Get().GetPartyService();
    // Every PC hosts a private one-player session of its own at the title: a party only counts with someone else in it.
    const bool together = party.IsInParty() && party.GetPartyMembers().size() > 1;
    const int role = !together ? 0 : party.IsLeader() ? 1 : 2;
    const auto localId = World::Get().GetTransport().GetLocalPlayerId();
    const bool ready = role != 0 && party.IsPlayerReady(localId);
    // The host's CONTINUE and NEW TOGETHER are usable only once everyone else is ready.
    bool othersReady = role == 1;
    if (othersReady)
        for (const auto memberId : party.GetPartyMembers())
            if (memberId != localId && !party.IsPlayerReady(memberId))
                othersReady = false;
    if (role != s_publishedRole || ready != s_publishedReady || othersReady != s_publishedOthersReady)
    {
        s_publishedRole = role;
        s_publishedReady = ready;
        s_publishedOthersReady = othersReady;
        SetMovieNumber("Main Menu", "_root.SkyrimSeamlessRole", role);
        SetMovieNumber("Main Menu", "_root.SkyrimSeamlessReady", ready ? 1.0 : 0.0);
        SetMovieNumber("Main Menu", "_root.SkyrimSeamlessOthersReady", othersReady ? 1.0 : 0.0);
        SetMovieNumber("Main Menu", "_root.SkyrimSeamlessRoleStamp", ++s_roleStamp);
        spdlog::info("Lobby: menu role {} (ready {})", role == 0 ? "solo" : role == 1 ? "host" : "guest", ready);
    }
    if (MovieNumber("Main Menu", "_root.SkyrimSeamlessReadyRequested") == 1.0)
    {
        SetMovieNumber("Main Menu", "_root.SkyrimSeamlessReadyRequested", 0.0);
        if (role != 0)
            party.SetReady(!ready);
    }
    if (MovieNumber("Main Menu", "_root.SkyrimSeamlessLeaveRequested") == 1.0)
    {
        SetMovieNumber("Main Menu", "_root.SkyrimSeamlessLeaveRequested", 0.0);
        if (role != 0)
        {
            spdlog::info("Lobby: leaving the party from the menu");
            World::Get().GetSteamLobbyService().QueueLeaveSession();
        }
    }
}

// The host's CONTINUE or NEW in a party asks the server to start everyone (the party's own launch then presses the
// same entry on every PC, the host's included, and that press goes through as usual). A guest has neither entry.
bool StartPartyInstead(uint8_t aMode) noexcept
{
    auto& party = World::Get().GetPartyService();
    if (!party.IsInParty() ||
        (party.GetPartyMembers().size() < 2 && !(aMode == PartyStartRequest::kContinue && party.IsHandoffCampaign())) ||
        IsSharedCampaignLaunching())
        return false;
    if (!party.IsLeader())
    {
        spdlog::warn("Lobby: a guest's {} press ignored (the host starts the party)", aMode == PartyStartRequest::kNew ? "New" : "Continue");
        return true;
    }
    // The server starts only with every member ready, the host included; pressing the entry is the host's ready.
    const auto localId = World::Get().GetTransport().GetLocalPlayerId();
    for (const auto memberId : party.GetPartyMembers())
    {
        if (memberId != localId && !party.IsPlayerReady(memberId))
        {
            spdlog::info("Lobby: the host pressed {} but not everyone is ready; the menu comes back", aMode == PartyStartRequest::kNew ? "New" : "Continue");
            // The press already faded the menu (and the lobby's black with it): undo both, or the host sits on black.
            if (s_leaveBlackAt)
            {
                s_leaveBlackAt = 0;
                SetMovieNumber("Main Menu", "_root.SSMLeaveBlack._alpha", 0.0);
            }
            if (auto* pMovie = MenuMovie("Main Menu"))
            {
                using TInvoke = bool(void*, const char*, const char*, ...);
                static VersionDbPtr<void> s_invoke(82664);
                SetMovieString("Main Menu", "_root.MenuHolder.Menu_mc.strFadeOutCallback", "");
                reinterpret_cast<TInvoke*>(s_invoke.GetPtr())(pMovie, "_root.MenuHolder.Menu_mc.DoFadeInMenu", nullptr);
            }
            return true;
        }
    }
    spdlog::info("Lobby: the host started the party ({})", aMode == PartyStartRequest::kNew ? "new game together" : "continue together");
    if (!party.IsPlayerReady(localId))
        party.SetReady(true);
    party.StartTogether(aMode);
    return true;
}

// Main frame, holding: New was pressed in the lobby. Starts the reset once no co-op checkpoint save is queued (it
// would otherwise run on the reverted world at the title).

void StartNewGameReset() noexcept
{
    if (!s_newGamePressed)
        return;
    if (StartPartyInstead(PartyStartRequest::kNew))
    {
        s_newGamePressed = false;
        return;
    }
    if (CheckpointSaves::IsPending())
        return;
    s_newGamePressed = false;
    spdlog::info("Lobby: New pressed; resetting to the title first");
    // The host's New Game is always a fresh character: the lobby's preview of a saved one must not finish in it.
    CharacterSnapshots::CancelApply(std::exchange(s_lobbyLookOperation, 0));
    // Black at once: ending the lobby takes its menu down a second before the reset's own fade (2026-10-02 capture: the
    // bare lobby world showed in between). The reset closes the Fader with every other menu.
    FadeOutGame(true, true, 0.f, true, 0.f);
    // As for a load under way: no console steps. The reset replaces the world and the camera, and handing the free
    // camera back to the player showed the player's view for a second before the black (2026-10-02 capture).
    LobbyService::EndLobby(true); // s_lock is recursive: safe from inside OnMainFrame
    s_coverUntilLoaded = true;
    s_coverSince = Now();
    PushPresentation(true, 0.05f, false);
    s_newGameAfterMenu = true;
    s_sawResetPending = false;
    s_newGameRequestedAt = Now();
    using TQuit = void();
    POINTER_SKYRIMSE(TQuit, quitToMainMenu, 55490);
    quitToMainMenu.Get()();
}

// The game goes straight into the live lobby at launch (owner 2026-10-01: "i would like to not see the default main
// menu at all on launch"): once, when the title's Main Menu has been up for a second, under our loading screen.
// Data\SkyrimTogetherReborn\lobby_off.txt turns it off (the test harness drives the vanilla title).
void AutoStartAtLaunch() noexcept
{
    static bool s_decided{};
    if (s_autoDone)
        return;
    if (!s_decided)
    {
        s_decided = true;
        wchar_t path[MAX_PATH]{};
        GetModuleFileNameW(nullptr, path, MAX_PATH);
        s_splashWanted = !std::filesystem::exists(std::filesystem::path(path).parent_path() / "Data" / "SkyrimTogetherReborn" / "lobby_off.txt");
    }
    const auto* pUI = UI::Get();
    if (!pUI || !pUI->GetMenuOpen(BSFixedString("Main Menu")) || pUI->GetMenuOpen(BSFixedString("Loading Menu")))
    {
        s_autoTitleSince = 0;
        return;
    }
    if (!s_autoTitleSince)
    {
        s_autoTitleSince = Now();
        StartCharacterScan();
        wchar_t path[MAX_PATH]{};
        GetModuleFileNameW(nullptr, path, MAX_PATH);
        if (std::filesystem::exists(std::filesystem::path(path).parent_path() / "Data" / "SkyrimTogetherReborn" / "lobby_off.txt"))
        {
            s_autoDone = true;
            spdlog::info("Lobby: auto start off (lobby_off.txt)");
            return;
        }
    }
    if (!s_splashActive && !s_splashHeld)
    {
        SetMovieNumber("Main Menu", "_root._alpha", 0.0);
        if (!FramesSteady(400) && Now() - s_autoTitleSince < 3000)
            return;
        StartSplash(); // SKYRIM over the smoke once the frames come evenly
    }
    // The load freezes the frame loop, so the wordmark is held before the lobby's load starts.
    if (!s_splashHeld)
        return;
    // Then the smoke fades out under the title's black (owner 2026-10-01: "the smoke stutters": the load's frames
    // come unevenly), and the load runs on SKYRIM over black.
    constexpr uint64_t kSmokeFade = 900;
    if (!s_autoSmokeFadeAt)
        s_autoSmokeFadeAt = Now();
    const auto smokeFade = Now() - s_autoSmokeFadeAt;
    EnsureTitleBlack(Ramp(smokeFade, 0, kSmokeFade));
    if (smokeFade < kSmokeFade + 100)
        return;
    s_titleBlackUp = true;
    s_autoDone = true;
    s_returnInProgress = false;
    s_gameplaySeen = false;
    spdlog::info("Lobby: starting the live lobby at the title");
    SetStatus(Phase::Enter, "entering the campfire lobby");
}

// The news panel shows Skyrim's own lore instead of Bethesda's updates (owner 2026-10-01): one loading-screen text,
// read from the loaded load screens (TESLoadScreen::loadingText, +0x30, CommonLibSSE-NG), chosen at random each time
// the lobby opens, then this build's version. Gameplay tips (addressed to "you", or about keys and menus) are left
// out; lore is told in the third person.
std::vector<std::string> LoreEntries() noexcept
{
    std::vector<std::string> entries;
    auto* pModManager = ModManager::Get();
    if (!pModManager)
        return entries;
    constexpr uint8_t kLoadScreenType = 0x51;
    // Gameplay words: tips explain mechanics (2026-10-01 dump: "Dual wielding potentially doubles melee damage",
    // "bashing ... will always stagger", race abilities) where lore tells of places, peoples and history.
    static constexpr std::array<std::string_view, 55> kTipWords{
        "you", "your", "press", "button", "key", "menu", "inventory", "favorites", "perk", "skill", "level", "health",
        "magicka", "stamina", "attack", "sneak", "journal", "quest", "bounty", "crime", "follower", "carry", "[", "<",
        "damage", "melee", "block", "blocking", "stagger", "bash", "bashing", "wield", "wielding", "weapon", "weapons",
        "armor", "ability", "abilities", "bonus", "resist", "resistance", "chance", "increase", "increases", "casting",
        "cast", "potion", "potions", "enchant", "enchanting", "bonuses", "skilled", "training", "spell", "spells"};
    const auto& forms = pModManager->FormsOfType(kLoadScreenType);
    for (uint32_t i = 0; i < forms.length; ++i)
    {
        auto* pForm = forms[i];
        if (!pForm)
            continue;
        const auto& text = *reinterpret_cast<const BSFixedString*>(reinterpret_cast<const uint8_t*>(pForm) + 0x30);
        const char* pText = text.AsAscii();
        if (!pText)
            continue;
        std::string entry(pText);
        if (entry.size() < 60 || entry.size() > 420)
            continue;
        std::string lower(entry);
        std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        const bool tip = std::any_of(kTipWords.begin(), kTipWords.end(), [&](std::string_view word) {
            // whole words only for the alphabetic ones ("you" must not match "young")
            for (size_t at = lower.find(word); at != std::string::npos; at = lower.find(word, at + 1))
            {
                const bool alpha = std::isalpha(static_cast<unsigned char>(word[0])) != 0;
                const bool startOk = !alpha || at == 0 || !std::isalpha(static_cast<unsigned char>(lower[at - 1]));
                const size_t end = at + word.size();
                const bool endOk = !alpha || end >= lower.size() || !std::isalpha(static_cast<unsigned char>(lower[end]));
                if (startOk && endOk)
                    return true;
            }
            return false;
        });
        if (!tip && std::find(entries.begin(), entries.end(), entry) == entries.end())
            entries.push_back(std::move(entry));
    }
    return entries;
}

std::string VersionLine() noexcept
{
    const auto current = UpdateService::CurrentVersion();
    // The author's name on the panel (owner 2026-10-01).
    std::string line = current.rfind("0.0.0-dev", 0) == 0 ? "Skyrim SE Multiplayer by TabbedScamper, development build"
                                                          : "Skyrim SE Multiplayer v" + current + " by TabbedScamper";
    // The updater's last check (Options > Updates).
    const auto state = World::Get().GetUpdateService().StateJson();
    const auto key = state.find("\"latest\":\"");
    if (key != std::string::npos)
    {
        const auto start = key + 10;
        const auto latest = state.substr(start, state.find('"', start) - start);
        if (!latest.empty() && UpdateService::IsOlder(current, latest))
            line += ". Version " + latest + " is available in Options > Updates.";
    }
    return line;
}

std::string s_motdBody;
uint64_t s_motdKeepUntil{};
constexpr char kMotdHeader[] = "SKYRIM SE MULTIPLAYER";

void ApplyMotd() noexcept
{
    auto* pUI = UI::Get();
    auto* pMenu = pUI ? pUI->FindMenuByName("Main Menu") : nullptr;
    if (!pMenu || !pMenu->uiMovie)
        return;
    if (s_motdBody.empty())
    {
        auto lore = LoreEntries();
        static std::mt19937 s_random(std::random_device{}());
        const auto& pick = lore.empty() ? std::string() : lore[std::uniform_int_distribution<size_t>(0, lore.size() - 1)(s_random)];
        s_motdBody = (pick.empty() ? std::string() : pick + "\n\n") + VersionLine();
        spdlog::info("Lobby: news panel set ({} lore entries to choose from)", lore.size());
    }
    SetMainMenuMotd(pMenu->uiMovie, kMotdHeader, s_motdBody.c_str());
}

// Bethesda's news can arrive after the lobby set the panel: put ours back for the first seconds.
void KeepMotd() noexcept
{
    if (!s_motdKeepUntil)
        return;
    if (Now() >= s_motdKeepUntil)
    {
        s_motdKeepUntil = 0;
        return;
    }
    static uint64_t s_next{};
    if (Now() < s_next)
        return;
    s_next = Now() + 1000;
    auto* pUI = UI::Get();
    auto* pMenu = pUI ? pUI->FindMenuByName("Main Menu") : nullptr;
    if (pMenu && pMenu->uiMovie && SetMainMenuMotd(pMenu->uiMovie, nullptr, nullptr) != kMotdHeader)
    {
        spdlog::info("Lobby: news panel replaced by the game; set again");
        ApplyMotd();
    }
}

// Editor fly camera, Godot style: hold the right mouse button to look; WASD fly, E / Space up, Q / C down, Shift
// faster, the wheel (while held) changes the speed. Raw mouse deltas come from the window thread.
std::atomic<int32_t> s_mouseDx{}, s_mouseDy{}, s_wheel{};
float s_flySpeed{250.f}; // game units per second
// Diagnostics for lobby_status: raw mouse events seen, frames with the button held, frames skipped (not foreground).
std::atomic<uint32_t> s_rawEvents{}, s_absoluteEvents{}, s_movingEvents{};
uint32_t s_heldFrames{}, s_backgroundFrames{};

bool KeyDown(int aKey) noexcept
{
    return (GetAsyncKeyState(aKey) & 0x8000) != 0;
}

void FlyCamera() noexcept
{
    static uint64_t s_last{};
    const auto now = Now();
    const float dt = s_last ? std::min(static_cast<float>(now - s_last) / 1000.f, 0.1f) : 0.f;
    s_last = now;
    const int32_t dx = s_mouseDx.exchange(0), dy = s_mouseDy.exchange(0), wheel = s_wheel.exchange(0);
    // By process, as MainMenuIntegration does: RendererWindow::IsForeground compared a handle that never matched the
    // focused game window (2026-10-01: 2,218 frames "background" while the OS reported the game in front).
    DWORD foregroundProcess = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &foregroundProcess);
    const bool foreground = foregroundProcess == GetCurrentProcessId();
    if (!foreground)
        ++s_backgroundFrames;
    if (!foreground || !KeyDown(VK_RBUTTON))
        return;
    ++s_heldFrames;
    constexpr float kLook = 0.0025f; // radians per mouse count
    s_pose.Yaw += dx * kLook;
    s_pose.Pitch = std::clamp(s_pose.Pitch + dy * kLook, -1.55f, 1.55f); // positive looks down
    if (wheel)
        s_flySpeed = std::clamp(s_flySpeed * std::pow(1.25f, wheel / 120.f), 20.f, 5000.f);
    // Skyrim yaw 0 faces +Y, clockwise; pitch positive looks down.
    const float cp = std::cos(s_pose.Pitch), sp = std::sin(s_pose.Pitch);
    const float sy = std::sin(s_pose.Yaw), cy = std::cos(s_pose.Yaw);
    const float forward = (KeyDown('W') ? 1.f : 0.f) - (KeyDown('S') ? 1.f : 0.f);
    const float right = (KeyDown('D') ? 1.f : 0.f) - (KeyDown('A') ? 1.f : 0.f);
    const float up = (KeyDown('E') || KeyDown(VK_SPACE) ? 1.f : 0.f) - (KeyDown('Q') || KeyDown('C') ? 1.f : 0.f);
    const float step = s_flySpeed * (KeyDown(VK_SHIFT) ? 3.f : 1.f) * dt;
    s_pose.X += (sy * cp * forward + cy * right) * step;
    s_pose.Y += (cy * cp * forward - sy * right) * step;
    s_pose.Z += (-sp * forward + up) * step;
}

void SetEditorCursor(bool aVisible) noexcept
{
    auto& overlay = World::Get().GetOverlayService();
    if (overlay.GetActive() != aVisible)
        overlay.SetActive(aVisible);
    auto* pApp = overlay.GetOverlayApp();
    auto pClient = pApp ? pApp->GetClient() : nullptr;
    if (auto pRenderer = pClient ? pClient->GetOverlayRenderHandler() : nullptr)
        pRenderer->SetCursorVisible(aVisible);
}

// Main frame, while holding: spawn or remove stand-ins, fly the camera, apply edits, save.
void ApplyEditor() noexcept
{
    static bool s_comboDown{};
    const bool comboDown = (GetAsyncKeyState(VK_CONTROL) & 0x8000) && (GetAsyncKeyState(VK_SHIFT) & 0x8000) &&
                           (GetAsyncKeyState(VK_F12) & 0x8000);
    if (comboDown && !s_comboDown)
    {
        s_editorOpen = !s_editorOpen;
        spdlog::info("Lobby: layout editor {}", s_editorOpen ? "opened" : "closed");
    }
    s_comboDown = comboDown;

    if (s_editorOpen && !s_standIns[1])
    {
        SpawnStandIns();
        SetEditorCursor(true);
    }
    else if (!s_editorOpen && s_standIns[1])
    {
        RemoveStandIns();
        s_cameraReleased = false;
        // F2 only toggles the cursor while the editor is open: never leave it off with the menu up, or on without it.
        SetEditorCursor(s_menuShown);
    }
    if (s_editorOpen)
        FlyCamera();
    TickGuests();
    // After a swap has fully settled (the rebuilt body is the one the copies should show).
    // Retried twice a second: a player who just joined has no server id for a moment.
    static uint64_t s_nextLookTry{};
    if (s_sendLook && s_swap == Swap::None && !s_characterApplying && Now() >= s_nextLookTry)
    {
        s_nextLookTry = Now() + 500;
        if (std::none_of(s_seats.begin(), s_seats.end(), [](const Seat& acSeat) { return acSeat.FormId != 0; }))
            s_sendLook = false;
        else if (World::Get().GetCharacterService().SendLookNow())
        {
            // The level with it: it is otherwise sent only when it changes while connected.
            if (auto* pPlayer = PlayerCharacter::Get())
            {
                PlayerLevelRequest level{};
                level.NewLevel = pPlayer->GetLevel();
                World::Get().GetTransport().Send(level);
            }
            // The clothes too: the lobby dressed this character after it joined the session (owner 2026-10-02: the
            // host's copy on the follower carried every item but wore none). Not at once: equips that reach a copy
            // before it is bound on the other PC are dropped there, so 3 s and 10 s after it sat down.
            s_announceWornAt = {Now() + 1500, Now() + 10000};
            s_sendLook = false;
        }
    }
    for (auto& at : s_announceWornAt)
        if (at && Now() >= at)
        {
            at = 0;
            World::Get().ctx().at<InventoryService>().AnnounceWorn();
        }
    uint32_t waiting = 0, keepPlace = 0;
    for (size_t i = 0; i < s_slots.size(); ++i)
    {
        auto& transition = s_transitions[i];
        // A copy is hidden by its root's hidden bit (+0xF4 bit 0) while placed and dressed: alpha reaches only a
        // high-process actor, and a copy that just spawned is not one yet, so it showed undressed (owner 2026-10-02).
        // The engine clears the bit on many paths: set it again every frame until the reveal.
        if (auto* pCopy = SeatedGuest(i); pCopy && (transition.PlaceAt || transition.Revealing))
            if (auto* pRoot = pCopy->GetNiNode())
                pRoot->flags |= 1;
        if (transition.PlaceAt && Now() < transition.PlaceAt)
        {
            keepPlace |= s_dirtyPlace & (1u << i); // still fading out
            waiting |= s_dirtyIdle & (1u << i);
            continue;
        }
        if (transition.PlaceAt)
        {
            transition.PlaceAt = 0;
            transition.Revealing = true;
        }
        if (s_dirtyPlace & (1u << i))
            PlaceSlot(i);
        if (s_dirtyIdle & (1u << i))
        {
            // A new stand-in has no 3D for a few frames and refuses the idle (2026-10-01 14:22:25): wait for it.
            auto* pActor = SlotActor(i);
            if (pActor && pActor->GetNiNode())
            {
                PlaySlotIdle(i);
                if (transition.Revealing)
                    transition.RevealAt = Now() + 900; // the pose's enter animation, then in
            }
            else if (pActor)
                waiting |= 1u << i;
        }
        // Not before what it wears has settled (owner 2026-10-02: characters showed naked for a second on a join: the
        // gear arrives after the body, through equips and a look rebuild): unchanged for 0.6 s, at most 3 s more.
        if (transition.Revealing && transition.RevealAt && Now() >= transition.RevealAt)
            if (auto* pActor = SlotActor(i))
            {
                const auto equipment = pActor->GetEquipment();
                const auto worn = static_cast<size_t>(std::count_if(equipment.Entries.begin(), equipment.Entries.end(),
                    [](const auto& acEntry) { return acEntry.IsWorn(); }));
                if (worn != transition.Worn || !transition.WornSince)
                {
                    transition.Worn = worn;
                    transition.WornSince = Now();
                }
                // Nothing worn yet is gear still on its way (the owner re-sends it after the copy sits): up to 5 s.
                if ((worn == 0 && Now() - transition.RevealAt < 5000) ||
                    (Now() - transition.WornSince < 600 && Now() - transition.RevealAt < 3000))
                    continue;
            }
        if (transition.Revealing && transition.RevealAt && Now() >= transition.RevealAt)
        {
            spdlog::info("Lobby: mark {} shown again with {} worn items, {} ms after its pose", i, transition.Worn,
                Now() - transition.RevealAt + 900);
            if (auto* pCopy = SeatedGuest(i))
                if (auto* pRoot = pCopy->GetNiNode())
                    pRoot->flags &= ~1u;
            transition.Revealing = false;
            transition.RevealAt = 0;
            transition.DoneAt = Now();
            SetActorAlpha(SlotActor(i), 1.f, true);
        }
    }
    s_dirtyPlace = keepPlace;
    s_dirtyIdle = waiting;
    if (s_saveRequested)
    {
        s_saveRequested = false;
        SaveLayout(s_editorMessage);
    }
}
} // namespace

namespace LobbyService
{
void RequestReturnToLobby(const char* apReason) noexcept
{
    ReturnReason reason = ReturnReason::EngineTitle;
    if (apReason && std::strcmp(apReason, "quit_to_menu") == 0)
        reason = ReturnReason::QuitToMenu;
    else if (apReason && std::strcmp(apReason, "party_left") == 0)
        reason = ReturnReason::PartyLeft;
    else if (apReason && std::strcmp(apReason, "disconnected") == 0)
        reason = ReturnReason::Disconnected;
    ReturnReason none = ReturnReason::None;
    s_returnRequested.compare_exchange_strong(none, reason);
}

std::string ReturnStatus() noexcept
{
    std::lock_guard lock(s_lock);
    const char* phase = "idle";
    const auto pending = s_returnRequested.load();
    if (pending != ReturnReason::None)
        phase = "requested";
    else if (s_returnInProgress)
        phase = s_splashActive ? "splash" : "waiting_title";
    else
    {
        switch (s_phase)
        {
        case Phase::Enter: phase = "enter"; break;
        case Phase::WaitWorld: phase = "wait_world"; break;
        case Phase::Stage: phase = "stage"; break;
        case Phase::Hold: phase = "lobby"; break;
        default: phase = "idle"; break;
        }
    }
    return fmt::format("{{\"reason\":\"{}\",\"phase\":\"{}\"}}",
        ReturnReasonName(pending != ReturnReason::None ? pending : s_lastReturnReason), phase);
}

bool Begin() noexcept
{
    std::lock_guard lock(s_lock);
    if (s_phase != Phase::Idle)
        return false;
    SetStatus(Phase::Enter, "entering the campfire lobby");
    return true;
}

void End() noexcept
{
    EndLobby(false);
}

void EndLobby(bool aLoadUnderWay) noexcept
{
    std::lock_guard lock(s_lock);
    if (s_phase == Phase::Idle)
        return;
    MT::WorldGeneration.fetch_add(1, std::memory_order_acq_rel);
    if (aLoadUnderWay)
        s_popTfcAfterLoad = true; // the load resets the camera, not ControlMap's context stack
    if (s_phase == Phase::Hold)
    {
        // A load already under way replaces the camera and the world: no console commands into it.
        if (s_menusHidden && !aLoadUnderWay)
            RunConsole("tm");
        auto* pCamera = PlayerCamera::Get();
        if (!aLoadUnderWay && pCamera && pCamera->state && pCamera->state->id == kFreeCamera)
            RunConsole("tfc");

        SetSavingDisabled(false);
        SetGodMode(false);
        SetPlayerControlsLocked(false);
    }
    RemoveStandIns();
    ReleaseSeats();
    s_transitions = {};
    s_swap = Swap::None;
    s_swapQueued = -1;
    s_charactersOpen = false;
    s_viewForm = 0;
    s_camMoveAt = 0;
    s_characterPending = false;
    s_characterApplying = false;
    s_havePrisonerLook = false;
    s_lookIsPrisoner = true;
    s_publishedName.clear();
    s_publishedDetail.clear();
    s_publishedCount = s_publishedParty = s_publishedIndex = s_publishedRole = -1;
    s_publishedView = 0;
    if (auto* pPlayer = PlayerCharacter::Get(); pPlayer && s_phase == Phase::Hold)
    {
        PAPYRUS_FUNCTION(void, Actor, SetHeadTracking, bool);
        s_pSetHeadTracking(pPlayer, true); // the lobby turned it off for the pose
    }
    s_editorOpen = s_cameraReleased = false;
    s_menusHidden = false;
    s_menuShown = false;
    s_holding = false;
    if (s_nativeMenuOpened)
    {
        s_nativeMenuOpened = false;
        if (!s_leaveBlackAt) // faded to black for a reset or load: that closes it (TickLeaveBlack's 15 s fallback)
        {
            HideNativeMenu("Main Menu");
            ShowNativeMenu("HUD Menu");
        }
    }
    PushPresentation(false, 0.f, false);
    SetEditorCursor(false);
    AudioPreview::StopLobbyPlaylist(1500);
    World::Get().GetOverlayService().SetLobbyMusic({});
    s_noModelLoad = false;
    EndSplash();
    SetWorldMusicPaused(false);
    SetWorldSoundsPaused(false);
    SetStatus(Phase::Idle, "idle");
}

void OnMainFrame() noexcept
{
    NoteFrame();
    if (!MT::Loading())
        BlockGameplayInputUnderMenus();
    if (World::Exists())
    {
        if (s_fullLeaveRequested.exchange(false))
        {
            auto& party = World::Get().GetPartyService();
            if (party.IsLeader() && party.GetPartyMembers().size() > 1)
                party.EndParty();
            else
                World::Get().GetSteamLobbyService().QueueLeaveSession(true);
        }
        World::Get().GetPartyService().DrainForcedLeave();
        auto& steam = World::Get().GetSteamLobbyService();
        steam.DrainLeaveOnMainFrame();
        if (!MT::CutEnabled(6))
            DrainTerminalQuitOnMainFrame();
    }
    // Flag polling and member publication are limited to an open Journal movie.
    static bool s_journalWasOpen{};
    auto* pJournalUI = UI::Get();
    if (auto* pMovie = !MT::Loading() && World::Exists() && pJournalUI &&
            pJournalUI->GetMenuOpen(BSFixedString("Journal Menu")) ?
            MenuMovie("Journal Menu") : nullptr)
    {
        static void* s_lastJournal{};
        static uint64_t s_nextPublish{};
        static double s_stamp{};
        static std::string s_lastJournalState;
        if (pMovie != s_lastJournal || !s_journalWasOpen)
        {
            s_lastJournal = pMovie;
            s_nextPublish = 0;
            s_lastJournalState.clear();
        }
        s_journalWasOpen = true;
        const auto now = Now();
        if (now >= s_nextPublish)
        {
            s_nextPublish = now + 250;
            auto& party = World::Get().GetPartyService();
            const auto& members = party.GetPartyMembers();
            const int role = party.IsInParty() && members.size() > 1 ? (party.IsLeader() ? 1 : 2) : 0;
            const auto count = std::min<size_t>(members.size(), 16);
            const auto& players = party.GetPlayers();
            std::string signature = fmt::format("{}/{}/{}", role, World::Get().GetSteamLobbyService().IsLobbyOpen(), count);
            for (size_t i = 0; i < count; ++i)
            {
                const auto id = members[i];
                const auto it = players.find(id);
                signature += fmt::format("/{}/{}", id, it == players.end() ? "" : it->second.c_str());
            }
            if (signature != s_lastJournalState)
            {
                s_lastJournalState = std::move(signature);
                SetMovieNumber("Journal Menu", "_root.SSMJournalRole", role);
                SetMovieNumber("Journal Menu", "_root.SSMJournalOpen",
                    World::Get().GetSteamLobbyService().IsLobbyOpen() ? 1.0 : 0.0);
                SetMovieNumber("Journal Menu", "_root.SSMJournalSelf", World::Get().GetTransport().GetLocalPlayerId());
                SetMovieNumber("Journal Menu", "_root.SSMJournalCount", static_cast<double>(count));
                for (size_t i = 0; i < count; ++i)
                {
                    const auto id = members[i];
                    SetMovieNumber("Journal Menu", fmt::format("_root.SSMJournalMember{}", i).c_str(), id);
                    const auto it = players.find(id);
                    SetMovieString("Journal Menu", fmt::format("_root.SSMJournalName{}", i).c_str(),
                        it == players.end() ? fmt::format("PLAYER {}", id) : std::string(it->second.c_str()));
                }
                SetMovieNumber("Journal Menu", "_root.SSMJournalStamp", ++s_stamp);
            }
        }
        const auto action = static_cast<int>(MovieNumber("Journal Menu", "_root.SSMJournalAction"));
        if (action > 0)
        {
            const auto target = static_cast<uint32_t>(std::max(0.0, MovieNumber("Journal Menu", "_root.SSMJournalTarget")));
            SetMovieNumber("Journal Menu", "_root.SSMJournalAction", 0);
            auto& party = World::Get().GetPartyService();
            auto& steam = World::Get().GetSteamLobbyService();
            const bool together = party.IsInParty() && party.GetPartyMembers().size() > 1;
            const bool host = together && party.IsLeader();
            const bool member = target && target != World::Get().GetTransport().GetLocalPlayerId() &&
                std::find(party.GetPartyMembers().begin(), party.GetPartyMembers().end(), target) != party.GetPartyMembers().end();
            switch (action)
            {
            case 1:
            case 2:
            {
                if (action == 1 && !together)
                    break;
                HideNativeMenu("Journal Menu");
                auto& overlay = World::Get().GetOverlayService();
                overlay.SetActive(true);
                if (auto* pApp = overlay.GetOverlayApp())
                    pApp->ExecuteAsync(action == 1 ? "showPauseCoop" : "showPauseOptions");
                if (action == 1) steam.QueueRefreshLobbyState();
                else World::Get().GetGameSettingsService().RequestSettings();
                break;
            }
            case 3: if (!together) steam.QueueHostSession(); break;
            case 4: steam.QueueInviteFriend(); break;
            case 5: if (host) party.SetSessionSettings(!steam.IsLobbyOpen(), {}, true); break;
            case 6:
            case 7:
                HideNativeMenu("Journal Menu");
                if (host)
                    party.EndParty();
                else
                    steam.QueueLeaveSession(true);
                break;
            case 8: if (host && member) party.ChangePartyLeader(target); break;
            case 9: if (host && member) party.KickPartyMember(target); break;
            }
        }
    }
    else
        s_journalWasOpen = false;
    if (s_loadEndsLobby.exchange(false))
    {
        bool holding{};
        {
            std::lock_guard lock(s_lock);
            holding = s_phase != Phase::Idle && s_phase != Phase::Enter && s_phase != Phase::WaitWorld;
        }
        if (holding)
        {
            spdlog::info("Lobby: a save loaded; the lobby ends");
            EndLobby(true);
        }
    }
    std::lock_guard lock(s_lock);
    const auto* pReturnUI = UI::Get();
    if (s_autoDone && s_phase == Phase::Idle && !s_newGameAfterMenu && !s_returnInProgress &&
        pReturnUI && !pReturnUI->GetMenuOpen(BSFixedString("Main Menu")) &&
        !pReturnUI->GetMenuOpen(BSFixedString("Loading Menu")) && !InLobbyWorld())
    {
        if (auto* pPlayer = PlayerCharacter::Get(); pPlayer && pPlayer->parentCell &&
            pPlayer->parentCell->formID != kCampfireCell)
            s_gameplaySeen = true;
    }
    if (s_gameplaySeen && pReturnUI && pReturnUI->GetMenuOpen(BSFixedString("Main Menu")) &&
        !s_newGameAfterMenu && !s_returnInProgress && s_returnRequested.load() == ReturnReason::None)
        RequestReturnToLobby("engine_title");
    const auto requestedReturn = s_returnRequested.exchange(ReturnReason::None);
    if (requestedReturn != ReturnReason::None && s_phase == Phase::Idle && !s_newGameAfterMenu &&
        !s_returnInProgress && SplashWanted())
    {
        s_lastReturnReason = requestedReturn;
        s_returnInProgress = true;
        s_gameplaySeen = false;
        s_autoDone = false;
        s_autoTitleSince = 0;
        s_autoSmokeFadeAt = 0;
        s_splashHeld = false;
        s_holdFrame = 0;
        s_characterIndex = -1;
        ScanCharacters();
        spdlog::info("Lobby: returning to the title ({})", ReturnReasonName(requestedReturn));
        if ((requestedReturn == ReturnReason::PartyLeft || requestedReturn == ReturnReason::Disconnected) &&
            (!pReturnUI || !pReturnUI->GetMenuOpen(BSFixedString("Main Menu"))))
        {
            using TQuit = void();
            POINTER_SKYRIMSE(TQuit, quitToMainMenu, 55490);
            quitToMainMenu.Get()();
        }
    }
    TickLeaveBlack();
    if (s_reshowSplashMenus.exchange(false, std::memory_order_acq_rel) && s_splashActive)
    {
        ShowNativeMenu("Mist Menu");
        ShowNativeMenu("TitleSequence Menu");
    }
    {
        std::string action;
        {
            std::lock_guard actionLock(s_actionLock);
            action.swap(s_pendingAction);
        }
        if (!action.empty())
            RunAction(action);
    }
    if (s_phase != Phase::Idle)
        TickSplash();
    switch (s_phase)
    {
    case Phase::Idle:
        TrackVisits();
        TickSplash();
        RunTitleRequest();
        PollNewGame();
        PollArrival();
        AutoStartAtLaunch();
        return;
    case Phase::Enter:
        s_lobbyWorld = true;
        MT::WorldGeneration.fetch_add(1, std::memory_order_acq_rel);
        s_lobbyWorldReplaced = false;
        // From the main menu this starts a game in that cell as the default Nord, without the intro.
        // Phase first: the load pumps frames that can come back in here before cow returns.
        SetStatus(Phase::WaitWorld, "loading the campfire cell");
        PushPresentation(true, 0.05f, false);
        SetWorldMusicPaused(true);
        SetWorldSoundsPaused(true);
        AudioPreview::StartLobbyPlaylist();
        // The splash carries through the load: smoke with no model, no tip text, no spinner.
        if (s_splashActive)
        {
            s_noModelLoad = true;
            SetMovieVisible("Loading Menu", "_root.Menu_mc._visible", false);
            SetMovieVisible("LoadWaitSpinner", "_root.Menu_mc._visible", false);
        }
        RunConsole(fmt::format("cow Tamriel {} {}", kGridX, kGridY));
        return;
    case Phase::WaitWorld:
    {
        static uint64_t s_inCellSince{};
        KeepMusic();
        // Other actors go as soon as the campfire cell is the player's and has 3D, before the vanilla load screen has
        // finished fading (owner 2026-10-01: "i heard enemies talking before i loaded in"). The cell check is what
        // keeps this off the title screen's placeholder player (the 2026-10-01 13:19 mid-load crash).
        if (auto* pPlayer = PlayerCharacter::Get();
            pPlayer && pPlayer->parentCell && pPlayer->parentCell->formID == kCampfireCell && pPlayer->GetNiNode())
        {
            // The world is in: close the loading screen's window onto the vanilla logo before anything shows.
            PushPresentation(true, 0.8f, false);
            if (const auto cleared = ClearOtherActors())
            {
                const auto* pUI = UI::Get();
                spdlog::info("Lobby: cleared {} other actors on arrival (vanilla load screen open={})", cleared,
                    pUI && pUI->GetMenuOpen(BSFixedString("Loading Menu")));
            }
        }
        if (!PlayerInLobbyCell())
        {
            s_inCellSince = 0;
            // Loading: eases toward 80 % (the load itself reports no progress).
            const float seconds = static_cast<float>(Now() - s_since) / 1000.f;
            PushPresentation(true, 0.05f + 0.75f * (1.f - std::exp(-seconds / 5.f)), false);
            if (Now() - s_since > 90000)
            {
                SetStatus(Phase::Idle, "failed: the campfire cell did not load within 90 s");
                PushPresentation(false, 0.f, false);
                AudioPreview::StopLobbyPlaylist(1500);
                SetWorldMusicPaused(false);
                SetWorldSoundsPaused(false);
                s_noModelLoad = false;
                EndSplash();
            }
            return;
        }
        // Other actors go the moment they load (owner 2026-10-01: enemies walked in right after the load screen).
        ClearOtherActors();
        HoldFadesOff();
        // Settled in the cell for 3 s before touching anything.
        if (!s_inCellSince)
        {
            s_inCellSince = Now();
            BeginLobbyCharacter();
            // A new game starts in first person, and the free camera entered from there leaves the body hidden
            // (owner 2026-10-01: "i think my charcater is invisible"): third person first, settled before tfc.
            ShowBodyForFreeCamera();
            // Anniversary Survival Mode asks "Enable Survival Mode?" from its quest's update timer a few seconds into
            // a new game (survival_mainscript PromptToStartSurvivalMode); it skips the question once this global is 1.
            // Without the plugin the console reports an unknown global and nothing happens.
            RunConsole("set Survival_PlayerHasBeenPrompted to 1");
        }
        PushPresentation(true, 0.8f + 0.15f * std::min(1.f, static_cast<float>(Now() - s_inCellSince) / 1500.f), false);
        if (Now() - s_inCellSince < 1500)
            return;
        if (PollLobbyCharacter(Now() - s_inCellSince, 6000))
            return;
        // The saved character finished (its 3D is rebuilt), or given up on after 20 s.
        if (s_characterApplying)
        {
            const auto status = CharacterSnapshots::ApplyStatus();
            const bool finished = status.rfind("done", 0) == 0 || status.rfind("failed", 0) == 0 || status == "idle";
            if (!finished && Now() - s_inCellSince < 20000)
                return;
            s_characterApplying = false;
            spdlog::info("Lobby: saved character {}", finished ? status.substr(0, 300) : "still applying after 20 s; going on");
            ShowBodyForFreeCamera(); // the rebuilt body, third person again
            s_inCellSince = Now() - 1000;
            return;
        }
        // The body shown and fully faded in before the free camera (give up after 8 s and log the camera state).
        if (!BodyReadyForFreeCamera() && Now() - s_inCellSince < 8000)
            return;
        if (!BodyReadyForFreeCamera())
        {
            auto* pCamera = PlayerCamera::Get();
            spdlog::warn("Lobby: body not confirmed shown before the free camera (camera state {}, fade {:.2f})",
                pCamera && pCamera->state ? static_cast<int>(pCamera->state->id) : -1, PlayerFade());
        }
        else
            spdlog::info("Lobby: body shown, fade {:.2f}, {} ms after arrival", PlayerFade(), Now() - s_inCellSince);
        s_inCellSince = 0;
        SetStatus(Phase::Stage, "staging the scene");
        return;
    }
    case Phase::Stage:
    {
        if (auto* pCamera = PlayerCamera::Get(); pCamera && pCamera->state && pCamera->state->id != kFreeCamera)
        {
            PopStaleFreeCameraContext();
            s_popTfcAfterLoad = false;
        }
        SetSavingDisabled(true);
        SetGodMode(true);
        SetPlayerControlsLocked(true);
        if (const auto cleared = ClearOtherActors())
            spdlog::info("Lobby: cleared {} other actors", cleared);
        InitSlots();
        LoadLayout();
        PlaceSlot(0);
        s_dirtyIdle |= 1u; // the player's idle, once the free camera is up
        RunConsole(fmt::format("fw {}", kWeather));
        RunConsole(fmt::format("set gamehour to {:.0f}", kHour));
        RunConsole("tfc");
        // Only the HUD goes: ToggleMenus (tm) would hide every menu, the splash's title and smoke with it (owner
        // 2026-10-01: "it disappears the moment i load in").
        HideNativeMenu("HUD Menu");
        SetStatus(Phase::Hold, "holding the lobby view");
        s_holding = true;
        PushPresentation(true, 1.f, false);
        s_holdSince = Now();
        s_menuShown = false;
        return;
    }
    case Phase::Hold:
    {
        KeepMusic();
        HoldFadesOff();
        // A moment for the poses and camera to settle under the loading screen, then the scene and the menu.
        if (!s_nativeMenuOpened && Now() - s_holdSince > 600)
        {
            // The real Main Menu (owner 2026-10-01: "i want the exact main menu over our 3d background"): opened
            // without its Mist Menu backdrop, unpaused by the menu queue hook, with its own buttons and cursor.
            ShowPlayer();
            s_nativeMenuOpened = true;
            s_revealAt = Now() + 4000; // fallback: reveal even if the vignette never reports fitted
            // ToggleMenus hides every Scaleform menu, the Main Menu with it: menus back on, only the HUD hidden.
            if (s_menusHidden)
            {
                RunConsole("tm");
                s_menusHidden = false;
            }
            HideNativeMenu("HUD Menu");
            SetPlayerControlsLocked(true); // again: the staging call ran before the world settled
            ShowMainMenuWithData();
            s_primeMenuAt = Now() + 500;
            WatchMainMenuState(30);
        }
        if (s_primeMenuAt && Now() >= s_primeMenuAt)
        {
            s_primeMenuAt = 0;
            PrimeMainMenu();
        }
        KeepMotd();
        if (s_nativeMenuOpened && !s_primeMenuAt)
        {
            TickCharacters();
            TickPartyRole();
        }
        if (s_revealAt && Now() >= s_revealAt && (FramesSteady(500) || Now() - s_revealAt > 4000))
        {
            s_revealAt = 0;
            PushPresentation(false, 1.f, false);
            ReleaseSplash();
        }
        // The texture loads asynchronously; then it follows the visible stage (window resizes, owner 2026-10-01:
        // a 4169x1095 window left it covering the middle only). Twice a second, a few variable reads.
        static uint64_t s_nextVignetteFit{};
        if (s_nativeMenuOpened && !s_primeMenuAt && Now() >= s_nextVignetteFit)
        {
            s_nextVignetteFit = Now() + 500;
            auto* pUI = UI::Get();
            auto* pMenu = pUI ? pUI->FindMenuByName("Main Menu") : nullptr;
            const bool fitted = pMenu && SizeMainMenuVignette(pMenu->uiMovie);
            if (fitted)
            {
                s_sizeVignetteUntil = 0;
                if (s_revealAt)
                    s_revealAt = Now() + 150; // a frame or two for the fitted vignette to draw
            }
            else if (s_sizeVignetteUntil && Now() >= s_sizeVignetteUntil)
            {
                s_sizeVignetteUntil = 0;
                spdlog::warn("Lobby: the vignette texture did not load");
            }
        }
        // Cells keep streaming actors in for a while after the load: every frame at first, then once a second.
        static uint64_t s_nextSweep{};
        if (Now() - s_holdSince < 15000)
            ClearOtherActors();
        if (Now() >= s_nextSweep)
        {
            s_nextSweep = Now() + 1000;
            if (const auto cleared = ClearOtherActors())
                spdlog::info("Lobby: cleared {} other actors", cleared);
            // Anything that knocked the view out of the free camera (it should not happen with nobody else around).
            auto* pCamera = PlayerCamera::Get();
            if (pCamera && pCamera->state && pCamera->state->id != kFreeCamera && Now() - s_since > 2000)
            {
                // Same path as staging: body shown first, or tfc from first person hides it again.
                ShowBodyForFreeCamera();
                if (BodyReadyForFreeCamera())
                {
                    RunConsole("tfc");
                    spdlog::info("Lobby: free camera re-entered (was state {})", pCamera->state->id);
                }
            }
        }
        HoldCamera();
        ApplyEditor();
        RunTitleRequest();
        StartNewGameReset();
        if (s_continuePressed.exchange(false))
        {
            if (StartPartyInstead(PartyStartRequest::kContinue))
                return;
            // The party's own Continue (its checkpoint was not loadable on this PC) is the game's: never the character
            // screen's pick, which put the two PCs in different worlds (2026-10-01).
            const auto chosen = IsSharedCampaignLaunching() ? LobbyCharacter{} : ChosenCharacter();
            if (chosen.Prisoner)
            {
                // A fresh start: the menu's own NEW path (the patched menu presses NEW and confirms it), which the
                // New Game hook turns into the lobby's reset.
                spdlog::info("Lobby: Continue pressed with PRISONER chosen; starting a new game");
                SetMovieNumber("Main Menu", "_root.SkyrimSeamlessLaunchMode", 1.0);
                return;
            }
            PushPresentation(true, 0.05f, false);
            s_continueUnderWay = true;
            if (!chosen.Save.empty() && CheckpointSaves::LoadFile(chosen.Save))
                spdlog::info("Lobby: Continue pressed; loading {}'s save {}", chosen.Name, chosen.Save.filename().string());
            else if (IsSharedCampaignLaunching())
            {
                spdlog::info("Lobby: Continue pressed; loading the last save");
                if (MainMenuInstanceAlive())
                    s_realContinue(); // queues the load (the Main Menu instance still exists)
            }
            else
            {
                spdlog::warn("Lobby: chosen character has no loadable save; Continue stayed in the lobby");
                PushPresentation(false, 1.f, false);
                s_continueUnderWay = false;
                return;
            }
            LobbyService::EndLobby(false);
            PushPresentation(true, 0.05f, false); // EndLobby clears it: keep the cover through the load
            s_arrival = Arrival::WaitSave;
            s_arrivalSince = Now();
            return;
        }
        // Any other load from the lobby (Load in the Journal): put the lobby away without console steps.
        if (s_nativeMenuOpened)
        {
            const auto* pUI = UI::Get();
            if (pUI && pUI->GetMenuOpen(BSFixedString("Loading Menu")))
            {
                spdlog::info("Lobby: a load started from the lobby; ending it");
                LobbyService::EndLobby(true);
                return;
            }
        }
        return;
    }
    }
}

bool DrainTerminalQuitOnMainFrame() noexcept
{
    if (!World::Exists())
        return false;
    if (MT::CutEnabled(6) && MT::CurrentSlot() != MT::Slot::S7)
    {
        MT::M6WrongSlot.fetch_add(1, std::memory_order_relaxed);
        MT::Report();
    }
    auto& steam = World::Get().GetSteamLobbyService();
    if (!steam.HasQuitToMainMenuRequest())
        return false;
    auto* ui = UI::Get();
    if (!ui || ui->GetMenuOpen(BSFixedString("Loading Menu")))
        return false;
    if (!steam.TakeQuitToMainMenuRequest() || ui->GetMenuOpen(BSFixedString("Main Menu")))
        return false;
    using TQuitToMainMenu = void();
    POINTER_SKYRIMSE(TQuitToMainMenu, quitToMainMenu, 55490);
    quitToMainMenu.Get()();
    return true;
}

std::string PauseMenuStateJson() noexcept
{
    auto* pUI = UI::Get();
    const bool open = pUI && pUI->GetMenuOpen(BSFixedString("Journal Menu")) && MenuMovie("Journal Menu");
    const bool systemTab = open && MovieNumber("Journal Menu", "_root.QuestJournalFader.Menu_mc.iCurrentTab") == 2.0;
    const bool coopOpen = systemTab &&
        MovieNumber("Journal Menu", "_root.QuestJournalFader.Menu_mc.SystemFader.Page_mc.iCurrentState") == 19.0;
    const auto& party = World::Get().GetPartyService();
    const auto& members = party.GetPartyMembers();
    const int role = party.IsInParty() && members.size() > 1 ? (party.IsLeader() ? 1 : 2) : 0;
    const char* actions = role == 0 ? "[\"HOST\",\"INVITE FRIEND\"]" :
        role == 1 ? "[\"PARTY\",\"INVITE FRIEND\",\"OPEN/CLOSE SESSION\",\"MAKE HOST\",\"REMOVE PLAYER\",\"END PARTY\"]" :
                    "[\"PARTY\",\"INVITE FRIEND\",\"LEAVE PARTY\"]";
    return fmt::format("\"open\":{},\"systemTab\":{},\"coopOpen\":{},\"role\":\"{}\",\"entries\":{},\"coopEntries\":{},\"memberCount\":{}",
        open, systemTab, coopOpen, role == 0 ? "solo" : role == 1 ? "host" : "guest",
        systemTab && !coopOpen ? "[\"CO-OP\",\"OPTIONS\"]" : "[]", coopOpen ? actions : "[]", members.size());
}

bool ChosenCharacterReady() noexcept
{
    return s_charactersReady.load();
}

bool PostPauseMenuRequest(int aAction) noexcept
{
    auto* pUI = UI::Get();
    if (!pUI || !pUI->GetMenuOpen(BSFixedString("Journal Menu")) || aAction < 1 || aAction > 9)
        return false;
    SetMovieNumber("Journal Menu", "_root.SSMJournalTarget", 0);
    SetMovieNumber("Journal Menu", "_root.SSMJournalAction", aAction);
    return true;
}

bool PauseMenuAction(const std::string& acAction) noexcept
{
    auto* pUI = UI::Get();
    if (!pUI || !pUI->GetMenuOpen(BSFixedString("Journal Menu")) || !MenuMovie("Journal Menu") ||
        MovieNumber("Journal Menu", "_root.QuestJournalFader.Menu_mc.iCurrentTab") != 2.0)
        return false;
    static const std::map<std::string, int> actions{
        {"co_op", 10}, {"coop", 10}, {"options", 11}, {"leave_party", 12}, {"end_party", 13},
        {"host", 14}, {"invite_friend", 15}, {"open_close_session", 16},
        {"open_session", 16}, {"close_session", 16},
        {"make_host", 17}, {"remove_player", 18}, {"party", 19}};
    std::string normalized = acAction;
    std::transform(normalized.begin(), normalized.end(), normalized.begin(), [](unsigned char c) {
        return c == ' ' || c == '-' ? '_' : static_cast<char>(std::tolower(c));
    });
    const auto it = actions.find(normalized);
    if (it == actions.end())
        return false;
    const auto state = MovieNumber("Journal Menu",
        "_root.QuestJournalFader.Menu_mc.SystemFader.Page_mc.iCurrentState");
    if ((it->second == 10 || it->second == 11) ? state != 0.0 : state != 19.0)
        return false;
    SetMovieNumber("Journal Menu", "_root.SSMJournalBridgeAction", it->second);
    return true;
}

void RequestFullLeave() noexcept
{
    s_fullLeaveRequested.store(true);
}

void SetCameraReleased(bool aRelease) noexcept
{
    std::lock_guard lock(s_lock);
    if (aRelease)
    {
        s_cameraReleased = true;
        spdlog::info("Lobby: camera released for framing");
        return;
    }
    if (auto* pFields = FreeCameraFields())
    {
        s_pose = {pFields[0], pFields[1], pFields[2], pFields[3], pFields[4]};
        spdlog::info("Lobby: camera framed at x={:.1f} y={:.1f} z={:.1f} pitch={:.4f} yaw={:.4f}", s_pose.X, s_pose.Y,
            s_pose.Z, s_pose.Pitch, s_pose.Yaw);
    }
    s_cameraReleased = false;
}

void SetEditorOpen(bool aOpen) noexcept
{
    std::lock_guard lock(s_lock);
    s_editorOpen = aOpen && s_phase == Phase::Hold;
}

bool SetSlotIdle(int aSlot, const std::string& acIdle) noexcept
{
    std::lock_guard lock(s_lock);
    if (aSlot < 0 || aSlot >= static_cast<int>(s_slots.size()) ||
        std::none_of(kIdles.begin(), kIdles.end(), [&](const Idle& i) { return acIdle == i.Name || acIdle == i.Event; }))
        return false;
    s_slots[aSlot].Idle = IdleIndex(acIdle);
    s_dirtyIdle |= 1u << aSlot;
    return true;
}

void RequestAction(const std::string& acAction) noexcept
{
    if (acAction != "continue" && acAction != "load" && acAction != "newgame" && acAction != "quit")
        return;
    std::lock_guard lock(s_actionLock);
    s_pendingAction = acAction;
}

void RequestTitleSequence(const std::string& acSequence) noexcept
{
    std::lock_guard lock(s_titleLock);
    s_titleRequest = acSequence;
}

size_t LoreDump() noexcept
{
    const auto entries = LoreEntries();
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::ofstream out(std::filesystem::path(path).parent_path() / "Data" / "SkyrimTogetherReborn" / "lobby_lore.txt", std::ios::trunc);
    for (const auto& entry : entries)
        out << entry << "\n---\n";
    out << VersionLine() << "\n";
    return entries.size();
}

void RequestSave() noexcept
{
    std::lock_guard lock(s_lock);
    s_saveRequested = true;
}

bool EditorOpen() noexcept
{
    return s_editorOpen;
}

bool MenuShown() noexcept
{
    return s_menuShown;
}

bool IsHolding() noexcept
{
    return s_holding;
}

void OnSplashTitleQueued(IMenu* apMenu) noexcept
{
    if (!apMenu || (!s_splashActive && !s_releaseAt))
        return;
    // Above the smoke's menu (depth 8): see the restore in TickSplash.
    *reinterpret_cast<int8_t*>(reinterpret_cast<uint8_t*>(apMenu) + 0x18) = 9;
    // Opened again by the lobby's load: straight onto the held frame, no new fade-in.
    if (s_holdFrame > 0 && apMenu->uiMovie)
    {
        using TInvoke = bool(void*, const char*, const char*, ...);
        static VersionDbPtr<void> s_invoke(82664);
        const bool ok = reinterpret_cast<TInvoke*>(s_invoke.GetPtr())(apMenu->uiMovie, "_root.sequence4.gotoAndStop", "%d",
            static_cast<int>(s_holdFrame));
        spdlog::info("Lobby: splash title queued again, held frame set={}", ok);
    }
    if (s_titleBlackUp)
        EnsureTitleBlack(100.0);
}

// The game's own title at launch, while its data loads: the frame loop (and so the splash) only starts once the title
// is up, about five seconds in (2026-10-01 log), and until then the dragon and the vanilla Main Menu showed (owner:
// "i can still see the original main menu flash"). As the game opens them: no dragon, the Main Menu transparent.
void OnLaunchMenuQueued(IMenu* apMenu, bool aMainMenu) noexcept
{
    const bool returnTitle = s_phase == Phase::Idle && !s_newGameAfterMenu &&
        ((aMainMenu && s_gameplaySeen) || s_returnRequested.load() != ReturnReason::None || s_returnInProgress);
    if (returnTitle && aMainMenu && s_gameplaySeen && s_returnRequested.load() == ReturnReason::None)
        RequestReturnToLobby("engine_title");
    const bool launchTitle = s_phase == Phase::Idle && ((!s_splashActive && !s_splashHeld) || returnTitle);
    if (!SplashWanted() || !(launchTitle || s_newGameAfterMenu) || !apMenu)
        return;
    ClearMistModel();
    if (aMainMenu && apMenu->uiMovie)
    {
        struct Value
        {
            void* ObjectInterface{};
            uint32_t Type{3};
            uint32_t Pad{};
            double Number{};
        } value;
        (void)value;
        LockHiddenTitle(apMenu->uiMovie);
    }
    spdlog::info("Lobby: launch title {} queued: kept hidden for the splash", aMainMenu ? "Main Menu" : "Mist Menu");
}

void OnSaveLoadHandled(bool aLoaded) noexcept
{
    CheckpointSaves::OnSaveLoadHandled(aLoaded);
    s_saveLoadHandled = s_leaveBlackAt != 0;
    s_loadEndsLobby = true;
}

bool HoldsRemote(uint32_t aFormId) noexcept
{
    if (!aFormId)
        return false;
    for (const auto& seated : s_seated)
        if (seated.load(std::memory_order_relaxed) == aFormId)
            return true;
    return false;
}

bool InLobbyWorld() noexcept
{
    return s_lobbyWorld;
}

bool NewGameIntroPending() noexcept
{
    return s_coverUntilLoaded && !s_newGameAfterMenu;
}

bool SharedWorld() noexcept
{
    auto* player = PlayerCharacter::Get();
    auto* ui = UI::Get();
    auto* main = MainSingleton();
    return player && player->parentCell && player->GetNiNode() && !InLobbyWorld() &&
        ui && !ui->GetMenuOpen(BSFixedString("Main Menu")) && main && !main[0x11] && !main[0x12] &&
        !MT::Loading() && MT::CurrentLoadPhase.load(std::memory_order_acquire) == MT::LoadPhase::Idle;
}

void RunWorldContextStepOnMainFrame() noexcept
{
    static uint32_t s_checkedGeneration{};
    const auto generation = MT::WorldGeneration.load(std::memory_order_acquire);
    if ((!MT::CutM6bEnabled() && !s_popTfcAfterLoad.load()) || !generation ||
        (generation == s_checkedGeneration && !s_popTfcAfterLoad.load()) || !SharedWorld())
        return;
    auto* camera = PlayerCamera::Get();
    if (!camera || !camera->state || camera->state->id == kFreeCamera)
        return;
    PopStaleFreeCameraContext();
    spdlog::info("sp_orphan_context world_generation={}", generation);
    if (s_popTfcAfterLoad.exchange(false))
    {
        SetGodMode(false);
        SetSavingDisabled(false);
        SetPlayerControlsLocked(false);
        spdlog::info("sp_lobby_residue controls, god_mode, in_chargen");
    }
    if (auto* controls = PlayerControls::GetInstance(); controls && controls->Data.remapMode &&
        (!UI::Get() || !UI::Get()->HasUnpausedMenuOpen()))
    {
        controls->Data.remapMode = false;
        controls->MarkInputRelease();
        s_menuInputBlocked = false;
        spdlog::info("sp_lobby_residue remap_mode");
    }
    s_checkedGeneration = generation;
}

bool HoldsMenuOpen(const char* apMenu) noexcept
{
    return s_splashActive && apMenu &&
           (std::strcmp(apMenu, "Mist Menu") == 0 || std::strcmp(apMenu, "TitleSequence Menu") == 0);
}

void OnRawMouse(const RAWMOUSE& acMouse) noexcept
{
    ++s_rawEvents; // counted always (diagnostics); deltas only feed the editor's fly camera
    if (!s_editorOpen)
    {
        if (acMouse.usFlags & MOUSE_MOVE_ABSOLUTE)
            ++s_absoluteEvents;
        else if (acMouse.lLastX || acMouse.lLastY)
            ++s_movingEvents;
        return;
    }
    if (acMouse.usFlags & MOUSE_MOVE_ABSOLUTE)
    {
        // Absolute devices (remote desktop, streaming, tablets) report 0..65535 positions: turn them into deltas.
        ++s_absoluteEvents;
        static LONG s_lastX = -1, s_lastY = -1;
        const bool virtualDesktop = (acMouse.usFlags & MOUSE_VIRTUAL_DESKTOP) != 0;
        const int width = GetSystemMetrics(virtualDesktop ? SM_CXVIRTUALSCREEN : SM_CXSCREEN);
        const int height = GetSystemMetrics(virtualDesktop ? SM_CYVIRTUALSCREEN : SM_CYSCREEN);
        const LONG x = static_cast<LONG>(static_cast<int64_t>(acMouse.lLastX) * width / 65535);
        const LONG y = static_cast<LONG>(static_cast<int64_t>(acMouse.lLastY) * height / 65535);
        if (s_lastX >= 0 && (x != s_lastX || y != s_lastY))
        {
            s_mouseDx += x - s_lastX;
            s_mouseDy += y - s_lastY;
            ++s_movingEvents;
        }
        s_lastX = x;
        s_lastY = y;
    }
    else if (acMouse.lLastX || acMouse.lLastY)
    {
        s_mouseDx += acMouse.lLastX;
        s_mouseDy += acMouse.lLastY;
        ++s_movingEvents;
    }
    if (acMouse.usButtonFlags & RI_MOUSE_WHEEL)
        s_wheel += static_cast<SHORT>(acMouse.usButtonData);
}

void DrawEditor() noexcept
{
    // Never wait on the render thread: a lobby step can be waiting on a frame, so a busy lock skips this frame.
    std::unique_lock lock(s_lock, std::try_to_lock);
    if (!lock.owns_lock() || !s_editorOpen || s_phase != Phase::Hold)
        return;
    ImGui::SetNextWindowSize(ImVec2(430, 620), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImVec2(40, 60), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Lobby layout"))
    {
        ImGui::End();
        return;
    }
    ImGui::TextWrapped("Hold right mouse to look around. While holding: WASD fly, E / Space up, Q / C down, Shift "
                       "faster, mouse wheel changes the speed. F2 hides or shows the cursor. Ctrl+Shift+F12 closes.");
    ImGui::Separator();
    ImGui::SliderFloat("Fly speed", &s_flySpeed, 20.f, 5000.f, "%.0f", ImGuiSliderFlags_Logarithmic);
    ImGui::DragFloat3("Camera xyz", &s_pose.X, 2.f);
    float angles[2]{s_pose.Pitch * 57.29578f, s_pose.Yaw * 57.29578f};
    if (ImGui::DragFloat2("Pitch / yaw", angles, 0.25f))
    {
        s_pose.Pitch = std::clamp(angles[0], -89.f, 89.f) / 57.29578f;
        s_pose.Yaw = angles[1] / 57.29578f;
    }
    ImGui::Separator();

    for (size_t i = 0; i < s_slots.size(); ++i)
    {
        ImGui::PushID(static_cast<int>(i));
        auto& slot = s_slots[i];
        if (ImGui::CollapsingHeader(i == 0 ? "Mark 0 (you)" : fmt::format("Mark {} (stand-in)", i).c_str(),
                i == 0 ? ImGuiTreeNodeFlags_DefaultOpen : 0))
        {
            if (ImGui::DragFloat3("Position", &slot.X, 1.f))
                s_dirtyPlace |= 1u << i;
            if (ImGui::SliderFloat("Heading", &slot.HeadingDeg, -180.f, 180.f, "%.0f deg"))
                s_dirtyPlace |= 1u << i;
            if (ImGui::Button("Face camera"))
            {
                slot.HeadingDeg = HeadingTo(Point{slot.X, slot.Y, slot.Z}, Point{s_pose.X, s_pose.Y, s_pose.Z}) * 57.29578f;
                s_dirtyPlace |= 1u << i;
            }
            ImGui::SameLine();
            if (ImGui::Button("Face fire"))
            {
                slot.HeadingDeg = HeadingTo(Point{slot.X, slot.Y, slot.Z}, kFire) * 57.29578f;
                s_dirtyPlace |= 1u << i;
            }
            ImGui::SameLine();
            if (ImGui::Button("Put under camera"))
            {
                slot.X = s_pose.X;
                slot.Y = s_pose.Y;
                s_dirtyPlace |= 1u << i;
            }
            if (ImGui::BeginCombo("Pose", kIdles[slot.Idle].Name))
            {
                for (int k = 0; k < static_cast<int>(kIdles.size()); ++k)
                    if (ImGui::Selectable(kIdles[k].Name, k == slot.Idle))
                    {
                        slot.Idle = k;
                        s_dirtyIdle |= 1u << i;
                    }
                ImGui::EndCombo();
            }
            ImGui::SameLine();
            if (ImGui::Button("Replay"))
                s_dirtyIdle |= 1u << i;
        }
        ImGui::PopID();
    }
    ImGui::Separator();
    if (ImGui::Button("Save layout"))
        s_saveRequested = true;
    ImGui::SameLine();
    if (ImGui::Button("Replay all idles"))
        s_dirtyIdle = 0x1F;
    if (!s_editorMessage.empty())
        ImGui::TextWrapped("%s", s_editorMessage.c_str());
    ImGui::End();
}

// Test diagnostics: the player's drawn body (owner 2026-10-01: "sometimes my character loads and sometimes it doesnt").
std::string PlayerBodyJson() noexcept
{
    auto* pPlayer = PlayerCharacter::Get();
    auto* pRoot = PlayerRoot(false);
    auto* pFirst = PlayerRoot(true);
    if (!pPlayer || !pRoot)
        return "\"body\":null,";
    auto* pBytes = reinterpret_cast<uint8_t*>(pRoot);
    std::string children;
    if (auto* pNode = pRoot->AsNode())
        for (uint16_t i = 0; i < pNode->children.length && i < 12; ++i)
            if (auto* pChild = pNode->children.data[i])
                children += fmt::format("{}\"{:X}\"", children.empty() ? "" : ",", *reinterpret_cast<uint32_t*>(reinterpret_cast<uint8_t*>(pChild) + 0xF4));
    return fmt::format("\"body\":{{\"root\":\"{:X}\",\"niNode\":\"{:X}\",\"first\":\"{:X}\",\"flags\":\"{:X}\",\"fade\":[{:.3f},{:.3f},{:.3f},{:.3f},{:.3f},{:.3f}],"
                       "\"childFlags\":[{}],\"beb\":{},\"ready\":{}}},",
        reinterpret_cast<uintptr_t>(pRoot), reinterpret_cast<uintptr_t>(pPlayer->GetNiNode()), reinterpret_cast<uintptr_t>(pFirst),
        *reinterpret_cast<uint32_t*>(pBytes + 0xF4), *reinterpret_cast<float*>(pBytes + 0x128), *reinterpret_cast<float*>(pBytes + 0x12C),
        *reinterpret_cast<float*>(pBytes + 0x130), *reinterpret_cast<float*>(pBytes + 0x134), *reinterpret_cast<float*>(pBytes + 0x138),
        *reinterpret_cast<float*>(pBytes + 0x13C), children, *(reinterpret_cast<uint8_t*>(pPlayer) + 0xBEB), BodyReadyForFreeCamera());
}

std::string Status() noexcept
{
    std::unique_lock lock(s_lock, std::try_to_lock);
    if (!lock.owns_lock())
        return "{\"status\":\"busy\"}";
    auto* pCamera = PlayerCamera::Get();
    const auto cameraState = pCamera && pCamera->state ? static_cast<int>(pCamera->state->id) : -1;
    auto* pPlayer = PlayerCharacter::Get();
    return fmt::format("{{\"editor\":{},\"rawMouse\":{},\"absolute\":{},\"moving\":{},\"held\":{},\"background\":{},\"pending\":[{},{}],"
                       "\"pose\":[{:.0f},{:.0f},{:.0f},{:.3f},{:.3f}],",
               s_editorOpen.load(), s_rawEvents.load(), s_absoluteEvents.load(), s_movingEvents.load(), s_heldFrames, s_backgroundFrames, s_mouseDx.load(),
               s_mouseDy.load(), s_pose.X, s_pose.Y, s_pose.Z, s_pose.Pitch, s_pose.Yaw) +
           fmt::format("\"overlay\":{},", World::Get().GetOverlayService().RenderStats()) +
           PlayerBodyJson() +
           fmt::format("\"status\":\"{}\",\"cameraState\":{},\"playerHidden\":{},\"cell\":\"{:X}\",\"player\":[{:.0f},{:.0f},{:.0f}]}}",
        s_status, cameraState, pPlayer && pPlayer->GetNiNode() ? (pPlayer->GetNiNode()->flags & 1) != 0 : true,
        pPlayer && pPlayer->parentCell ? pPlayer->parentCell->formID : 0,
        pPlayer ? pPlayer->position.x : 0.f, pPlayer ? pPlayer->position.y : 0.f, pPlayer ? pPlayer->position.z : 0.f);
}

std::string ChosenOwnPlaythroughKey() noexcept
{
    return ::ChosenOwnPlaythroughKey();
}

std::filesystem::path ChosenOwnSnapshotPath() noexcept
{
    return ::ChosenOwnSnapshotPath();
}

bool ChosenCharacterForNewGame(CharacterSnapshot& aOut, bool& aFullState, std::string& aChoice) noexcept
{
    return ReadChosenCharacterForNewGame(aOut, aFullState, aChoice);
}

std::string ChosenCharacterName() noexcept
{
    const auto chosen = ChosenCharacter();
    return chosen.RawName.empty() ? chosen.Name : chosen.RawName;
}
} // namespace LobbyService
