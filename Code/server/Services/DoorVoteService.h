#pragma once

#include <Events/PacketEvent.h>
#include <Messages/DoorVoteData.h>
#include <map>
#include <set>

struct World;
struct Player;
struct UpdateEvent;
struct DoorVoteRequest;

struct DoorVoteService
{
    DoorVoteService(World& aWorld, entt::dispatcher& aDispatcher) noexcept;

private:
    struct Vote
    {
        DoorVoteData Data;
        uint32_t Initiator{};
        uint32_t Leader{};
        String InitiatorName;
        std::set<uint32_t> Members;
        std::set<uint32_t> Ready;
        std::set<uint32_t> Loaded;
        uint64_t Deadline{};
    };

    void OnRequest(const PacketEvent<DoorVoteRequest>& aEvent) noexcept;
    void OnUpdate(const UpdateEvent&) noexcept;
    bool IsNear(Player* aPlayer, const DoorVoteData& aData, float aRadius) const noexcept;
    bool IsCurrent(uint32_t aPartyId, const Vote& aVote) const noexcept;
    void Broadcast(Vote& aVote, DoorVoteAction aAction, const String& aNotice) const noexcept;
    void Cancel(Vote& aVote, const char* aReason) const noexcept;
    void Changed(Vote& aVote, bool aExtendDeadline = true) noexcept;

    World& m_world;
    std::map<uint32_t, Vote> m_votes;
    uint64_t m_nextVoteId{};
    entt::scoped_connection m_requestConnection;
    entt::scoped_connection m_updateConnection;
};
