#include "InventoryService.h"

#include <Components.h>
#include <World.h>
#include <GameServer.h>

#include <Messages/NotifyObjectInventoryChanges.h>
#include <Messages/RequestInventoryChanges.h>
#include <Messages/NotifyInventoryChanges.h>
#include <Messages/RequestEquipmentChanges.h>
#include <Messages/NotifyEquipmentChanges.h>
#include <Messages/DrawWeaponRequest.h>
#include <Messages/NotifyDrawWeapon.h>
#include <Messages/AnimObjectRequest.h>
#include <Messages/NotifyAnimObject.h>
#include <Messages/RequestNpcWorn.h>
#include <Messages/NotifyNpcWorn.h>
#include <Messages/RequestNpcLoot.h>
#include <Messages/NotifyNpcLoot.h>

namespace
{
struct NpcWornCache { NpcWornData Data; };
}
struct NpcInventoryRelay
{
    World& WorldRef;
    entt::scoped_connection WornConnection, LootConnection;

    NpcInventoryRelay(World& world, entt::dispatcher& dispatcher) : WorldRef(world)
    {
        WornConnection = dispatcher.sink<PacketEvent<RequestNpcWorn>>().connect<&NpcInventoryRelay::Worn>(this);
        LootConnection = dispatcher.sink<PacketEvent<RequestNpcLoot>>().connect<&NpcInventoryRelay::Loot>(this);
    }
    void Worn(const PacketEvent<RequestNpcWorn>& event)
    {
        const auto& request = event.Packet;
        if (!event.pPlayer || !request.Valid()) return;
        const auto entity = static_cast<entt::entity>(request.ServerId);
        const auto* owner = WorldRef.try_get<OwnerComponent>(entity);
        const auto* character = WorldRef.try_get<CharacterComponent>(entity);
        const auto* cell = WorldRef.try_get<CellIdComponent>(entity);
        if (!owner || !owner->GetOwner() || !character || character->IsPlayer() || !cell ||
            owner->OwnershipEpoch != request.OwnershipEpoch) return;
        auto* party = WorldRef.GetPartyService().GetPlayerParty(event.pPlayer);
        if (!party || event.pPlayer->GetParty().JoinedPartyId != owner->GetOwner()->GetParty().JoinedPartyId) return;
        if (!request.Sequence)
        {
            if (!event.pPlayer->GetCellComponent().IsInRange(*cell, character->IsDragon())) return;
            if (auto* cached = WorldRef.try_get<NpcWornCache>(entity); cached && cached->Data.OwnershipEpoch == request.OwnershipEpoch)
            {
                NotifyNpcWorn reply; static_cast<NpcWornData&>(reply) = cached->Data; event.pPlayer->Send(reply);
            }
            return;
        }
        if (owner->GetOwner() != event.pPlayer) return;
        auto& cache = WorldRef.get_or_emplace<NpcWornCache>(entity);
        if (cache.Data.OwnershipEpoch == request.OwnershipEpoch && cache.Data.Sequence >= request.Sequence) return;
        cache.Data = request;
        NotifyNpcWorn notify; static_cast<NpcWornData&>(notify) = request;
        for (auto* member : party->Members)
            if (member != event.pPlayer && member->GetCellComponent().IsInRange(*cell, character->IsDragon())) member->Send(notify);
    }
    void Loot(const PacketEvent<RequestNpcLoot>& event)
    {
        // The transaction adapter is disabled on both ends. Do not mutate stock,
        // trust a shadow lease, or leave a caller waiting for an owner response.
        if (!event.pPlayer || !event.Packet.Valid() ||
            (event.Packet.Op != NpcLootOp::Fetch && event.Packet.Op != NpcLootOp::Transfer)) return;
        NotifyNpcLoot reply;
        static_cast<NpcLootData&>(reply) = event.Packet;
        reply.Op = event.Packet.Op == NpcLootOp::Fetch ? NpcLootOp::FetchResult : NpcLootOp::TransferResult;
        reply.Accepted = reply.ContentsKnown = false; reply.Contents = {};
        event.pPlayer->Send(reply);
    }
};

InventoryService::InventoryService(World& aWorld, entt::dispatcher& aDispatcher)
    : m_world(aWorld)
    , m_npcRelay(std::make_unique<NpcInventoryRelay>(aWorld, aDispatcher))
{
    // Constructing another context value here invalidates EnTT's in-progress
    // InventoryService insertion. Keep the relay alive with its owning service.
    m_inventoryChangeConnection = aDispatcher.sink<PacketEvent<RequestInventoryChanges>>().connect<&InventoryService::OnInventoryChanges>(this);
    m_equipmentChangeConnection = aDispatcher.sink<PacketEvent<RequestEquipmentChanges>>().connect<&InventoryService::OnEquipmentChanges>(this);
    m_drawWeaponConnection = aDispatcher.sink<PacketEvent<DrawWeaponRequest>>().connect<&InventoryService::OnWeaponDrawnRequest>(this);
    m_animObjectConnection = aDispatcher.sink<PacketEvent<AnimObjectRequest>>().connect<&InventoryService::OnAnimObjectRequest>(this);
}

InventoryService::~InventoryService() = default;

void InventoryService::OnInventoryChanges(const PacketEvent<RequestInventoryChanges>& acMessage) noexcept
{
    auto& message = acMessage.Packet;

    auto view = m_world.view<InventoryComponent>();

    const auto it = view.find(static_cast<entt::entity>(message.ServerId));

    if (it == view.end())
        return;

    bool isRemoteNpcInteraction = false;

    const auto* pOwnerComponent = m_world.try_get<OwnerComponent>(*it);
    if (pOwnerComponent)
    {
        const auto* pOwner = pOwnerComponent->GetOwner();
        if (pOwnerComponent->OwnershipEpoch != message.OwnershipEpoch)
        {
            const uint32_t ownerId = pOwner ? pOwner->GetId() : 0;
            spdlog::debug(
                "Rejected inventory change from player {:X} for actor {:X} at stale epoch {}; current owner is {:X} at epoch {}",
                acMessage.pPlayer->GetId(), message.ServerId, message.OwnershipEpoch, ownerId, pOwnerComponent->OwnershipEpoch);
            return;
        }

        if (pOwner != acMessage.pPlayer)
        {
            const auto* pCharacterComponent = m_world.try_get<CharacterComponent>(*it);
            const auto* pCellComponent = m_world.try_get<CellIdComponent>(*it);
            // A non-owner may still change an NPC's inventory through normal gameplay interactions
            // such as pickpocketing or looting. The epoch and range checks keep the interaction tied
            // to the currently visible incarnation of that NPC.
            isRemoteNpcInteraction = pOwner && pCharacterComponent && pCellComponent && !pCharacterComponent->IsPlayer()
                && acMessage.pPlayer->GetCellComponent().IsInRange(*pCellComponent, pCharacterComponent->IsDragon());

            if (!isRemoteNpcInteraction)
            {
                const uint32_t ownerId = pOwner ? pOwner->GetId() : 0;
                spdlog::debug(
                    "Rejected inventory change from player {:X} for actor {:X} because it is owned by player {:X}", acMessage.pPlayer->GetId(), message.ServerId, ownerId);
                return;
            }
        }
    }
    else if (message.OwnershipEpoch != 0)
    {
        spdlog::warn(
            "Rejected inventory change from player {:X} because object {:X} unexpectedly carried ownership epoch {}",
            acMessage.pPlayer->GetId(), message.ServerId, message.OwnershipEpoch);
        return;
    }

    auto& inventoryComponent = view.get<InventoryComponent>(*it);
    inventoryComponent.Content.AddOrRemoveEntry(message.Item);
    inventoryComponent.HasAuthoritativeMutation = true;

    if (!message.UpdateClients && !isRemoteNpcInteraction)
        return;

    NotifyInventoryChanges notify;
    notify.ServerId = message.ServerId;
    notify.OwnershipEpoch = message.OwnershipEpoch;
    notify.Item = message.Item;

    // SharedDropService creates linked world references. Inventory deltas must
    // never ask another actor to drop an unlinked, independently collectible copy.
    notify.Drop = false;

    const entt::entity cOrigin = static_cast<entt::entity>(message.ServerId);
    // As for equips below: a player character's recorded cell can be stale, so its deltas go to the whole party (Atlas
    // QF-2, W12). Only the owner changes a player character here; NPCs and objects keep the range relay.
    if (const auto* pCharacter = m_world.try_get<CharacterComponent>(cOrigin); pCharacter && pCharacter->IsPlayer())
    {
        GameServer::Get()->SendToParty(notify, acMessage.pPlayer->GetParty(), acMessage.GetSender());
        return;
    }
    if (!GameServer::Get()->SendToPlayersInRange(notify, cOrigin, acMessage.GetSender()))
        spdlog::error("{}: SendToPlayersInRange failed", __FUNCTION__);
}

void InventoryService::OnEquipmentChanges(const PacketEvent<RequestEquipmentChanges>& acMessage) noexcept
{
    auto& message = acMessage.Packet;

    auto view = m_world.view<InventoryComponent>();

    const auto it = view.find(static_cast<entt::entity>(message.ServerId));

    if (it == view.end())
    {
        static uint32_t s_lastMissing{};
        if (s_lastMissing != message.ServerId)
        {
            s_lastMissing = message.ServerId;
            spdlog::warn("Equipment change from player {:X} for unknown actor {:X}: dropped", acMessage.pPlayer->GetId(), message.ServerId);
        }
        return;
    }

    const auto* pOwnerComponent = m_world.try_get<OwnerComponent>(*it);
    if (pOwnerComponent)
    {
        if (pOwnerComponent->GetOwner() != acMessage.pPlayer || pOwnerComponent->OwnershipEpoch != message.OwnershipEpoch)
        {
            const uint32_t ownerId = pOwnerComponent->GetOwner() ? pOwnerComponent->GetOwner()->GetId() : 0;
            spdlog::warn(
                "Rejected equipment change from player {:X} for actor {:X}; current owner is {:X} and requested epoch {} does not match {}",
                acMessage.pPlayer->GetId(), message.ServerId, ownerId, message.OwnershipEpoch, pOwnerComponent->OwnershipEpoch);
            return;
        }
    }
    else if (message.OwnershipEpoch != 0)
    {
        spdlog::warn(
            "Rejected equipment change from player {:X} because object {:X} unexpectedly carried ownership epoch {}",
            acMessage.pPlayer->GetId(), message.ServerId, message.OwnershipEpoch);
        return;
    }

    auto& inventoryComponent = view.get<InventoryComponent>(*it);
    inventoryComponent.Content.UpdateEquipment(message.CurrentInventory);
    inventoryComponent.HasAuthoritativeMutation = true;

    NotifyEquipmentChanges notify;
    notify.ServerId = message.ServerId;
    notify.OwnershipEpoch = message.OwnershipEpoch;
    notify.ItemId = message.ItemId;
    notify.EquipSlotId = message.EquipSlotId;
    notify.Count = message.Count;
    notify.Unequip = message.Unequip;
    notify.IsSpell = message.IsSpell;
    notify.IsShout = message.IsShout;
    notify.HasStack = message.HasStack && message.Stack.BaseId == message.ItemId && message.Stack.Count > 0;
    if (notify.HasStack) notify.Stack = message.Stack;

    const entt::entity cOrigin = static_cast<entt::entity>(message.ServerId);
    // A player's outfit reaches the whole party whatever the recorded cell says (2026-10-03: the follower's equips never
    // reached the host's copy of her while the host's reached hers; the range check used a stale cell for her character).
    if (const auto* pCharacter = m_world.try_get<CharacterComponent>(cOrigin); pCharacter && pCharacter->IsPlayer())
    {
        GameServer::Get()->SendToParty(notify, acMessage.pPlayer->GetParty(), acMessage.GetSender());
        static uint64_t s_lastForwardLog{};
        if (const auto now = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
            now - s_lastForwardLog > 5000)
        {
            s_lastForwardLog = now;
            spdlog::info("Equipment change for player actor {:X} forwarded to the party of player {:X} (party {})", message.ServerId,
                acMessage.pPlayer->GetId(), acMessage.pPlayer->GetParty().JoinedPartyId ? *acMessage.pPlayer->GetParty().JoinedPartyId : 0u);
        }
        return;
    }
    if (!GameServer::Get()->SendToPlayersInRange(notify, cOrigin, acMessage.GetSender()))
        spdlog::error("{}: SendToPlayersInRange failed", __FUNCTION__);
}

void InventoryService::OnAnimObjectRequest(const PacketEvent<AnimObjectRequest>& acMessage) noexcept
{
    const auto& message = acMessage.Packet;
    if (!message.AnimObject || message.Kind >= AnimObjectRequest::kKindCount)
        return;
    auto characterView = m_world.view<CharacterComponent, OwnerComponent>();
    const auto it = characterView.find(static_cast<entt::entity>(message.Id));
    if (it == std::end(characterView) || characterView.get<OwnerComponent>(*it).GetOwner() != acMessage.pPlayer)
        return;
    NotifyAnimObject notify{};
    notify.Id = message.Id;
    notify.Kind = message.Kind;
    notify.AnimObject = message.AnimObject;
    GameServer::Get()->SendToPlayersInRange(notify, *it, acMessage.pPlayer);
}

void InventoryService::OnWeaponDrawnRequest(const PacketEvent<DrawWeaponRequest>& acMessage) noexcept
{
    auto& message = acMessage.Packet;

    auto characterView = m_world.view<CharacterComponent, OwnerComponent>();
    const auto it = characterView.find(static_cast<entt::entity>(message.Id));

    if (it != std::end(characterView) && characterView.get<OwnerComponent>(*it).GetOwner() == acMessage.pPlayer)
    {
        auto& characterComponent = characterView.get<CharacterComponent>(*it);
        characterComponent.SetWeaponDrawn(message.IsWeaponDrawn);
        spdlog::debug("Updating weapon drawn state {:x}:{}", message.Id, message.IsWeaponDrawn);
        // Relay it: the state used to be stored only, so copies learned a draw only at spawn (owner report: archers
        // held their bows on the host while the follower's copies kept them on their backs).
        NotifyDrawWeapon notify{};
        notify.Id = message.Id;
        notify.IsWeaponDrawn = message.IsWeaponDrawn;
        GameServer::Get()->SendToPlayersInRange(notify, *it, acMessage.pPlayer);
    }
}
