#include <Services/SceneTimelineService.h>

#include <World.h>
#include <Services/PartyService.h>

#include <Messages/SceneTimelineRequest.h>
#include <Messages/NotifySceneTimeline.h>

SceneTimelineService::SceneTimelineService(World& aWorld, entt::dispatcher& aDispatcher) noexcept
    : m_world(aWorld)
    , m_sceneConnection(aDispatcher.sink<PacketEvent<SceneTimelineRequest>>()
          .connect<&SceneTimelineService::OnSceneTimeline>(this))
{
}

void SceneTimelineService::OnSceneTimeline(const PacketEvent<SceneTimelineRequest>& acMessage) noexcept
{
    auto& partyService = m_world.GetPartyService();
    auto* pParty = partyService.GetPlayerParty(acMessage.pPlayer);
    if (!pParty || !partyService.IsPlayerLeader(acMessage.pPlayer) || pParty->SessionState < 2)
        return;

    const auto& snapshot = acMessage.Packet.Snapshot;
    if (!snapshot.IsValid() || snapshot.ServerSequence != 0 ||
        snapshot.AuthorityEpoch != pParty->StartEpoch)
    {
        spdlog::warn("Rejected invalid scene timeline from player {}", acMessage.pPlayer->GetId());
        return;
    }

    const auto leaderId = acMessage.pPlayer->GetId();
    auto& last = m_lastTransactionByLeader[leaderId];
    if (last.first != pParty->StartEpoch)
        last = {pParty->StartEpoch, 0};
    if (snapshot.TransactionId <= last.second)
        return;
    last.second = snapshot.TransactionId;

    NotifySceneTimeline notify{};
    notify.Snapshot = snapshot;
    notify.Snapshot.AuthorityEpoch = pParty->StartEpoch;
    notify.Snapshot.ServerSequence = ++m_nextSequence;
    for (auto* pMember : pParty->Members)
    {
        if (pMember != acMessage.pPlayer)
            pMember->Send(notify);
    }
}
