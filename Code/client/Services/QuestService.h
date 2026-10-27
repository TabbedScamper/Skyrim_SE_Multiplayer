#pragma once

#include <World.h>
#include <Events/EventDispatcher.h>
#include <Games/Events.h>
#include <Messages/NotifyQuestAliasFills.h>
#include <Messages/QuestAliasFills.h>
#include <Forms/TESQuest.h>
#include <atomic>
#include <map>

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
    void OnMainFrame() noexcept;
    static bool RunStage(TESQuest::Stage* apStage, TESQuest* apQuest, bool aForce,
        uint8_t aKind, bool (*apOriginal)(TESQuest::Stage*, TESQuest*, bool)) noexcept;
    static void MarkQuestStateDirty(uint32_t aFormId) noexcept;
    static void CaptureQuestReset(TESQuest* apQuest) noexcept;
    static void ApplyStageItem(void* apItem, bool aForce, void (*apOriginal)(void*, bool)) noexcept;
    static bool GetHostMask(uint32_t aFormId, uint16_t aStage,
        uint32_t& aMask, uint8_t& aCount, bool& aOverflow) noexcept;
    static uint32_t LocalRunCount() noexcept;
    static bool ApplyAsFollower() noexcept;
    struct StageContext
    {
        StageContext(uint32_t aFormId, uint16_t aStage, uint32_t aMask,
            uint8_t aCount, bool aOverflow, bool aStateOnly = false) noexcept;
        ~StageContext() noexcept;
        uint32_t PreviousForm{};
        uint16_t PreviousStage{};
        uint32_t PreviousMask{};
        uint8_t PreviousCount{};
        bool PreviousOverflow{};
        bool PreviousActive{};
        bool PreviousStateOnly{};
    };

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
    bool SendAliasFills(TESQuest* apQuest, const RequestQuestUpdate* apUpdate = nullptr,
        const QuestAliasFills* apRun = nullptr) noexcept;
    void PushRecord(uint32_t aFormId, QuestAliasFills aRecord) noexcept;
    void CaptureState(TESQuest* apQuest, uint8_t aKind) noexcept;
    void ApplyPendingAliasFills() noexcept;
    bool ResolveAliasFills(TESQuest* apQuest, const QuestAliasFills& aFills) noexcept;
    struct LocalAliasFill
    {
        uint32_t AliasId{};
        uint32_t FormId{};
        bool Required{};
        bool ClearRequired{};
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
    std::mutex m_runMutex;
    std::deque<QuestAliasFills> m_runQueue;
    std::unordered_set<uint32_t> m_dirtyQuests;
    std::unordered_map<uint32_t, uint32_t> m_questSeq;
    std::unordered_map<uint64_t, uint16_t> m_runCounts;
    struct FollowerMask { uint32_t Mask{}; uint8_t Count{}; bool Overflow{}; };
    std::unordered_map<uint64_t, FollowerMask> m_hostMasks;
    std::unordered_map<uint32_t, std::map<uint32_t, NotifyQuestAliasFills>> m_pendingRuns;
    std::deque<uint32_t> m_pendingQuestOrder;
    size_t m_pendingRunCount{};
    std::unordered_map<uint32_t, uint32_t> m_appliedSeq;
    std::unordered_map<uint32_t, uint64_t> m_waitSince;
    std::unordered_map<uint64_t, uint32_t> m_localMasks;
    std::unordered_map<uint32_t, uint8_t> m_startAttempts;
    std::atomic_bool m_captureLeader{};
    std::atomic_bool m_applyFollower{};
    std::atomic_uint64_t m_captureEpoch{};
    std::atomic_uint8_t m_captureSession{};
    Map<uint32_t, Map<uint32_t, uint8_t>> m_requiredEmptySamples;
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
