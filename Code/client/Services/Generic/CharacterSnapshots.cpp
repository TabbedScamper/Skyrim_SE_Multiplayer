#include <Services/CharacterSnapshots.h>

#include <World.h>
#include <PlayerCharacter.h>
#include <Forms/TESNPC.h>
#include <Forms/ActorValueInfo.h>
#include <Interface/UI.h>
#include <Games/TES.h>
#include <Services/PapyrusService.h>
#include <Misc/GameVM.h>

#include <AI/AIProcess.h>
#include <Forms/TESRace.h>

#include <fstream>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <unordered_set>

namespace
{
// Form types in the engine's per-type form arrays (ModManager::FormsOfType); each form's own type is checked too.
constexpr uint8_t kSpellType = 22;
constexpr uint8_t kPerkType = 92;
constexpr uint8_t kShoutType = 119;
constexpr uint8_t kWordType = 120;
// TESForm flags (+0x10): the player knows the form (TESForm::SetPlayerKnows 0x1401E6150 sets 0x40) and, for a word
// of power, a soul unlocked it (Papyrus Game.IsWordUnlocked 0x140A25940 reads byte +0x12 bit 0).
constexpr uint32_t kPlayerKnows = 0x40;
constexpr uint32_t kWordUnlocked = 0x10000;
// Unspent perk points: one byte on the player, which Papyrus Game.AddPerkPoints (0x140A23E50) adds to, capped at 255.
constexpr size_t kPerkPointsOffset = 0xB11;
constexpr uint32_t kCarryWeight = 32;
constexpr uint32_t kDragonSouls = 133;

bool ToGameId(uint32_t aFormId, GameId& aId) noexcept
{
    return World::Get().GetModSystem().GetServerModId(aFormId, aId);
}

std::string JsonText(const char* acText)
{
    std::string out;
    for (const char* p = acText ? acText : ""; *p; ++p)
    {
        if (*p == '"' || *p == '\\')
            out += '\\';
        if (static_cast<unsigned char>(*p) >= 0x20)
            out += *p;
    }
    return out;
}
} // namespace

namespace CharacterSnapshots
{
bool Capture(CharacterSnapshot& aOut, std::string& aError) noexcept
{
    auto* pPlayer = PlayerCharacter::Get();
    auto* pNpc = pPlayer ? Cast<TESNPC>(pPlayer->baseForm) : nullptr;
    auto* pUi = UI::Get();
    auto* pMods = ModManager::Get();
    if (!pPlayer || !pNpc || !pPlayer->parentCell || !pPlayer->GetNiNode() || !pPlayer->pSkills || !*pPlayer->pSkills || !pMods)
    {
        aError = "player not loaded";
        return false;
    }
    if (!pUi || pUi->GetMenuOpen(BSFixedString("Loading Menu")) || pUi->GetMenuOpen(BSFixedString("RaceSex Menu")))
    {
        aError = "loading or in the character creator";
        return false;
    }

    aOut = CharacterSnapshot{};
    if (pNpc->fullName.value.data)
        aOut.Name = pNpc->fullName.value.AsAscii();
    // The same appearance payload remote players receive (CharacterService::SendAppearance): race, face and sex.
    pNpc->MarkChanged(0x3000800);
    aOut.ChangeFlags = pNpc->GetChangeFlags();
    pNpc->Serialize(&aOut.AppearanceBuffer);
    const auto& tints = pPlayer->GetTints();
    aOut.FaceTints.Entries.resize(tints.length);
    for (auto i = 0u; i < tints.length; ++i)
    {
        aOut.FaceTints.Entries[i].Alpha = tints[i]->alpha;
        aOut.FaceTints.Entries[i].Color = tints[i]->color;
        aOut.FaceTints.Entries[i].Type = tints[i]->type;
        if (tints[i]->texture)
            aOut.FaceTints.Entries[i].Name = tints[i]->texture->name.AsAscii();
    }

    const auto* pSkills = *pPlayer->pSkills;
    aOut.Level = pPlayer->GetLevel();
    aOut.Xp = pSkills->xp;
    aOut.LevelThreshold = pSkills->levelThreshold;
    aOut.PerkPoints = *(reinterpret_cast<const uint8_t*>(pPlayer) + kPerkPointsOffset);
    for (uint32_t i = 0; i < Skills::kTotal; ++i)
        aOut.Skills.push_back({pSkills->skills[i].level, pSkills->skills[i].xp, pSkills->skills[i].levelThreshold,
            pSkills->legendaryLevels[i]});
    // Base values only: what the character earned, before gear, potions or spells.
    for (uint32_t id = ActorValueInfo::kOneHanded; id <= ActorValueInfo::kEnchanting; ++id)
        aOut.BaseValues.push_back({id, pPlayer->actorValueOwner.GetBaseValue(id)});
    for (const uint32_t id : {static_cast<uint32_t>(ActorValueInfo::kHealth), static_cast<uint32_t>(ActorValueInfo::kMagicka),
             static_cast<uint32_t>(ActorValueInfo::kStamina), kCarryWeight})
        aOut.BaseValues.push_back({id, pPlayer->actorValueOwner.GetBaseValue(id)});
    aOut.DragonSouls = pPlayer->actorValueOwner.GetValue(kDragonSouls);

    // Every loaded perk the player has, at its rank (Actor::GetPerkRank, 37698). Works for any plugin's perks.
    const auto& perks = pMods->FormsOfType(kPerkType);
    for (uint32_t i = 0; i < perks.length; ++i)
    {
        auto* pPerk = perks[i];
        if (!pPerk || static_cast<uint8_t>(pPerk->formType) != kPerkType)
            continue;
        if (const auto rank = pPlayer->GetPerkRank(pPerk->formID))
            if (GameId id; ToGameId(pPerk->formID, id))
                aOut.Perks.push_back({id, rank});
    }
    // Spells and shouts the player was given (not race abilities, which the destination's race grants itself).
    // Actor::addedSpells holds plain form pointers (the declared element type has one indirection too many); each is
    // checked against the form table, so a wrong layout yields an empty list rather than garbage.
    auto& added = pPlayer->addedSpells;
    auto* const* ppForms = reinterpret_cast<TESForm* const*>(added.capacity >= 0 ? static_cast<void*>(added.data) : static_cast<void*>(&added.data));
    for (uint32_t i = 0; i < added.size && i < 4096; ++i)
    {
        auto* pForm = ppForms[i];
        GameId id;
        if (!pForm || TESForm::GetById(pForm->formID) != pForm || !ToGameId(pForm->formID, id))
            continue;
        if (static_cast<uint8_t>(pForm->formType) == kShoutType)
            aOut.Shouts.push_back(id);
        else if (static_cast<uint8_t>(pForm->formType) == kSpellType)
            aOut.Spells.push_back(id);
    }
    const auto& words = pMods->FormsOfType(kWordType);
    for (uint32_t i = 0; i < words.length; ++i)
    {
        auto* pWord = words[i];
        if (!pWord || static_cast<uint8_t>(pWord->formType) != kWordType || !(pWord->flags & kPlayerKnows))
            continue;
        if (GameId id; ToGameId(pWord->formID, id))
            aOut.Words.push_back({id, (pWord->flags & kWordUnlocked) != 0});
    }

    // Personal items: every stack except quest-object instances of this world, which stay behind and are listed.
    auto inventory = pPlayer->GetActorInventory();
    for (auto& entry : inventory.Entries)
    {
        if (entry.IsQuestItem)
            aOut.ExcludedQuestItems.push_back(entry.BaseId);
        else
            aOut.Items.Entries.push_back(std::move(entry));
    }
    aOut.Items.CurrentMagicEquipment = inventory.CurrentMagicEquipment;
    // The engine's list can hold a base stack plus a negative change entry for the same item, the change carrying the
    // equipped mark (Rex's iron arrows: x23 and x-2 worn, 2026-09-30). Fold each negative entry into a matching stack:
    // counts add up and the equipped mark moves with it, so the destination gets one equipped stack of 21.
    auto& entries = aOut.Items.Entries;
    for (size_t i = 0; i < entries.size();)
    {
        if (entries[i].Count >= 0)
        {
            ++i;
            continue;
        }
        const auto partner = std::find_if(entries.begin(), entries.end(), [&](const Inventory::Entry& acEntry) {
            return &acEntry != &entries[i] && acEntry.Count > 0 && acEntry.BaseId == entries[i].BaseId &&
                   acEntry.ExtraEnchantId == entries[i].ExtraEnchantId && acEntry.ExtraPoisonId == entries[i].ExtraPoisonId &&
                   acEntry.EnchantData.Effects.size() == entries[i].EnchantData.Effects.size();
        });
        if (partner != entries.end())
        {
            partner->Count += entries[i].Count;
            partner->ExtraWorn |= entries[i].ExtraWorn;
            partner->ExtraWornLeft |= entries[i].ExtraWornLeft;
        }
        entries.erase(entries.begin() + static_cast<std::ptrdiff_t>(i));
    }
    std::erase_if(entries, [](const Inventory::Entry& acEntry) { return acEntry.Count <= 0; });
    return true;
}

bool Apply(const CharacterSnapshot& acSnapshot, std::string& aReport, std::string& aError) noexcept
{
    auto* pPlayer = PlayerCharacter::Get();
    auto* pNpc = pPlayer ? Cast<TESNPC>(pPlayer->baseForm) : nullptr;
    auto* pUi = UI::Get();
    auto* pMods = ModManager::Get();
    if (!pPlayer || !pNpc || !pPlayer->parentCell || !pPlayer->GetNiNode() || !pPlayer->pSkills || !*pPlayer->pSkills || !pMods)
    {
        aError = "player not loaded";
        return false;
    }
    if (!pUi || pUi->GetMenuOpen(BSFixedString("Loading Menu")) || pUi->GetMenuOpen(BSFixedString("RaceSex Menu")))
    {
        aError = "loading or in the character creator";
        return false;
    }
    if (acSnapshot.Version != CharacterSnapshot::kVersion || acSnapshot.Skills.size() != CharacterSnapshot::kSkillCount)
    {
        aError = "snapshot version or skill count does not match";
        return false;
    }
    auto& mods = World::Get().GetModSystem();
    const auto resolve = [&mods](const GameId& acId) -> TESForm* { return TESForm::GetById(mods.GetGameId(acId)); };
    uint32_t missing = 0;

    // Papyrus natives: the same paths mods use, with the game's own side effects (perk entry points, spell
    // application, shout menu, word unlock notifications).
    PAPYRUS_FUNCTION(void, Actor, AddPerk, TESForm*);
    PAPYRUS_FUNCTION(void, Actor, RemovePerk, TESForm*);
    PAPYRUS_FUNCTION(bool, Actor, AddSpell, TESForm*, bool);
    PAPYRUS_FUNCTION(bool, Actor, AddShout, TESForm*);
    PAPYRUS_FUNCTION(bool, Actor, RemoveShout, TESForm*);
    GLOBAL_PAPYRUS_FUNCTION(void, Game, TeachWord, uint32_t, void*, TESForm*);
    GLOBAL_PAPYRUS_FUNCTION(void, Game, UnlockWord, uint32_t, void*, TESForm*);
    using TSetPlayerKnows = void(TESForm*, bool);
    POINTER_SKYRIMSE(TSetPlayerKnows, setPlayerKnows, 14639); // TESForm::SetPlayerKnows, 0x1401E6150

    // Perks: drop the ones this character lacks, add the ones it has (vanilla ranks are separate perk forms).
    std::unordered_set<uint32_t> wantedPerks;
    for (const auto& perk : acSnapshot.Perks)
        if (auto* pPerk = resolve(perk.Id); pPerk && static_cast<uint8_t>(pPerk->formType) == kPerkType)
            wantedPerks.insert(pPerk->formID);
        else
            ++missing;
    uint32_t perksRemoved = 0, perksAdded = 0;
    const auto& perks = pMods->FormsOfType(kPerkType);
    for (uint32_t i = 0; i < perks.length; ++i)
    {
        auto* pPerk = perks[i];
        if (!pPerk || static_cast<uint8_t>(pPerk->formType) != kPerkType)
            continue;
        const bool has = pPlayer->GetPerkRank(pPerk->formID) > 0;
        const bool wanted = wantedPerks.contains(pPerk->formID);
        if (has && !wanted)
        {
            s_pRemovePerk(pPlayer, pPerk);
            ++perksRemoved;
        }
        else if (!has && wanted)
        {
            s_pAddPerk(pPlayer, pPerk);
            ++perksAdded;
        }
    }

    // Spells and shouts the loaded character was given, replaced by the snapshot's. Race abilities are untouched.
    std::unordered_set<uint32_t> wantedSpells, wantedShouts;
    for (const auto& id : acSnapshot.Spells)
        if (auto* pForm = resolve(id))
            wantedSpells.insert(pForm->formID);
        else
            ++missing;
    for (const auto& id : acSnapshot.Shouts)
        if (auto* pForm = resolve(id))
            wantedShouts.insert(pForm->formID);
        else
            ++missing;
    std::vector<TESForm*> current;
    auto& added = pPlayer->addedSpells;
    auto* const* ppForms = reinterpret_cast<TESForm* const*>(added.capacity >= 0 ? static_cast<void*>(added.data) : static_cast<void*>(&added.data));
    for (uint32_t i = 0; i < added.size && i < 4096; ++i)
        if (auto* pForm = ppForms[i]; pForm && TESForm::GetById(pForm->formID) == pForm)
            current.push_back(pForm);
    uint32_t spellsRemoved = 0, spellsAdded = 0, shoutsRemoved = 0, shoutsAdded = 0;
    for (auto* pForm : current)
    {
        const auto type = static_cast<uint8_t>(pForm->formType);
        if (type == kSpellType && !wantedSpells.contains(pForm->formID))
            spellsRemoved += pPlayer->RemoveSpell(reinterpret_cast<MagicItem*>(pForm)) ? 1 : 0;
        else if (type == kShoutType && !wantedShouts.contains(pForm->formID))
            shoutsRemoved += s_pRemoveShout(pPlayer, pForm) ? 1 : 0;
    }
    for (const auto id : wantedSpells)
        spellsAdded += s_pAddSpell(pPlayer, TESForm::GetById(id), false) ? 1 : 0;
    for (const auto id : wantedShouts)
        shoutsAdded += s_pAddShout(pPlayer, TESForm::GetById(id)) ? 1 : 0;

    // Words of power: known and unlocked exactly as the snapshot. There is no native "forget": a word this character
    // never learned has its known flag cleared through TESForm::SetPlayerKnows and its unlocked flag cleared.
    std::unordered_map<uint32_t, bool> wantedWords;
    for (const auto& word : acSnapshot.Words)
        if (auto* pWord = resolve(word.Id); pWord && static_cast<uint8_t>(pWord->formType) == kWordType)
            wantedWords[pWord->formID] = word.Unlocked;
        else
            ++missing;
    uint32_t wordsTaught = 0, wordsUnlocked = 0, wordsForgotten = 0;
    const auto& words = pMods->FormsOfType(kWordType);
    for (uint32_t i = 0; i < words.length; ++i)
    {
        auto* pWord = words[i];
        if (!pWord || static_cast<uint8_t>(pWord->formType) != kWordType)
            continue;
        const auto wanted = wantedWords.find(pWord->formID);
        const bool known = (pWord->flags & kPlayerKnows) != 0;
        if (wanted == wantedWords.end())
        {
            if (known || (pWord->flags & kWordUnlocked))
            {
                pWord->flags &= ~kWordUnlocked;
                setPlayerKnows.Get()(pWord, false);
                ++wordsForgotten;
            }
            continue;
        }
        if (!known)
        {
            s_pTeachWord(0, nullptr, pWord);
            ++wordsTaught;
        }
        if (wanted->second && !(pWord->flags & kWordUnlocked))
        {
            s_pUnlockWord(0, nullptr, pWord);
            ++wordsUnlocked;
        }
        else if (!wanted->second && (pWord->flags & kWordUnlocked))
            pWord->flags &= ~kWordUnlocked;
    }

    // Level and progress: the fields the game's own save writes (player skills block, NPC base level, perk points).
    auto* pSkills = *pPlayer->pSkills;
    pNpc->actorData.level = acSnapshot.Level;
    pSkills->xp = acSnapshot.Xp;
    pSkills->levelThreshold = acSnapshot.LevelThreshold;
    for (uint32_t i = 0; i < Skills::kTotal; ++i)
    {
        pSkills->skills[i].level = acSnapshot.Skills[i].Level;
        pSkills->skills[i].xp = acSnapshot.Skills[i].Xp;
        pSkills->skills[i].levelThreshold = acSnapshot.Skills[i].Threshold;
        pSkills->legendaryLevels[i] = acSnapshot.Skills[i].Legendary;
    }
    *(reinterpret_cast<uint8_t*>(pPlayer) + kPerkPointsOffset) = acSnapshot.PerkPoints;
    pNpc->MarkChanged(0x2); // CHANGE_ACTOR_BASE_DATA (UESP save format): the level is written to the next save
    // The character's name (as remote copies are named, CharacterService::OnNotifyPlayerAppearance).
    if (!acSnapshot.Name.empty())
    {
        pNpc->fullName.value.Set(acSnapshot.Name.c_str());
        pNpc->MarkChanged(0x20); // CHANGE_ACTOR_BASE_FULLNAME
    }
    // Base values after perks and spells, so their permanent bonuses are not counted twice.
    for (const auto& value : acSnapshot.BaseValues)
        pPlayer->actorValueOwner.SetBaseValue(value.ActorValue, value.Value);
    pPlayer->actorValueOwner.ModValue(kDragonSouls, acSnapshot.DragonSouls - pPlayer->actorValueOwner.GetValue(kDragonSouls));

    // Items: every personal stack of the loaded character goes, this world's quest items stay, the snapshot's come in
    // (TESObjectREFR::AddOrRemoveItem keeps enchantments, charge, tempering, poison and re-equips worn gear).
    uint32_t stacksRemoved = 0, stacksAdded = 0;
    auto inventory = pPlayer->GetInventory();
    for (auto entry : inventory.Entries)
    {
        if (entry.IsQuestItem || entry.Count <= 0)
            continue;
        entry.Count = -entry.Count;
        pPlayer->AddOrRemoveItem(entry);
        ++stacksRemoved;
    }
    for (const auto& entry : acSnapshot.Items.Entries)
    {
        if (!resolve(entry.BaseId))
        {
            ++missing;
            continue;
        }
        pPlayer->AddOrRemoveItem(entry);
        ++stacksAdded;
    }

    aReport = fmt::format("\"perksAdded\":{},\"perksRemoved\":{},\"spellsAdded\":{},\"spellsRemoved\":{},\"shoutsAdded\":{},"
                          "\"shoutsRemoved\":{},\"wordsTaught\":{},\"wordsUnlocked\":{},\"wordsForgotten\":{},\"stacksRemoved\":{},"
                          "\"stacksAdded\":{},\"missingForms\":{}",
        perksAdded, perksRemoved, spellsAdded, spellsRemoved, shoutsAdded, shoutsRemoved, wordsTaught, wordsUnlocked,
        wordsForgotten, stacksRemoved, stacksAdded, missing);
    return true;
}

namespace
{
std::mutex s_applyLock;
std::optional<CharacterSnapshot> s_queuedApply;
std::string s_applyStatus = "idle";

struct RunningApply
{
    CharacterSnapshot Snapshot;
    std::string Report;
    uint64_t Since{};
    void* OldHead{};
    bool NewSkeleton{};
};
std::optional<RunningApply> s_running;

void SetStatus(std::string aStatus)
{
    spdlog::info("Character snapshot apply: {}", aStatus);
    std::lock_guard lock(s_applyLock);
    s_applyStatus = std::move(aStatus);
}

// The appearance record onto the local player, as CreatorTogether applies a remote player's look: native NPC load,
// the full Actor::SwitchRace (37925) when the race changes, a new skeleton when race or sex changed, then the
// RaceSex menu's DoReset3D(true) (40255).
bool BeginAppearance(RunningApply& aRun, std::string& aError)
{
    auto* pPlayer = PlayerCharacter::Get();
    auto* pNpc = pPlayer ? Cast<TESNPC>(pPlayer->baseForm) : nullptr;
    if (!pNpc || !pPlayer->currentProcess)
    {
        aError = "player not loaded";
        return false;
    }
    if (aRun.Snapshot.AppearanceBuffer.empty())
        return true;
    const auto oldSex = pNpc->actorData.actorBaseFlags & 1;
    auto* pOldRace = pPlayer->race;
    if (!pNpc->Deserialize(aRun.Snapshot.AppearanceBuffer, aRun.Snapshot.ChangeFlags, true))
    {
        aError = "appearance record refused";
        return false;
    }
    aRun.NewSkeleton = pOldRace != pNpc->raceForm.race || oldSex != (pNpc->actorData.actorBaseFlags & 1);
    if (pOldRace != pNpc->raceForm.race)
    {
        auto* pNewRace = pNpc->raceForm.race;
        // TESNPC::SwitchRace returns early when the record already set the race: run the full actor transition from
        // the old race, then load the record again (the switch resets head parts).
        pNpc->raceForm.race = pOldRace;
        using TSwitchRace = void(Actor*, TESRace*, bool);
        POINTER_SKYRIMSE(TSwitchRace, s_switchRace, 37925);
        s_switchRace.Get()(pPlayer, pNewRace, false);
        if (!pNpc->Deserialize(aRun.Snapshot.AppearanceBuffer, aRun.Snapshot.ChangeFlags, true))
        {
            aError = "appearance record refused after the race change";
            return false;
        }
    }
    // The player's tint save (0x14073E7B0) indexes layers by the NPC's original race (+0x1E8), the race before any
    // vampire or werewolf change. Left at the old race, Rex's 21 layers came back as the host race's 18 after a save
    // and reload (2026-09-30). A transferred character is never mid-transformation, so it is the current race.
    pNpc->originalRace = pNpc->raceForm.race;
    if (aRun.NewSkeleton)
    {
        using TSet3DFlags = void(AIProcess*, uint8_t);
        POINTER_SKYRIMSE(TSet3DFlags, s_set3DFlags, 39907);
        s_set3DFlags.Get()(pPlayer->currentProcess, 0x20);
    }
    aRun.OldHead = pPlayer->GetFaceGenNiNode();
    using TReset3D = void(Actor*, bool);
    POINTER_SKYRIMSE(TReset3D, s_reset3D, 40255);
    s_reset3D.Get()(pPlayer, true);
    return true;
}

// Face tints: each of the player's masks takes the snapshot's colour and opacity for the same texture, then the
// player's own face-tint rebuild (40699, 0x14075CF20: CreateTints over the player's masks at +0xB20 for every face
// head part) redraws the face texture.
uint32_t ApplyTints(const CharacterSnapshot& acSnapshot)
{
    auto* pPlayer = PlayerCharacter::Get();
    if (!pPlayer || acSnapshot.FaceTints.Entries.empty())
        return 0;
    // A race switch leaves the old race's tint layers: rebuild the player's list from the current race and sex's
    // layer templates first (40696, 0x14075CAE0; it keeps the list when the count already matches).
    // The tint list keeps its own template race (+0xB48), race (+0xB50) and sex index (+0xB58) after the array; the
    // player's tint save (0x14073E7B0) indexes layers by that template race and the load (0x14073EAD0) rebuilds them
    // from it. A live race switch leaves them at the old race: Rex's 21 layers came back as the old race's 18 after a
    // save and reload (2026-09-30). Point them at the current race and sex, as the character creator does.
    if (auto* pNpc = Cast<TESNPC>(pPlayer->baseForm); pNpc && pNpc->raceForm.race)
    {
        auto* pBase = reinterpret_cast<uint8_t*>(pPlayer);
        *reinterpret_cast<TESRace**>(pBase + 0xB48) = pNpc->raceForm.race;
        *reinterpret_cast<TESRace**>(pBase + 0xB50) = pNpc->raceForm.race;
        *reinterpret_cast<uint32_t*>(pBase + 0xB58) = pNpc->actorData.actorBaseFlags & 1;
    }
    using TResetTints = void(PlayerCharacter*);
    POINTER_SKYRIMSE(TResetTints, s_resetTints, 40696);
    s_resetTints.Get()(pPlayer);
    auto& tints = const_cast<GameArray<TintMask*>&>(pPlayer->GetTints());
    uint32_t matched = 0;
    const auto& entries = acSnapshot.FaceTints.Entries;
    // Same race and sex: the rebuilt list follows the race's layer templates in the captured order, so pair layers by
    // position (checking the type). Texture names did not compare equal across the rebuild (Rex onto the host,
    // 2026-09-30: 21 layers each, 0 matched by name).
    if (entries.size() == tints.length)
    {
        for (uint32_t i = 0; i < tints.length; ++i)
            if (tints[i] && tints[i]->type == entries[i].Type)
            {
                tints[i]->color = entries[i].Color;
                tints[i]->alpha = entries[i].Alpha;
                ++matched;
            }
        using TUpdateFaceTints = void(PlayerCharacter*);
        POINTER_SKYRIMSE(TUpdateFaceTints, s_updateFaceTintsByIndex, 40699);
        s_updateFaceTintsByIndex.Get()(pPlayer);
        return matched;
    }
    for (uint32_t i = 0; i < tints.length; ++i)
    {
        auto* pMask = tints[i];
        if (!pMask || !pMask->texture)
            continue;
        const char* name = pMask->texture->name.AsAscii();
        for (const auto& entry : acSnapshot.FaceTints.Entries)
        {
            if (entry.Type != pMask->type || _stricmp(entry.Name.c_str(), name ? name : "") != 0)
                continue;
            pMask->color = entry.Color;
            pMask->alpha = entry.Alpha;
            ++matched;
            break;
        }
    }
    using TUpdateFaceTints = void(PlayerCharacter*);
    POINTER_SKYRIMSE(TUpdateFaceTints, s_updateFaceTints, 40699);
    s_updateFaceTints.Get()(pPlayer);
    return matched;
}
} // namespace

std::optional<std::filesystem::path> s_captureTo;
uint64_t s_captureSince{};

void QueueCaptureTo(const std::filesystem::path& aPath) noexcept
{
    std::lock_guard lock(s_applyLock);
    s_captureTo = aPath;
    s_captureSince = GetTickCount64();
}

bool ReadFile(const std::filesystem::path& aPath, CharacterSnapshot& aOut) noexcept
{
    std::ifstream file(aPath, std::ios::binary);
    if (!file)
        return false;
    std::string bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    if (bytes.empty() || bytes.size() > (1u << 22))
        return false;
    Buffer buffer(bytes.size() + 16);
    std::memcpy(buffer.GetWriteData(), bytes.data(), bytes.size());
    Buffer::Reader reader(&buffer);
    return aOut.Deserialize(reader);
}

namespace
{
// Writes the character for a save made a moment ago, once the player can be read (a save from a menu completes
// with the menu still open; retried for 30 s).
void RunQueuedCapture()
{
    std::filesystem::path path;
    {
        std::lock_guard lock(s_applyLock);
        if (!s_captureTo)
            return;
        if (GetTickCount64() - s_captureSince > 30000)
        {
            spdlog::warn("Character snapshot for {} not written: the player could not be read", s_captureTo->filename().string());
            s_captureTo.reset();
            return;
        }
        path = *s_captureTo;
    }
    CharacterSnapshot snapshot;
    std::string error;
    if (!Capture(snapshot, error))
        return;
    Buffer buffer(1 << 22);
    Buffer::Writer writer(&buffer);
    snapshot.Serialize(writer);
    const auto temporary = std::filesystem::path(path).concat(".tmp");
    {
        std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
        file.write(reinterpret_cast<const char*>(buffer.GetWriteData()), static_cast<std::streamsize>(writer.Size()));
    }
    std::error_code fsError;
    std::filesystem::rename(temporary, path, fsError);
    if (fsError)
        spdlog::error("Character snapshot {} not written: {}", path.filename().string(), fsError.message());
    else
        spdlog::info("Character snapshot written with the save: {} ({} bytes, {} level {})", path.filename().string(),
            writer.Size(), snapshot.Name.c_str(), snapshot.Level);
    std::lock_guard lock(s_applyLock);
    if (s_captureTo == path)
        s_captureTo.reset();
}
} // namespace

void QueueApply(CharacterSnapshot aSnapshot) noexcept
{
    std::lock_guard lock(s_applyLock);
    s_queuedApply = std::move(aSnapshot);
    s_applyStatus = "queued";
}

std::string ApplyStatus() noexcept
{
    std::lock_guard lock(s_applyLock);
    return s_applyStatus;
}

void OnMainFrame() noexcept
{
    RunQueuedCapture();
    const auto now = GetTickCount64();
    if (!s_running)
    {
        std::optional<CharacterSnapshot> queued;
        {
            std::lock_guard lock(s_applyLock);
            queued.swap(s_queuedApply);
        }
        if (!queued)
            return;
        RunningApply run{std::move(*queued), {}, now};
        std::string error;
        if (!Apply(run.Snapshot, run.Report, error) || !BeginAppearance(run, error))
        {
            SetStatus("failed: " + error);
            return;
        }
        s_running = std::move(run);
        SetStatus("rebuilding");
        return;
    }
    auto* pPlayer = PlayerCharacter::Get();
    auto* pHead = pPlayer ? pPlayer->GetFaceGenNiNode() : nullptr;
    // The rebuild is asynchronous: wait for a head node, a new one when it was replaced, and give it a moment.
    const bool rebuilt = pPlayer && pPlayer->GetNiNode() && pHead && (pHead != s_running->OldHead || now - s_running->Since > 3000);
    if (!rebuilt || now - s_running->Since < 500)
    {
        if (now - s_running->Since > 20000)
        {
            SetStatus("failed: the player's head was not rebuilt within 20 s");
            s_running.reset();
        }
        return;
    }
    const auto matched = ApplyTints(s_running->Snapshot);
    auto* pNpc = Cast<TESNPC>(pPlayer->baseForm);
    SetStatus(fmt::format("done: {{{},\"newSkeleton\":{},\"tintsMatched\":{},\"race\":\"{:X}\",\"headReplaced\":{}}}",
        s_running->Report, s_running->NewSkeleton, matched, pNpc && pNpc->raceForm.race ? pNpc->raceForm.race->formID : 0,
        pHead != s_running->OldHead));
    s_running.reset();
}

std::string DescribeLiveLook() noexcept
{
    auto* pPlayer = PlayerCharacter::Get();
    auto* pNpc = pPlayer ? Cast<TESNPC>(pPlayer->baseForm) : nullptr;
    if (!pNpc)
        return "\"live\":null";
    std::string parts;
    for (uint32_t i = 0; pNpc->headparts && i < pNpc->headpartsCount; ++i)
        if (pNpc->headparts[i])
            parts += fmt::format("{}\"{:X}\"", parts.empty() ? "" : ",", reinterpret_cast<const TESForm*>(pNpc->headparts[i])->formID);
    uint64_t morphHash = 14695981039346656037ULL;
    if (pNpc->faceMorphs)
    {
        for (const float option : pNpc->faceMorphs->option)
            morphHash = (morphHash ^ static_cast<uint64_t>(static_cast<int64_t>(option * 1000.f))) * 1099511628211ULL;
        for (const uint32_t preset : pNpc->faceMorphs->presets)
            morphHash = (morphHash ^ preset) * 1099511628211ULL;
    }
    uint64_t tintHash = 14695981039346656037ULL;
    const auto& tints = pPlayer->GetTints();
    for (uint32_t i = 0; i < tints.length; ++i)
        if (tints[i])
            tintHash = (tintHash ^ tints[i]->color ^ (static_cast<uint64_t>(tints[i]->alpha * 1000.f) << 32)) * 1099511628211ULL;
    return fmt::format("\"live\":{{\"race\":\"{:X}\",\"sex\":{},\"headParts\":[{}],\"morphHash\":\"{:016X}\",\"weight\":{:.1f},"
                       "\"skin\":[{},{},{}],\"hairColor\":\"{:X}\",\"tintLayers\":{},\"tintHash\":\"{:016X}\"}}",
        pNpc->raceForm.race ? pNpc->raceForm.race->formID : 0, pNpc->actorData.actorBaseFlags & 1, parts, morphHash,
        pNpc->weight, pNpc->color.red, pNpc->color.green, pNpc->color.blue,
        pNpc->headData && pNpc->headData->hairColor ? reinterpret_cast<const TESForm*>(pNpc->headData->hairColor)->formID : 0, tints.length, tintHash);
}

std::string Describe(const CharacterSnapshot& acSnapshot) noexcept
{
    std::string skills;
    for (size_t i = 0; i < acSnapshot.Skills.size(); ++i)
        skills += fmt::format("{}\"{}\":{:.0f}", i ? "," : "", Skills::GetSkillString(static_cast<Skills::Skill>(i)),
            acSnapshot.Skills[i].Level);
    std::string values;
    for (size_t i = 0; i < acSnapshot.BaseValues.size(); ++i)
        values += fmt::format("{}\"{}\":{:.1f}", i ? "," : "", acSnapshot.BaseValues[i].ActorValue, acSnapshot.BaseValues[i].Value);
    uint32_t unlocked = 0;
    for (const auto& word : acSnapshot.Words)
        unlocked += word.Unlocked ? 1 : 0;
    // FNV-1a over the appearance record and tints: equal hashes mean the same look data.
    uint64_t lookHash = 14695981039346656037ULL;
    const auto mix = [&lookHash](uint64_t aValue) { lookHash = (lookHash ^ aValue) * 1099511628211ULL; };
    for (const char c : acSnapshot.AppearanceBuffer)
        mix(static_cast<uint8_t>(c));
    for (const auto& tint : acSnapshot.FaceTints.Entries)
    {
        mix(tint.Color);
        mix(static_cast<uint64_t>(tint.Alpha * 1000.f));
        mix(tint.Type);
    }
    int64_t gold = 0;
    for (const auto& entry : acSnapshot.Items.Entries)
        if (entry.BaseId.ModId == 0 && entry.BaseId.BaseId == 0xF)
            gold += entry.Count;
    return fmt::format("\"name\":\"{}\",\"level\":{},\"xp\":{:.1f},\"threshold\":{:.1f},\"perkPoints\":{},\"skills\":{{{}}},"
                       "\"baseValues\":{{{}}},\"dragonSouls\":{:.0f},\"perks\":{},\"spells\":{},\"shouts\":{},\"words\":{},"
                       "\"wordsUnlocked\":{},\"itemStacks\":{},\"gold\":{},\"excludedQuestItems\":{},\"appearanceBytes\":{},"
                       "\"tints\":{},\"lookHash\":\"{:016X}\"",
        JsonText(acSnapshot.Name.c_str()), acSnapshot.Level, acSnapshot.Xp, acSnapshot.LevelThreshold, acSnapshot.PerkPoints,
        skills, values, acSnapshot.DragonSouls, acSnapshot.Perks.size(), acSnapshot.Spells.size(), acSnapshot.Shouts.size(),
        acSnapshot.Words.size(), unlocked, acSnapshot.Items.Entries.size(), gold, acSnapshot.ExcludedQuestItems.size(),
        acSnapshot.AppearanceBuffer.size(), acSnapshot.FaceTints.Entries.size(), lookHash);
}
} // namespace CharacterSnapshots
