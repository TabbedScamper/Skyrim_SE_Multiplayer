#include <Services/SaveLook.h>

#include <Forms/TESNPC.h>
#include <Forms/TESRace.h>
#include <Games/TES.h>
#include <Games/IFormFactory.h>
#include <PlayerCharacter.h>
#include <Actor.h>
#include <SaveLoad.h>

#include <zlib.h>

#include <cmath>

#include <fstream>

// Save layout (SkyrimAtlas systems/save-load.md; xEdit wbFileHeader, wbFileLocationTable, wbChangedForm):
// "TESV_SAVEGAME", u32 header size, header (version 12, save number, player name, level, location, game date, race
// editor ID, sex u16, XP, XP to level, FILETIME, screenshot width/height, compression type u16), screenshot
// (w*h*4 from version 10), then for compressed saves u32 uncompressed and u32 compressed sizes and the body (LZ4 block
// for type 2, zlib for type 1). Body: form version u8, plugin info (u32 size, u8 count, names), light plugins (u16 count,
// names; form version 78 and up), the file location table, ..., the change forms, the formID array.
namespace
{
namespace fs = std::filesystem;

thread_local const std::vector<uint32_t>* t_activeFormIds{};

struct Reader
{
    const uint8_t* Data{};
    size_t Size{};
    size_t Pos{};

    bool Has(size_t aCount) const noexcept { return Pos + aCount <= Size; }
    template <class T> bool Read(T& aOut) noexcept
    {
        if (!Has(sizeof(T)))
            return false;
        std::memcpy(&aOut, Data + Pos, sizeof(T));
        Pos += sizeof(T);
        return true;
    }
    bool WString(std::string& aOut) noexcept
    {
        uint16_t length{};
        if (!Read(length) || !Has(length))
            return false;
        aOut.assign(reinterpret_cast<const char*>(Data + Pos), length);
        Pos += length;
        return true;
    }
    bool Skip(size_t aCount) noexcept
    {
        if (!Has(aCount))
            return false;
        Pos += aCount;
        return true;
    }
};

// LZ4 block format (no frame): sequences of literals and back references.
bool Lz4Decompress(const uint8_t* apIn, size_t aInSize, std::vector<uint8_t>& aOut, size_t aOutSize) noexcept
{
    aOut.assign(aOutSize, 0);
    size_t in = 0, out = 0;
    while (in < aInSize)
    {
        const uint8_t token = apIn[in++];
        size_t literals = token >> 4;
        if (literals == 15)
        {
            uint8_t more{};
            do
            {
                if (in >= aInSize)
                    return false;
                more = apIn[in++];
                literals += more;
            } while (more == 255);
        }
        if (in + literals > aInSize || out + literals > aOutSize)
            return false;
        std::memcpy(aOut.data() + out, apIn + in, literals);
        in += literals;
        out += literals;
        if (in >= aInSize)
            break; // the last sequence has literals only
        if (in + 2 > aInSize)
            return false;
        const size_t offset = apIn[in] | (apIn[in + 1] << 8);
        in += 2;
        if (offset == 0 || offset > out)
            return false;
        size_t match = (token & 0x0F) + 4;
        if ((token & 0x0F) == 15)
        {
            uint8_t more{};
            do
            {
                if (in >= aInSize)
                    return false;
                more = apIn[in++];
                match += more;
            } while (more == 255);
        }
        if (out + match > aOutSize)
            return false;
        for (size_t i = 0; i < match; ++i, ++out) // overlapping copies repeat the pattern
            aOut[out] = aOut[out - offset];
    }
    return out == aOutSize;
}

bool ZlibDecompress(const uint8_t* apIn, size_t aInSize, std::vector<uint8_t>& aOut, size_t aOutSize) noexcept
{
    aOut.assign(aOutSize, 0);
    uLongf size = static_cast<uLongf>(aOutSize);
    return uncompress(aOut.data(), &size, apIn, static_cast<uLong>(aInSize)) == Z_OK && size == aOutSize;
}

// The 3-byte reference written in a change form (big-endian): 2 bits of kind, 22 bits of value. Kind 0 indexes the
// formID array (1-based, 0 = none), 1 is a Skyrim.esm form, 2 a form created in that game (not present here).
uint32_t ResolveRef(uint32_t aRef, const std::vector<uint32_t>& acFormIds) noexcept
{
    const uint32_t kind = aRef >> 22;
    const uint32_t value = aRef & 0x3FFFFF;
    if (kind == 1)
        return value;
    if (kind == 0 && value != 0 && value <= acFormIds.size())
        return acFormIds[value - 1];
    return 0;
}
} // namespace

namespace
{
// The face tints (skin colour, warpaint, dirt...): PlayerCharacter::SaveGame writes them near the end of the player
// reference's record through 14073E7B0 (40407): three race references (original race, tint template race, current),
// a u32, a u32 count, then per layer {u16 template index, u32 colour, f32 alpha} in the template race's layer order (a
// missing template writes 0xFFFF, -1, FLT_MAX). Found from the end of the record by that shape: three references that
// resolve to races, a sane count, and layers that fit with alphas in 0..1. TabbedScamper, 2026-10-02: Nord, Argonian,
// Argonian, 0, 19 layers at byte 29753 of 30128.
void ReadTints(const SaveLook::Parsed& acParsed, const std::vector<uint32_t>& acFormIds, CharacterSnapshot& aOut) noexcept
{
    const auto& data = acParsed.ActorData;
    const auto* p = reinterpret_cast<const uint8_t*>(data.data());
    const auto isRace = [&](size_t aAt) {
        const uint32_t ref = (uint32_t{p[aAt]} << 16) | (uint32_t{p[aAt + 1]} << 8) | p[aAt + 2];
        auto* pForm = TESForm::GetById(ResolveRef(ref, acFormIds));
        return pForm && Cast<TESRace>(pForm) != nullptr;
    };
    if (data.size() < 40)
        return;
    for (size_t at = data.size() - 17; at-- > 0;)
    {
        if (!isRace(at) || !isRace(at + 3) || !isRace(at + 6))
            continue;
        uint32_t sex{}, count{};
        std::memcpy(&sex, p + at + 9, 4);
        std::memcpy(&count, p + at + 13, 4);
        if (sex > 1 || count == 0 || count > 64 || at + 17 + size_t{count} * 10 > data.size())
            continue;
        std::vector<Tints::Entry> layers;
        bool sane = true;
        for (uint32_t i = 0; i < count && sane; ++i)
        {
            const auto* layer = p + at + 17 + size_t{i} * 10;
            uint16_t index{};
            uint32_t colour{};
            float alpha{};
            std::memcpy(&index, layer, 2);
            std::memcpy(&colour, layer + 2, 4);
            std::memcpy(&alpha, layer + 6, 4);
            Tints::Entry entry{};
            entry.Type = SaveLook::kAnyTintType;
            if (index == 0xFFFF)
                entry.Alpha = -1.f; // no template: left as the race has it
            else if (std::isfinite(alpha) && alpha >= 0.f && alpha <= 1.f)
            {
                entry.Alpha = alpha;
                entry.Color = colour;
            }
            else
                sane = false;
            layers.push_back(entry);
        }
        if (!sane)
            continue;
        aOut.FaceTints.Entries.assign(layers.begin(), layers.end());
        spdlog::info("Save look: {} face tint layers from byte {} of the player's record", count, at);
        return;
    }
    spdlog::info("Save look: no face tints found in the player's record");
}

// The character's items, read by the game's own loader. The player reference's record is, in order (SaveGame chain,
// SkyrimAtlas corpus): the initial data the save loop writes (its kind from 1406204C0: 6 = cell changed or promoted,
// 34 bytes; 4 = moved, 27 bytes; 0 = none), a havok block when moved by havok (size-prefixed), then Actor::SaveGame
// (140699230) writes two u32 and calls TESObjectREFR::SaveGame (1402E0B10): form flags, base, scale, extra data, the
// inventory, the animation graph. TESObjectREFR::LoadGame (19529, 1402E0C70) reads those back; run on an actor that
// never enters the world, with the animation bit cleared so it stops after the inventory.
void ReadItems(const SaveLook::Parsed& acParsed, const std::vector<uint32_t>& acFormIds, CharacterSnapshot& aOut) noexcept
{
    const auto& data = acParsed.ActorData;
    const uint32_t flags = acParsed.ActorFlags;
    if (data.empty() || !(flags & 0x8000020))
        return;
    size_t start = (flags & 0x2000008) ? 34 : (flags & 0x6) ? 27 : 0;
    if (flags & 0x4) // havok moved: a size-prefixed block (the save's variable-size count, low two bits = width)
    {
        if (start >= data.size())
            return;
        const auto first = static_cast<uint8_t>(data[start]);
        const size_t width = (first & 3) == 0 ? 1 : (first & 3) == 1 ? 2 : 4;
        if (start + width > data.size())
            return;
        uint32_t raw = 0;
        std::memcpy(&raw, data.data() + start, width);
        start += width + (raw >> 2);
    }
    start += 8; // Actor::SaveGame's two u32
    if (start >= data.size())
        return;

    auto* pPlayer = PlayerCharacter::Get();
    auto scratch = Actor::New();
    if (!scratch || !pPlayer)
        return;
    scratch->SetObjectReference(Cast<TESNPC>(pPlayer->baseForm));

    BGSLoadFormBuffer buffer(flags & ~0x10000000u); // no animation graph
    buffer.SetSize(static_cast<uint32_t>(data.size()));
    buffer.buffer = data.data();
    buffer.position = static_cast<uint32_t>(start);
    buffer.formId = scratch->formID;
    buffer.form = scratch;
    using TLoadGame = void(TESObjectREFR*, BGSLoadFormBuffer*);
    POINTER_SKYRIMSE(TLoadGame, s_referenceLoadGame, 19529);
    t_activeFormIds = &acFormIds;
    s_referenceLoadGame.Get()(scratch, &buffer);
    t_activeFormIds = nullptr;
    buffer.buffer = nullptr;

    auto inventory = scratch->GetActorInventory();
    uint32_t worn = 0;
    for (auto& entry : inventory.Entries)
    {
        if (entry.IsQuestItem || entry.Count <= 0)
            continue;
        worn += entry.ExtraWorn || entry.ExtraWornLeft ? 1 : 0;
        aOut.Items.Entries.push_back(std::move(entry));
    }
    aOut.Items.CurrentMagicEquipment = inventory.CurrentMagicEquipment;
    scratch->Delete();
    spdlog::info("Save look: {} item stacks from the save ({} worn), the record read from byte {} of {}",
        aOut.Items.Entries.size(), worn, start, data.size());
}
} // namespace

namespace SaveLook
{
const std::vector<uint32_t>* ActiveFormIds() noexcept
{
    return t_activeFormIds;
}

bool Parse(const fs::path& acSave, Parsed& aOut, std::string& aError) noexcept
{
    std::ifstream file(acSave, std::ios::binary);
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    Reader head{bytes.data(), bytes.size()};
    if (bytes.size() < 64 || std::memcmp(bytes.data(), "TESV_SAVEGAME", 13) != 0)
    {
        aError = "not a Skyrim save";
        return false;
    }
    head.Pos = 13;
    uint32_t headerSize{}, version{}, saveNumber{}, level{}, shotWidth{}, shotHeight{};
    uint16_t sex{}, compression{};
    float xp{}, xpNext{};
    uint64_t fileTime{};
    std::string name, location, gameDate;
    if (!head.Read(headerSize))
        return aError = "header unreadable", false;
    const size_t headerEnd = head.Pos + headerSize;
    if (!head.Read(version) || !head.Read(saveNumber) || !head.WString(name) || !head.Read(level) || !head.WString(location) ||
        !head.WString(gameDate) || !head.WString(aOut.RaceEditorId) || !head.Read(sex) || !head.Read(xp) || !head.Read(xpNext) ||
        !head.Read(fileTime) || !head.Read(shotWidth) || !head.Read(shotHeight))
        return aError = "header unreadable", false;
    if (version >= 12 && !head.Read(compression))
        return aError = "header unreadable", false;
    aOut.Female = sex == 1;
    head.Pos = headerEnd;
    if (!head.Skip(static_cast<size_t>(shotWidth) * shotHeight * (version >= 10 ? 4 : 3)))
        return aError = "screenshot cut short", false;

    std::vector<uint8_t> body;
    if (compression != 0)
    {
        uint32_t plainSize{}, packedSize{};
        if (!head.Read(plainSize) || !head.Read(packedSize) || !head.Has(packedSize) || plainSize > (256u << 20))
            return aError = "compressed body unreadable", false;
        const bool ok = compression == 2 ? Lz4Decompress(bytes.data() + head.Pos, packedSize, body, plainSize)
                                         : ZlibDecompress(bytes.data() + head.Pos, packedSize, body, plainSize);
        if (!ok)
            return aError = "body did not decompress", false;
    }
    else
        body.assign(bytes.begin() + static_cast<std::ptrdiff_t>(head.Pos), bytes.end());
    const size_t bodyStart = head.Pos; // where the body would begin in the file, for file-relative offsets

    Reader r{body.data(), body.size()};
    uint8_t formVersion{};
    uint32_t pluginInfoSize{};
    uint8_t pluginCount{};
    if (!r.Read(formVersion) || !r.Read(pluginInfoSize) || !r.Read(pluginCount))
        return aError = "plugin list unreadable", false;
    for (uint32_t i = 0; i < pluginCount; ++i)
        if (std::string plugin; r.WString(plugin))
            aOut.Plugins.push_back(std::move(plugin));
        else
            return aError = "plugin list unreadable", false;
    if (formVersion >= 78)
    {
        uint16_t lightCount{};
        if (!r.Read(lightCount))
            return aError = "light plugin list unreadable", false;
        for (uint32_t i = 0; i < lightCount; ++i)
            if (std::string plugin; r.WString(plugin))
                aOut.LightPlugins.push_back(std::move(plugin));
            else
                return aError = "light plugin list unreadable", false;
    }
    uint32_t table[10]{};
    for (auto& value : table)
        if (!r.Read(value))
            return aError = "file location table unreadable", false;
    const uint32_t formIdArrayOffset = table[0], changeFormsOffset = table[4], changeFormCount = table[9];

    // The table's offsets count from the start of the file as if it were not compressed; try that and body-relative.
    for (const int64_t base : {static_cast<int64_t>(bodyStart), static_cast<int64_t>(bodyStart) - 8, int64_t{0}})
    {
        const int64_t idsAt = static_cast<int64_t>(formIdArrayOffset) - base;
        const int64_t formsAt = static_cast<int64_t>(changeFormsOffset) - base;
        if (idsAt < 0 || formsAt < 0 || static_cast<size_t>(idsAt) + 4 > body.size() || static_cast<size_t>(formsAt) > body.size())
            continue;
        Reader ids{body.data(), body.size(), static_cast<size_t>(idsAt)};
        uint32_t idCount{};
        if (!ids.Read(idCount) || !ids.Has(static_cast<size_t>(idCount) * 4))
            continue;
        std::vector<uint32_t> formIds(idCount);
        for (auto& id : formIds)
            ids.Read(id);

        Reader forms{body.data(), body.size(), static_cast<size_t>(formsAt)};
        bool sane = true;
        for (uint32_t i = 0; i < changeFormCount && sane; ++i)
        {
            uint8_t ref[3]{};
            uint32_t flags{};
            uint8_t type{}, formVersionByte{};
            if (!forms.Has(3))
            {
                sane = false;
                break;
            }
            std::memcpy(ref, forms.Data + forms.Pos, 3);
            forms.Pos += 3;
            if (!forms.Read(flags) || !forms.Read(type) || !forms.Read(formVersionByte))
            {
                sane = false;
                break;
            }
            uint32_t length1{}, length2{};
            switch (type >> 6)
            {
            case 0:
            {
                uint8_t a{}, b{};
                sane = forms.Read(a) && forms.Read(b);
                length1 = a, length2 = b;
                break;
            }
            case 1:
            {
                uint16_t a{}, b{};
                sane = forms.Read(a) && forms.Read(b);
                length1 = a, length2 = b;
                break;
            }
            case 2:
                sane = forms.Read(length1) && forms.Read(length2);
                break;
            default:
                sane = false;
            }
            if (!sane || !forms.Has(length1))
            {
                sane = false;
                break;
            }
            const uint32_t refId = (uint32_t{ref[0]} << 16) | (uint32_t{ref[1]} << 8) | ref[2];
            // The player's base NPC (Skyrim.esm 0x7: the look) and the player's reference (0x14: the items).
            const bool npc = refId == ((1u << 22) | 0x7), reference = refId == ((1u << 22) | 0x14);
            if (npc || reference)
            {
                std::vector<uint8_t> data;
                if (length2 > 0)
                {
                    if (!ZlibDecompress(forms.Data + forms.Pos, length1, data, length2))
                        return aError = "a player record did not decompress", false;
                }
                else
                    data.assign(forms.Data + forms.Pos, forms.Data + forms.Pos + length1);
                (npc ? aOut.ChangeFlags : aOut.ActorFlags) = flags;
                (npc ? aOut.Data : aOut.ActorData).assign(reinterpret_cast<const char*>(data.data()), data.size());
                if (!aOut.Data.empty() && !aOut.ActorData.empty())
                    break;
            }
            forms.Pos += length1;
        }
        if (sane && !aOut.Data.empty())
        {
            aOut.FormIds = std::move(formIds);
            return true;
        }
        if (sane)
            return aError = "the save has no record of the player's base", false;
    }
    return aError = "change forms not found", false;
}

bool BuildLook(const Parsed& acParsed, CharacterSnapshot& aOut, std::string& aError) noexcept
{
    auto* pMods = ModManager::Get();
    if (!pMods || acParsed.Data.empty())
        return aError = "nothing to load", false;
    // The save's formID array numbered by the save's load order: renumbered by plugin name into this game's.
    std::vector<uint32_t> formIds;
    formIds.reserve(acParsed.FormIds.size());
    uint32_t unresolved = 0;
    for (const auto id : acParsed.FormIds)
    {
        const uint32_t index = id >> 24;
        const bool light = index == 0xFE;
        const auto& names = light ? acParsed.LightPlugins : acParsed.Plugins;
        const uint32_t slot = light ? (id >> 12) & 0xFFF : index;
        const Mod* pMod = slot < names.size() ? pMods->GetByName(names[slot].c_str()) : nullptr;
        uint32_t current = 0;
        if (pMod && pMod->IsLite() && pMod->liteId != 0xFFFF)
            current = 0xFE000000 | (uint32_t{pMod->liteId} << 12) | (id & 0xFFF);
        else if (pMod && pMod->IsLoaded())
            current = (uint32_t{pMod->standardId} << 24) | (id & 0xFFFFFF);
        unresolved += current == 0 ? 1 : 0;
        formIds.push_back(current);
    }

    // One scratch NPC, reused: it only ever holds a save's look for the moment it is read back out.
    static TESNPC* s_scratch{};
    if (!s_scratch)
    {
        s_scratch = IFormFactory::Create<TESNPC>();
        if (!s_scratch)
            return aError = "no scratch NPC", false;
        s_scratch->Initialize();
    }
    t_activeFormIds = &formIds;
    const bool loaded = s_scratch->Deserialize(String(acParsed.Data.data(), acParsed.Data.size()), acParsed.ChangeFlags);
    t_activeFormIds = nullptr;
    if (!loaded)
        return aError = "the record was refused", false;
    if (!s_scratch->raceForm.race)
        return aError = "no race after loading the record", false;

    aOut = CharacterSnapshot{};
    // Race (0x2000000), sex (0x1000000) and face (0x800): the look as the lobby and the party send it. Passed directly:
    // the scratch NPC is not in the game's change map, so its own flags are empty.
    aOut.ChangeFlags = 0x3000800;
    s_scratch->Serialize(&aOut.AppearanceBuffer, aOut.ChangeFlags);
    ReadItems(acParsed, formIds, aOut);
    ReadTints(acParsed, formIds, aOut);
    spdlog::info("Save look: race {:X} ({}) {} from a {}-byte record, flags {:X}, {} of {} references unresolved",
        s_scratch->raceForm.race->formID, acParsed.RaceEditorId, acParsed.Female ? "female" : "male", acParsed.Data.size(),
        acParsed.ChangeFlags, unresolved, formIds.size());
    return true;
}
} // namespace SaveLook

// The reference reader while a save's record loads (Games/SaveLoad.cpp calls this first).
bool SaveLookReadRef(const char* apBuffer, uint32_t aCapacity, uint32_t& aPosition, uint32_t& aFormId) noexcept
{
    const auto* pIds = t_activeFormIds;
    if (!pIds)
        return false;
    if (aPosition + 3 > aCapacity)
    {
        aFormId = 0;
        return true;
    }
    const auto* p = reinterpret_cast<const uint8_t*>(apBuffer + aPosition);
    aFormId = ResolveRef((uint32_t{p[0]} << 16) | (uint32_t{p[1]} << 8) | p[2], *pIds);
    aPosition += 3;
    return true;
}
