#include <Services/CameraService.h>

#include <World.h>
#include <Services/PartyService.h>

#include <Messages/CameraStateRequest.h>
#include <Messages/NotifyCameraState.h>

CameraService::CameraService(World& aWorld, entt::dispatcher& aDispatcher) noexcept
    : m_world(aWorld)
    , m_cameraStateConnection(aDispatcher.sink<PacketEvent<CameraStateRequest>>().connect<&CameraService::OnCameraState>(this))
{
}

void CameraService::OnCameraState(const PacketEvent<CameraStateRequest>& acMessage) noexcept
{
    auto& partyService = m_world.GetPartyService();
    auto* pParty = partyService.GetPlayerParty(acMessage.pPlayer);
    if (!pParty || !partyService.IsPlayerLeader(acMessage.pPlayer) ||
        pParty->SessionState != 2)
        return;

    const auto& snapshot = acMessage.Packet.Snapshot;
    if (snapshot.AuthorityEpoch != pParty->StartEpoch || !snapshot.IsValid())
    {
        spdlog::warn("Rejected invalid/stale camera snapshot from leader {}", acMessage.pPlayer->GetId());
        return;
    }

    NotifyCameraState notify{};
    notify.Snapshot = snapshot;
    // Never relay a client-selected epoch as authority; bind it to the active
    // server party epoch after validation.
    notify.Snapshot.AuthorityEpoch = pParty->StartEpoch;
    for (auto* pMember : pParty->Members)
    {
        if (pMember != acMessage.pPlayer)
            pMember->Send(notify);
    }
}
