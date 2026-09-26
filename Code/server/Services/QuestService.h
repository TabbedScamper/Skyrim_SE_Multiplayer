#pragma once

#include <Events/PacketEvent.h>
#include <Structs/GameId.h>

struct World;
struct UpdateEvent;
struct RequestQuestUpdate;
struct RequestQuestAliasFills;
struct QuestAliasFills;

/**
 * @brief Dispatch quest sync messages.
 *
 * This service is currently not in use.
 */
class QuestService
{
public:
    QuestService(World& aWorld, entt::dispatcher& aDispatcher);

private:
    void OnQuestChanges(const PacketEvent<RequestQuestUpdate>& aChanges) noexcept;
    void OnAliasFills(const PacketEvent<RequestQuestAliasFills>& aMessage) noexcept;
    bool ProcessQuestChanges(const PacketEvent<RequestQuestUpdate>& aChanges, const QuestAliasFills* apFills) noexcept;

    World& m_world;

    entt::scoped_connection m_questUpdateConnection;
    entt::scoped_connection m_aliasFillsConnection;
    struct AliasSource
    {
        uint32_t Leader{};
        uint64_t Epoch{};
        uint64_t Sequence{};
    };
    Map<uint32_t, AliasSource> m_aliasSources;
    entt::scoped_connection m_updateConnection;
    entt::scoped_connection m_joinConnection;
};
