#pragma once

#include <Games/Events.h>
#include <Messages/RequestSharedDrop.h>
#include <Messages/NotifySharedDrop.h>
#include <mutex>
#include <map>
#include <set>

struct World;
struct TransportService;
struct UpdateEvent;
struct DisconnectedEvent;

struct SharedDropService : BSTEventSink<TESContainerChangedEvent>, BSTEventSink<TESLoadGameEvent>
{
    SharedDropService(World&, entt::dispatcher&, TransportService&) noexcept;
    ~SharedDropService() noexcept;
    bool TracksPlayerDrops() const noexcept;
    bool TryHold(TESObjectREFR*, TESObjectREFR*) noexcept;
    // Called at ObjectService's existing main-frame boundary. Native references and
    // inventory are never created, destroyed, or picked up by the network worker.
    void OnMainFrame() noexcept;
    bool IsShared(uint32_t aFormId) const noexcept;
    bool IsOwner(uint32_t aFormId) const noexcept;
    uint32_t ResolvePhysics(const GameId&) const noexcept;
    GameId PhysicsId(uint32_t aFormId) const noexcept;
    uint32_t PhysicsGeneration(uint32_t aFormId) const noexcept;
    std::vector<NotifySharedDrop> TakePhysics() noexcept;
    std::vector<uint32_t> OwnedReferences() const noexcept;
    bool HasRemoteReferences() const noexcept;
    void SendPhysics(uint32_t aFormId, const PhysicsReferenceUpdate&, uint64_t aTick) noexcept;
    BSTEventResult OnEvent(const TESContainerChangedEvent*, const EventDispatcher<TESContainerChangedEvent>*) override;
    BSTEventResult OnEvent(const TESLoadGameEvent*, const EventDispatcher<TESLoadGameEvent>*) override;

private:
    struct Copy
    {
        SharedDropData Data;
        uint32_t FormId{};
        uint64_t Pending{}, Deadline{};
        bool Ready{}, Grant{}, Terminal{};
    };
    struct NativeDrop { uint32_t Reference{}, Base{}; int32_t Count{}; uint64_t Token{}; uint32_t Dropper{}; };
    TESObjectREFR* Spawn(const SharedDropData&, bool aForPickup = false) noexcept;
    void RemoveCopy(Copy&, bool aKeep) noexcept;
    void Queue(const SharedDropData&, SharedDropAction, uint64_t aToken = 0) noexcept;
    void OnUpdate(const UpdateEvent&) noexcept;
    void OnNotify(const NotifySharedDrop&) noexcept;
    void OnDisconnected(const DisconnectedEvent&) noexcept;
    void Handle(const NotifySharedDrop&) noexcept;
    bool Loading() const noexcept;
    World& m_world;
    TransportService& m_transport;
    mutable std::recursive_mutex m_lock;
    std::map<uint32_t, Copy> m_copies;
    std::map<uint64_t, uint32_t> m_origins;
    std::set<uint32_t> m_granted;
    std::vector<NativeDrop> m_nativeDrops;
    std::vector<RequestSharedDrop> m_outgoing;
    std::vector<NotifySharedDrop> m_incoming;
    std::vector<NotifySharedDrop> m_physics;
    uint64_t m_nextToken{1}, m_nextSnapshot{}, m_epoch{};
    uint32_t m_localPlayer{};
    bool m_disconnected{}, m_loaded{};
    entt::scoped_connection m_updateConnection, m_notifyConnection, m_disconnectConnection;
};
