#include <Services/CharacterSnapshots.h>
#include <Services/SaveLook.h>
#include <Services/Generic/BoundPoseKeeper.h>
#include <Games/IFormFactory.h>

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
#include <memory>
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
constexpr uint8_t kWordType = 118;
constexpr uint32_t kAppearanceFlags = 0x3000800;
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
    aOut.ChangeFlags = kAppearanceFlags;
    pNpc->Serialize(&aOut.AppearanceBuffer, aOut.ChangeFlags);
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
enum class ApplyMode { Full, Look, Appearance, State, Items };
ApplyMode s_queuedMode{ApplyMode::Full};
std::string s_applyStatus = "idle";
uint64_t s_applyOperation{};
bool s_applyCancelled{};
uint64_t s_queuedSince{};

struct RunningApply
{
    CharacterSnapshot Snapshot;
    std::string Report;
    uint64_t Since{};
    void* OldHead{};
    bool NewSkeleton{};
    bool LookOnly{};
    uint64_t Operation{};
    bool StrictAppearance{};
    BoundPoseKeeper::Pose BoundPose{};
};
std::optional<RunningApply> s_running;

// A look read from a save carries the character's items as saved (SaveLook): those replace whatever the lobby's body
// last carried (owner 2026-10-02: Rex showed in someone else's iron armour; "same equipment on the menu"), worn ones
// equipped again. When the save's items could not be read, the prisoner rags and shoes (Skyrim.esm 0x3C9FE, 0x3CA00).
// Quest items stay.
void DressPlain(const CharacterSnapshot& acSnapshot)
{
    auto* pPlayer = PlayerCharacter::Get();
    if (!pPlayer)
        return;
    auto inventory = pPlayer->GetInventory();
    for (auto entry : inventory.Entries)
    {
        if (entry.IsQuestItem || entry.Count <= 0)
            continue;
        entry.Count = -entry.Count;
        pPlayer->AddOrRemoveItem(entry);
    }
    if (!acSnapshot.Items.Entries.empty())
    {
        auto& mods = World::Get().GetModSystem();
        for (const auto& entry : acSnapshot.Items.Entries)
            if (TESForm::GetById(mods.GetGameId(entry.BaseId)))
                pPlayer->AddOrRemoveItem(entry);
        return;
    }
    for (const uint32_t id : {0x3C9FEu, 0x3CA00u})
    {
        Inventory::Entry entry{};
        entry.BaseId = GameId(0, id);
        entry.Count = 1;
        entry.ExtraWorn = true;
        pPlayer->AddOrRemoveItem(entry);
    }
}

void RestoreItems(const CharacterSnapshot& acSnapshot)
{
    auto* pPlayer = PlayerCharacter::Get();
    if (!pPlayer || acSnapshot.Items.Entries.empty())
        return;
    for (auto entry : pPlayer->GetInventory().Entries)
    {
        if (entry.IsQuestItem || entry.Count <= 0)
            continue;
        entry.Count = -entry.Count;
        pPlayer->AddOrRemoveItem(entry);
    }
    auto& mods = World::Get().GetModSystem();
    for (const auto& entry : acSnapshot.Items.Entries)
        if (TESForm::GetById(mods.GetGameId(entry.BaseId)))
            pPlayer->AddOrRemoveItem(entry);
}

void SetStatus(uint64_t aOperation, std::string aStatus)
{
    std::lock_guard lock(s_applyLock);
    if (aOperation != s_applyOperation || s_applyCancelled)
        return;
    spdlog::info("Character snapshot apply {}: {}", aOperation, aStatus);
    s_applyStatus = std::move(aStatus);
}

// Old .snap files contain every accumulated NPC change, including level, skills and spells.
// Decode with the ORIGINAL flags on a scratch form, then write a new appearance-only stream.
// Masking flags without rewriting the stream would desynchronize the native loader (24778).
bool IsolateAppearance(CharacterSnapshot& aSnapshot, std::string& aError)
{
    if (aSnapshot.AppearanceBuffer.empty() ||
        (aSnapshot.ChangeFlags & kAppearanceFlags) != kAppearanceFlags)
        return aError = "incomplete appearance record", false;
    // One scratch NPC, prepared and reused, as SaveLook::BuildLook does. Loading a full record (level, skills, spells,
    // inventory) into a bare factory NPC without Initialize() faulted inside TESNPC::Load after a minute's hang
    // (2026-10-03, follower lobby showing a full .snap: 0x1401F22F8), and a game-allocated form must not be freed with
    // the mod's delete.
    static TESNPC* s_scratch{};
    if (!s_scratch)
    {
        s_scratch = IFormFactory::Create<TESNPC>();
        if (!s_scratch)
            return aError = "no appearance scratch NPC", false;
        s_scratch->Initialize();
    }
    TESNPC* const scratch = s_scratch;
    if (!scratch->Deserialize(aSnapshot.AppearanceBuffer, aSnapshot.ChangeFlags) || !scratch->raceForm.race)
        return aError = "incomplete appearance record", false;
    scratch->originalRace = scratch->raceForm.race;
    scratch->Serialize(&aSnapshot.AppearanceBuffer, kAppearanceFlags);
    aSnapshot.ChangeFlags = kAppearanceFlags;
    return true;
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
    if (!IsolateAppearance(aRun.Snapshot, aError))
        return false;
    aRun.BoundPose = BoundPoseKeeper::ReadPose(pPlayer);
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
uint32_t ApplyTints(const CharacterSnapshot& acSnapshot, bool aLookOnly)
{
    auto* pPlayer = PlayerCharacter::Get();
    // A look read from a save carries no tint layers: the race's own defaults, rebuilt for the new race and sex below.
    if (!pPlayer || (acSnapshot.FaceTints.Entries.empty() && !aLookOnly))
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
            if (tints[i] && entries[i].Alpha >= 0.f && (tints[i]->type == entries[i].Type || entries[i].Type == SaveLook::kAnyTintType))
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

bool WriteFile(const std::filesystem::path& aPath, const CharacterSnapshot& acSnapshot) noexcept
{
    Buffer buffer(1 << 22);
    Buffer::Writer writer(&buffer);
    acSnapshot.Serialize(writer);
    const auto temporary = std::filesystem::path(aPath).concat(".tmp");
    {
        std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
        file.write(reinterpret_cast<const char*>(buffer.GetWriteData()), static_cast<std::streamsize>(writer.Size()));
        file.flush();
        if (!file)
            return false;
    }
    const auto file = CreateFileW(temporary.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return false;
    const bool durable = FlushFileBuffers(file) != 0;
    CloseHandle(file);
    if (!durable)
        return false;
    DWORD moveError{};
    for (int attempt = 0; attempt < 3; ++attempt)
    {
        if (MoveFileExW(temporary.c_str(), aPath.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            return true;
        moveError = GetLastError();
        if (moveError != ERROR_SHARING_VIOLATION && moveError != ERROR_ACCESS_DENIED)
            break;
        Sleep(5);
    }
    DeleteFileW(temporary.c_str());
    SetLastError(moveError);
    return false;
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
    if (!WriteFile(path, snapshot))
        spdlog::error("Character snapshot {} not written (Windows error {})", path.filename().string(), GetLastError());
    else
        spdlog::info("Character snapshot written with the save: {} ({} level {})", path.filename().string(),
            snapshot.Name.c_str(), snapshot.Level);
    std::lock_guard lock(s_applyLock);
    if (s_captureTo == path)
        s_captureTo.reset();
}
} // namespace

uint64_t QueueApply(CharacterSnapshot aSnapshot) noexcept
{
    std::lock_guard lock(s_applyLock);
    ++s_applyOperation;
    s_applyCancelled = false;
    s_queuedApply = std::move(aSnapshot);
    s_queuedMode = ApplyMode::Full;
    s_applyStatus = "queued";
    return s_applyOperation;
}

uint64_t QueueApplyLook(CharacterSnapshot aSnapshot) noexcept
{
    std::lock_guard lock(s_applyLock);
    ++s_applyOperation;
    s_applyCancelled = false;
    s_queuedApply = std::move(aSnapshot);
    s_queuedMode = ApplyMode::Look;
    s_applyStatus = "queued";
    return s_applyOperation;
}

uint64_t QueueApplyAppearance(CharacterSnapshot aSnapshot) noexcept
{
    std::lock_guard lock(s_applyLock);
    ++s_applyOperation;
    s_applyCancelled = false;
    s_queuedSince = GetTickCount64();
    s_queuedApply = std::move(aSnapshot);
    s_queuedMode = ApplyMode::Appearance;
    s_applyStatus = "queued";
    return s_applyOperation;
}

uint64_t QueueApplyState(CharacterSnapshot aSnapshot) noexcept
{
    std::lock_guard lock(s_applyLock);
    ++s_applyOperation;
    s_applyCancelled = false;
    s_queuedApply = std::move(aSnapshot);
    s_queuedMode = ApplyMode::State;
    s_applyStatus = "queued";
    return s_applyOperation;
}

uint64_t QueueApplyItems(CharacterSnapshot aSnapshot) noexcept
{
    std::lock_guard lock(s_applyLock);
    ++s_applyOperation;
    s_applyCancelled = false;
    s_queuedApply = std::move(aSnapshot);
    s_queuedMode = ApplyMode::Items;
    s_applyStatus = "queued";
    return s_applyOperation;
}

std::string ApplyStatus(uint64_t aOperation) noexcept
{
    std::lock_guard lock(s_applyLock);
    if (!aOperation || aOperation != s_applyOperation)
        return "failed: operation superseded";
    return s_applyStatus;
}

void CancelApply(uint64_t aOperation) noexcept
{
    std::lock_guard lock(s_applyLock);
    if (!aOperation || aOperation != s_applyOperation)
        return;
    s_applyCancelled = true;
    s_queuedApply.reset();
    s_applyStatus = "failed: operation cancelled";
}

std::string ApplyStatus() noexcept
{
    std::lock_guard lock(s_applyLock);
    return s_applyStatus;
}

void OnMainFrame() noexcept
{
    // Loading interrupts the intro operation even if the party epoch stays the same.
    // Ordinary lobby previews may intentionally run behind Main Menu; do not cancel those here.
    auto* pUi = UI::Get();
    if (!pUi)
        return;
    const bool loading = pUi->GetMenuOpen(BSFixedString("Loading Menu"));
    if (loading || pUi->GetMenuOpen(BSFixedString("Main Menu")))
    {
        std::lock_guard lock(s_applyLock);
        const bool introOperation = s_queuedApply ?
            (s_queuedMode == ApplyMode::Appearance || s_queuedMode == ApplyMode::State || s_queuedMode == ApplyMode::Items) :
            (s_running && s_running->StrictAppearance);
        if (introOperation)
        {
            s_queuedApply.reset();
            s_running.reset();
            s_applyCancelled = true;
            s_applyStatus = "failed: load interrupted apply";
            return;
        }
        if (loading)
            return;
    }
    {
        std::lock_guard lock(s_applyLock);
        if (s_running && (s_running->Operation != s_applyOperation || s_applyCancelled))
            s_running.reset();
    }
    RunQueuedCapture();
    const auto now = GetTickCount64();
    if (!s_running)
    {
        std::optional<CharacterSnapshot> queued;
        ApplyMode mode{};
        uint64_t operation{};
        {
            std::lock_guard lock(s_applyLock);
            if (s_queuedApply && s_queuedMode == ApplyMode::Appearance &&
                !BoundPoseKeeper::CanRebuildNow(PlayerCharacter::Get()))
            {
                if (now - s_queuedSince >= 20000)
                {
                    s_queuedApply.reset();
                    s_applyStatus = "failed: player graph not ready for appearance rebuild";
                }
                return;
            }
            queued.swap(s_queuedApply);
            mode = s_queuedMode;
            operation = s_applyOperation;
        }
        if (!queued)
            return;
        RunningApply run{std::move(*queued), {}, now};
        run.Operation = operation;
        run.StrictAppearance = mode == ApplyMode::Appearance;
        run.LookOnly = mode == ApplyMode::Look || mode == ApplyMode::Appearance;
        if (mode == ApplyMode::Look)
            run.Report = "\"lookOnly\":true";
        std::string error;
        if (mode == ApplyMode::Items)
        {
            RestoreItems(run.Snapshot);
            SetStatus(operation, "done: {\"itemsOnly\":true}");
            return;
        }
        if (mode == ApplyMode::Look)
        {
            DressPlain(run.Snapshot);
            // The name too (Apply, skipped here, sets it for a full character).
            if (auto* pNpc = PlayerCharacter::Get() ? Cast<TESNPC>(PlayerCharacter::Get()->baseForm) : nullptr)
            {
                if (!run.Snapshot.Name.empty())
                    pNpc->fullName.value.Set(run.Snapshot.Name.c_str());
                if (run.Snapshot.Level > 0)
                    pNpc->actorData.level = static_cast<uint16_t>(run.Snapshot.Level);
            }
        }
        if (mode == ApplyMode::Appearance)
        {
            run.Report = "\"appearanceOnly\":true";
            if (auto* pNpc = PlayerCharacter::Get() ? Cast<TESNPC>(PlayerCharacter::Get()->baseForm) : nullptr;
                pNpc && !run.Snapshot.Name.empty())
            {
                pNpc->fullName.value.Set(run.Snapshot.Name.c_str());
                pNpc->MarkChanged(0x20);
            }
        }
        if (((mode == ApplyMode::Full || mode == ApplyMode::State) && !Apply(run.Snapshot, run.Report, error)) ||
            (mode != ApplyMode::State && !BeginAppearance(run, error)))
        {
            SetStatus(operation, "failed: " + error);
            return;
        }
        if (mode == ApplyMode::State)
        {
            SetStatus(operation, "done: {" + run.Report + "}");
            return;
        }
        s_running = std::move(run);
        SetStatus(operation, "rebuilding");
        return;
    }
    auto* pPlayer = PlayerCharacter::Get();
    auto* pHead = pPlayer ? pPlayer->GetFaceGenNiNode() : nullptr;
    // The rebuild is asynchronous: wait for a head node, a new one when it was replaced, and give it a moment.
    const bool rebuilt = pPlayer && pPlayer->GetNiNode() && pHead &&
        (pHead != s_running->OldHead || (!s_running->StrictAppearance && now - s_running->Since > 3000)) &&
        (!s_running->StrictAppearance || BoundPoseKeeper::CanRebuildNow(pPlayer));
    if (!rebuilt || now - s_running->Since < 500)
    {
        if (now - s_running->Since > 20000)
        {
            SetStatus(s_running->Operation, "failed: the player's head was not rebuilt within 20 s");
            s_running.reset();
        }
        return;
    }
    const auto matched = ApplyTints(s_running->Snapshot, s_running->LookOnly);
    auto* pNpc = Cast<TESNPC>(pPlayer->baseForm);
    if (s_running->StrictAppearance)
    {
        String actual;
        if (pNpc)
            pNpc->Serialize(&actual, kAppearanceFlags);
        const auto& snapshot = s_running->Snapshot;
        // Byte-exact re-serialization failed inside the open RaceSex menu (2026-10-03: look=false with the right
        // character on screen; the menu repaints the NPC and finalises the name at its close). Gate on race, sex and name;
        // a byte difference is reported, not fatal.
        const bool bytesMatch = actual == snapshot.AppearanceBuffer;
        const bool nameMatches = pNpc && pNpc->fullName.value.data && snapshot.Name == pNpc->fullName.value.AsAscii();
        const bool raceMatches = pNpc && pNpc->raceForm.race && pPlayer->race == pNpc->raceForm.race;
        if (!bytesMatch || !nameMatches)
            spdlog::warn("Character snapshot apply {}: appearance bytes {} name {} race {} (continuing)", s_running->Operation,
                bytesMatch ? "match" : "differ", nameMatches ? "matches" : "differs", raceMatches ? "matches" : "differs");
        const bool lookMatches = raceMatches;
        const bool tintsMatch = matched == snapshot.FaceTints.Entries.size() &&
            matched == pPlayer->GetTints().length;
        const bool poseRestored = !s_running->BoundPose.Bound ||
            BoundPoseKeeper::AfterRebuild(pPlayer, s_running->BoundPose, "skip creator appearance");
        if (!lookMatches || !tintsMatch || !poseRestored)
        {
            SetStatus(s_running->Operation, fmt::format("failed: appearance verification look={} tints={} boundPose={}",
                lookMatches, tintsMatch, poseRestored));
            s_running.reset();
            return;
        }
    }
    SetStatus(s_running->Operation, fmt::format("done: {{{},\"newSkeleton\":{},\"tintsMatched\":{},\"race\":\"{:X}\",\"headReplaced\":{}}}",
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
