#pragma once

#include <Events/PacketEvent.h>

struct World;
struct RequestSaidOnce;

class SaidOnceService
{
public:
    SaidOnceService(World& aWorld, entt::dispatcher& aDispatcher) noexcept;

private:
    void OnRequest(const PacketEvent<RequestSaidOnce>& aEvent) noexcept;

    World& m_world;
    entt::scoped_connection m_requestConnection;
};
