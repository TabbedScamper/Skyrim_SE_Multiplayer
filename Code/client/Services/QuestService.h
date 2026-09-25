#pragma once

#include <World.h>
#include <Events/EventDispatcher.h>
#include <Games/Events.h>

struct NotifyQuestUpdate;

struct TESQuest;

/**
 * @brief Handles quest sync
 *
 * This service is currently not in use.
 */
class QuestService final : public BSTEventSink<TESQuestStartStopEvent>, BSTEventSink<TESQuestStageEvent>
{
public:
    struct DebugEvent
    {
        uint64_t Sequence{};
        uint64_t TimeMs{};
        uint32_t FormId{};
        uint16_t Stage{};
        String Kind{};
        bool ScopedOverride{};
        bool InParty{};
        bool Leader{};
    };

    QuestService(World&, entt::dispatcher&);
    ~QuestService() = default;

    static bool IsNonSyncableQuest(TESQuest* apQuest);
    static void DebugDumpQuests();
    static bool StopQuest(uint32_t aformId);
    [[nodiscard]] Vector<DebugEvent> GetRecentDebugEvents() const;

private:
    friend struct QuestEventHandler;

    void OnConnected(const ConnectedEvent&) noexcept;

    BSTEventResult OnEvent(const TESQuestStartStopEvent*, const EventDispatcher<TESQuestStartStopEvent>*) override;
    BSTEventResult OnEvent(const TESQuestStageEvent*, const EventDispatcher<TESQuestStageEvent>*) override;

    void OnQuestUpdate(const NotifyQuestUpdate&) noexcept;
    void RecordDebugEvent(const char* acKind, uint32_t aFormId, uint16_t aStage,
        bool aScopedOverride) noexcept;

    World& m_world;

    entt::scoped_connection m_joinedConnection;
    entt::scoped_connection m_leftConnection;
    entt::scoped_connection m_questUpdateConnection;
    mutable std::mutex m_debugEventMutex;
    std::deque<DebugEvent> m_debugEvents;
    uint64_t m_debugEventSequence{};
    uint64_t m_appliedQuestEpoch{};
    Map<uint32_t, uint64_t> m_appliedQuestRevisions;
};
