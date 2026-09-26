#pragma once

#include <Events/PacketEvent.h>
#include <Messages/QuestItemState.h>
#include <optional>

struct World;
struct RequestQuestItems;
struct Player;

struct QuestItemService
{
    QuestItemService(World& aWorld, entt::dispatcher& aDispatcher);
    void OnRequest(const PacketEvent<RequestQuestItems>& aEvent) noexcept;

private:
    void SendSnapshot(Player* aPlayer, uint64_t aEpoch, uint64_t aToken) const;
    World& m_world;
    Vector<QuestItemState> m_items;
    // One configured ledger is one campaign, like the existing quest journal.
    // A second live party must not mutate or receive the first party's inventory.
    std::optional<uint32_t> m_partyId;
    entt::scoped_connection m_requestConnection;
};
