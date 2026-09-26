#include <Games/References.h>
#include <Games/Skyrim/EquipManager.h>
#include <atomic>
#include <AI/AIProcess.h>
#include <Misc/MiddleProcess.h>
#include <Misc/GameVM.h>
#include <DefaultObjectManager.h>
#include <Forms/TESNPC.h>
#include <Forms/TESFaction.h>
#include <Components/TESActorBaseData.h>
#include <ExtraData/ExtraFactionChanges.h>
#include <ExtraData/ExtraLeveledCreature.h>
#include <Games/Memory.h>
#include <Combat/CombatController.h>

#include <Events/HealthChangeEvent.h>
#include <Events/InventoryChangeEvent.h>
#include <Events/MountEvent.h>
#include <Events/DialogueEvent.h>
#include <Games/Misc/MenuTopicManager.h>
#include <Events/HitEvent.h>
#include <Events/RemoveSpellEvent.h>

#include <Games/TES.h>
#include <World.h>
#include <Services/PapyrusService.h>
#include <Services/PartyService.h>
#include <Services/ObjectService.h>
#include <Services/CorpseRagdollService.h>
#include <Services/TransportService.h>

#include <Forms/ActorValueInfo.h>
#include <Forms/TESRace.h>

#include <Effects/ValueModifierEffect.h>

#include <Games/Skyrim/Misc/InventoryEntry.h>
#include <Games/Skyrim/ExtraData/ExtraCount.h>
#include <NetImmerse/NiPointer.h>
#include <Games/Misc/ActorKnowledge.h>

#include <ExtraData/ExtraDataList.h>
#include <ExtraData/ExtraCharge.h>
#include <ExtraData/ExtraCount.h>
#include <ExtraData/ExtraEnchantment.h>
#include <ExtraData/ExtraHealth.h>
#include <ExtraData/ExtraPoison.h>
#include <ExtraData/ExtraSoul.h>
#include <ExtraData/ExtraTextDisplayData.h>
#include <Forms/EnchantmentItem.h>
#include <Forms/AlchemyItem.h>
#include <Forms/TESObjectCELL.h>

#include <Structs/Skyrim/AnimationGraphDescriptor_BHR_Master.h>

#include <Games/Overrides.h>
#include <Games/Skyrim/BSAnimationGraphManager.h>
#include <Havok/hkbStateMachine.h>
#include <Havok/hkbBehaviorGraph.h>

#include <ModCompat/BehaviorVar.h>

namespace
{
void QueueActorInventoryChange(Actor* apActor, InventoryChangeEvent aEvent, TESObjectREFR* apTransferReference = nullptr)
{
    auto ownershipToken = Utils::GetLocalOwnershipToken(apActor->formID);
    if (!ownershipToken && apTransferReference == PlayerCharacter::Get())
        ownershipToken = Utils::GetRemoteOwnershipToken(apActor->formID);

    if (!ownershipToken)
        return;

    aEvent.ServerId = ownershipToken->ServerId;
    aEvent.OwnershipEpoch = ownershipToken->OwnershipEpoch;
    World::Get().GetRunner().Trigger(std::move(aEvent));
}
}

#ifdef SAVE_STUFF

#include <Games/Skyrim/SaveLoad.h>
#include "Actor.h"

void Actor::Save_Reversed(const uint32_t aChangeFlags, Buffer::Writer& aWriter)
{
    BGSSaveFormBuffer buffer;

    Save(&buffer);

    AIProcess* pProcess = currentProcess;
    const int32_t handlerId = pProcess != nullptr ? pProcess->handlerId : -1;

    aWriter.WriteBytes((uint8_t*)&handlerId, 4); // TODO: is this needed ?
    aWriter.WriteBytes((uint8_t*)&flags1, 4);

    //     if (!handlerId
    //         && (uint8_t)AIProcess::GetBoolInSubStructure(pProcess))
    //     {
    //         Actor::SaveSkinFar(this);
    //     }

    TESObjectREFR::Save_Reversed(aChangeFlags, aWriter);

    if (pProcess)
        ; // Skyrim saves the process manager state, but we don't give a shit so skip !

    aWriter.WriteBytes((uint8_t*)&unk194, 4);
    aWriter.WriteBytes((uint8_t*)&headTrackingUpdateDelay, 4);
    aWriter.WriteBytes((uint8_t*)&unk9C, 4);
    // We skip 0x180 as it's not something we care about, some timer related data

    aWriter.WriteBytes((uint8_t*)&unk98, 4);
    // skip A8 - related to timers
    // skip AC - related to timers as well
    aWriter.WriteBytes((uint8_t*)&unkB0, 4);
    // skip E4 - never seen this used
    // skip E8 - same as E4
    aWriter.WriteBytes((uint8_t*)&unk84, 4);
    aWriter.WriteBytes((uint8_t*)&unkA4, 4);
    // skip baseForm->weight
    // skip 12C

    // Save actor state sub_6F0FB0
}

#endif

TP_THIS_FUNCTION(TRemoveSpell, bool, Actor, MagicItem*);
TP_THIS_FUNCTION(TCharacterConstructor, Actor*, Actor);
TP_THIS_FUNCTION(TCharacterConstructor2, Actor*, Actor, uint8_t aUnk);
TP_THIS_FUNCTION(TCharacterDestructor, Actor*, Actor);
TP_THIS_FUNCTION(TAddInventoryItem, void, Actor, TESBoundObject* apItem, ExtraDataList* apExtraData, int32_t aCount, TESObjectREFR* apOldOwner);
TP_THIS_FUNCTION(TPickUpObject, void*, Actor, TESObjectREFR* apObject, int32_t aCount, bool aUnk1, float aUnk2);
TP_THIS_FUNCTION(TDropObject, void*, Actor, void* apResult, TESBoundObject* apObject, ExtraDataList* apExtraData, int32_t aCount, NiPoint3* apLocation, NiPoint3* apRotation);
TP_THIS_FUNCTION(TSetPosition, char, Actor, NiPoint3& acPosition);
TP_THIS_FUNCTION(TKnockExplosion, void, AIProcess, Actor*, const NiPoint3&, float);
TP_THIS_FUNCTION(TActorProcess, char, Actor, float aValue);
TP_THIS_FUNCTION(TNativeExtraDataAdd, BSExtraData*, ExtraDataList, BSExtraData*);
TP_THIS_FUNCTION(TNativeSetInteraction, void, ExtraDataList, void*);

using TGetLocation = TESForm*(TESForm*);
static TGetLocation* FUNC_GetActorLocation;

TCharacterConstructor* RealCharacterConstructor;
TCharacterConstructor2* RealCharacterConstructor2;
TCharacterDestructor* RealCharacterDestructor;

static TRemoveSpell* RealRemoveSpell = nullptr;
static TAddInventoryItem* RealAddInventoryItem = nullptr;
static TPickUpObject* RealPickUpObject = nullptr;
static TDropObject* RealDropObject = nullptr;
static TSetPosition* RealSetPosition = nullptr;
static TKnockExplosion* RealKnockExplosion = nullptr;
static std::atomic<uint64_t> s_nullKnockExplosionSkips{};
static TActorProcess* RealActorProcess = nullptr;
static TNativeExtraDataAdd* RealNativeExtraDataAdd = nullptr;
static TNativeSetInteraction* RealNativeSetInteraction = nullptr;
static std::atomic<uint32_t> s_interactionAddTraceCount{};
static std::atomic<uint32_t> s_interactionSetTraceCount{};
static std::atomic<uint32_t> s_mountPackageTraceCount{};
static std::atomic<bool> s_interactionTraceArmed{};
static std::atomic<uint32_t> s_remoteProcessTrialRiderFormId{};
static std::atomic<uint32_t> s_remoteProcessTrialMountFormId{};
static std::atomic<uint64_t> s_remoteProcessTrialTicks{};

float Actor::GetSpeed() noexcept
{
    static BSFixedString speedSampledStr("SpeedSampled");
    float speed = 0.f;
    animationGraphHolder.GetVariableFloat(&speedSampledStr, &speed);

    return speed;
}

void Actor::SetSpeed(float aSpeed) noexcept
{
    static BSFixedString speedSampledStr("SpeedSampled");
    animationGraphHolder.SetVariableFloat(&speedSampledStr, aSpeed);
}

TESNPC* Actor::GetLeveledPick() const noexcept
{
    const auto* pExtra = static_cast<ExtraLeveledCreature*>(extraData.GetByType(ExtraDataType::LeveledCreature));
    TESActorBase* pTemplate = pExtra ? pExtra->templateBase : nullptr;
    if (!pTemplate || pTemplate->formType != FormType::Npc || pTemplate->IsTemporary())
        return nullptr;

    return static_cast<TESNPC*>(pTemplate);
}

uint16_t Actor::GetLevel() const noexcept
{
    TP_THIS_FUNCTION(TGetLevel, uint16_t, const Actor);
    POINTER_SKYRIMSE(TGetLevel, s_getLevel, 37334);
    return TiltedPhoques::ThisCall(s_getLevel, this);
}

void Actor::ForcePosition(const NiPoint3& acPosition) noexcept
{
    ScopedReferencesOverride recursionGuard;

    // It just works TM
    SetPosition(acPosition, true);
}

void Actor::QueueUpdate() noexcept
{
    auto* pSetting = INISettingCollection::Get()->GetSetting("bUseFaceGenPreprocessedHeads:General");
    const auto originalValue = pSetting->data;
    pSetting->data = 0;

    TP_THIS_FUNCTION(TQueueUpdate, void, Actor, bool);
    POINTER_SKYRIMSE(TQueueUpdate, QueueUpdate, 40255);

    TiltedPhoques::ThisCall(QueueUpdate, this, true);

    pSetting->data = originalValue;
}

GamePtr<Actor> Actor::Create(TESNPC* apBaseForm) noexcept
{
    auto pActor = New();
    // Prevent saving
    pActor->SetSkipSaveFlag(true);
    pActor->GetExtension()->SetRemote(true);

    const auto pPlayer = static_cast<Actor*>(GetById(0x14));
    auto pCell = pPlayer->parentCell;
    const auto pWorldSpace = pPlayer->GetWorldSpace();

    pActor->SetLevelMod(4);
    pActor->MarkChanged(0x40000000);
    pActor->SetParentCell(pCell);
    pActor->SetObjectReference(apBaseForm);

    auto position = pPlayer->position;
    auto rotation = pPlayer->rotation;

    if (pCell && !(pCell->cellFlags & 1))
        pCell = nullptr;

    ModManager::Get()->Spawn(position, rotation, pCell, pWorldSpace, pActor);

    pActor->ForcePosition(position);

    pActor->GetMagicCaster(MagicSystem::CastingSource::LEFT_HAND);
    pActor->GetMagicCaster(MagicSystem::CastingSource::RIGHT_HAND);
    pActor->GetMagicCaster(MagicSystem::CastingSource::OTHER);

    pActor->flags &= 0xFFDFFFFF;

    return pActor;
}

GamePtr<Actor> Actor::Spawn(uint32_t aBaseFormId) noexcept
{
    TESNPC* pNpc = Cast<TESNPC>(TESForm::GetById(aBaseFormId));
    return Actor::Create(pNpc);
}

void Actor::SetLevelMod(uint32_t aLevel) noexcept
{
    TP_THIS_FUNCTION(TActorSetLevelMod, void, ExtraDataList, uint32_t);
    POINTER_SKYRIMSE(TActorSetLevelMod, realSetLevelMod, 11806);

    const auto pExtraDataList = &extraData;

    TiltedPhoques::ThisCall(realSetLevelMod, pExtraDataList, aLevel);
}

ActorExtension* Actor::GetExtension() noexcept
{
    if (AsExActor())
    {
        return static_cast<ActorExtension*>(AsExActor());
    }

    if (AsExPlayerCharacter())
    {
        return static_cast<ActorExtension*>(AsExPlayerCharacter());
    }

    return nullptr;
}

ExActor* Actor::AsExActor() noexcept
{
    if (formType == Type && this != PlayerCharacter::Get())
        return static_cast<ExActor*>(this);

    return nullptr;
}

ExPlayerCharacter* Actor::AsExPlayerCharacter() noexcept
{
    if (this == PlayerCharacter::Get())
        return static_cast<ExPlayerCharacter*>(this);

    return nullptr;
}

extern thread_local bool g_forceAnimation;

void Actor::SetWeaponDrawnEx(bool aDraw) noexcept
{
    spdlog::debug("Setting weapon drawn: {:X}:{}, current state: {}", formID, aDraw, actorState.IsWeaponDrawn());

    if (actorState.IsWeaponDrawn() == aDraw)
    {
        actorState.SetWeaponDrawn(!aDraw);

        spdlog::debug("Setting weapon drawn after update: {:X}:{}, current state: {}", formID, aDraw, actorState.IsWeaponDrawn());
    }

    g_forceAnimation = true;
    SetWeaponDrawn(aDraw);
    g_forceAnimation = false;
}

static thread_local bool s_execInitPackage = false;

void Actor::SetPackage(TESPackage* apPackage) noexcept
{
    s_execInitPackage = true;
    PutCreatedPackage(apPackage);
    s_execInitPackage = false;
}

void Actor::SetPlayerRespawnMode(bool aSet) noexcept
{
    SetEssentialEx(aSet);
    // Makes the player go in an unrecoverable bleedout state
    SetNoBleedoutRecovery(aSet);

    if (formID != 0x14)
    {
        auto pPlayerFaction = Cast<TESFaction>(TESForm::GetById(0xDB1));
        SetFactionRank(pPlayerFaction, 1);
    }
}

void Actor::SetEssentialEx(bool aSet) noexcept
{
    SetEssential(aSet);
    TESNPC* pBase = Cast<TESNPC>(baseForm);
    if (pBase)
        pBase->actorData.SetEssential(aSet);
}

void Actor::SetNoBleedoutRecovery(bool aSet) noexcept
{
    TP_THIS_FUNCTION(TSetNoBleedoutRecovery, void, Actor, bool);
    POINTER_SKYRIMSE(TSetNoBleedoutRecovery, s_setNoBleedoutRecovery, 38533);
    TiltedPhoques::ThisCall(s_setNoBleedoutRecovery, this, aSet);
}

void Actor::DispelAllSpells(bool aNow) noexcept
{
    magicTarget.DispelAllSpells(aNow);
}

bool Actor::IsInCombat() const noexcept
{
    PAPYRUS_FUNCTION(bool, Actor, IsInCombat);
    return s_pIsInCombat(this);
}

Actor* Actor::GetCombatTarget() const noexcept
{
    PAPYRUS_FUNCTION(Actor*, Actor, GetCombatTarget);
    return s_pGetCombatTarget(this);
}

// TODO: this is a really hacky solution.
// The internal targeting system should be disabled instead.
void Actor::StartCombatEx(Actor* apTarget) noexcept
{
    if (GetCombatTarget() != apTarget)
    {
        StopCombat();
        StartCombat(apTarget);
    }
}

void Actor::SetCombatTargetEx(Actor* apTarget) noexcept
{
    if (pCombatController)
        pCombatController->SetTarget(apTarget);
}

void Actor::StartCombat(Actor* apTarget) noexcept
{
    PAPYRUS_FUNCTION(void, Actor, StartCombat, Actor*);
    s_pStartCombat(this, apTarget);
}

void Actor::StopCombat() noexcept
{
    PAPYRUS_FUNCTION(void, Actor, StopCombat);
    s_pStopCombat(this);
}

bool Actor::RemoveSpell(MagicItem* apSpell) noexcept
{
    if (!apSpell)
    {
        spdlog::error(__FUNCTION__ ": apSpell is null");
        return false;
    }
    // spdlog::info(__FUNCTION__ ": removing: {} from actor: {}", apSpell->formID, formID);
    return TiltedPhoques::ThisCall(RealRemoveSpell, this, apSpell);
}

bool Actor::HasPerk(uint32_t aPerkFormId) const noexcept
{
    return GetPerkRank(aPerkFormId) != 0;
}

uint8_t Actor::GetPerkRank(uint32_t aPerkFormId) const noexcept
{
    BGSPerk* pPerk = Cast<BGSPerk>(TESForm::GetById(aPerkFormId));
    if (!pPerk)
        return 0;

    TP_THIS_FUNCTION(TGetPerkRank, uint8_t, const Actor, BGSPerk*);
    POINTER_SKYRIMSE(TGetPerkRank, getPerkRank, 37698);

    return TiltedPhoques::ThisCall(getPerkRank, this, pPerk);
}

bool TP_MAKE_THISCALL(HookRemoveSpell, Actor, MagicItem* apSpell)
{
    bool result = TiltedPhoques::ThisCall(RealRemoveSpell, apThis, apSpell);
    if (apThis->GetExtension()->IsLocalPlayer() && result)
    {
        //spdlog::info(__FUNCTION__ ": spell: {}, ID: {} from local player", apSpell->GetName() , apSpell->formID);
       RemoveSpellEvent removalEvent;

        removalEvent.TargetId = apThis->formID;
        removalEvent.SpellId = apSpell->formID;
        World::Get().GetRunner().Trigger(removalEvent);
    }

    return result;
}

Actor* TP_MAKE_THISCALL(HookCharacterConstructor, Actor)
{
    TP_EMPTY_HOOK_PLACEHOLDER;

    TiltedPhoques::ThisCall(RealCharacterConstructor, apThis);

    return apThis;
}

Actor* TP_MAKE_THISCALL(HookCharacterConstructor2, Actor, uint8_t aUnk)
{
    TP_EMPTY_HOOK_PLACEHOLDER;

    TiltedPhoques::ThisCall(RealCharacterConstructor2, apThis, aUnk);

    return apThis;
}

Actor* TP_MAKE_THISCALL(HookCharacterDestructor, Actor)
{
    TP_EMPTY_HOOK_PLACEHOLDER;

    auto pExtension = apThis->GetExtension();

    if (pExtension)
    {
        pExtension->~ActorExtension();
    }

    TiltedPhoques::ThisCall(RealCharacterDestructor, apThis);

    return apThis;
}

GamePtr<Actor> Actor::New() noexcept
{
    auto* const pActor = Memory::Allocate<Actor>();

    TiltedPhoques::ThisCall(RealCharacterConstructor, pActor);

    return {pActor};
}

void Actor::InterruptCast(bool abRefund) noexcept
{
    TP_THIS_FUNCTION(TInterruptCast, void, Actor, bool abRefund);

    POINTER_SKYRIMSE(TInterruptCast, s_interruptCast, 38757);

    TiltedPhoques::ThisCall(s_interruptCast, this, abRefund);
}

TESForm* Actor::GetEquippedWeapon(uint32_t aSlotId) const noexcept
{
    if (currentProcess && currentProcess->middleProcess)
    {
        auto pMiddleProcess = currentProcess->middleProcess;

        if (aSlotId == 0 && pMiddleProcess->leftEquippedObject)
            return pMiddleProcess->leftEquippedObject->pObject;

        else if (aSlotId == 1 && pMiddleProcess->rightEquippedObject)
            return pMiddleProcess->rightEquippedObject->pObject;
    }

    return nullptr;
}

TESForm* Actor::GetEquippedAmmo() const noexcept
{
    if (currentProcess && currentProcess->middleProcess && currentProcess->middleProcess->ammoEquippedObject)
    {
        // TODO: rtti cast to check if is ammo object? or actually, just call AIProcess::GetCurrentAmmo()
        return currentProcess->middleProcess->ammoEquippedObject->pObject;
    }

    return nullptr;
}

// Get owner of a summon or raised corpse
Actor* Actor::GetCommandingActor() const noexcept
{
    if (currentProcess && currentProcess->middleProcess && currentProcess->middleProcess->commandingActor)
    {
        auto handle = currentProcess->middleProcess->commandingActor.handle;
        auto* pOwner = Cast<Actor>(TESObjectREFR::GetByHandle(handle.iBits));
        return pOwner;
    }

    return nullptr;
}

// Get owner of a summon or raised corpse
void Actor::SetCommandingActor(BSPointerHandle<TESObjectREFR> aCommandingActor) noexcept
{
    if (currentProcess && currentProcess->middleProcess)
    {
        currentProcess->middleProcess->commandingActor = aCommandingActor;
        flags2 |= ActorFlags::IS_COMMANDED_ACTOR;
    }
}

bool Actor::IsPlayerSummon() const noexcept
{
    const Actor* pCommandingActor = GetCommandingActor();
    return pCommandingActor && pCommandingActor->formID == 0x14;
}

TESForm* Actor::GetCurrentLocation()
{
    // we use the safe function which also
    // checks the form type
    return FUNC_GetActorLocation(this);
}

Factions Actor::GetFactions() const noexcept
{
    Factions result;

    auto& modSystem = World::Get().GetModSystem();

    auto* pNpc = Cast<TESNPC>(baseForm);
    if (pNpc)
    {
        auto& factions = pNpc->actorData.factions;

        for (auto i = 0u; i < factions.length; ++i)
        {
            Faction faction;

            modSystem.GetServerModId(factions[i].faction->formID, faction.Id);
            faction.Rank = factions[i].rank;

            result.NpcFactions.push_back(faction);
        }
    }

    auto* pChanges = Cast<ExtraFactionChanges>(extraData.GetByType(ExtraDataType::Faction));
    if (pChanges)
    {
        for (auto i = 0u; i < pChanges->entries.length; ++i)
        {
            Faction faction;

            modSystem.GetServerModId(pChanges->entries[i].faction->formID, faction.Id);
            faction.Rank = pChanges->entries[i].rank;

            result.ExtraFactions.push_back(faction);
        }
    }

    return result;
}

ActorValues Actor::GetEssentialActorValues() const noexcept
{
    ActorValues actorValues;

    int essentialValues[] = {ActorValueInfo::kHealth, ActorValueInfo::kStamina, ActorValueInfo::kMagicka};
    for (auto i : essentialValues)
    {
        float value = actorValueOwner.GetValue(i);
        actorValues.ActorValuesList.insert({i, value});
        float maxValue = actorValueOwner.GetPermanentValue(i);
        actorValues.ActorMaxValuesList.insert({i, maxValue});
    }

    return actorValues;
}

float Actor::GetActorValue(uint32_t aId) const noexcept
{
    return actorValueOwner.GetValue(aId);
}

float Actor::GetActorPermanentValue(uint32_t aId) const noexcept
{
    return actorValueOwner.GetPermanentValue(aId);
}

void Actor::SetActorValue(uint32_t aId, float aValue) noexcept
{
    actorValueOwner.SetValue(aId, aValue);
}

void Actor::ForceActorValue(ActorValueOwner::ForceMode aMode, uint32_t aId, float aValue) noexcept
{
    float initialValue = aMode == ActorValueOwner::ForceMode::PERMANENT 
                         ? GetActorPermanentValue(aId)
                         : GetActorValue(aId);

    if (aValue == initialValue)
        return;

    actorValueOwner.ForceCurrent(aMode, aId, aValue - initialValue);
}

Inventory Actor::GetActorInventory() const noexcept
{
    Inventory inventory = GetInventory();

    inventory.CurrentMagicEquipment = GetMagicEquipment();

    return inventory;
}

MagicEquipment Actor::GetMagicEquipment() const noexcept
{
    MagicEquipment equipment;

    auto& modSystem = World::Get().GetModSystem();

    uint32_t mainId = magicItems[0] ? magicItems[0]->formID : 0;
    modSystem.GetServerModId(mainId, equipment.LeftHandSpell);

    uint32_t secondaryId = magicItems[1] ? magicItems[1]->formID : 0;
    modSystem.GetServerModId(secondaryId, equipment.RightHandSpell);

    uint32_t shoutId = equippedShout ? equippedShout->formID : 0;
    modSystem.GetServerModId(shoutId, equipment.Shout);

    return equipment;
}

Inventory Actor::GetEquipment() const noexcept
{
    Inventory inventory = GetInventory();
    inventory.RemoveByFilter([](const auto& entry) { return !entry.IsWorn(); });
    inventory.CurrentMagicEquipment = GetMagicEquipment();
    return inventory;
}

int32_t Actor::GetGoldAmount() const noexcept
{
    TP_THIS_FUNCTION(TGetGoldAmount, int32_t, const Actor);
    POINTER_SKYRIMSE(TGetGoldAmount, s_getGoldAmount, 37527);
    return TiltedPhoques::ThisCall(s_getGoldAmount, this);
}

namespace
{
// Actors whose biped parts SetActorInventory asked to rebuild, and when.
std::unordered_map<uint32_t, std::chrono::steady_clock::time_point> s_pendingReset3D;
// Queued from the client update (off the main thread), flushed on the main thread (Main::Update).
std::mutex s_pendingReset3DLock;
} // namespace

void Actor::SetActorInventory(const Inventory& acInventory) noexcept
{
    spdlog::info("Setting inventory for actor {:X}", formID);

    // The UnEquipAll() that used to be here is redundant,
    // as RemoveAllItems() unequips every item if needed.
    // Placing this UnEquipAll() here seems to trigger a Skyrim bug/race.

    Inventory currentInventory = GetActorInventory();

    // Already what the owner has: re-applying removes every item and re-equips it (and rebuilds the
    // 3D), which showed as the actor going naked for a moment on every ownership handover.
    if (currentInventory == acInventory)
    {
        spdlog::info("Inventory for actor {:X} already matches; left as is", formID);
        return;
    }

    if (!this->GetExtension()->IsPlayer() && currentInventory.ContainsQuestItems())
        SetInventoryRetainingQuestItems(currentInventory, acInventory);
    else
        SetInventory(acInventory);

    SetMagicEquipment(acInventory.CurrentMagicEquipment);

    // RemoveAllItems + re-equip leaves the worn forms right but not always their meshes.
    // Measured on the follower: the Headsman wore the same 4 items as on the host but his root
    // had 4 children instead of 8 (naked), and the Imperial horse lost its saddle and bridle.
    // Queue one biped rebuild per actor; inventory is often applied twice in a few ms while a
    // save loads, and rebuilding immediately each time raced the actor update (a crash in the
    // native per-actor update 15 s into a Continue).
    if (!GetExtension()->IsPlayer())
    {
        std::lock_guard lock(s_pendingReset3DLock);
        s_pendingReset3D[formID] = std::chrono::steady_clock::now() + std::chrono::milliseconds(250);
    }
}

void Actor::QueueReset3D(const uint32_t aDelayMs) noexcept
{
    std::lock_guard lock(s_pendingReset3DLock);
    s_pendingReset3D[formID] = std::chrono::steady_clock::now() + std::chrono::milliseconds(aDelayMs);
}

// Main thread only (from HookMainLoop): DoReset3D (ID 40255) calls BipedAnim::RemoveAllParts before
// it checks whether to use the task queue, so its destructive half ran on whatever thread called it.
void Actor::FlushPendingReset3D() noexcept
{
    const auto now = std::chrono::steady_clock::now();
    std::vector<uint32_t> due;
    {
        std::lock_guard lock(s_pendingReset3DLock);
        for (auto it = s_pendingReset3D.begin(); it != s_pendingReset3D.end();)
        {
            if (now < it->second)
            {
                ++it;
                continue;
            }
            due.push_back(it->first);
            it = s_pendingReset3D.erase(it);
        }
    }
    for (const auto formId : due)
    {
        // Actor::DoReset3D(true) (ID 40255) drops every biped part and rebuilds them from what is worn.
        auto* pActor = Cast<Actor>(TESForm::GetById(formId));
        // Not while dying, dead, knocked down or ragdolling (ActorState1 lifeState bits 21-24,
        // knockState 25-27): the rebuild drew the falling intro prisoner naked for a moment and put
        // his ragdoll back at the actor's position.
        const uint32_t flags1 = pActor ? pActor->actorState.flags1 : 0;
        const bool physicsOwned = ((flags1 >> 21) & 0xF) != 0 || ((flags1 >> 25) & 0x7) != 0;
        if (pActor && !physicsOwned && !pActor->GetExtension()->IsPlayer() && !pActor->IsDead() && pActor->GetNiNode())
        {
            TP_THIS_FUNCTION(TDoReset3D, void, Actor, bool aRebuildParts);
            POINTER_SKYRIMSE(TDoReset3D, s_doReset3D, 40255);
            TiltedPhoques::ThisCall(s_doReset3D, pActor, true);
        }
    }
}

void Actor::SetMagicEquipment(const MagicEquipment& acEquipment) noexcept
{
    auto* pEquipManager = EquipManager::Get();
    auto& modSystem = World::Get().GetModSystem();

    if (acEquipment.LeftHandSpell)
    {
        uint32_t mainHandWeaponId = modSystem.GetGameId(acEquipment.LeftHandSpell);
        spdlog::debug("Setting left hand spell: {:X}", mainHandWeaponId);
        pEquipManager->EquipSpell(this, TESForm::GetById(mainHandWeaponId), 0);
    }

    if (acEquipment.RightHandSpell)
    {
        uint32_t secondaryHandWeaponId = modSystem.GetGameId(acEquipment.RightHandSpell);
        spdlog::debug("Setting right hand spell: {:X}", secondaryHandWeaponId);
        pEquipManager->EquipSpell(this, TESForm::GetById(secondaryHandWeaponId), 1);
    }

    if (acEquipment.Shout)
    {
        uint32_t shoutId = modSystem.GetGameId(acEquipment.Shout);
        spdlog::debug("Setting shout: {:X}", shoutId);
        pEquipManager->EquipShout(this, TESForm::GetById(shoutId));
    }
}

void Actor::SetActorValues(const ActorValues& acActorValues) noexcept
{
    for (auto& value : acActorValues.ActorMaxValuesList)
        ForceActorValue(ActorValueOwner::ForceMode::PERMANENT, value.first, value.second);

    for (auto& value : acActorValues.ActorValuesList)
        ForceActorValue(ActorValueOwner::ForceMode::DAMAGE, value.first, value.second);
}

void Actor::SetFactions(const Factions& acFactions) noexcept
{
    RemoveFromAllFactions();

    auto& modSystem = World::Get().GetModSystem();

    for (auto& entry : acFactions.NpcFactions)
    {
        auto pForm = GetById(modSystem.GetGameId(entry.Id));
        auto pFaction = Cast<TESFaction>(pForm);
        if (pFaction)
        {
            SetFactionRank(pFaction, entry.Rank);
        }
    }

    for (auto& entry : acFactions.ExtraFactions)
    {
        auto pForm = GetById(modSystem.GetGameId(entry.Id));
        auto pFaction = Cast<TESFaction>(pForm);
        if (pFaction)
        {
            SetFactionRank(pFaction, entry.Rank);
        }
    }
}

void Actor::SetFactionRank(const TESFaction* apFaction, int8_t aRank) noexcept
{
    TP_THIS_FUNCTION(TSetFactionRankInternal, void, Actor, const TESFaction*, int8_t);

    POINTER_SKYRIMSE(TSetFactionRankInternal, s_setFactionRankInternal, 37677);

    TiltedPhoques::ThisCall(s_setFactionRankInternal, this, apFaction, aRank);
}

void Actor::SetPlayerTeammate(bool aSet) noexcept
{
    TP_THIS_FUNCTION(TSetPlayerTeammate, void, Actor, bool aSet, bool abCanDoFavor);
    POINTER_SKYRIMSE(TSetPlayerTeammate, setPlayerTeammate, 37717);
    return TiltedPhoques::ThisCall(setPlayerTeammate, this, aSet, true);
}

void Actor::UnEquipAll() noexcept
{
    EquipManager::Get()->UnequipAll(this);

    // Taken from skyrim's code shouts can be two form types apparently
    if (equippedShout && ((int)equippedShout->formType - 41) <= 1)
    {
        EquipManager::Get()->UnEquipShout(this, equippedShout);
        equippedShout = nullptr;
    }
}

void Actor::RemoveFromAllFactions() noexcept
{
    PAPYRUS_FUNCTION(void, Actor, RemoveFromAllFactions);

    s_pRemoveFromAllFactions(this);
}

TP_THIS_FUNCTION(TInitiateMountPackage, bool, Actor, Actor* apMount);
static TInitiateMountPackage* RealInitiateMountPackage = nullptr;

bool Actor::InitiateMountPackage(Actor* apMount) noexcept
{
    return TiltedPhoques::ThisCall(RealInitiateMountPackage, this, apMount);
}

uint32_t Actor::GetNativeMountFormId() noexcept
{
    TP_THIS_FUNCTION(TGetMount, bool, Actor, NiPointer<Actor>&);
    POINTER_SKYRIMSE(TGetMount, s_getMount, 38702);
    NiPointer<Actor> mount{};
    const bool mounted = TiltedPhoques::ThisCall(s_getMount, this, mount);
    const uint32_t formId = mounted && mount.object ? mount.object->formID : 0;
    // This project's lightweight NiPointer has no destructor; the native
    // out-parameter retains its actor just like CommonLib's RAII pointer.
    if (mount.object)
        mount.object->handleRefObject.DecRefHandle();
    return formId;
}

bool Actor::SetNativeVehicle(TESObjectREFR* apVehicle) noexcept
{
    // Resolve only after the VM has registered the native binding. Unlike a
    // static PAPYRUS_FUNCTION wrapper, an early menu-time miss is not cached.
    const auto* address = World::Get().ctx().at<PapyrusService>().Get(
        "Actor", "SetVehicle");
    if (!address || !GameVM::Get() || !GameVM::Get()->virtualMachine)
        return false;
    PapyrusFunction<void, Actor, TESObjectREFR*> setVehicle(address);
    setVehicle(this, apVehicle);
    return true;
}

Actor::NativeMountState Actor::GetNativeMountState() const noexcept
{
    NativeMountState state{};
    const auto read = [](const void* address, void* destination, size_t size) noexcept
    {
        SIZE_T bytes{};
        return address && ReadProcessMemory(GetCurrentProcess(), address,
            destination, size, &bytes) && bytes == size;
    };
    if (const auto* horse = extraData.GetByType(ExtraDataType::Horse))
    {
        state.HorseExtra = true;
        read(reinterpret_cast<const uint8_t*>(horse) + 0x10,
            &state.HorseHandle, sizeof(state.HorseHandle));
    }
    if (const auto* extra = extraData.GetByType(ExtraDataType::Interaction))
    {
        state.InteractionExtra = true;
        const void* interaction{};
        if (read(reinterpret_cast<const uint8_t*>(extra) + 0x10,
                &interaction, sizeof(interaction)) && interaction)
        {
            state.InteractionPointerPresent = true;
            read(reinterpret_cast<const uint8_t*>(interaction) + 0x10,
                &state.InteractionActorHandle, sizeof(state.InteractionActorHandle));
            read(reinterpret_cast<const uint8_t*>(interaction) + 0x14,
                &state.InteractionTargetHandle, sizeof(state.InteractionTargetHandle));
        }
    }
    return state;
}

void Actor::SetRemoteProcessTrial(uint32_t aRiderFormId,
    uint32_t aMountFormId) noexcept
{
    s_remoteProcessTrialRiderFormId.store(aRiderFormId, std::memory_order_release);
    s_remoteProcessTrialMountFormId.store(aMountFormId, std::memory_order_release);
    s_remoteProcessTrialTicks.store(0, std::memory_order_relaxed);
}

uint64_t Actor::GetRemoteProcessTrialTicks() noexcept
{
    return s_remoteProcessTrialTicks.load(std::memory_order_relaxed);
}

void Actor::GenerateMagicCasters() noexcept
{
    using CS = MagicSystem::CastingSource;

    for (int i = 0; i < 4; i++)
    {
        if (casters[i] == nullptr)
            casters[i] = Cast<ActorMagicCaster>(GetMagicCaster(static_cast<CS>(i)));
    }
}

bool Actor::IsDead() const noexcept
{
    // This can be queried while a new game is still registering Papyrus
    // functions. Do not cache the lookup: a null result during that window
    // would otherwise remain null for the lifetime of the process.
    PapyrusFunction<bool, Actor> isDead(World::Get().ctx().at<PapyrusService>().Get("Actor", "IsDead"));

    if (!isDead)
        return false;

    return isDead(this);
}

uint64_t Actor::GetNullKnockExplosionSkips() noexcept
{
    return s_nullKnockExplosionSkips.load(std::memory_order_relaxed);
}

bool Actor::IsDragon() const noexcept
{
    const ActorExtension* pExtension = const_cast<Actor*>(this)->GetExtension();
    return BehaviorVar::IsDragon(pExtension->GraphDescriptorHash);
}

// Set around our death sync's KillImpl call (see HookKillImpl).
thread_local bool t_syncKill = false;
thread_local bool t_syncDecapitate = false;

NiNode* Actor::GetDetachedLimbNode(uint32_t aLimb) noexcept
{
    using TGetLimb = NiNode*(AIProcess*, uint32_t);
    POINTER_SKYRIMSE(TGetLimb, getLimb, 39974);
    return currentProcess && aLimb == 1 ? getLimb.Get()(currentProcess, aLimb) : nullptr;
}

void Actor::ApplyRemoteDecapitation() noexcept
{
    using TDecapitate = void(Actor*);
    POINTER_SKYRIMSE(TDecapitate, decapitate, 37639);
    const bool previous = t_syncDecapitate;
    t_syncDecapitate = true;
    decapitate.Get()(this);
    t_syncDecapitate = previous;
}

void Actor::Kill() noexcept
{
    // Never kill players
    ActorExtension* pExtension = GetExtension();
    if (pExtension->IsPlayer())
        return;

    // TODO: these args are kind of bogus of course
    t_syncKill = true;
    KillImpl(nullptr, 100.f, true, !(pExtension && pExtension->IsRemote()));
    t_syncKill = false;

    // Papyrus kill will not go through if it is queued by a kill move
    /*
    PAPYRUS_FUNCTION(void, Actor, Kill, void*);
    s_pKill(this, NULL);
    */
}

void Actor::KillIntoRagdoll() noexcept
{
    if (GetExtension()->IsPlayer())
        return;
    t_syncKill = true;
    KillImpl(nullptr, 100.f, true, true);
    t_syncKill = false;
}

void Actor::Reset() noexcept
{
    using ObjectReference = TESObjectREFR;

    PAPYRUS_FUNCTION(void, ObjectReference, Reset, int, TESObjectREFR*);

    s_pReset(this, 0, nullptr);
}

bool Actor::PlayIdle(TESIdleForm* apIdle) noexcept
{
    PAPYRUS_FUNCTION(bool, Actor, PlayIdle, TESIdleForm*);
    return s_pPlayIdle(this, apIdle);
}

void Actor::Respawn() noexcept
{
    Resurrect(false);
    Reset();
}

bool Actor::IsVampireLord() const noexcept
{
    return race && race->formID == 0x200283A;
}

extern thread_local bool g_forceAnimation;

void Actor::FixVampireLordModel() noexcept
{
    TESBoundObject* pLordArmor = Cast<TESBoundObject>(TESForm::GetById(0x2011a84));
    if (!pLordArmor)
        return;

    {
        ScopedInventoryOverride _;
        AddObjectToContainer(pLordArmor, nullptr, 1, nullptr);
    }

    EquipManager::Get()->Equip(this, pLordArmor, nullptr, 1, nullptr, false, true, false, false);

    g_forceAnimation = true;

    BSFixedString str("isLevitating");
    uint32_t isLevitating = GetAnimationVariableInt(&str);
    spdlog::critical("isLevitating {}", isLevitating);

    // By default, a loaded vampire lord is not levitating.
    if (isLevitating)
    {
        BSFixedString levitation("LevitationToggle");
        SendAnimationEvent(&levitation);
    }

    // TODO: weapon draw code does not seem to take care of this
    //BSFixedString weapEquip("WeapEquip");
    //SendAnimationEvent(&weapEquip);

    g_forceAnimation = false;
}

char TP_MAKE_THISCALL(HookSetPosition, Actor, NiPoint3& aPosition)
{
    const auto pExtension = apThis ? apThis->GetExtension() : nullptr;
    const bool bIsRemote = pExtension && pExtension->IsRemote();
    const bool scopedOverride = ScopedReferencesOverride::IsOverriden();
    if (apThis)
    {
        const auto caller = reinterpret_cast<uintptr_t>(_ReturnAddress());
        const auto base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
        ObjectService::RecordNativeSetPosition(apThis,
            caller >= base ? caller - base : 0, &aPosition,
            bIsRemote, scopedOverride);
    }

    if (bIsRemote && !scopedOverride)
        return 1;

    // Don't interfere with non actor references, or the player, or if we are calling our self
    if (apThis->formType != Actor::Type || apThis == PlayerCharacter::Get() || ScopedReferencesOverride::IsOverriden())
        return TiltedPhoques::ThisCall(RealSetPosition, apThis, aPosition);

    ScopedReferencesOverride recursionGuard;

    // It just works TM
    apThis->SetPosition(aPosition, false);

    return 1;
}

TP_THIS_FUNCTION(TForceState, void, Actor, const NiPoint3&, float, float, TESObjectCELL*, TESWorldSpace*, bool);
static TForceState* RealForceState = nullptr;

void TP_MAKE_THISCALL(HookForceState, Actor, const NiPoint3& acPosition, float aX, float aZ, TESObjectCELL* apCell, TESWorldSpace* apWorldSpace, bool aUnkBool)
{
    /*const auto pNpc = Cast<TESNPC>(apThis->baseForm);
    if (pNpc)
    {
        spdlog::info("For TESNPC: {}, spawn at {} {} {}", pNpc->fullName.value, apPosition->m_x, apPosition->m_y,
                     apPosition->m_z);
    }*/

    // if (apThis != PlayerCharacter::Get())
    //     return;

    return TiltedPhoques::ThisCall(RealForceState, apThis, acPosition, aX, aZ, apCell, apWorldSpace, aUnkBool);
}

TP_THIS_FUNCTION(TSpawnActorInWorld, bool, Actor);
static TSpawnActorInWorld* RealSpawnActorInWorld = nullptr;

// TODO: this isn't SpawnActorInWorld, this is TESObjectREFR::UpdateReference3D()
bool TP_MAKE_THISCALL(HookSpawnActorInWorld, Actor)
{
    const auto* pNpc = Cast<TESNPC>(apThis->baseForm);
    if (pNpc)
    {
        spdlog::info("Spawn Actor: {:X}, and NPC {}", apThis->formID, pNpc->fullName.value);
    }

    return TiltedPhoques::ThisCall(RealSpawnActorInWorld, apThis);
}

TP_THIS_FUNCTION(TDamageActor, bool, Actor, float aDamage, Actor* apHitter, bool aKillMove);
static TDamageActor* RealDamageActor = nullptr;

// Actor::KillImpl (vtable 0x10E, ID 37896). On a PC that does not own an NPC, a local kill (a
// scene or quest script, a local projectile) killed and ragdolled its copy on its own: the intro
// prisoner was 70 units from the owner's ragdoll when the owner's stream arrived and snapped
// across. The owner decides the death; this PC shows it through the death sync and the owner's
// ragdoll stream. Our own death sync (Actor::Kill) and a kill by this PC's player still go through.
TP_THIS_FUNCTION(TKillImpl, void, Actor, Actor* apAttacker, float aDamage, bool aSendEvent, bool aRagdollInstant);
static TKillImpl* RealKillImpl = nullptr;
std::atomic<uint64_t> s_blockedRemoteKills{0};

// 37639 (140697C50) queues dismemberment; 37640 (140697D10) creates the clone later.
// Authorize that queued completion by actor, since it need not run under the caller's TLS guard.
using TDecapitate = void(Actor*);
using TCreateHead = void(Actor*, bool);
TDecapitate* RealDecapitate{};
TCreateHead* RealCreateHead{};

void HookDecapitate(Actor* apActor)
{
    const auto* extension = apActor->GetExtension();
    if (World::Get().GetTransport().IsConnected() && extension && extension->IsRemote() && !t_syncDecapitate)
    {
        spdlog::info("Dismember {:X}: suppressed local Decapitate; waiting for owner", apActor->formID);
        return;
    }
    if (!extension || !extension->IsRemote())
        CorpseRagdollService::RecordDismember(apActor, false);
    RealDecapitate(apActor);
}

void HookCreateHead(Actor* apActor, bool aArg)
{
    const auto* extension = apActor->GetExtension();
    if (World::Get().GetTransport().IsConnected() && extension && extension->IsRemote() &&
        !CorpseRagdollService::IsDismemberAuthorized(apActor->formID))
        return;
    auto* before = apActor->GetDetachedLimbNode(1);
    RealCreateHead(apActor, aArg);
    if (apActor->GetDetachedLimbNode(1) != before)
        CorpseRagdollService::RecordDismember(apActor, true);
}

void TP_MAKE_THISCALL(HookKillImpl, Actor, Actor* apAttacker, float aDamage, bool aSendEvent, bool aRagdollInstant)
{
    const auto* pExtension = apThis ? apThis->GetExtension() : nullptr;
    const bool attackerIsLocalPlayer = apAttacker && apAttacker->GetExtension() && apAttacker->GetExtension()->IsLocalPlayer();
    if (!t_syncKill && pExtension && pExtension->IsRemote() && !pExtension->IsPlayer() && !attackerIsLocalPlayer)
    {
        if (s_blockedRemoteKills.fetch_add(1, std::memory_order_relaxed) < 32)
            spdlog::info("Kept {:X} alive here: its owner decides the death (attacker {:X})", apThis->formID,
                apAttacker ? apAttacker->formID : 0);
        return;
    }
    TiltedPhoques::ThisCall(RealKillImpl, apThis, apAttacker, aDamage, aSendEvent, aRagdollInstant);
}

// TODO: this is flawed, since it does not account for invulnerable actors
bool TP_MAKE_THISCALL(HookDamageActor, Actor, float aDamage, Actor* apHitter, bool aKillMove)
{
    if (apHitter)
        World::Get().GetRunner().Trigger(HitEvent(apHitter->formID, apThis->formID));

    float realDamage = GameplayFormulas::CalculateRealDamage(apThis, aDamage, aKillMove);

    float currentHealth = apThis->GetActorValue(ActorValueInfo::kHealth);
    bool wouldKill = (currentHealth - realDamage) <= 0.f;

    const auto* pExHittee = apThis->GetExtension();
    if (pExHittee->IsLocalPlayer())
    {
        if (!World::Get().GetServerSettings().PvpEnabled)
        {
            if (apHitter && apHitter->GetExtension()->IsRemotePlayer())
                return false;
        }

        World::Get().GetRunner().Trigger(HealthChangeEvent(apThis->formID, -realDamage));
        return TiltedPhoques::ThisCall(RealDamageActor, apThis, aDamage, apHitter, aKillMove);
    }
    else if (pExHittee->IsRemotePlayer())
    {
        return wouldKill;
    }

    if (apHitter)
    {
        const auto* pExHitter = apHitter->GetExtension();
        if (pExHitter->IsLocalPlayer())
        {
            World::Get().GetRunner().Trigger(HealthChangeEvent(apThis->formID, -realDamage));
            return TiltedPhoques::ThisCall(RealDamageActor, apThis, aDamage, apHitter, aKillMove);
        }
        if (pExHitter->IsRemotePlayer())
        {
            return wouldKill;
        }
    }

    if (pExHittee->IsLocal())
    {
        World::Get().GetRunner().Trigger(HealthChangeEvent(apThis->formID, -realDamage));
        return TiltedPhoques::ThisCall(RealDamageActor, apThis, aDamage, apHitter, aKillMove);
    }
    else
    {
        return wouldKill;
    }
}

TP_THIS_FUNCTION(TApplyActorEffect, void, ActiveEffect, Actor* apTarget, float aEffectValue, unsigned int unk1);
static TApplyActorEffect* RealApplyActorEffect = nullptr;

void TP_MAKE_THISCALL(HookApplyActorEffect, ActiveEffect, Actor* apTarget, float aEffectValue, unsigned int unk1)
{
    const auto* pValueModEffect = Cast<ValueModifierEffect>(apThis);

    if (pValueModEffect)
    {
        if (pValueModEffect->actorValueIndex == ActorValueInfo::kHealth && aEffectValue > 0.0f)
        {
            if (apTarget && apTarget->GetExtension())
            {
                const auto pExTarget = apTarget->GetExtension();
                if (pExTarget->IsLocal())
                {
                    World::Get().GetRunner().Trigger(HealthChangeEvent(apTarget->formID, aEffectValue));
                    return TiltedPhoques::ThisCall(RealApplyActorEffect, apThis, apTarget, aEffectValue, unk1);
                }
                return;
            }
        }
    }

    return TiltedPhoques::ThisCall(RealApplyActorEffect, apThis, apTarget, aEffectValue, unk1);
}

TP_THIS_FUNCTION(TRegenAttributes, void*, Actor, int aId, float regenValue);
static TRegenAttributes* RealRegenAttributes = nullptr;

void* TP_MAKE_THISCALL(HookRegenAttributes, Actor, int aId, float aRegenValue)
{
    if (aId != ActorValueInfo::kHealth)
    {
        return TiltedPhoques::ThisCall(RealRegenAttributes, apThis, aId, aRegenValue);
    }

    const auto* pExTarget = apThis->GetExtension();
    if (pExTarget->IsRemote())
    {
        return 0;
    }

    World::Get().GetRunner().Trigger(HealthChangeEvent(apThis->formID, aRegenValue));
    return TiltedPhoques::ThisCall(RealRegenAttributes, apThis, aId, aRegenValue);
}

void TP_MAKE_THISCALL(HookAddInventoryItem, Actor, TESBoundObject* apItem, ExtraDataList* apExtraData, int32_t aCount, TESObjectREFR* apOldOwner)
{
    if (!ScopedInventoryOverride::IsOverriden())
    {
        auto& modSystem = World::Get().GetModSystem();

        Inventory::Entry item{};
        modSystem.GetServerModId(apItem->formID, item.BaseId);
        item.Count = aCount;

        if (apExtraData)
            apThis->GetItemFromExtraData(item, apExtraData);

        QueueActorInventoryChange(apThis, InventoryChangeEvent(apThis->formID, std::move(item)), apOldOwner);
    }

    TiltedPhoques::ThisCall(RealAddInventoryItem, apThis, apItem, apExtraData, aCount, apOldOwner);
}

void* TP_MAKE_THISCALL(HookPickUpObject, Actor, TESObjectREFR* apObject, int32_t aCount, bool aUnk1, float aUnk2)
{
    if (!ScopedInventoryOverride::IsOverriden())
    {
        auto& modSystem = World::Get().GetModSystem();

        Inventory::Entry item{};
        modSystem.GetServerModId(apObject->baseForm->formID, item.BaseId);
        item.Count = aCount;

        if (apObject->GetExtraDataList())
            apThis->GetItemFromExtraData(item, apObject->GetExtraDataList());

        // This is here so that objects that are picked up on both clients, aka non temps, are synced through activation sync.
        // The inventory change event should always be sent to the server, otherwise the server inventory won't be updated.
        bool shouldUpdateClients = apObject->IsTemporary() && !ScopedActivateOverride::IsOverriden();

        QueueActorInventoryChange(apThis, InventoryChangeEvent(apThis->formID, std::move(item), false, shouldUpdateClients));
    }

    return TiltedPhoques::ThisCall(RealPickUpObject, apThis, apObject, aCount, aUnk1, aUnk2);
}

void Actor::PickUpObject(TESObjectREFR* apObject, int32_t aCount, bool aUnk1, float aUnk2) noexcept
{
    TiltedPhoques::ThisCall(RealPickUpObject, this, apObject, aCount, aUnk1, aUnk2);
}

void* TP_MAKE_THISCALL(HookDropObject, Actor, void* apResult, TESBoundObject* apObject, ExtraDataList* apExtraData, int32_t aCount, NiPoint3* apLocation, NiPoint3* apRotation)
{
    auto& modSystem = World::Get().GetModSystem();

    Inventory::Entry item{};
    modSystem.GetServerModId(apObject->formID, item.BaseId);
    item.Count = -aCount;

    if (apExtraData)
        apThis->GetItemFromExtraData(item, apExtraData);

    QueueActorInventoryChange(apThis, InventoryChangeEvent(apThis->formID, std::move(item), true));

    ScopedInventoryOverride _;

    return TiltedPhoques::ThisCall(RealDropObject, apThis, apResult, apObject, apExtraData, aCount, apLocation, apRotation);
}

void Actor::DropOrPickUpObject(const Inventory::Entry& arEntry, NiPoint3* apLocation, NiPoint3* apRotation) noexcept
{
    ExtraDataList* pExtraData = GetExtraDataFromItem(arEntry);

    ModSystem& modSystem = World::Get().GetModSystem();

    uint32_t objectId = modSystem.GetGameId(arEntry.BaseId);
    TESBoundObject* pObject = Cast<TESBoundObject>(TESForm::GetById(objectId));
    if (!pObject)
    {
        spdlog::warn("Object to drop not found, {:X}:{:X}.", arEntry.BaseId.ModId, arEntry.BaseId.BaseId);
        return;
    }

    if (arEntry.Count < 0)
        DropObject(pObject, pExtraData, -arEntry.Count, apLocation, apRotation);
    // TODO: pick up
}

void Actor::DropObject(TESBoundObject* apObject, ExtraDataList* apExtraData, int32_t aCount, NiPoint3* apLocation, NiPoint3* apRotation) noexcept
{
    spdlog::debug("Dropping object, form id: {:X}, count: {}, actor: {:X}", apObject->formID, aCount, formID);
    BSPointerHandle<TESObjectREFR> result{};
    TiltedPhoques::ThisCall(RealDropObject, this, &result, apObject, apExtraData, aCount, apLocation, apRotation);
}

TP_THIS_FUNCTION(TUpdateDetectionState, void, ActorKnowledge, void*);
static TUpdateDetectionState* RealUpdateDetectionState = nullptr;

void TP_MAKE_THISCALL(HookUpdateDetectionState, ActorKnowledge, void* apState)
{
    auto pOwner = TESObjectREFR::GetByHandle(apThis->hOwner);
    auto pTarget = TESObjectREFR::GetByHandle(apThis->hTarget);

    if (pOwner && pTarget)
    {
        auto pOwnerActor = Cast<Actor>(pOwner);
        auto pTargetActor = Cast<Actor>(pTarget);
        if (pOwnerActor && pTargetActor)
        {
            if (pOwnerActor->GetExtension()->IsRemotePlayer() && pTargetActor->GetExtension()->IsLocalPlayer())
            {
                spdlog::debug("Cancelling detection from remote player to local player, owner: {:X}, target: {:X}", pOwner->formID, pTarget->formID);
                return;
            }
        }
    }

    return TiltedPhoques::ThisCall(RealUpdateDetectionState, apThis, apState);
}

struct DialogueItem;

// TODO: This is an AIProcess function
TP_THIS_FUNCTION(TProcessResponse, uint64_t, void, DialogueItem* apVoice, Actor* apTalkingActor, Actor* apTalkedToActor);
static TProcessResponse* RealProcessResponse = nullptr;

uint64_t TP_MAKE_THISCALL(HookProcessResponse, void, DialogueItem* apVoice, Actor* apTalkingActor, Actor* apTalkedToActor)
{
    if (apTalkingActor)
    {
        if (apTalkingActor->GetExtension()->IsRemotePlayer())
            return 0;
    }
    return TiltedPhoques::ThisCall(RealProcessResponse, apThis, apVoice, apTalkingActor, apTalkedToActor);
}

bool TP_MAKE_THISCALL(HookInitiateMountPackage, Actor, Actor* apMount)
{
    const bool connected = World::Get().GetTransport().IsConnected();
    const bool trace = connected &&
        s_mountPackageTraceCount.fetch_add(1, std::memory_order_relaxed) < 256;
    const auto caller = trace ? reinterpret_cast<uintptr_t>(_ReturnAddress()) : 0;
    const bool started = TiltedPhoques::ThisCall(RealInitiateMountPackage,
        apThis, apMount);
    if (trace)
    {
        const auto base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
        spdlog::info("Native mount package rider={:X} mount={:X} remote={} riderProcess={} mountProcess={} started={} callerRva={:X}",
            apThis->formID, apMount ? apMount->formID : 0,
            apThis->GetExtension()->IsRemote(), apThis->currentProcess != nullptr,
            apMount && apMount->currentProcess != nullptr, started,
            caller >= base ? caller - base : 0);
    }
    if (started && apMount && apThis->GetExtension()->IsLocal())
        World::Get().GetRunner().Trigger(MountEvent(apThis->formID, apMount->formID));

    return started;
}

TP_THIS_FUNCTION(TUnequipObject, void, Actor, void* apUnk1, TESBoundObject* apObject, int32_t aUnk2, void* apUnk3);
static TUnequipObject* RealUnequipObject = nullptr;

void TP_MAKE_THISCALL(HookUnequipObject, Actor, void* apUnk1, TESBoundObject* apObject, int32_t aUnk2, void* apUnk3)
{
    TiltedPhoques::ThisCall(RealUnequipObject, apThis, apUnk1, apObject, aUnk2, apUnk3);
}

TP_THIS_FUNCTION(TSpeakSoundFunction, bool, Actor, const char* apName, uint32_t* a3, uint32_t a4, uint32_t a5, uint32_t a6, uint64_t a7, uint64_t a8, uint64_t a9, bool a10, uint64_t a11, bool a12, bool a13, bool a14);
TP_THIS_FUNCTION(TSetSoundVolume, bool, void, float);
static TSpeakSoundFunction* RealSpeakSoundFunction = nullptr;
static std::atomic<uint32_t> s_nativeVoiceProbeCount{0};
static std::atomic<uint32_t> s_mutedRemoteVoiceProbeCount{0};

bool TP_MAKE_THISCALL(HookSpeakSoundFunction, Actor, const char* apName, uint32_t* a3, uint32_t a4, uint32_t a5, uint32_t a6, uint64_t a7, uint64_t a8, uint64_t a9, bool a10, uint64_t a11, bool a12, bool a13, bool a14)
{
    spdlog::debug("a3: {:X}, a4: {}, a5: {}, a6: {}, a7: {}, a8: {:X}, a9: {:X}, a10: {}, a11: {:X}, a12: {}, a13: {}, a14: {}", (uint64_t)a3, a4, a5, a6, a7, a8, a9, a10, a11, a12, a13, a14);

    if (World::Get().GetTransport().IsConnected() && s_nativeVoiceProbeCount.fetch_add(1, std::memory_order_relaxed) < 96)
        spdlog::info("Native voice start actor {:X} authority={} playerDialogue={} file={}",
            apThis->formID, apThis->GetExtension()->IsLocal() ? "local" : "remote",
            MenuTopicManager::IsPlayerDialogueSpeaker(apThis), apName ? apName : "");

    const bool result = TiltedPhoques::ThisCall(RealSpeakSoundFunction, apThis, apName, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12, a13, a14);

    // Skyrim can invoke this function twice for one line: the first call
    // reports success but leaves the handle invalid. Publish only the call
    // that actually acquired a playable sound, or followers replay it twice.
    const bool soundStarted = result && a3 && a3[0] != 0xFFFFFFFFu;
    if (soundStarted && (apThis->GetExtension()->IsLocal() ||
        MenuTopicManager::IsPlayerDialogueSpeaker(apThis)))
        World::Get().GetRunner().Trigger(DialogueEvent(apThis->formID, apName));

    // Preserve Skyrim's native voice handle and duration for scene waits, but
    // never audibly present a remote NPC's independently timed local line.
    // The owner-originated replay calls RealSpeakSoundFunction directly, so
    // it is not muted by this hook.
    auto& world = World::Get();
    const bool remotePartyNpc = world.GetTransport().IsConnected() &&
        world.GetPartyService().IsInParty() && apThis->GetExtension()->IsRemote() &&
        !apThis->GetExtension()->IsPlayer() && !MenuTopicManager::IsPlayerDialogueSpeaker(apThis);
    if (remotePartyNpc && s_mutedRemoteVoiceProbeCount.load(std::memory_order_relaxed) < 96)
        spdlog::info("Native remote voice handle actor {:X} result={} soundId={} file={}",
            apThis->formID, result, a3 ? a3[0] : 0xFFFFFFFFu, apName ? apName : "");
    if (remotePartyNpc && soundStarted)
    {
        POINTER_SKYRIMSE(TSetSoundVolume, s_setSoundVolume, 67626);
        const bool muted = TiltedPhoques::ThisCall(s_setSoundVolume, a3, 0.f);
        if (s_mutedRemoteVoiceProbeCount.fetch_add(1, std::memory_order_relaxed) < 96)
            spdlog::info("Native remote voice muted actor {:X} soundId={} success={} file={}",
                apThis->formID, a3[0], muted, apName ? apName : "");
    }
    return result;
}

void TP_MAKE_THISCALL(HookKnockExplosion, AIProcess, Actor* apActor,
    const NiPoint3& acLocation, float aMagnitude)
{
    // A queued explosion can outlive an actor's AI process during a scene or
    // load transition. Native KnockExplosion unconditionally reads [this+8].
    if (!apThis)
    {
        s_nullKnockExplosionSkips.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    TiltedPhoques::ThisCall(RealKnockExplosion, apThis, apActor,
        acLocation, aMagnitude);
}

void Actor::SpeakSound(const char* pFile)
{
    uint32_t handle[3]{};
    handle[0] = -1;
    TiltedPhoques::ThisCall(RealSpeakSoundFunction, this, pFile, handle, 0, 0x32, 0, 0, 0, 0, 0, 0, 0, 1, 1);
}

char TP_MAKE_THISCALL(HookActorProcess, Actor, float a2)
{
    // Remote AI cannot be allowed to execute independently of the owner.
    // This opt-in two-actor trial tests whether continued native processing
    // is required for a mount interaction to remain alive. It is not a
    // production authority policy and never applies while disconnected.
    if (apThis->GetExtension()->IsRemote())
    {
        const auto formId = apThis->formID;
        if (!World::Get().GetTransport().IsConnected() ||
            (formId != s_remoteProcessTrialRiderFormId.load(std::memory_order_acquire) &&
             formId != s_remoteProcessTrialMountFormId.load(std::memory_order_acquire)))
            return 0;
        s_remoteProcessTrialTicks.fetch_add(1, std::memory_order_relaxed);
    }

    return TiltedPhoques::ThisCall(RealActorProcess, apThis, a2);
}

BSExtraData* TP_MAKE_THISCALL(HookNativeExtraDataAdd, ExtraDataList,
    BSExtraData* apNewData)
{
    const auto caller = reinterpret_cast<uintptr_t>(_ReturnAddress());
    const bool connected = World::Get().GetTransport().IsConnected();
    if (connected)
        s_interactionTraceArmed.store(true, std::memory_order_relaxed);
    const bool trace = apNewData && connected &&
        apNewData->GetType() == ExtraDataType::Interaction &&
        s_interactionAddTraceCount.fetch_add(1, std::memory_order_relaxed) < 96;
    uint32_t actorHandle{};
    uint32_t targetHandle{};
    if (trace)
    {
        void* interaction{};
        SIZE_T bytes{};
        if (ReadProcessMemory(GetCurrentProcess(),
                reinterpret_cast<const uint8_t*>(apNewData) + 0x10,
                &interaction, sizeof(interaction), &bytes) &&
            bytes == sizeof(interaction) && interaction)
        {
            ReadProcessMemory(GetCurrentProcess(),
                reinterpret_cast<const uint8_t*>(interaction) + 0x10,
                &actorHandle, sizeof(actorHandle), &bytes);
            ReadProcessMemory(GetCurrentProcess(),
                reinterpret_cast<const uint8_t*>(interaction) + 0x14,
                &targetHandle, sizeof(targetHandle), &bytes);
        }
    }
    auto* result = TiltedPhoques::ThisCall(RealNativeExtraDataAdd, apThis,
        apNewData);
    if (trace)
    {
        const auto base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
        spdlog::info("Native interaction add list={} actorHandle={} targetHandle={} callerRva={:X} result={}",
            fmt::ptr(apThis), actorHandle, targetHandle,
            caller >= base ? caller - base : 0, fmt::ptr(result));
    }
    return result;
}

void TP_MAKE_THISCALL(HookNativeSetInteraction, ExtraDataList,
    void* apInteractionPointer)
{
    const auto caller = reinterpret_cast<uintptr_t>(_ReturnAddress());
    const bool connected = World::Get().GetTransport().IsConnected();
    if (connected)
        s_interactionTraceArmed.store(true, std::memory_order_relaxed);
    const bool trace = (connected || s_interactionTraceArmed.load(std::memory_order_relaxed)) &&
        s_interactionSetTraceCount.fetch_add(1, std::memory_order_relaxed) < 256;
    const bool before = trace && apThis->Contains(ExtraDataType::Interaction);
    void* interaction{};
    uint32_t actorHandle{};
    uint32_t targetHandle{};
    if (trace && apInteractionPointer)
    {
        SIZE_T bytes{};
        if (ReadProcessMemory(GetCurrentProcess(), apInteractionPointer,
                &interaction, sizeof(interaction), &bytes) &&
            bytes == sizeof(interaction) && interaction)
        {
            ReadProcessMemory(GetCurrentProcess(),
                reinterpret_cast<const uint8_t*>(interaction) + 0x10,
                &actorHandle, sizeof(actorHandle), &bytes);
            ReadProcessMemory(GetCurrentProcess(),
                reinterpret_cast<const uint8_t*>(interaction) + 0x14,
                &targetHandle, sizeof(targetHandle), &bytes);
        }
    }
    TiltedPhoques::ThisCall(RealNativeSetInteraction, apThis,
        apInteractionPointer);
    if (trace)
    {
        const auto base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
        spdlog::info("Native interaction set list={} hasInput={} actorHandle={} targetHandle={} before={} after={} callerRva={:X}",
            fmt::ptr(apThis), interaction != nullptr, actorHandle,
            targetHandle, before, apThis->Contains(ExtraDataType::Interaction),
            caller >= base ? caller - base : 0);
    }
}

TP_THIS_FUNCTION(TAddDeathItems, void, Actor);
static TAddDeathItems* RealAddDeathItems = nullptr;

void TP_MAKE_THISCALL(HookAddDeathItems, Actor)
{
    if (apThis->GetExtension()->IsRemote())
        return;

    TiltedPhoques::ThisCall(RealAddDeathItems, apThis);
}

TP_THIS_FUNCTION(TIsFleeing, bool, Actor);
static TIsFleeing* RealIsFleeing = nullptr;

bool TP_MAKE_THISCALL(HookIsFleeing, Actor)
{
    // TODO: Player or RemotePlayer? Can players be in fleeing mode in skyrim?
    // TODO: investigate why the flee flag is set at all on remote players sometimes.
    if (apThis->GetExtension()->IsPlayer())
        return false;
    
    return TiltedPhoques::ThisCall(RealIsFleeing, apThis);
}

static TiltedPhoques::Initializer s_actorHooks(
    []()
    {
        POINTER_SKYRIMSE(TActorProcess, s_actorProcess, 37356);
        POINTER_SKYRIMSE(TSetPosition, s_setPosition, 19790);
        POINTER_SKYRIMSE(TKnockExplosion, s_knockExplosion, 39895);
        POINTER_SKYRIMSE(TRemoveSpell, s_removeSpell, 38717);
        POINTER_SKYRIMSE(TCharacterConstructor, s_characterCtor, 40245);
        POINTER_SKYRIMSE(TCharacterConstructor2, s_characterCtor2, 40246);
        POINTER_SKYRIMSE(TCharacterDestructor, s_characterDtor, 37175);
        POINTER_SKYRIMSE(TGetLocation, s_GetActorLocation, 19812);
        POINTER_SKYRIMSE(TForceState, s_ForceState, 37313);
        POINTER_SKYRIMSE(TSpawnActorInWorld, s_SpawnActorInWorld, 19742);
        POINTER_SKYRIMSE(TDamageActor, s_damageActor, 37335);
        POINTER_SKYRIMSE(TKillImpl, s_killImpl, 37896);
        RealKillImpl = s_killImpl.Get();
        POINTER_SKYRIMSE(TApplyActorEffect, s_applyActorEffect, 35086);
        POINTER_SKYRIMSE(TRegenAttributes, s_regenAttributes, 37448);
        POINTER_SKYRIMSE(TAddInventoryItem, s_addInventoryItem, 37525);
        POINTER_SKYRIMSE(TPickUpObject, s_pickUpObject, 37521);
        POINTER_SKYRIMSE(TDropObject, s_dropObject, 40454);
        POINTER_SKYRIMSE(TUpdateDetectionState, s_updateDetectionState, 42704);
        POINTER_SKYRIMSE(TProcessResponse, s_processResponse, 39643);
        POINTER_SKYRIMSE(TInitiateMountPackage, s_initiateMountPackage, 37905);
        POINTER_SKYRIMSE(TUnequipObject, s_unequipObject, 37975);
        POINTER_SKYRIMSE(TSpeakSoundFunction, s_speakSoundFunction, 37542);
        POINTER_SKYRIMSE(TAddDeathItems, addDeathItems, 37198);
        POINTER_SKYRIMSE(TDecapitate, decapitate, 37639);
        POINTER_SKYRIMSE(TCreateHead, createHead, 37640);
        POINTER_SKYRIMSE(TIsFleeing, isFleeing, 37577);
        POINTER_SKYRIMSE(TNativeExtraDataAdd, s_nativeExtraDataAdd, 12315);

        RealActorProcess = s_actorProcess.Get();
        RealNativeExtraDataAdd = s_nativeExtraDataAdd.Get();
        // The installed 1.7.104 executable's ExtraInteraction setter. Its
        // prologue guard avoids hooking an unrelated address on another build.
        const auto gameBase = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
        constexpr uint8_t setInteractionPrologue[]{0x40, 0x53, 0x55, 0x56, 0x57, 0x41, 0x56};
        const auto* candidate = reinterpret_cast<const uint8_t*>(gameBase + 0x1739A0);
        if (memcmp(candidate, setInteractionPrologue,
                sizeof(setInteractionPrologue)) == 0)
            RealNativeSetInteraction = reinterpret_cast<TNativeSetInteraction*>(
                const_cast<uint8_t*>(candidate));
        RealSetPosition = s_setPosition.Get();
        RealKnockExplosion = s_knockExplosion.Get();
        RealRemoveSpell = s_removeSpell.Get();
        FUNC_GetActorLocation = s_GetActorLocation.Get();
        RealCharacterConstructor = s_characterCtor.Get();
        RealCharacterConstructor2 = s_characterCtor2.Get();
        RealForceState = s_ForceState.Get();
        RealSpawnActorInWorld = s_SpawnActorInWorld.Get();
        RealDamageActor = s_damageActor.Get();
        RealApplyActorEffect = s_applyActorEffect.Get();
        RealRegenAttributes = s_regenAttributes.Get();
        RealAddInventoryItem = s_addInventoryItem.Get();
        RealPickUpObject = s_pickUpObject.Get();
        RealDropObject = s_dropObject.Get();
        RealUpdateDetectionState = s_updateDetectionState.Get();
        RealProcessResponse = s_processResponse.Get();
        RealInitiateMountPackage = s_initiateMountPackage.Get();
        RealUnequipObject = s_unequipObject.Get();
        RealSpeakSoundFunction = s_speakSoundFunction.Get();
        RealAddDeathItems = addDeathItems.Get();
        RealDecapitate = decapitate.Get();
        RealCreateHead = createHead.Get();
        RealIsFleeing = isFleeing.Get();

        TP_HOOK(&RealActorProcess, HookActorProcess);
        TP_HOOK(&RealNativeExtraDataAdd, HookNativeExtraDataAdd);
        if (RealNativeSetInteraction)
            TP_HOOK(&RealNativeSetInteraction, HookNativeSetInteraction);
        TP_HOOK(&RealSetPosition, HookSetPosition);
        TP_HOOK(&RealKnockExplosion, HookKnockExplosion);
        TP_HOOK(&RealKillImpl, HookKillImpl);
        TP_HOOK(&RealRemoveSpell, HookRemoveSpell);
        TP_HOOK(&RealCharacterConstructor, HookCharacterConstructor);
        TP_HOOK(&RealCharacterConstructor2, HookCharacterConstructor2);
        TP_HOOK(&RealForceState, HookForceState);
        TP_HOOK(&RealSpawnActorInWorld, HookSpawnActorInWorld);
        TP_HOOK(&RealDamageActor, HookDamageActor);
        TP_HOOK(&RealApplyActorEffect, HookApplyActorEffect);
        TP_HOOK(&RealRegenAttributes, HookRegenAttributes);
        TP_HOOK(&RealAddInventoryItem, HookAddInventoryItem);
        TP_HOOK(&RealPickUpObject, HookPickUpObject);
        TP_HOOK(&RealDropObject, HookDropObject);
        TP_HOOK(&RealUpdateDetectionState, HookUpdateDetectionState);
        TP_HOOK(&RealProcessResponse, HookProcessResponse);
        TP_HOOK(&RealInitiateMountPackage, HookInitiateMountPackage);
        TP_HOOK(&RealUnequipObject, HookUnequipObject);
        TP_HOOK(&RealSpeakSoundFunction, HookSpeakSoundFunction);
        TP_HOOK(&RealAddDeathItems, HookAddDeathItems);
        TP_HOOK(&RealDecapitate, HookDecapitate);
        TP_HOOK(&RealCreateHead, HookCreateHead);
        TP_HOOK(&RealIsFleeing, HookIsFleeing);
    });
