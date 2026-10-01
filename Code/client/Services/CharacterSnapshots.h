#pragma once

#include <Structs/CharacterSnapshot.h>

#include <filesystem>
#include <string>

// Phase 1 of the guest character (owner design 2026-09-30): read the local player's character, never its world.
// Apply comes later, field by field, through the engine's own setters.
namespace CharacterSnapshots
{
// Reads the local player. Main thread, in game, no RaceSex or loading menu. False with a reason otherwise.
bool Capture(CharacterSnapshot& aOut, std::string& aError) noexcept;
// A JSON summary of a snapshot (counts, level, skills, names of a few forms) for the test bridge.
std::string Describe(const CharacterSnapshot& acSnapshot) noexcept;
// The local player's live look record (race, sex, head parts, face sliders, weight, skin and hair colour, tint
// layers) as JSON fields, to compare two characters by meaning rather than by saved bytes.
std::string DescribeLiveLook() noexcept;
// Makes the local player this character, except its appearance (a later pass): level, XP, skills, perk points, base
// values, dragon souls, perks, spells, shouts, words and personal items, through the game's own setters. This world's
// quest items are kept. Main thread, in game, same gates as Capture. aReport receives a JSON summary of the changes.
bool Apply(const CharacterSnapshot& acSnapshot, std::string& aReport, std::string& aError) noexcept;
// The whole character including appearance, on the main game frame over a few frames: Apply, then the appearance
// record (race switch when needed), a 3D rebuild, and once the new head exists the face tints. Poll ApplyStatus.
void QueueApply(CharacterSnapshot aSnapshot) noexcept;
// "idle", "applying", "rebuilding", "done: {report}" or "failed: reason".
std::string ApplyStatus() noexcept;
// Runs the queued apply; called at the start of every main loop frame.
void OnMainFrame() noexcept;
// After a save: capture the character on the next main frame it is possible and write it to aPath (the save's own
// name with .snap), so any save made with the mod carries its character without being loaded.
void QueueCaptureTo(const std::filesystem::path& aPath) noexcept;
// Reads a .snap file. False when missing, unreadable or another format version.
bool ReadFile(const std::filesystem::path& aPath, CharacterSnapshot& aOut) noexcept;
} // namespace CharacterSnapshots
