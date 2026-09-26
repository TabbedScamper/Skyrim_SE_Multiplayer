#pragma once
#include <Events/PacketEvent.h>
#include <Messages/NotifyDialogueListen.h>
#include <unordered_map>
#include <unordered_set>

struct World;
struct Player;
struct UpdateEvent;
struct RequestDialogueListen;

struct DialogueListenService
{
    DialogueListenService(World&, entt::dispatcher&) noexcept;
private:
    struct Stream
    {
        uint32_t Party{};
        NotifyDialogueListen Message;
        std::unordered_set<uint32_t> Recipients;
    };
    void OnRequest(const PacketEvent<RequestDialogueListen>&) noexcept;
    void OnUpdate(const UpdateEvent&) noexcept;
    bool Near(Player*, uint32_t aNpc, float aRadius) const noexcept;
    bool Current(const Stream&) const noexcept;
    void Broadcast(Stream&, bool aChanged) noexcept;
    void Close(Stream&) noexcept;
    World& m_world;
    std::unordered_map<uint32_t, Stream> m_streams;
    // Retain revisions after close so delayed active packets cannot resurrect a stream.
    std::unordered_map<uint32_t, std::pair<uint64_t, uint64_t>> m_lastRevision;
    uint64_t m_nextUpdate{};
    entt::scoped_connection m_requestConnection;
    entt::scoped_connection m_updateConnection;
};
