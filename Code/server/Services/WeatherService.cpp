#include <Services/WeatherService.h>

#include <Components.h>
#include <GameServer.h>
#include <World.h>

#include <Messages/RequestWeatherChange.h>
#include <Messages/NotifyWeatherChange.h>
#include <Messages/RequestCurrentWeather.h>

WeatherService::WeatherService(World& aWorld, entt::dispatcher& aDispatcher) noexcept
    : m_world(aWorld)
{
    m_weatherChangeConnection = aDispatcher.sink<PacketEvent<RequestWeatherChange>>().connect<&WeatherService::OnWeatherChange>(this);
    m_currentWeatherConnection = aDispatcher.sink<PacketEvent<RequestCurrentWeather>>().connect<&WeatherService::OnRequestCurrentWeather>(this);
}

void WeatherService::OnWeatherChange(const PacketEvent<RequestWeatherChange>& acMessage) const noexcept
{
    NotifyWeatherChange notify{};
    notify.Id = acMessage.Packet.Id;
    notify.HasSky = acMessage.Packet.HasSky;
    notify.LastId = acMessage.Packet.LastId;
    notify.Percent = acMessage.Packet.Percent;
    notify.WindSpeed = acMessage.Packet.WindSpeed;
    notify.WindAngle = acMessage.Packet.WindAngle;

    auto* pParty = m_world.GetPartyService().GetPlayerParty(acMessage.pPlayer);
    if (!pParty)
        return;

    pParty->CachedWeather = notify.Id;
    if (notify.HasSky)
    {
        pParty->CachedHasSky = true;
        pParty->CachedLastWeather = notify.LastId;
        pParty->CachedWeatherPercent = notify.Percent;
        pParty->CachedWindSpeed = notify.WindSpeed;
        pParty->CachedWindAngle = notify.WindAngle;
    }

    if (!acMessage.pPlayer->GetCharacter())
        return;

    const auto origin = *acMessage.pPlayer->GetCharacter();

    GameServer::Get()->SendToPartyInRange(notify, acMessage.pPlayer->GetParty(), origin, acMessage.pPlayer);
}

void WeatherService::OnRequestCurrentWeather(const PacketEvent<RequestCurrentWeather>& acMessage) const noexcept
{
    auto* pParty = m_world.GetPartyService().GetPlayerParty(acMessage.pPlayer);
    if (!pParty)
        return;

    NotifyWeatherChange notify{};
    notify.Id = pParty->CachedWeather;
    notify.HasSky = pParty->CachedHasSky;
    notify.LastId = pParty->CachedLastWeather;
    notify.Percent = pParty->CachedWeatherPercent;
    notify.WindSpeed = pParty->CachedWindSpeed;
    notify.WindAngle = pParty->CachedWindAngle;

    acMessage.pPlayer->Send(notify);
}
