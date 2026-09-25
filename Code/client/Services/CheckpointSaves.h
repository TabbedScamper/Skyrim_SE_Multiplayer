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
namespace CheckpointSaves
{
// Consumes the flag the native save hook sets when the local game wrote a save of its own.
[[nodiscard]] bool TakeLocalSaveMade() noexcept;
// A fresh checkpoint ID for the leader to announce.
[[nodiscard]] String NewCheckpointId(uint64_t aEpoch) noexcept;
// Starts making checkpoint acId: reuses a save made in the last 30 s, otherwise queues one.
void Begin(const String& acId) noexcept;
// Finishes a begun checkpoint once its save is on disk. Returns true when nothing is pending.
bool Poll() noexcept;
// The newest checkpoint this PC wrote, or empty.
[[nodiscard]] String Latest() noexcept;
// True if this PC holds SSC_<id>.ess.
[[nodiscard]] bool Has(const String& acId) noexcept;
// Loads SSC_<id> the way the console LoadGame command does. False if the load did not start.
bool Load(const String& acId) noexcept;
} // namespace CheckpointSaves
