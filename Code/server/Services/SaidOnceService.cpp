#include <Services/SaidOnceService.h>

#include <Messages/RequestSaidOnce.h>
#include <Messages/NotifySaidOnce.h>
#include <Services/PartyService.h>
#include <World.h>
#include <Game/Player.h>

SaidOnceService::SaidOnceService(World& aWorld, entt::dispatcher& aDispatcher) noexcept
    : m_world(aWorld)
    , m_requestConnection(aDispatcher.sink<PacketEvent<RequestSaidOnce>>().connect<&SaidOnceService::OnRequest>(this))
{
}

void SaidOnceService::OnRequest(const PacketEvent<RequestSaidOnce>& aEvent) noexcept
{
    if (!aEvent.pPlayer || !aEvent.Packet.Info)
        return;
    auto* party = m_world.GetPartyService().GetPlayerParty(aEvent.pPlayer);
    if (!party || party->SessionState < 2)
        return;
    NotifySaidOnce notify;
    notify.Info = aEvent.Packet.Info;
    for (auto* member : party->Members)
    {
        if (member && member != aEvent.pPlayer)
            member->Send(notify);
    }
}
