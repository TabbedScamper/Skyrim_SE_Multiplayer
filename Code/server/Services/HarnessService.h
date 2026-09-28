#pragma once
#include <Events/PacketEvent.h>
#include <Messages/HarnessBarrier.h>
#include <map>
struct World;
struct UpdateEvent;
struct HarnessService
{
    HarnessService(World&, entt::dispatcher&);
private:
    struct Run { HarnessBarrier Barrier; HarnessData Step; uint64_t Deadline{}; bool Finished{}; };
    void OnRequest(const PacketEvent<RequestHarness>&) noexcept;
    void OnUpdate(const UpdateEvent&) noexcept;
    void Broadcast(const Run&, HarnessOp, const String& = {}) const;
    World& m_world;
    std::map<uint32_t, Run> m_runs;
    entt::scoped_connection m_request, m_update;
};
