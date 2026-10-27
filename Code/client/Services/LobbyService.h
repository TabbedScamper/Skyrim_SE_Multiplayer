#pragma once

#include <filesystem>
#include <string>

struct IMenu;
struct CharacterSnapshot;

// The live party lobby on the main menu (docs/MAIN_MENU_LOBBY_DESIGN.md): from the title screen the game enters a
// campfire spot from Interesting Locations by DrainvolOFF, the HUD is hidden, saving is off, a fixed free camera looks
// across the fire at the party, and the weather and time of day set the mood. First stage: the local player, posed on
// the first mark, as the default Nord prisoner a new game starts with. Runs on the main thread (main-loop hook).
namespace LobbyService
{
// Starts the lobby from the main menu (test entry point: lobby_preview). False when one is already running.
bool Begin() noexcept;
// Thread-safe request from a quit, party leave, or disconnect. The main frame drives the title reset.
void RequestReturnToLobby(const char* apReason) noexcept;
// Test bench: post a pause-menu request code as the CO-OP list's own buttons do (6 = confirmed leave, 7 = confirmed end).
bool PostPauseMenuRequest(int aAction) noexcept;
// The character list is read (the lobby's choice can be resolved).
bool ChosenCharacterReady() noexcept;
[[nodiscard]] std::string ReturnStatus() noexcept;
// Leaves the lobby view (shows the HUD again, ends the free camera); the next load replaces the lobby world.
void End() noexcept;
// As End(); with aLoadUnderWay, skips the console steps (tm, tfc) a load in progress replaces anyway.
void EndLobby(bool aLoadUnderWay) noexcept;
// True while the loaded world is the lobby's throwaway one: from its load until another game has loaded in its place.
// A party's start must not count it as the shared world being ready.
[[nodiscard]] bool InLobbyWorld() noexcept;
// True only when the local player is in an ordinary loaded world, outside the live lobby and reset.
[[nodiscard]] bool SharedWorld() noexcept;
// This PC's own New Game has started (52118 ran) and its first load has not finished: the intro's no-cell stretch, where
// a follower must apply the host's first MQ101 stage before it gets a cell. Interim for the Atlas CampaignWorld predicate.
[[nodiscard]] bool NewGameIntroPending() noexcept;
// The selected ordinary playthrough; empty for PRISONER and SSC campaigns.
[[nodiscard]] std::string ChosenOwnPlaythroughKey() noexcept;
[[nodiscard]] std::filesystem::path ChosenOwnSnapshotPath() noexcept;
// Freeze the locally chosen named character when New Game Together begins. fullState is false for legacy saves.
bool ChosenCharacterForNewGame(CharacterSnapshot& aOut, bool& aFullState, std::string& aChoice) noexcept;
std::string ChosenCharacterName() noexcept;
// The engine's save load handler finished (any thread that runs it; the main thread in practice).
void OnSaveLoadHandled(bool aLoaded) noexcept;
// A party member's copy seated at the lobby's fire: its owner's movement and actions are not applied (any thread).
[[nodiscard]] bool HoldsRemote(uint32_t aFormId) noexcept;
// Called every main-loop frame before the engine's frame (SkyrimVM64.cpp HookMainLoop).
void OnMainFrame() noexcept;
void RunWorldContextStepOnMainFrame() noexcept;
bool DrainTerminalQuitOnMainFrame() noexcept;
// Native Journal movie bridge; actions are routed through its own list/confirmation handlers.
[[nodiscard]] std::string PauseMenuStateJson() noexcept;
bool PauseMenuAction(const std::string& acAction) noexcept;
void RequestFullLeave() noexcept;
// Framing tool: release the fixed camera so the free camera can be flown by hand (aRelease true), then capture its
// current position and angle as the lobby camera (aRelease false) and keep holding it there.
void SetCameraReleased(bool aRelease) noexcept;
// Hidden layout editor (Ctrl+Shift+F12 while the lobby holds, or lobby_edit): an ImGui panel to move the camera and
// the five party marks (stand-in copies of the player fill the empty ones), pick each mark's idle, and save the layout
// to Data\SkyrimTogetherReborn\lobby_layout.json, which the lobby loads the next time it stages.
void SetEditorOpen(bool aOpen) noexcept;
[[nodiscard]] bool EditorOpen() noexcept;
// The lobby menu is up (it owns the cursor; F2 brings the cursor back after Escape).
[[nodiscard]] bool MenuShown() noexcept;
// The scene is set (Hold): the Main Menu opened over it must not pause the game (UI.cpp menu queue hook).
[[nodiscard]] bool IsHolding() noexcept;
// The splash keeps the smoke ("Mist Menu") and the title ("TitleSequence Menu") open: their hide messages are held
// back while it shows (CreatorTogether's menu message hook).
[[nodiscard]] bool HoldsMenuOpen(const char* apMenu) noexcept;
// The splash's title menu is being put on the menu stack (UI_AddToActiveQueue): its depth and held frame.
void OnSplashTitleQueued(IMenu* apMenu) noexcept;
// The game's own title menus at launch (Mist Menu, Main Menu), queued before the frame loop runs: kept hidden.
void OnLaunchMenuQueued(IMenu* apMenu, bool aMainMenu) noexcept;
// Raw mouse input (InputService, window thread): look and flight-speed deltas for the editor's fly camera.
void OnRawMouse(const RAWMOUSE& acMouse) noexcept;
// ImGui draw callback (render thread): only edits the layout; the main frame applies it.
void DrawEditor() noexcept;
// Test entry points for the same editor actions (lobby_idle, lobby_save).
bool SetSlotIdle(int aSlot, const std::string& acIdle) noexcept;
void RequestSave() noexcept;
// Test: writes the news panel's lore entries to Data\SkyrimTogetherReborn\lobby_lore.txt; returns how many.
[[nodiscard]] size_t LoreDump() noexcept;
// Test: shows the intro title menu playing a sequence ("Sequence1".."Sequence4"), or "hide".
void RequestTitleSequence(const std::string& acSequence) noexcept;
// Lobby menu buttons (UI "lobbyAction", any thread; run on the main frame): "continue" loads the newest own save,
// "load" opens the Journal for its load list, "newgame" returns to the main menu and starts its New Game, "quit".
void RequestAction(const std::string& acAction) noexcept;
// One line for tests: phase and what happened.
[[nodiscard]] std::string Status() noexcept;
} // namespace LobbyService
