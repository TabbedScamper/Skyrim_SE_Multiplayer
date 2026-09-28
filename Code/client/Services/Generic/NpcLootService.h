#pragma once

#include <Messages/NpcInventory.h>
#include <Games/Events.h>
#include <atomic>
#include <array>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

struct World;
struct Actor;
struct UpdateEvent;
struct DisconnectedEvent;
struct EquipmentChangeEvent;
struct InventoryChangeEvent;
struct NotifyNpcWorn;
struct NotifyOwnershipTransfer;

// Worn observation and existing-stock guard only. Lazy contents, render-copy
// creation and menu interception are disabled pending transaction recovery and
// native count isolation. Menus and contents retain the legacy path.
struct NpcLootService : BSTEventSink<TESLoadGameEvent>
{
    NpcLootService(World&, entt::dispatcher&) noexcept;
    ~NpcLootService() noexcept;
    BSTEventResult OnEvent(const TESLoadGameEvent*, const EventDispatcher<TESLoadGameEvent>*) override;
    static void MarkMainThread() noexcept;
    static bool IsMainThread() noexcept;
    void OnMainFrame() noexcept;
    static void TraceWornModels(Actor*, uint32_t aSlots, const char* aPhase, uint64_t aSequence = 0) noexcept;

private:
    struct State
    {
        NpcWornData Data;
        uint32_t ObservedEpoch{}, Cell{};
        uint32_t BodyProofForm{};
        uint8_t WaitReason{};
        uint64_t NextQuery{}, NextCapture{}, LastSeen{}, NextObserve{};
        const void* Root{}; // Comparison only, never dereferenced across frames.
        bool Pending{}, GuardSeeded{};
    };
    bool Worn(Actor*, Vector<NpcWornItem>&) const noexcept;
    void OnUpdate(const UpdateEvent&) noexcept;
    void OnWorn(const NotifyNpcWorn&) noexcept;
    void OnEquipment(const EquipmentChangeEvent&) noexcept;
    void OnInventory(const InventoryChangeEvent&) noexcept;
    void OnDisconnected(const DisconnectedEvent&) noexcept;
    void OnOwnership(const NotifyOwnershipTransfer&) noexcept;
    void OnTargetChanged(entt::registry&, entt::entity) noexcept;
    void Publish(Actor*, uint32_t, uint32_t, uint64_t) noexcept;
    void Observe(Actor*, State&, uint64_t) noexcept;

    World& m_world;
    std::unordered_map<uint32_t, State> m_received, m_sent;
    std::unordered_set<uint32_t> m_dirty;
    std::recursive_mutex m_lock;
    struct Target { uint32_t Form{}, Id{}, Epoch{}; bool Local{}; };
    Vector<Target> m_targets;
    Vector<NpcWornData> m_outgoing;
    bool m_active{};
    uint64_t m_scan{}, m_session{}, m_nextTargets{};
    std::atomic_bool m_loaded{};
    std::atomic_bool m_targetsDirty{true};
    std::array<entt::scoped_connection, 15> m_targetConnections;
    entt::scoped_connection m_update, m_worn, m_equipment, m_inventory, m_disconnect, m_ownership;
};
