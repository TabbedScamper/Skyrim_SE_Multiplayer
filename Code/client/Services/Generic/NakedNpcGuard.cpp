#include <TiltedOnlinePCH.h>
#include <Services/Generic/NakedNpcGuard.h>

#include <World.h>
#include <Components.h>
#include <Actor.h>
#include <EquipManager.h>
#include <Forms/TESObjectARMO.h>
#include <Games/ActorExtension.h>
#include <Games/Overrides.h>
#include <ExtraData/ExtraContainerChanges.h>
#include <Events/DisconnectedEvent.h>
#include <Events/EquipmentChangeEvent.h>
#include <Events/InventoryChangeEvent.h>
#include <Messages/AssignCharacterResponse.h>
#include <Messages/CharacterSpawnRequest.h>
#include <Messages/NotifyOwnershipTransfer.h>
#include <Messages/NotifyEquipmentChanges.h>
#include <Messages/NotifyInventoryChanges.h>

namespace
{
// Research/credit (handoff for docs/REFERENCE_RESEARCH.md):
// arifkulpu/Invisible-and-naked-NPC-fixer, src/VisibilityFixer.cpp:298-332:
// adopt missing-slot detection and existing-inventory equip; reject its reset fallback.
// MrMartexX/TiltedEvolution PR #10 removes RunNakedNPCBugChecks/ResetInventory(false):
// an outfit is NOT evidence that an item still exists (looting must remain permanent).
// rfortier/TiltedEvolution-rwf, InventoryService.cpp:296-367, Actor.cpp:532-572,
// and tiltedphoques/TiltedEvolution PR #860: adopt delayed, non-destructive equip and
// assignment fencing. Extend it to remote copies and corpses using owner worn intent.
// Naked Dead NPC Fix (Nexus SE 99024, ThirdEyeSqueegee/wSkeever): original
// ThirdEyeSqueegee/SkeeverModTwo is now unavailable; public fork clayne/SkeeverModTwo,
// src/Hooks.cpp:11-72, hooks Character::Load3D and silently equips existing body,
// hands, feet and head armor. Adopt the existing-item/corpse policy, but check each
// missing slot independently and require owner intent instead of arbitrary loot.
// Native 1.7.104: 38894/0x1406DC310 -> 38929/0x1406DE920 -> 38001/0x1406B2570
// checks inventory count; 38004/0x1406B3650 removes conflicting biped slots. Therefore
// require positive stock AND entirely free slot masks before calling silent equip.
// Slot 32 (body) is bit 2 of TESObjectARMO::slotType; the same policy covers all armor.
constexpr uint64_t kCheckIntervalMs = 1000;
constexpr uint64_t kSettleMs = 1750; // Longer than InventoryService's 1500 ms death hold.

TESObjectARMO* Armor(World& aWorld, GameId aId) noexcept
{
    auto* pForm = TESForm::GetById(aWorld.GetModSystem().GetGameId(aId));
    return pForm && pForm->formType == FormType::Armor ? Cast<TESObjectARMO>(pForm) : nullptr;
}

bool CanReadArmor(Actor* apActor) noexcept
{
    const auto* pChanges = apActor->GetContainerChanges();
    if (!pChanges || !pChanges->entries)
        return false;
    // GetArmor's serializer assumes non-null forms and extra-list containers.
    for (const auto* pEntry : *pChanges->entries)
        if (pEntry && (!pEntry->form || !pEntry->dataList))
            return false;
    return true;
}

bool SameInstance(Inventory::Entry aLeft, Inventory::Entry aRight) noexcept
{
    aLeft.Count = aRight.Count = 1;
    aLeft.ExtraWorn = aRight.ExtraWorn = false;
    aLeft.ExtraWornLeft = aRight.ExtraWornLeft = false;
    if (aLeft != aRight || aLeft.EnchantData.IsWeapon != aRight.EnchantData.IsWeapon ||
        aLeft.EnchantData.Effects.size() != aRight.EnchantData.Effects.size())
        return false;
    for (size_t i = 0; i < aLeft.EnchantData.Effects.size(); ++i)
    {
        const auto& left = aLeft.EnchantData.Effects[i];
        const auto& right = aRight.EnchantData.Effects[i];
        if (left.Magnitude != right.Magnitude || left.Area != right.Area || left.Duration != right.Duration ||
            left.RawCost != right.RawCost || left.EffectId != right.EffectId)
            return false;
    }
    return true;
}

// Reuse the real extra list; never synthesize an enchanted/tempered/quest item.
bool FindExtraList(Actor* apActor, TESObjectARMO* apArmor, const Inventory::Entry& acItem,
    ExtraDataList*& apResult) noexcept
{
    apResult = nullptr;
    for (auto* pEntry : *apActor->GetContainerChanges()->entries)
    {
        if (!pEntry || pEntry->form != apArmor)
            continue;
        for (auto* pExtra : *pEntry->dataList)
        {
            if (!pExtra)
                continue;
            Inventory::Entry item;
            item.BaseId = acItem.BaseId;
            TESObjectREFR::GetItemFromExtraData(item, pExtra);
            if (SameInstance(item, acItem))
            {
                apResult = pExtra;
                return true;
            }
        }
    }
    return !acItem.ContainsExtraData();
}

bool LocalTokenMatches(World& aWorld, uint32_t aFormId, uint32_t aServerId, uint32_t aEpoch) noexcept
{
    if (!aEpoch)
        return false;
    auto view = aWorld.view<FormIdComponent, LocalComponent>(entt::exclude<WaitingForAssignmentComponent, WaitingFor3D, PlayerComponent>);
    for (auto entity : view)
    {
        const auto& local = view.get<LocalComponent>(entity);
        if (view.get<FormIdComponent>(entity).Id == aFormId && local.Id == aServerId && local.OwnershipEpoch == aEpoch)
            return true;
    }
    return false;
}
}

NakedNpcGuard::NakedNpcGuard(World& aWorld, entt::dispatcher& aDispatcher) noexcept
    : m_world(aWorld)
{
    m_assignConnection = aDispatcher.sink<AssignCharacterResponse>().connect<&NakedNpcGuard::OnAssign>(this);
    m_spawnConnection = aDispatcher.sink<CharacterSpawnRequest>().connect<&NakedNpcGuard::OnSpawn>(this);
    m_transferConnection = aDispatcher.sink<NotifyOwnershipTransfer>().connect<&NakedNpcGuard::OnTransfer>(this);
    m_equipmentConnection = aDispatcher.sink<NotifyEquipmentChanges>().connect<&NakedNpcGuard::OnEquipment>(this);
    m_inventoryConnection = aDispatcher.sink<NotifyInventoryChanges>().connect<&NakedNpcGuard::OnInventory>(this);
    m_localEquipmentConnection = aDispatcher.sink<EquipmentChangeEvent>().connect<&NakedNpcGuard::OnLocalEquipment>(this);
    m_localInventoryConnection = aDispatcher.sink<InventoryChangeEvent>().connect<&NakedNpcGuard::OnLocalInventory>(this);
    m_disconnectConnection = aDispatcher.sink<DisconnectedEvent>().connect<&NakedNpcGuard::OnDisconnected>(this);
}

void NakedNpcGuard::Remember(uint32_t aServerId, uint32_t aEpoch, const Inventory& acInventory) noexcept
{
    if (!aEpoch)
        return;
    auto& set = m_wornSets[aServerId];
    if (set.Epoch > aEpoch)
        return;
    set = {};
    set.Epoch = aEpoch;
    set.Known = true;
    set.Worn = acInventory;
    set.Worn.RemoveByFilter([](const auto& entry) { return entry.Count <= 0 || !entry.IsWorn(); });
    set.LastSeen = GetTickCount64();
    set.SettleUntil = set.LastSeen + kSettleMs;
}

void NakedNpcGuard::Equipment(uint32_t aServerId, uint32_t aEpoch, GameId aItem, bool aUnequip) noexcept
{
    auto it = m_wornSets.find(aServerId);
    if (it == m_wornSets.end() || it->second.Epoch != aEpoch || !it->second.Known)
        return;
    auto* pArmor = Armor(m_world, aItem);
    if (!pArmor)
        return;
    auto& set = it->second;
    set.Worn.RemoveByFilter([&](const auto& entry)
    {
        auto* pOther = Armor(m_world, entry.BaseId);
        return entry.BaseId == aItem || (!aUnequip && pOther && (pOther->slotType & pArmor->slotType));
    });
    std::erase_if(set.FormOnly, [&](const auto& id)
    {
        return std::none_of(set.Worn.Entries.begin(), set.Worn.Entries.end(),
            [&](const auto& entry) { return entry.BaseId == id; });
    });
    if (!aUnequip)
    {
        // Equipment deltas contain a form, not instance metadata. Repair only an
        // unambiguous existing instance below; never pick between different variants.
        Inventory::Entry entry;
        entry.BaseId = aItem;
        entry.Count = 1;
        entry.ExtraWorn = true;
        set.Worn.Entries.push_back(entry);
        set.FormOnly.push_back(aItem);
    }
    set.SettleUntil = GetTickCount64() + kSettleMs;
}

void NakedNpcGuard::Removed(uint32_t aServerId, uint32_t aEpoch, const Inventory::Entry& acItem) noexcept
{
    auto it = m_wornSets.find(aServerId);
    if (it == m_wornSets.end() || it->second.Epoch != aEpoch)
        return;
    // A removal also revokes permission to dress a duplicate of the same form.
    // A subsequent owner equip can explicitly restore that permission.
    if (acItem.Count < 0)
        it->second.Worn.RemoveByFilter([&](const auto& entry) { return entry.BaseId == acItem.BaseId; });
    it->second.SettleUntil = GetTickCount64() + kSettleMs;
}

void NakedNpcGuard::OnAssign(const AssignCharacterResponse& acMessage) noexcept
{
    // An initial non-authoritative empty assignment is not an instruction to strip
    // the native actor. Only a local owner may seed from its actual worn inventory.
    if (!acMessage.PlayerId && (acMessage.InventoryAuthoritative || !acMessage.CurrentInventory.Entries.empty()))
        Remember(acMessage.ServerId, acMessage.OwnershipEpoch, acMessage.CurrentInventory);
}

void NakedNpcGuard::OnSpawn(const CharacterSpawnRequest& acMessage) noexcept
{
    if (!acMessage.IsPlayer)
        Remember(acMessage.ServerId, acMessage.OwnershipEpoch, acMessage.InventoryContent);
}

void NakedNpcGuard::OnTransfer(const NotifyOwnershipTransfer& acMessage) noexcept
{
    Remember(acMessage.ServerId, acMessage.OwnershipEpoch, acMessage.CurrentActorData.InitialInventory);
}

void NakedNpcGuard::OnEquipment(const NotifyEquipmentChanges& acMessage) noexcept
{
    if (!acMessage.IsSpell && !acMessage.IsShout)
        Equipment(acMessage.ServerId, acMessage.OwnershipEpoch, acMessage.ItemId, acMessage.Unequip);
}

void NakedNpcGuard::OnInventory(const NotifyInventoryChanges& acMessage) noexcept
{
    Removed(acMessage.ServerId, acMessage.OwnershipEpoch, acMessage.Item);
}

void NakedNpcGuard::OnLocalEquipment(const EquipmentChangeEvent& acEvent) noexcept
{
    if (acEvent.IsSpell || acEvent.IsShout || acEvent.IsAmmo ||
        !LocalTokenMatches(m_world, acEvent.ActorId, acEvent.ServerId, acEvent.OwnershipEpoch))
        return;
    GameId item;
    if (!m_world.GetModSystem().GetServerModId(acEvent.ItemId, item))
        return;
    auto* pActor = Cast<Actor>(TESForm::GetById(acEvent.ActorId));
    auto it = m_wornSets.find(acEvent.ServerId);
    if ((it == m_wornSets.end() || it->second.Epoch != acEvent.OwnershipEpoch) && pActor && CanReadArmor(pActor))
        Remember(acEvent.ServerId, acEvent.OwnershipEpoch, pActor->GetArmor());
    Equipment(acEvent.ServerId, acEvent.OwnershipEpoch, item, acEvent.Unequip);
}

void NakedNpcGuard::OnLocalInventory(const InventoryChangeEvent& acEvent) noexcept
{
    if (LocalTokenMatches(m_world, acEvent.FormId, acEvent.ServerId, acEvent.OwnershipEpoch))
        Removed(acEvent.ServerId, acEvent.OwnershipEpoch, acEvent.Item);
}

void NakedNpcGuard::OnDisconnected(const DisconnectedEvent&) noexcept
{
    m_wornSets.clear();
    m_copies.clear();
    m_nextScan = 0;
}

void NakedNpcGuard::Update() noexcept
{
    if (!m_world.GetTransport().IsConnected())
        return;
    const auto now = GetTickCount64();
    if (now < m_nextScan)
        return;
    m_nextScan = now + 250;

    // Retain intent for parked/unloaded entities; expire retired server IDs.
    for (auto entity : m_world.view<RemoteComponent>())
        if (auto it = m_wornSets.find(m_world.get<RemoteComponent>(entity).Id); it != m_wornSets.end())
            it->second.LastSeen = now;
    for (auto entity : m_world.view<LocalComponent>())
        if (auto it = m_wornSets.find(m_world.get<LocalComponent>(entity).Id); it != m_wornSets.end())
            it->second.LastSeen = now;

    for (auto entity : m_world.view<FormIdComponent>(entt::exclude<PlayerComponent>))
    {
        const auto* pLocal = m_world.try_get<LocalComponent>(entity);
        const auto* pRemote = m_world.try_get<RemoteComponent>(entity);
        if ((!pLocal && !pRemote) || (pLocal && pRemote))
            continue;
        const auto serverId = pLocal ? pLocal->Id : pRemote->Id;
        const auto epoch = pLocal ? pLocal->OwnershipEpoch : pRemote->OwnershipEpoch;
        if (!epoch)
            continue;
        const auto formId = m_world.get<FormIdComponent>(entity).Id;
        auto& copy = m_copies[formId];
        if (copy.Entity != entity || copy.ServerId != serverId || copy.Epoch != epoch)
            copy = {entity, serverId, epoch, now + kSettleMs + formId % 250, now};
        copy.LastSeen = now;
        auto* pActor = Cast<Actor>(TESForm::GetById(formId));
        if (!pActor || formId == 0x14 || pActor->GetExtension()->IsPlayer())
            continue;
        if (m_world.any_of<WaitingForAssignmentComponent, WaitingFor3D>(entity) || !pActor->GetNiNode())
        {
            copy.NextCheck = now + kSettleMs;
            continue;
        }
        if (now < copy.NextCheck)
            continue;
        copy.NextCheck = now + kCheckIntervalMs;
        if (!CanReadArmor(pActor))
            continue;

        auto it = m_wornSets.find(serverId);
        if (it == m_wornSets.end() || it->second.Epoch != epoch)
        {
            // Never infer remote intent from the possibly broken local copy or
            // from its default outfit. Local observations are the owner's evidence.
            if (pLocal && (it == m_wornSets.end() || it->second.Epoch < epoch))
                Remember(serverId, epoch, pActor->GetArmor());
            continue;
        }
        const auto& set = it->second;
        if (!set.Known || now < set.SettleUntil || set.Worn.Entries.empty())
            continue;

        // Existing outfit pieces are included in GetArmor. An outfit record alone
        // cannot authorize adding a missing piece or overriding an empty worn set.
        auto inventory = pActor->GetArmor();
        uint32_t occupied = 0;
        for (const auto& entry : inventory.Entries)
            if (entry.Count > 0 && entry.IsWorn())
                if (auto* pArmor = Armor(m_world, entry.BaseId))
                    occupied |= pArmor->slotType;

        uint32_t repaired = 0;
        for (const auto& desired : set.Worn.Entries)
        {
            auto* pArmor = Armor(m_world, desired.BaseId);
            if (!pArmor || !pArmor->slotType || (pArmor->slotType & occupied))
                continue;
            const Inventory::Entry* pCandidate = nullptr;
            int64_t total = 0;
            bool ambiguous = false;
            for (const auto& entry : inventory.Entries)
            {
                if (entry.BaseId != desired.BaseId)
                    continue;
                total += entry.Count; // Include negative base-container deltas.
                if (entry.Count > 0)
                {
                    if (pCandidate && !SameInstance(*pCandidate, entry))
                        ambiguous = true;
                    pCandidate = &entry;
                }
            }
            if (total <= 0 || !pCandidate || pCandidate->IsWorn() || ambiguous)
                continue;
            const bool formOnly = std::find(set.FormOnly.begin(), set.FormOnly.end(), desired.BaseId) != set.FormOnly.end();
            if (!formOnly && !SameInstance(desired, *pCandidate))
                continue;
            ExtraDataList* pExtra = nullptr;
            if (!FindExtraList(pActor, pArmor, *pCandidate, pExtra))
                continue;
            ScopedInventoryOverride inventoryOverride;
            // EquipManager supplies ScopedEquipOverride. Local repairs follow the
            // normal owner equipment-sync path; remote repairs cannot echo it.
            EquipManager::Get()->Equip(pActor, pArmor, pExtra, 1, nullptr, false, false, false, false);
            const auto after = pActor->GetArmor();
            if (std::any_of(after.Entries.begin(), after.Entries.end(), [&](const auto& entry)
                { return entry.BaseId == desired.BaseId && entry.Count > 0 && entry.IsWorn(); }))
            {
                occupied |= pArmor->slotType;
                ++repaired;
            }
        }
        if (repaired)
            spdlog::info("Naked fix: {:X} re-equipped {} items ({})", formId, repaired,
                pActor->IsDead() ? "dead NPC; owner worn set" : (pRemote ? "remote owner worn set" : "local owner worn set"));
    }

    for (auto it = m_copies.begin(); it != m_copies.end();)
        it = now - it->second.LastSeen > 60000 ? m_copies.erase(it) : std::next(it);
    for (auto it = m_wornSets.begin(); it != m_wornSets.end();)
        it = now - it->second.LastSeen > 60000 ? m_wornSets.erase(it) : std::next(it);
}
