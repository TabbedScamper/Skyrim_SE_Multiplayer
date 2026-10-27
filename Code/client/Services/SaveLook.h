#pragma once
// A character's look read straight from a save file (owner 2026-10-02: saves made before the mod wrote a character
// file showed an Argonian and a Khajiit as the default Nord at the lobby; loading each save to capture it would stop on
// the "content no longer present" prompt). The save's own record of the player's base NPC (form 0x7: race, sex, face,
// head parts, hair colour) is loaded onto a scratch NPC and taken back out in the mod's look format. Face tint layers
// live in the player's own record and are not read.
#include <Structs/CharacterSnapshot.h>

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace SaveLook
{
// A face tint read from a save names no layer type: it pairs with the rebuilt list by position (CharacterSnapshots).
constexpr uint32_t kAnyTintType = 0xFFFFFFFF;

struct Parsed
{
    uint32_t ChangeFlags{};
    std::string Data;                   // the NPC change form's data, decompressed
    std::vector<uint32_t> FormIds;      // the save's formID array, as the save's load order numbers them
    std::vector<std::string> Plugins;   // the save's plugins, by index
    std::vector<std::string> LightPlugins;
    std::string RaceEditorId;           // from the header (for the log)
    bool Female{};
    uint32_t ActorFlags{};              // the player reference's record (form 0x14): gear
    std::string ActorData;
};

// Any thread: reads the file and finds the player NPC's record. False with aError when it is not there or unreadable.
bool Parse(const std::filesystem::path& acSave, Parsed& aOut, std::string& aError) noexcept;

// Main thread: the look as a snapshot carrying the appearance and the character's items as saved (worn, enchanted,
// tempered: CharacterSnapshots::QueueApplyLook puts both on the lobby's body).
bool BuildLook(const Parsed& acParsed, CharacterSnapshot& aOut, std::string& aError) noexcept;

// The save's 3-byte references while BuildLook loads the record (BGSLoadFormBuffer's form ID read). Null otherwise.
const std::vector<uint32_t>* ActiveFormIds() noexcept;
} // namespace SaveLook
