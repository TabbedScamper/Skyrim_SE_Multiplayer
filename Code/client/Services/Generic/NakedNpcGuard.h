#pragma once

#include <Structs/Inventory.h>
#include <Messages/NotifyInventoryChanges.h>
#include <Messages/NotifyEquipmentChanges.h>
#include <atomic>
#include <mutex>
#include <unordered_map>
#include <variant>

struct World;
struct AssignCharacterResponse;
struct CharacterSpawnRequest;
struct NotifyNpcWorn;
struct NotifyOwnershipTransfer;
struct EquipmentChangeEvent;
struct InventoryChangeEvent;
struct DisconnectedEvent;

struct NakedNpcGuard
{
    NakedNpcGuard(World&, entt::dispatcher&) noexcept;
    void Update() noexcept;
    void UpdateNative() noexcept;
    void Reset() noexcept;
    void WornSnapshot(uint32_t, uint32_t, const Inventory&) noexcept;
    // Route remote NPC deltas through one ordered main-thread queue, before
    // quest-item interception. Returns false for players and local inventory.
    bool Defer(const NotifyInventoryChanges&) noexcept;
    bool Defer(const NotifyEquipmentChanges&) noexcept;

private:
    struct Target { uint32_t Form{}, Id{}, Epoch{}; bool Local{}; };
    struct Snapshot { uint32_t Id{}, Epoch{}; uint64_t Sequence{}; Inventory Worn; bool Contents{}; };
    struct Transfer { uint32_t Id{}, Epoch{}; };
    struct StockChange { NotifyInventoryChanges Message; int64_t OwnerCount{}; bool Known{}; };
    using Change = std::variant<StockChange, NotifyEquipmentChanges>;
    using Input = std::variant<Snapshot, Transfer, NotifyInventoryChanges, NotifyEquipmentChanges>;
    struct State
    {
        uint32_t Epoch{};
        uint64_t Sequence{}, SettleUntil{}, NextCheck{}, LastSeen{}, RootSince{};
        const void* Root{}; // Comparison only; reacquire live objects each frame.
        Inventory Worn;
        // Absolute owner stock plus ordered deltas disambiguate a delayed
        // delivery from a genuinely new second item. No native pointers retained.
        Inventory OwnerStock;
        Inventory FailedSupply; // Stop repeating an addition the native reader could not identify.
        Vector<GameId> FormOnly;
        // Weapons the owner's worn list holds in a hand (ExtraWorn right, ExtraWornLeft left). Equipped only from
        // this copy's own stock, like armor; never minted.
        Inventory Weapons;
        uint64_t WeaponLoggedSequence{};
        Vector<Change> Changes;
        bool Complete{}, StockKnown{}, Unmapped{}, Observe{}, Held{}, LoggedWait{};
        uint8_t Life{0xff};
        // Worn sequence held when this copy died, and until when to wait for the owner's post-death list.
        uint64_t DeathSequence{}, DeathHoldUntil{};
    };
    bool RemoteNpc(uint32_t, uint32_t) const noexcept;
    void Push(Input) noexcept;
    void OnAssign(const AssignCharacterResponse&) noexcept;
    void OnSpawn(const CharacterSpawnRequest&) noexcept;
    void OnWorn(const NotifyNpcWorn&) noexcept;
    void OnTransfer(const NotifyOwnershipTransfer&) noexcept;
    void OnDisconnected(const DisconnectedEvent&) noexcept;
    void ApplyInput(const Input&, uint64_t) noexcept;

    World& m_world;
    // Only value transfer uses this lock. Never held across native calls.
    std::mutex m_lock;
    Vector<Input> m_input;
    Vector<Target> m_targets;
    bool m_active{};
    uint64_t m_session{}, m_nextTargets{};
    std::atomic_uint64_t m_generation{1};
    // The remaining fields belong exclusively to HookMainLoop.
    uint64_t m_nativeGeneration{}, m_nextHeartbeat{};
    uint32_t m_checkedActors{};
    size_t m_cursor{};
    std::unordered_map<uint32_t, State> m_states;
    entt::scoped_connection m_assign, m_spawn, m_worn, m_transfer;
    entt::scoped_connection m_disconnect;
};
