#pragma once

// Matched checkpoint saves: every save the party leader makes during a shared session is
// mirrored on every member as "SSC_<id>.ess", so a later Continue loads the same world on each
// PC instead of each PC's own last save (measured: the host continued at MQ101 stage 75 while
// the follower continued at stage 160).
//
// Saves only ever go through the engine's own save paths: calling Save_Impl from our frame
// update never returned (a 0-byte SSC_*.ess.tmp was left and the game hung). The leader's
// checkpoint is a copy of the save it just made; other members queue an ordinary save
// (BGSSaveLoadManager task, as Papyrus Game.RequestSave does) and copy that once written.
#include <filesystem>
#include <string>
#include <vector>
#include <Messages/CheckpointRoster.h>

namespace CheckpointSaves
{
// Consumes the flag the native save hook sets when the local game wrote a save of its own.
[[nodiscard]] bool TakeLocalSaveMade() noexcept;
// A fresh checkpoint ID for the leader to announce.
[[nodiscard]] String NewCheckpointId(uint64_t aEpoch) noexcept;
// Starts making checkpoint acId: queues a save, or with aReuseOwnRecentSave (the leader, whose save announced it)
// reuses a save made in the last 30 s.
void Begin(const String& acId, bool aReuseOwnRecentSave) noexcept;
// Finishes a begun checkpoint once its save is on disk. Returns true when nothing is pending.
bool Poll() noexcept;
// Records the main-loop thread (36564's caller); a save requested on any other thread is queued instead. Also runs a
// load queued by QueueLoad.
void MarkMainThread() noexcept;
void DrainRespawnOnMainFrame() noexcept;
void DrainSaveWorkOnMainFrame() noexcept;
void DrainTerminalOnMainFrame() noexcept;
[[nodiscard]] bool HasPendingTerminal() noexcept;
[[nodiscard]] bool HasPendingQuit() noexcept;
// The native save-load handler has finished; confirms a successful load or clears a refused one.
void OnSaveLoadHandled(bool aLoaded) noexcept;
// Publish a changed form's save flags on the main frame after a worker applied replicated state.
void QueueSaveChange(uint32_t aFormId, uint32_t aFlags) noexcept;
// Remove a temporary reference's existing change-map entry on the main frame.
void QueueSaveErase(uint32_t aFormId) noexcept;
// Test bench: load SSC_<id> at the start of the next main loop frame (works at the main menu, where the app update
// does not run).
void QueueLoad(const String& acId) noexcept; // "file:<save file name without .ess>" loads any save
// This PC's Skyrim saves folder.
[[nodiscard]] std::filesystem::path SavesFolder() noexcept;
// The last QueueLoad: 0 while waiting for the main loop, 1 when the engine started it, -1 when it was refused.
[[nodiscard]] int QueuedLoadState() noexcept;
// A checkpoint id is safe as a file name: letters, digits, '_' and '-', at most 64 characters.
[[nodiscard]] bool ValidId(const String& acId) noexcept;
// True while a begun checkpoint's save has not been copied yet.
[[nodiscard]] bool IsPending() noexcept;
// SSC_<id>.ess in this PC's saves folder.
[[nodiscard]] std::filesystem::path PathOf(const String& acId) noexcept;
// Writes a save received from the leader as SSC_<id>.ess (atomically). Drop-in saves ("dropin_*") are not listed as
// shared checkpoints.
bool WriteReceived(const String& acId, const std::string& acBytes) noexcept;
// The newest checkpoint this PC wrote, or empty.
[[nodiscard]] String Latest() noexcept;
[[nodiscard]] Vector<String> Available(size_t aMaximum, bool aRequireRoster = true) noexcept;
// The newest checkpoints on this PC (at most aMaximum) as a JSON array for the lobby picker: id, player, level,
// location, gameDate, savedMs and image (the save's own screenshot as a data URL).
[[nodiscard]] std::string ListJson(size_t aMaximum) noexcept;
// The characters this PC can join with: the newest .snap per character name beside its saves, as a JSON array (path,
// name, level, location, savedMs, image from the save's screenshot).
[[nodiscard]] std::string CharactersJson(size_t aMaximum) noexcept;
// One playthrough this PC has saves of, for the lobby's character picker: its newest save and the character file to
// show it with (the save's own .snap, else the newest .snap of the playthrough; empty when there is none). A playthrough
// is what Skyrim's own Load screen groups by: the ID in an ordinary save's file name (Save1_D8669625_..., the hex
// after the save's kind); co-op checkpoints (SSC_*) use a separate character-name key space.
struct SavedCharacter
{
    std::string Key; // the hex ID, "name:<character>", or "ssc:name:<character>"
    uint32_t Id{};   // the playthrough ID (0 for a name-grouped one), as Skyrim's character list entries carry it
    std::string Name;
    uint32_t Level{};
    std::string Location;
    std::filesystem::path Save;
    std::filesystem::path Snap;
    bool Coop{};
    int64_t SavedMs{};
};
// The characters of this PC's saves, newest first, at most aMaximum. Reads only file headers; any thread.
[[nodiscard]] std::vector<SavedCharacter> SavedCharacters(size_t aMaximum) noexcept;
// The follower's own playthrough, selected before the shared world loads. Empty clears it.
void SetCarryHomeSource(const std::string& acKey) noexcept;
// Match a drop-in character file to one of this PC's ordinary playthroughs.
void SetCarryHomeSourceFromSnapshot(const std::filesystem::path& acSnapshot) noexcept;
// Capture the follower now (main game thread). A fresh shared character has no source and is skipped.
bool WriteCarryHome() noexcept;
[[nodiscard]] bool CarryHomeReady() noexcept;
// Test bench: queue the engine's own save request (35769, 0xF0000080, as Game.RequestSave); false without a manager.
bool RequestNativeSave() noexcept;
// Test bridge status for the selected source.
[[nodiscard]] std::string CarryHomeStatus() noexcept;
[[nodiscard]] std::string CurrentCharacterKey() noexcept;
[[nodiscard]] std::string CharacterKeyForSnapshot(const std::filesystem::path& acSnapshot) noexcept;
bool WriteRoster(const String& acId, const Vector<RosterPosition>& acEntries) noexcept;
[[nodiscard]] std::string RosterJson(const String& acId) noexcept;
// Deletes every save of one playthrough (SavedCharacter::Key: its .ess files and their .skse co-saves and .snap
// character files) by moving them to the Windows Recycle Bin. Returns how many saves went.
size_t DeleteCharacterSaves(const std::string& acKey) noexcept;
// Loads any save in the saves folder the way the console LoadGame command does. False if the load did not start.
bool LoadFile(const std::filesystem::path& acSave) noexcept;
// True if this PC holds SSC_<id>.ess.
[[nodiscard]] bool Has(const String& acId) noexcept;
// Loads SSC_<id> the way the console LoadGame command does. False if the load did not start.
bool Load(const String& acId) noexcept;
} // namespace CheckpointSaves
