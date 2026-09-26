#pragma once

#include <Games/Events.h>
#include <Messages/RequestQuestItems.h>
#include <Messages/NotifyQuestItems.h>
#include <map>
#include <vector>

struct World;
struct TransportService;
struct UpdateEvent;
struct DisconnectedEvent;
struct InventoryChangeEvent;
struct NotifyInventoryChanges;

struct QuestItemService : BSTEventSink<TESLoadGameEvent>
{
    QuestItemService(World& aWorld, entt::dispatcher& aDispatcher, TransportService& aTransport) noexcept;
    ~QuestItemService() noexcept;
    BSTEventResult OnEvent(const TESLoadGameEvent*, const EventDispatcher<TESLoadGameEvent>*) override;
    void Discover() noexcept;
    void ScriptRemoved(uint32_t aBaseId) noexcept;
    std::vector<std::pair<uint32_t, int64_t>> CaptureCounts() const;
    void AliasReleased(uint32_t aQuestId, uint32_t aInstance, uint32_t aAliasId, uint32_t aOldReference) noexcept;
    void QuestCompleted(uint32_t aQuestId, uint32_t aInstance) noexcept;
    bool HandleInventoryNotify(const NotifyInventoryChanges& aMessage) noexcept;
    bool Ready() const noexcept;

private:
    void OnUpdate(const UpdateEvent&) noexcept;
    void OnDisconnected(const DisconnectedEvent&) noexcept;
    void OnInventory(const InventoryChangeEvent&) noexcept;
    void OnNotify(const NotifyQuestItems&) noexcept;
    void Reset() noexcept;
    void Reconcile() noexcept;
    void Send(QuestItemState aItem, QuestItemAction aAction) noexcept;
    void Merge(const QuestItemState& aItem) noexcept;
    bool Retiring(const QuestItemState& aItem) const noexcept;

    struct Pending { RequestQuestItems Request; uint64_t RetryAt{}; };
    World& m_world;
    TransportService& m_transport;
    std::vector<QuestItemState> m_items, m_snapshot, m_retiring;
    std::vector<Pending> m_pending;
    std::map<uint32_t, uint64_t> m_referenceRetry;
    std::map<uint32_t, uint32_t> m_releasedReferences;
    uint64_t m_epoch{}, m_snapshotToken{}, m_nextSnapshot{}, m_nextScan{};
    uint32_t m_leader{}, m_page{};
    bool m_ready{}, m_applying{}, m_loading{};
    entt::scoped_connection m_updateConnection, m_disconnectConnection, m_inventoryConnection, m_notifyConnection;
};
