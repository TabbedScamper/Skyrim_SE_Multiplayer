#include <Services/InventoryService.h>
#include <Services/ActorValueService.h>
#include <Services/CorpseRagdollService.h>
#include <Services/Generic/QuestItemService.h>
#include <Services/Generic/SharedDropService.h>
#include <Services/Generic/NakedNpcGuard.h>
#include <Services/Generic/NpcLootService.h>

#include <Messages/RequestObjectInventoryChanges.h>
#include <Messages/NotifyObjectInventoryChanges.h>
#include <Messages/RequestInventoryChanges.h>
#include <Messages/NotifyInventoryChanges.h>
#include <Messages/RequestEquipmentChanges.h>
#include <Messages/NotifyEquipmentChanges.h>
#include <Messages/DrawWeaponRequest.h>
#include <Messages/NotifyDrawWeapon.h>
#include <Messages/NpcInventory.h>

#include <Events/UpdateEvent.h>
#include <Events/DisconnectedEvent.h>
#include <Events/InventoryChangeEvent.h>
#include <Events/EquipmentChangeEvent.h>

#include <World.h>
#include <Games/Skyrim/Interface/UI.h>
#include <PlayerCharacter.h>
#include <Forms/TESObjectCELL.h>
#include <Actor.h>
#include <Structs/ObjectData.h>
#include <Forms/TESWorldSpace.h>
#include <Games/TES.h>
#include <Games/Overrides.h>
#include <EquipManager.h>
#include <Games/ActorExtension.h>
#include <Forms/TESNPC.h>
#include <DefaultObjectManager.h>
#include <ExtraData/ExtraContainerChanges.h>
#include <variant>
#include <unordered_map>

InventoryService::InventoryService(World& aWorld, entt::dispatcher& aDispatcher, TransportService& aTransport) noexcept
    : m_world(aWorld)
    , m_dispatcher(aDispatcher)
    , m_transport(aTransport)
{
    m_updateConnection = m_dispatcher.sink<UpdateEvent>().connect<&InventoryService::OnUpdate>(this);
    m_inventoryConnection = m_dispatcher.sink<InventoryChangeEvent>().connect<&InventoryService::OnInventoryChangeEvent>(this);
    m_equipmentConnection = m_dispatcher.sink<EquipmentChangeEvent>().connect<&InventoryService::OnEquipmentChangeEvent>(this);
    m_inventoryChangeConnection = m_dispatcher.sink<NotifyInventoryChanges>().connect<&InventoryService::OnNotifyInventoryChanges>(this);
    m_equipmentChangeConnection = m_dispatcher.sink<NotifyEquipmentChanges>().connect<&InventoryService::OnNotifyEquipmentChanges>(this);
}

void InventoryService::OnUpdate(const UpdateEvent&) noexcept
{
    m_world.GetNakedNpcGuard().Update();
    if (!m_transport.IsConnected()) return;
    RunWeaponStateUpdates();
    WatchLocalWorn();
}

// Script equips (the intro's prisoner clothes: AddItem + EquipItem from MQ101) send no equip message, so the other players'
// copy of this player stayed naked (owner 2026-10-03: "we are missing our clothes"). Once a second, compare this player's
// worn set with the last one announced and announce the whole set again when it changed. O(worn items) per second.
void InventoryService::WatchLocalWorn() noexcept
{
    const auto now = GetTickCount64();
    if (now < m_nextWornCheck)
        return;
    m_nextWornCheck = now + 1000;
    auto* pPlayer = PlayerCharacter::Get();
    if (!pPlayer || !pPlayer->parentCell)
        return;
    uint64_t signature = 1469598103934665603ull;
    for (const auto& entry : pPlayer->GetEquipment().Entries)
        if (entry.IsWorn())
        {
            signature = (signature ^ (static_cast<uint64_t>(entry.BaseId.ModId) << 32 | entry.BaseId.BaseId)) * 1099511628211ull;
            signature = (signature ^ (entry.ExtraWornLeft ? 2u : 1u)) * 1099511628211ull;
        }
    // A copy created after the last announcement was created naked (2026-10-03: the set went out at 01:25:50, the other
    // PC made its copy of this player later): announce again when the number of other players' copies here changes, and
    // every 30 s as a safety net until spawns carry the current worn set (IER).
    // RemoteComponent counts NPC copies too: a change is only a hint, rate-limited to one announcement per 3 s.
    const auto copies = static_cast<uint32_t>(m_world.view<RemoteComponent>().size());
    if (copies != m_lastCopyCount)
        m_copiesChangedPending = true;
    m_lastCopyCount = copies;
    const bool due = signature != m_lastWornSignature || now - m_lastWornAnnounce >= 30000 ||
        (m_copiesChangedPending && now - m_lastWornAnnounce >= 3000);
    if (!due)
        return;
    m_copiesChangedPending = false;
    m_lastWornSignature = signature;
    m_lastWornAnnounce = now;
    AnnounceWorn();
}

void InventoryService::OnInventoryChangeEvent(const InventoryChangeEvent& acEvent) noexcept
{
    if (!m_transport.IsConnected())
        return;

    // The post-drop native container event carries the actual world reference
    // and count. SharedDropService reports that removal atomically with creation.
    if (acEvent.Drop && acEvent.FormId == 0x14 && m_world.GetSharedDropService().TracksPlayerDrops())
        return;

    auto view = m_world.view<FormIdComponent>();

    const auto iter = std::find_if(std::begin(view), std::end(view), [view, formId = acEvent.FormId](auto entity) { return view.get<FormIdComponent>(entity).Id == formId; });

    if (iter == std::end(view))
        return;

    uint32_t serverId = 0;
    if (acEvent.OwnershipEpoch != 0)
    {
        const auto* pLocalComponent = m_world.try_get<LocalComponent>(*iter);
        const auto* pRemoteComponent = m_world.try_get<RemoteComponent>(*iter);
        const bool ownershipMatches = (pLocalComponent && pLocalComponent->Id == acEvent.ServerId && pLocalComponent->OwnershipEpoch == acEvent.OwnershipEpoch)
            || (pRemoteComponent && pRemoteComponent->Id == acEvent.ServerId && pRemoteComponent->OwnershipEpoch == acEvent.OwnershipEpoch);

        if (!ownershipMatches)
        {
            spdlog::debug("Discarded an inventory change for actor {:X} because ownership changed after it was queued (epoch {})", acEvent.ServerId, acEvent.OwnershipEpoch);
            return;
        }

        serverId = acEvent.ServerId;
    }
    else
    {
        if (Cast<Actor>(TESForm::GetById(acEvent.FormId)))
            return;

        const std::optional<uint32_t> serverIdRes = Utils::GetServerId(*iter);
        if (!serverIdRes)
        {
            spdlog::warn(
                "Discarded inventory change for form {:X} because it has no server entity (item {:X}, count {})", acEvent.FormId, acEvent.Item.BaseId.BaseId, acEvent.Item.Count);
            return;
        }
        serverId = *serverIdRes;
    }

    if (acEvent.OwnershipEpoch != 0 && acEvent.FormId != 0x14 && !acEvent.Drop)
    {
        const uint64_t key = (uint64_t(acEvent.FormId) << 32) ^ (uint64_t(acEvent.Item.BaseId.ModId) << 24) ^ acEvent.Item.BaseId.BaseId;
        auto& recent = m_recentNpcDeltas[key];
        const auto nowMs = GetTickCount64();
        if (nowMs - recent.AtMs > 3000)
            recent.Net = 0;
        recent.Net += acEvent.Item.Count;
        recent.AtMs = nowMs;
        recent.ServerId = serverId;
        recent.OwnershipEpoch = acEvent.OwnershipEpoch;
        recent.Item = acEvent.Item;
        if (m_recentNpcDeltas.size() > 256)
            std::erase_if(m_recentNpcDeltas, [nowMs](const auto& entry) { return nowMs - entry.second.AtMs > 3000; });
    }

    RequestInventoryChanges request;
    request.ServerId = serverId;
    request.OwnershipEpoch = acEvent.OwnershipEpoch;
    request.Item = acEvent.Item;
    request.Drop = acEvent.Drop;
    request.UpdateClients = acEvent.UpdateClients;

    m_transport.Send(request);

    spdlog::info("Sending item request, item: {:X}, count: {}, target object: {:X}", acEvent.Item.BaseId.BaseId, acEvent.Item.Count, acEvent.FormId);
}

void InventoryService::OnEquipmentChangeEvent(const EquipmentChangeEvent& acEvent) noexcept
{
    if (!m_transport.IsConnected())
        return;

    auto view = m_world.view<FormIdComponent>();

    const auto iter = std::find_if(std::begin(view), std::end(view), [view, formId = acEvent.ActorId](auto entity) { return view.get<FormIdComponent>(entity).Id == formId; });

    if (iter == std::end(view))
        return;

    const auto* pLocalComponent = m_world.try_get<LocalComponent>(*iter);
    if (acEvent.OwnershipEpoch == 0 || !pLocalComponent || pLocalComponent->Id != acEvent.ServerId || pLocalComponent->OwnershipEpoch != acEvent.OwnershipEpoch)
    {
        spdlog::debug("Discarded an equipment change for actor {:X} because ownership changed after it was queued (epoch {})", acEvent.ServerId, acEvent.OwnershipEpoch);
        return;
    }

    Actor* pActor = Cast<Actor>(TESForm::GetById(acEvent.ActorId));
    if (!pActor)
        return;

    auto& modSystem = World::Get().GetModSystem();

    RequestEquipmentChanges request;
    request.ServerId = acEvent.ServerId;
    request.OwnershipEpoch = acEvent.OwnershipEpoch;

    if (!modSystem.GetServerModId(acEvent.EquipSlotId, request.EquipSlotId))
        return;
    if (!modSystem.GetServerModId(acEvent.ItemId, request.ItemId))
        return;

    // Equipping proves the NPC owns the item. If this PC just reported it removed and never re-added (the engine
    // re-applied the outfit: Ralof entering Helgen Keep, 2026-09-30 15:46:53, sent -1 for his cuirass and boots and
    // then equipped both), report the add first so every other PC keeps the items (he was naked on the follower).
    if (!acEvent.Unequip && !acEvent.IsSpell && !acEvent.IsShout && acEvent.ActorId != 0x14)
    {
        const uint64_t key = (uint64_t(acEvent.ActorId) << 32) ^ (uint64_t(request.ItemId.ModId) << 24) ^ request.ItemId.BaseId;
        const auto recent = m_recentNpcDeltas.find(key);
        if (recent != m_recentNpcDeltas.end() && recent->second.Net < 0 && GetTickCount64() - recent->second.AtMs <= 3000 &&
            recent->second.ServerId == acEvent.ServerId && recent->second.OwnershipEpoch == acEvent.OwnershipEpoch)
        {
            RequestInventoryChanges restore;
            restore.ServerId = acEvent.ServerId;
            restore.OwnershipEpoch = acEvent.OwnershipEpoch;
            restore.Item = recent->second.Item;
            restore.Item.Count = static_cast<int32_t>(-recent->second.Net);
            restore.UpdateClients = true;
            m_transport.Send(restore);
            spdlog::info("Equipped {:X} on {:X} after reporting it removed ({}); re-added it for the other PCs",
                acEvent.ItemId, acEvent.ActorId, recent->second.Net);
            m_recentNpcDeltas.erase(recent);
        }
    }

    request.Count = acEvent.Count;
    request.Unequip = acEvent.Unequip;
    request.IsSpell = acEvent.IsSpell;
    request.IsShout = acEvent.IsShout;
    request.IsAmmo = acEvent.IsAmmo;
    request.CurrentInventory = pActor->GetEquipment();
    if (!request.Unequip && !request.IsSpell && !request.IsShout)
    {
        const bool left = TESForm::GetById(acEvent.EquipSlotId) == DefaultObjectManager::Get().leftEquipSlot;
        const bool wornWeapon = pActor->GetEquippedWeapon(left ? 0 : 1) == TESForm::GetById(acEvent.ItemId);
        for (const auto& entry : request.CurrentInventory.Entries)
        {
            if (entry.BaseId != request.ItemId || !entry.IsWorn() ||
                (wornWeapon && entry.ExtraWornLeft != left))
                continue;
            if (request.HasStack) { request.HasStack = false; break; }
            request.Stack = entry;
            request.Stack.Count = 1;
            request.HasStack = true;
        }
    }

    m_transport.Send(request);

    spdlog::info("Sending equipment request, item: {:X}, count: {}, target object: {:X}, unequip: {}",
        acEvent.ItemId, acEvent.Count, acEvent.ActorId, acEvent.Unequip);
}

void InventoryService::AnnounceWorn() noexcept
{
    auto* pPlayer = PlayerCharacter::Get();
    if (!m_transport.IsConnected() || !pPlayer)
        return;
    auto view = m_world.view<FormIdComponent, LocalComponent>();
    const auto it = std::find_if(view.begin(), view.end(), [view](auto entity) { return view.get<FormIdComponent>(entity).Id == 0x14; });
    if (it == view.end())
        return;
    const auto& local = view.get<LocalComponent>(*it);
    auto& modSystem = m_world.GetModSystem();
    GameId right{}, left{};
    modSystem.GetServerModId(DefaultObjectManager::Get().rightEquipSlot->formID, right);
    modSystem.GetServerModId(DefaultObjectManager::Get().leftEquipSlot->formID, left);
    const auto equipment = pPlayer->GetEquipment();
    uint32_t sent = 0;
    for (const auto& entry : equipment.Entries)
    {
        if (!entry.IsWorn())
            continue;
        auto* pItem = TESForm::GetById(modSystem.GetGameId(entry.BaseId));
        if (!pItem)
            continue;
        const auto type = static_cast<uint8_t>(pItem->formType);
        RequestEquipmentChanges request;
        request.ServerId = local.Id;
        request.OwnershipEpoch = local.OwnershipEpoch;
        request.ItemId = entry.BaseId;
        // Weapons and spells name the hand; armour and ammunition have no slot.
        if (type == 0x29 || type == 0x16)
            request.EquipSlotId = entry.ExtraWornLeft ? left : right;
        request.Count = type == 0x2A ? static_cast<uint32_t>(std::max(entry.Count, 1)) : 1;
        request.IsAmmo = type == 0x2A;
        request.CurrentInventory = equipment;
        request.Stack = entry;
        request.Stack.Count = 1;
        request.HasStack = true;
        m_transport.Send(request);
        ++sent;
    }
    // What came off since the last announcement goes out as an unequip: a script or swap that empties a hand or slot
    // sends no message of its own, and the copy kept the old weapon (owner 2026-10-03).
    std::vector<std::pair<GameId, bool>> current;
    for (const auto& entry : equipment.Entries)
        if (entry.IsWorn())
            current.emplace_back(entry.BaseId, entry.ExtraWornLeft);
    uint32_t removed = 0;
    for (const auto& [item, leftHand] : m_lastAnnouncedWorn)
    {
        if (std::find(current.begin(), current.end(), std::pair{item, leftHand}) != current.end())
            continue;
        auto* pItem = TESForm::GetById(modSystem.GetGameId(item));
        if (!pItem)
            continue;
        const auto type = static_cast<uint8_t>(pItem->formType);
        RequestEquipmentChanges request;
        request.ServerId = local.Id;
        request.OwnershipEpoch = local.OwnershipEpoch;
        request.ItemId = item;
        if (type == 0x29 || type == 0x16)
            request.EquipSlotId = leftHand ? left : right;
        request.Count = 1;
        request.Unequip = true;
        request.IsAmmo = type == 0x2A;
        request.CurrentInventory = equipment;
        m_transport.Send(request);
        ++removed;
    }
    m_lastAnnouncedWorn = std::move(current);
    spdlog::info("Announced {} worn items and {} removals to the party (server id {:X}, epoch {})", sent, removed, local.Id,
        local.OwnershipEpoch);
}

void InventoryService::OnNotifyInventoryChanges(const NotifyInventoryChanges& acMessage) noexcept
{
    if (m_world.GetNakedNpcGuard().Defer(acMessage)) return;
    if (m_world.ctx().at<QuestItemService>().HandleInventoryNotify(acMessage))
        return;
    if (acMessage.OwnershipEpoch != 0)
    {
        Actor* pActor = nullptr;

        auto remoteView = m_world.view<RemoteComponent, FormIdComponent>(entt::exclude<LocalComponent>);
        const auto remoteIt = std::find_if(remoteView.begin(), remoteView.end(), [remoteView, &acMessage](const entt::entity aEntity)
        {
            const auto& remoteComponent = remoteView.get<RemoteComponent>(aEntity);
            return remoteComponent.Id == acMessage.ServerId && remoteComponent.OwnershipEpoch == acMessage.OwnershipEpoch;
        });

        if (remoteIt != remoteView.end())
            pActor = Cast<Actor>(TESForm::GetById(remoteView.get<FormIdComponent>(*remoteIt).Id));
        else
        {
            auto localView = m_world.view<LocalComponent, FormIdComponent>();
            const auto localIt = std::find_if(localView.begin(), localView.end(), [localView, &acMessage](const entt::entity aEntity)
            {
                const auto& localComponent = localView.get<LocalComponent>(aEntity);
                return localComponent.Id == acMessage.ServerId && localComponent.OwnershipEpoch == acMessage.OwnershipEpoch;
            });

            if (localIt != localView.end())
                pActor = Cast<Actor>(TESForm::GetById(localView.get<FormIdComponent>(*localIt).Id));
        }

        if (!pActor)
        {
            spdlog::debug("Discarded an inventory update for actor {:X} because epoch {} is no longer current", acMessage.ServerId, acMessage.OwnershipEpoch);
            return;
        }

        ScopedInventoryOverride _;

        // Old Drop notifications are inventory removals only. World copies are
        // created exclusively from a server-identified shared drop.
        pActor->AddOrRemoveItem(acMessage.Item);

        return;
    }

    TESObjectREFR* pObject = Utils::GetByServerId<TESObjectREFR>(acMessage.ServerId);
    if (!pObject)
        return;

    ScopedInventoryOverride _;
    pObject->AddOrRemoveItem(acMessage.Item);
}

void InventoryService::OnNotifyEquipmentChanges(const NotifyEquipmentChanges& acMessage) noexcept
{
    {
        static uint64_t s_lastReceiptLog{};
        if (GetTickCount64() - s_lastReceiptLog > 5000)
        {
            s_lastReceiptLog = GetTickCount64();
            spdlog::info("Equipment update received for actor {:X} epoch {} item {:X}:{:X} unequip {}", acMessage.ServerId,
                acMessage.OwnershipEpoch, acMessage.ItemId.ModId, acMessage.ItemId.BaseId, acMessage.Unequip);
        }
    }
    if (m_world.GetNakedNpcGuard().Defer(acMessage))
    {
        spdlog::info("Equipment update for actor {:X} deferred by the naked NPC guard", acMessage.ServerId);
        return;
    }
    auto view = m_world.view<RemoteComponent, FormIdComponent>(entt::exclude<LocalComponent>);
    const auto it = std::find_if(view.begin(), view.end(), [view, &acMessage](const entt::entity aEntity)
    {
        const auto& remoteComponent = view.get<RemoteComponent>(aEntity);
        return remoteComponent.Id == acMessage.ServerId && remoteComponent.OwnershipEpoch == acMessage.OwnershipEpoch;
    });
    if (it == view.end())
    {
        spdlog::warn("Discarded an equipment update for actor {:X} because epoch {} is no longer current", acMessage.ServerId, acMessage.OwnershipEpoch);
        return;
    }

    Actor* pActor = Cast<Actor>(TESForm::GetById(view.get<FormIdComponent>(*it).Id));
    if (!pActor)
        return;

    auto& modSystem = World::Get().GetModSystem();

    uint32_t itemId = modSystem.GetGameId(acMessage.ItemId);
    TESForm* pItem = TESForm::GetById(itemId);

    if (!pItem)
    {
        spdlog::error("Could not find inventory item {:X}:{:X}", acMessage.ItemId.ModId, acMessage.ItemId.BaseId);
        return;
    }

    uint32_t equipSlotId = modSystem.GetGameId(acMessage.EquipSlotId);
    TESForm* pEquipSlot = TESForm::GetById(equipSlotId);

    uint32_t slotId = 0;
    if (pEquipSlot == DefaultObjectManager::Get().rightEquipSlot)
        slotId = 1;

    auto* pEquipManager = EquipManager::Get();

    if (acMessage.IsSpell)
    {
        if (acMessage.Unequip)
            pEquipManager->UnEquipSpell(pActor, pItem, slotId);
        else
            pEquipManager->EquipSpell(pActor, pItem, slotId);

        return;
    }
    else if (acMessage.IsShout)
    {
        if (acMessage.Unequip)
            pEquipManager->UnEquipShout(pActor, pItem);
        else
            pEquipManager->EquipShout(pActor, pItem);

        return;
    }

    // TODO: ExtraData necessary? probably
    if (acMessage.Unequip)
    {
        ExtraDataList* wornList = nullptr;
        if (const auto* changes = pActor->GetContainerChanges(); changes && changes->entries)
            for (auto* entry : *changes->entries)
                if (entry && entry->form == pItem && entry->dataList)
                    for (auto* extra : *entry->dataList)
                        if (extra && (extra->Contains(ExtraDataType::Worn) || extra->Contains(ExtraDataType::WornLeft)))
                        { wornList = extra; break; }
        pEquipManager->UnEquip(pActor, pItem, wornList, acMessage.Count, pEquipSlot, false, true, false, false, nullptr);
    }
    else
    {
        if (acMessage.HasStack && acMessage.Stack.BaseId == acMessage.ItemId)
        {
            const auto desired = acMessage.Stack;
            auto unwornDesired = desired;
            unwornDesired.ExtraWorn = unwornDesired.ExtraWornLeft = false;
            const auto findStack = [&]() {
                ExtraDataList* found = nullptr;
                bool worn = false;
                if (const auto* changes = pActor->GetContainerChanges(); changes && changes->entries)
                    for (auto* entry : *changes->entries)
                        if (entry && entry->form == pItem && entry->dataList)
                            for (auto* extra : *entry->dataList)
                            {
                                if (!extra) continue;
                                Inventory::Entry candidate;
                                candidate.BaseId = acMessage.ItemId;
                                TESObjectREFR::GetItemFromExtraData(candidate, extra);
                                if (!NpcSameItem(candidate, desired, false)) continue;
                                if (candidate.IsWorn() && candidate.ExtraWornLeft == desired.ExtraWornLeft) worn = true;
                                else found = extra;
                            }
                return std::pair{found, worn};
            };
            auto [exact, alreadyWorn] = findStack();
            if (alreadyWorn)
            {
                spdlog::info("Inventory equip for {:X}: {:X} exact stack already worn", pActor->formID, itemId);
                return;
            }
            const auto inventory = pActor->GetActorInventory();
            const bool owned = std::any_of(inventory.Entries.begin(), inventory.Entries.end(), [&](const auto& entry) {
                return entry.Count > 0 && NpcSameItem(entry, desired, false);
            });
            if (!owned && pActor->GetExtension()->IsRemotePlayer())
                if (auto* bound = Cast<TESBoundObject>(pItem))
                {
                    auto unworn = desired;
                    unworn.Count = 1;
                    unworn.ExtraWorn = unworn.ExtraWornLeft = false;
                    const bool mapped = !unworn.ExtraEnchantId ||
                        (unworn.ExtraEnchantId.ModId == UINT32_MAX ? !unworn.EnchantData.Effects.empty() :
                            TESForm::GetById(modSystem.GetGameId(unworn.ExtraEnchantId)) != nullptr);
                    if (!mapped)
                    {
                        spdlog::info("Inventory equip for {:X}: {:X} enchantment not mapped here; skipped", pActor->formID, itemId);
                        return;
                    }
                    ScopedInventoryOverride inventoryOverride;
                    pActor->AddObjectToContainer(bound, TESObjectREFR::GetExtraDataFromItem(unworn), 1, nullptr);
                    const auto updated = findStack();
                    exact = updated.first;
                    alreadyWorn = updated.second;
                }
            // No exact match but the copy holds the item only in extra-data stacks: the native equip with a null list then
            // does nothing (2026-10-03: the follower's armor was "equipped" every 30 s on the host's copy and never shown).
            // A player's copy wears the first unworn stack of the item instead.
            if (!exact && pActor->GetExtension()->IsRemotePlayer())
                if (const auto* changes = pActor->GetContainerChanges(); changes && changes->entries)
                    for (auto* entry : *changes->entries)
                        if (!exact && entry && entry->form == pItem && entry->dataList)
                            for (auto* extra : *entry->dataList)
                                if (extra && !extra->Contains(ExtraDataType::Worn) && !extra->Contains(ExtraDataType::WornLeft))
                                {
                                    exact = extra;
                                    break;
                                }
            if (!alreadyWorn && (exact || (!unwornDesired.ContainsExtraData() && (owned || pActor->IsItemInInventory(itemId)))))
                pEquipManager->Equip(pActor, pItem, exact, 1, pEquipSlot, false, true, false, false);
            else if (!alreadyWorn && pActor->GetExtension()->IsRemotePlayer())
            {
                // No stack on the copy matched the owner's exactly (2026-10-03: the follower's armor never showed on the
                // host's copy; every announcement was received and silently skipped here). A player's copy only shows
                // what its owner wears: wear the plain item rather than nothing.
                if (!pActor->IsItemInInventory(itemId))
                    if (auto* pBound = Cast<TESBoundObject>(pItem))
                    {
                        ScopedInventoryOverride _;
                        pActor->AddObjectToContainer(pBound, nullptr, 1, nullptr);
                    }
                spdlog::info("Inventory equip for remote player {:X}: {:X} worn without its exact stack", pActor->formID, itemId);
                pEquipManager->Equip(pActor, pItem, nullptr, 1, pEquipSlot, false, true, false, false);
            }
            return;
        }
        // Do not detach an unchanged outfit, even if the local death state has
        // not caught up with the owner yet.
        if (pItem->formType == FormType::Armor)
        {
            for (const auto& armor : pActor->GetWornArmor().Entries)
            {
                if (armor.BaseId == acMessage.ItemId)
                {
                    spdlog::info("Inventory equip for actor {:X}: armor {:X} already worn; left as is", pActor->formID, itemId);
                    return;
                }
            }
        }

        // A remote PLAYER's copy shows what its owner wears: the native equip silently refuses an item missing from
        // the copy's inventory, and loot taken from a container or corpse never reached the copy (armor picked up and
        // worn stayed invisible to the other player, owners 2026-09-30 19:51, D1F3B from DDF87). The owner's
        // inventory is authoritative, so the copy is given the item first. NPCs keep the naked guard's stock rules.
        if (pActor->GetExtension()->IsRemotePlayer() && !pActor->IsItemInInventory(pItem->formID))
            if (auto* pBound = Cast<TESBoundObject>(pItem))
            {
                ScopedInventoryOverride _;
                pActor->AddObjectToContainer(pBound, nullptr, (std::max)(1, static_cast<int32_t>(acMessage.Count)), nullptr);
                spdlog::info("Inventory equip for remote player {:X}: added missing {:X} before equipping", pActor->formID, itemId);
            }
        // Native equip (38894 -> 38929 -> 38001 -> 38004 / 0x1406B3650)
        // removes conflicting biped slots under ScopedEquipOverride. Unequipping
        // and restoring every armor piece here detaches unrelated corpse gear.
        pEquipManager->Equip(pActor, pItem, nullptr, acMessage.Count, pEquipSlot, false, true, false, false);
    }
}

void InventoryService::RunWeaponStateUpdates() noexcept
{
    if (!m_transport.IsConnected())
        return;

    static std::chrono::steady_clock::time_point lastSendTimePoint;
    constexpr auto cDelayBetweenUpdates = 500ms;

    const auto now = std::chrono::steady_clock::now();
    if (now - lastSendTimePoint < cDelayBetweenUpdates)
        return;

    lastSendTimePoint = now;

    auto view = m_world.view<FormIdComponent, LocalComponent>();

    for (auto entity : view)
    {
        const auto& formIdComponent = view.get<FormIdComponent>(entity);
        Actor* const pActor = Cast<Actor>(TESForm::GetById(formIdComponent.Id));
        // The form is gone while a leave or quit tears the world down (2026-10-03: crash on the follower's leave).
        if (!pActor)
            continue;
        auto& localComponent = view.get<LocalComponent>(entity);

        bool isWeaponDrawn = pActor->actorState.IsWeaponDrawn();
        if (isWeaponDrawn != localComponent.IsWeaponDrawn)
        {
            localComponent.IsWeaponDrawn = isWeaponDrawn;

            DrawWeaponRequest request;
            request.Id = localComponent.Id;
            request.IsWeaponDrawn = isWeaponDrawn;

            m_transport.Send(request);
        }
    }
}
