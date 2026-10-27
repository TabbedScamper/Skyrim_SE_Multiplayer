#pragma once

#include <cstdint>

struct World;
struct TransportService;
struct NotifySaidOnce;

class SaidOnceService
{
public:
    SaidOnceService(World& aWorld, entt::dispatcher& aDispatcher, TransportService& aTransport) noexcept;
    void OnMainFrame() noexcept;

private:
    void OnNotify(const NotifySaidOnce& aMessage) noexcept;

    World& m_world;
    TransportService& m_transport;
    entt::scoped_connection m_notifyConnection;
};
