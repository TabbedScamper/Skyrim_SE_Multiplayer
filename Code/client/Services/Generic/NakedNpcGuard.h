#pragma once

#include <Structs/Inventory.h>
#include <unordered_map>

struct World;
struct AssignCharacterResponse;
struct CharacterSpawnRequest;
struct NotifyOwnershipTransfer;
struct NotifyEquipmentChanges;
struct NotifyInventoryChanges;
struct EquipmentChangeEvent;
struct InventoryChangeEvent;
struct DisconnectedEvent;

struct NakedNpcGuard
{
    NakedNpcGuard(World& aWorld, entt::dispatcher& aDispatcher) noexcept;
    // Run after InventoryService has replayed its held death-time changes.
    void Update() noexcept;

private:
    struct WornSet
    {
        uint32_t Epoch{};
        bool Known{}; // A known empty set means intentionally unequipped.
        Inventory Worn;
        Vector<GameId> FormOnly; // Equipment deltas do not identify an extra-data instance.
        uint64_t SettleUntil{};
        uint64_t LastSeen{};
    };
    struct Copy
    {
        entt::entity Entity{entt::null};
        uint32_t ServerId{};
        uint32_t Epoch{};
        uint64_t NextCheck{};
        uint64_t LastSeen{};
    };

    void Remember(uint32_t aServerId, uint32_t aEpoch, const Inventory& acInventory) noexcept;
    void Equipment(uint32_t aServerId, uint32_t aEpoch, GameId aItem, bool aUnequip) noexcept;
    void Removed(uint32_t aServerId, uint32_t aEpoch, const Inventory::Entry& acItem) noexcept;
    void OnAssign(const AssignCharacterResponse& acMessage) noexcept;
    void OnSpawn(const CharacterSpawnRequest& acMessage) noexcept;
    void OnTransfer(const NotifyOwnershipTransfer& acMessage) noexcept;
    void OnEquipment(const NotifyEquipmentChanges& acMessage) noexcept;
    void OnInventory(const NotifyInventoryChanges& acMessage) noexcept;
    void OnLocalEquipment(const EquipmentChangeEvent& acEvent) noexcept;
    void OnLocalInventory(const InventoryChangeEvent& acEvent) noexcept;
    void OnDisconnected(const DisconnectedEvent&) noexcept;

    World& m_world;
    std::unordered_map<uint32_t, WornSet> m_wornSets;
    std::unordered_map<uint32_t, Copy> m_copies;
    uint64_t m_nextScan{};
    entt::scoped_connection m_assignConnection;
    entt::scoped_connection m_spawnConnection;
    entt::scoped_connection m_transferConnection;
    entt::scoped_connection m_equipmentConnection;
    entt::scoped_connection m_inventoryConnection;
    entt::scoped_connection m_localEquipmentConnection;
    entt::scoped_connection m_localInventoryConnection;
    entt::scoped_connection m_disconnectConnection;
};
