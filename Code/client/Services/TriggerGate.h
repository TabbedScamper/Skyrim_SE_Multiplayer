#pragma once

#include <Games/Events.h>
#include <memory>

struct World;
struct TransportService;
struct UpdateEvent;
struct NotifyPartyUnstuck;

struct TriggerGate : BSTEventSink<TESLoadGameEvent>
{
    TriggerGate(World& aWorld, entt::dispatcher& aDispatcher, TransportService& aTransport) noexcept;
    ~TriggerGate() noexcept;

    TP_NOCOPYMOVE(TriggerGate);

    // Called only at SkyrimVM's trigger sinks, before direct and alias delivery.
    bool Hold(uint8_t aKind, TESObjectREFR* apTrigger, TESObjectREFR* apActor) noexcept;
    BSTEventResult OnEvent(const TESLoadGameEvent*, const EventDispatcher<TESLoadGameEvent>*) override;

private:
    void OnUpdate(const UpdateEvent&) noexcept;
    void OnUnstuck(const NotifyPartyUnstuck& acMessage) noexcept;
    void RequestUnstuck() noexcept;

    struct State;
    std::unique_ptr<State> m_state;
    World& m_world;
    TransportService& m_transport;
    entt::scoped_connection m_updateConnection;
    entt::scoped_connection m_unstuckConnection;
};
