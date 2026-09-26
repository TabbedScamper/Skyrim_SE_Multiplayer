#pragma once

#include <World.h>
#include <Events/EventDispatcher.h>
#include <Games/Events.h>
#include <Messages/NotifyQuestAliasFills.h>
#include <atomic>

struct NotifyQuestUpdate;
struct RequestQuestUpdate;
struct UpdateEvent;
struct DisconnectedEvent;

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
    // Stages the leader has entered in the current authority epoch (from its quest updates).
    // A follower's own native stage write for one of them is the same stage arriving late.
    static bool HostReachedStage(uint32_t aFormId, uint16_t aStage, uint64_t aEpoch) noexcept;
    [[nodiscard]] Vector<DebugEvent> GetRecentDebugEvents() const;
    bool PrepareAliasesForStage(TESQuest* apQuest) noexcept;

private:
    friend struct QuestEventHandler;

    void OnConnected(const ConnectedEvent&) noexcept;

    BSTEventResult OnEvent(const TESQuestStartStopEvent*, const EventDispatcher<TESQuestStartStopEvent>*) override;
    BSTEventResult OnEvent(const TESQuestStageEvent*, const EventDispatcher<TESQuestStageEvent>*) override;

    void OnQuestUpdate(const NotifyQuestUpdate&) noexcept;
    void OnAliasFills(const NotifyQuestAliasFills&) noexcept;
    void OnUpdate(const UpdateEvent&) noexcept;
    void OnDisconnected(const DisconnectedEvent&) noexcept;
    bool RefreshAliasSession() noexcept;
    bool SendAliasFills(TESQuest* apQuest, const RequestQuestUpdate* apUpdate = nullptr) noexcept;
    void ApplyPendingAliasFills() noexcept;
    bool ResolveAliasFills(TESQuest* apQuest, const QuestAliasFills& aFills) noexcept;
    struct LocalAliasFill
    {
        uint32_t AliasId{};
        uint32_t FormId{};
    };
    bool ApplyAliasFills(TESQuest* apQuest, const Vector<LocalAliasFill>& aFills) noexcept;
    void RecordDebugEvent(const char* acKind, uint32_t aFormId, uint16_t aStage,
        bool aScopedOverride) noexcept;

    World& m_world;

    entt::scoped_connection m_joinedConnection;
    entt::scoped_connection m_leftConnection;
    entt::scoped_connection m_questUpdateConnection;
    entt::scoped_connection m_aliasFillsConnection;
    entt::scoped_connection m_updateConnection;
    entt::scoped_connection m_disconnectConnection;
    Map<uint32_t, Vector<QuestAliasFill>> m_sentAliasFills;
    std::deque<NotifyQuestAliasFills> m_pendingAliasFills;
    struct AliasState
    {
        QuestAliasFills Fills;
        Vector<LocalAliasFill> LocalFills;
        bool Ready{};
    };
    std::mutex m_aliasMutex;
    Map<uint32_t, AliasState> m_leaderAliasFills;
    uint64_t m_aliasEpoch{};
    uint32_t m_aliasLeader{};
    Vector<uint32_t> m_aliasMembers;
    uint64_t m_lastAliasSequence{};
    uint64_t m_nextAliasSample{};
    uint64_t m_nextAliasRetry{};
    uint64_t m_aliasRetryAfter{};
    size_t m_aliasQuestCursor{};
    bool m_aliasSession{};
    std::atomic_bool m_aliasOverflow{};
    mutable std::mutex m_debugEventMutex;
    std::deque<DebugEvent> m_debugEvents;
    uint64_t m_debugEventSequence{};
    uint64_t m_appliedQuestEpoch{};
    Map<uint32_t, uint64_t> m_appliedQuestRevisions;
};
