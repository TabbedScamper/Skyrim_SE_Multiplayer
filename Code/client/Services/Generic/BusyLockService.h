#pragma once

#include <Games/Events.h>
#include <Messages/BusyLockData.h>
#include <atomic>

struct World;
struct TransportService;
struct UpdateEvent;
struct DisconnectedEvent;
struct NotifyBusyLock;
struct MenuOpenCloseEvent;

struct BusyLockService : BSTEventSink<MenuOpenCloseEvent>, BSTEventSink<TESLoadGameEvent>, BSTEventSink<TESDeathEvent>
{
    BusyLockService(World& aWorld, entt::dispatcher& aDispatcher, TransportService& aTransport) noexcept;
    ~BusyLockService() noexcept;

    bool TryHold(TESObjectREFR* aReference, TESObjectREFR* aActivator, uint8_t aUnk1,
        TESBoundObject* aObject, int32_t aCount, char aDefaultProcessing, const void* aCaller) noexcept;
    bool TryHoldMenu(TESObjectREFR* aReference, bool aBarter, int32_t aMode) noexcept;
    BSTEventResult OnEvent(const MenuOpenCloseEvent*, const EventDispatcher<MenuOpenCloseEvent>*) override;
    BSTEventResult OnEvent(const TESLoadGameEvent*, const EventDispatcher<TESLoadGameEvent>*) override;
    BSTEventResult OnEvent(const TESDeathEvent*, const EventDispatcher<TESDeathEvent>*) override;

private:
    enum class Operation : uint8_t { Activate, Container, Barter };
    struct Activation
    {
        Operation Type{Operation::Activate};
        uint32_t Handle{}, Cell{}, WorldSpace{}, Object{};
        int32_t Count{}, Mode{};
        uint8_t Unk1{};
        char DefaultProcessing{};
    };

    bool Begin(TESObjectREFR* aReference, const Activation& aActivation, BusyLockKind aKind) noexcept;
    bool GetReferenceId(TESObjectREFR* aReference, GameId& aId) noexcept;
    bool Ready() const noexcept;
    bool CanReplay(TESObjectREFR* aReference) noexcept;
    uint32_t OpenMenus(TESObjectREFR* aReference) const noexcept;
    void Replay() noexcept;
    void Reset(BusyLockReason aReason) noexcept;
    void Send(const BusyLockData& aData, BusyLockAction aAction, BusyLockReason aReason = BusyLockReason::Closed) noexcept;
    void OnUpdate(const UpdateEvent&) noexcept;
    void OnDisconnected(const DisconnectedEvent&) noexcept;
    void OnNotify(const NotifyBusyLock& aMessage) noexcept;

    World& m_world;
    TransportService& m_transport;
    BusyLockData m_request;
    Activation m_activation;
    uint64_t m_nextRequest{}, m_deadline{}, m_openDeadline{}, m_nextHeartbeat{}, m_closedAt{};
    uint32_t m_lifecycle{};
    bool m_waiting{}, m_leased{}, m_seenMenu{};
    EventDispatcher<MenuOpenCloseEvent>* m_menuSource{};
    std::atomic_bool m_acceptMenuRequests{};
    std::atomic_uint32_t m_invalidations{}, m_menuEvents{};
    std::atomic<BusyLockReason> m_invalidationReason{BusyLockReason::Load};
    entt::scoped_connection m_updateConnection, m_disconnectConnection, m_notifyConnection;
};
