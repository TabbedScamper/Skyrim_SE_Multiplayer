#include <Services/SceneTimelineService.h>

#include <World.h>
#include <Services/PartyService.h>
#include <Services/TransportService.h>

#include <Events/UpdateEvent.h>
#include <Events/DisconnectedEvent.h>
#include <Messages/SceneTimelineRequest.h>
#include <Messages/NotifySceneTimeline.h>

#include <Games/TES.h>
#include <Forms/TESQuest.h>

SceneTimelineService::SceneTimelineService(World& aWorld, entt::dispatcher& aDispatcher,
    TransportService& aTransport) noexcept
    : m_world(aWorld)
    , m_transport(aTransport)
    , m_nextTransactionId((GetTickCount64() << 16) ^ GetCurrentProcessId())
    , m_updateConnection(aDispatcher.sink<UpdateEvent>().connect<&SceneTimelineService::OnUpdate>(this))
    , m_disconnectConnection(aDispatcher.sink<DisconnectedEvent>().connect<&SceneTimelineService::OnDisconnected>(this))
    , m_sceneConnection(aDispatcher.sink<NotifySceneTimeline>().connect<&SceneTimelineService::OnSceneTimeline>(this))
{
}

void SceneTimelineService::OnUpdate(const UpdateEvent&) noexcept
{
    const auto& party = m_world.GetPartyService();
    if (!m_transport.IsConnected() || !party.IsInParty() || party.GetStartEpoch() == 0)
    {
        if (m_epoch != 0)
            Clear();
        return;
    }
    if (m_epoch != party.GetStartEpoch())
    {
        m_localStates.clear();
        m_lastServerSequence = 0;
        m_epoch = party.GetStartEpoch();
    }

    const auto nowMs = GetTickCount64();
    if (nowMs < m_nextSampleMs)
        return;
    m_nextSampleMs = nowMs + 50;
    const auto networkTick = m_transport.GetClock().GetCurrentTick();

    auto* pModManager = ModManager::Get();
    if (!pModManager)
        return;
    for (const auto* pQuest : pModManager->quests)
    {
        if (!pQuest || !pQuest->IsEnabled())
            continue;
        const auto& scenes = pQuest->scenes;
        if (!scenes.data || scenes.length > scenes.capacity || scenes.length > 1024)
            continue;
        for (uint32_t i = 0; i < scenes.length; ++i)
        {
            const auto* pScene = scenes.data[i];
            if (!pScene)
                continue;
            SceneState state{pScene->rawPhaseWord, pScene->isPlaying, nowMs};
            const auto it = m_localStates.find(pScene->formID);
            const bool changed = it == m_localStates.end() ? state.Playing :
                (it->second.Playing != state.Playing || it->second.RawPhaseWord != state.RawPhaseWord);
            m_localStates.insert_or_assign(pScene->formID, state);
            if (!party.IsLeader())
                continue;
            // An initial playing scene is sent so a late-loaded follower sees it.
            if (!changed)
                continue;
            SceneTimelineRequest request{};
            auto& snapshot = request.Snapshot;
            if (!m_world.GetModSystem().GetServerModId(pScene->formID, snapshot.SceneId) ||
                !m_world.GetModSystem().GetServerModId(pQuest->formID, snapshot.QuestId))
                continue;
            snapshot.Tick = networkTick;
            snapshot.AuthorityEpoch = m_epoch;
            snapshot.TransactionId = ++m_nextTransactionId;
            snapshot.RawPhaseWord = state.RawPhaseWord;
            snapshot.Playing = state.Playing;
            if (snapshot.IsValid())
                m_transport.Send(request);
        }
    }
}

void SceneTimelineService::OnSceneTimeline(const NotifySceneTimeline& acMessage) noexcept
{
    const auto& party = m_world.GetPartyService();
    const auto& snapshot = acMessage.Snapshot;
    if (!party.IsInParty() || party.IsLeader() || !snapshot.IsValid() ||
        snapshot.AuthorityEpoch != party.GetStartEpoch() ||
        snapshot.ServerSequence <= m_lastServerSequence)
        return;
    m_lastServerSequence = snapshot.ServerSequence;

    const auto formId = m_world.GetModSystem().GetGameId(snapshot.SceneId);
    const auto it = m_localStates.find(formId);
    const bool observed = it != m_localStates.end();
    const bool matches = observed && it->second.Playing == snapshot.Playing &&
        it->second.RawPhaseWord == snapshot.RawPhaseWord;
    const auto nowMs = GetTickCount64();
    const auto sampleAge = observed && nowMs >= it->second.SampleMs ? nowMs - it->second.SampleMs : UINT64_MAX;
    spdlog::info("Scene authority scene={:X} hostPlaying={} hostRawPhase={} localPlaying={} localRawPhase={} observed={} matches={} sampleAgeMs={} sequence={} epoch={}",
        formId, snapshot.Playing, snapshot.RawPhaseWord,
        observed ? it->second.Playing : false,
        observed ? it->second.RawPhaseWord : UINT32_MAX,
        observed, matches, sampleAge, snapshot.ServerSequence, snapshot.AuthorityEpoch);
}

void SceneTimelineService::OnDisconnected(const DisconnectedEvent&) noexcept
{
    Clear();
}

void SceneTimelineService::Clear() noexcept
{
    m_epoch = 0;
    m_nextSampleMs = 0;
    m_lastServerSequence = 0;
    m_localStates.clear();
}
