#include <Services/InventoryService.h>
#include <Services/ActorValueService.h>
#include <Services/CorpseRagdollService.h>
#include <Services/Generic/QuestItemService.h>
#include <Services/Generic/SharedDropService.h>
#include <Services/Generic/NakedNpcGuard.h>

#include <Messages/RequestObjectInventoryChanges.h>
#include <Messages/NotifyObjectInventoryChanges.h>
#include <Messages/RequestInventoryChanges.h>
#include <Messages/NotifyInventoryChanges.h>
#include <Messages/RequestEquipmentChanges.h>
#include <Messages/NotifyEquipmentChanges.h>
#include <Messages/DrawWeaponRequest.h>
#include <Messages/NotifyDrawWeapon.h>

#include <Events/UpdateEvent.h>
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

namespace
{
// Inventory and equipment changes for a remote actor that is dying or dead are held briefly, and a
// removal cancelled by an equip of the same item is dropped: at a death the owner's engine
// removes and re-equips clothing, and applying that one message at a time drew the falling intro
// prisoner naked. Real changes (looting) still apply after the hold.
constexpr uint64_t kDeathChangeHoldMs = 1500;
struct HeldInventory
{
    uint64_t DueMs{};
    NotifyInventoryChanges Message;
};
struct HeldEquipment
{
    uint64_t DueMs{};
    NotifyEquipmentChanges Message;
};
std::vector<HeldInventory> s_heldInventory;
std::vector<HeldEquipment> s_heldEquipment;
bool s_replayingHeld{};

bool DyingOrDead(Actor* apActor) noexcept
{
    // The owner's death/ragdoll can arrive before the local native death transition.
    return apActor && (((apActor->actorState.flags1 >> 21) & 0xF) != 0 || apActor->IsDead() ||
        ActorValueService::IsDeathPending(apActor->formID) || CorpseRagdollService::IsFollowingOwner(apActor->formID));
}

Actor* RemoteActorFor(World& aWorld, uint32_t aServerId, uint32_t aEpoch) noexcept
{
    auto view = aWorld.view<RemoteComponent, FormIdComponent>(entt::exclude<LocalComponent>);
    for (auto entity : view)
    {
        const auto& remote = view.get<RemoteComponent>(entity);
        if (remote.Id == aServerId && remote.OwnershipEpoch == aEpoch)
            return Cast<Actor>(TESForm::GetById(view.get<FormIdComponent>(entity).Id));
    }
    return nullptr;
}
} // namespace

void InventoryService::OnUpdate(const UpdateEvent& acUpdateEvent) noexcept
{
    // Held death-time changes: drop removals cancelled by an equip of the same item for the
    // same actor, then apply the rest once due.
    if (!s_heldInventory.empty() || !s_heldEquipment.empty())
    {
        const auto now = GetTickCount64();
        for (auto removal = s_heldInventory.begin(); removal != s_heldInventory.end();)
        {
            const auto& item = removal->Message.Item;
            bool cancelled = false;
            if (item.Count < 0)
            {
                for (auto equip = s_heldEquipment.begin(); equip != s_heldEquipment.end(); ++equip)
                {
                    if (equip->Message.ServerId == removal->Message.ServerId &&
                        equip->Message.OwnershipEpoch == removal->Message.OwnershipEpoch && !equip->Message.Unequip &&
                        equip->Message.ItemId == item.BaseId)
                    {
                        spdlog::info("Death-time change for server {:X}: item {:X} removed and re-equipped; left as is",
                            removal->Message.ServerId, item.BaseId.BaseId);
                        s_heldEquipment.erase(equip);
                        cancelled = true;
                        break;
                    }
                }
            }
            removal = cancelled ? s_heldInventory.erase(removal) : std::next(removal);
        }
        s_replayingHeld = true;
        for (auto it = s_heldInventory.begin(); it != s_heldInventory.end();)
        {
            if (now < it->DueMs)
            {
                ++it;
                continue;
            }
            const auto message = it->Message;
            it = s_heldInventory.erase(it);
            OnNotifyInventoryChanges(message);
        }
        for (auto it = s_heldEquipment.begin(); it != s_heldEquipment.end();)
        {
            if (now < it->DueMs)
            {
                ++it;
                continue;
            }
            const auto message = it->Message;
            it = s_heldEquipment.erase(it);
            // An equip of something the actor already wears changes nothing but re-attaches it.
            auto* pActor = RemoteActorFor(m_world, message.ServerId, message.OwnershipEpoch);
            bool alreadyWorn = false;
            if (pActor && !message.Unequip)
            {
                for (const auto& entry : pActor->GetActorInventory().Entries)
                {
                    if (entry.BaseId == message.ItemId && entry.IsWorn())
                        alreadyWorn = true;
                }
            }
            if (alreadyWorn)
                spdlog::info("Death-time equip for server {:X}: item {:X} already worn; left as is", message.ServerId, message.ItemId.BaseId);
            else
                OnNotifyEquipmentChanges(message);
        }
        s_replayingHeld = false;
    }
    RunWeaponStateUpdates();
    m_world.GetNakedNpcGuard().Update();
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

    request.Count = acEvent.Count;
    request.Unequip = acEvent.Unequip;
    request.IsSpell = acEvent.IsSpell;
    request.IsShout = acEvent.IsShout;
    request.IsAmmo = acEvent.IsAmmo;
    request.CurrentInventory = pActor->GetEquipment();

    m_transport.Send(request);

    spdlog::info("Sending equipment request, item: {:X}, count: {}, target object: {:X}, unequip: {}",
        acEvent.ItemId, acEvent.Count, acEvent.ActorId, acEvent.Unequip);
}

void InventoryService::OnNotifyInventoryChanges(const NotifyInventoryChanges& acMessage) noexcept
{
    if (!s_replayingHeld && acMessage.OwnershipEpoch != 0 &&
        DyingOrDead(RemoteActorFor(m_world, acMessage.ServerId, acMessage.OwnershipEpoch)))
    {
        s_heldInventory.push_back({GetTickCount64() + kDeathChangeHoldMs, acMessage});
        spdlog::info("Death-time inventory held for server {:X}: item {:X}, count {}, quest {}",
            acMessage.ServerId, acMessage.Item.BaseId.BaseId, acMessage.Item.Count, acMessage.Item.IsQuestItem);
        return;
    }
    // Quest notifications also mutate the actor's inventory. Let the death hold run
    // first; surviving messages still use the quest service when replayed.
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
    if (!s_replayingHeld && DyingOrDead(RemoteActorFor(m_world, acMessage.ServerId, acMessage.OwnershipEpoch)))
    {
        s_heldEquipment.push_back({GetTickCount64() + kDeathChangeHoldMs, acMessage});
        spdlog::info("Death-time equipment held for server {:X}: item {:X}, unequip {}",
            acMessage.ServerId, acMessage.ItemId.BaseId, acMessage.Unequip);
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
        spdlog::debug("Discarded an equipment update for actor {:X} because epoch {} is no longer current", acMessage.ServerId, acMessage.OwnershipEpoch);
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
        pEquipManager->UnEquip(pActor, pItem, nullptr, acMessage.Count, pEquipSlot, false, true, false, false, nullptr);
    }
    else
    {
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
