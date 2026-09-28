#include <Services/WeatherService.h>

#include <Events/UpdateEvent.h>
#include <Events/DisconnectedEvent.h>
#include <Events/PartyJoinedEvent.h>
#include <Events/PartyLeftEvent.h>

#include <Messages/RequestWeatherChange.h>
#include <Messages/NotifyWeatherChange.h>
#include <Messages/RequestCurrentWeather.h>

#include <Sky/Sky.h>
#include <Forms/TESWeather.h>

WeatherService::WeatherService(World& aWorld, TransportService& aTransport, entt::dispatcher& aDispatcher)
    : m_world(aWorld)
    , m_transport(aTransport)
{
    m_updateConnection = aDispatcher.sink<UpdateEvent>().connect<&WeatherService::OnUpdate>(this);
    m_disconnectConnection = aDispatcher.sink<DisconnectedEvent>().connect<&WeatherService::OnDisconnected>(this);
    m_partyJoinedConnection = aDispatcher.sink<PartyJoinedEvent>().connect<&WeatherService::OnPartyJoinedEvent>(this);
    m_partyLeftConnection = aDispatcher.sink<PartyLeftEvent>().connect<&WeatherService::OnPartyLeftEvent>(this);
    m_playerAddedConnection = m_world.on_destroy<WaitingFor3D>().connect<&WeatherService::OnWaitingFor3DRemoved>(this);
    m_playerRemovedConnection = m_world.on_destroy<PlayerComponent>().connect<&WeatherService::OnPlayerComponentRemoved>(this);
    m_weatherChangeConnection = aDispatcher.sink<NotifyWeatherChange>().connect<&WeatherService::OnWeatherChange>(this);
}

void WeatherService::OnUpdate(const UpdateEvent& acEvent) noexcept
{
    RunWeatherUpdates(acEvent.Delta);
}

void WeatherService::OnDisconnected(const DisconnectedEvent& acEvent) noexcept
{
    ToggleGameWeatherSystem(true);
}

void WeatherService::OnPartyJoinedEvent(const PartyJoinedEvent& acEvent) noexcept
{
    if (!acEvent.IsLeader)
    {
        // TODO: why is this loop here? Party should always have a leader.
        auto view = m_world.view<PlayerComponent>();
        const auto& partyService = m_world.GetPartyService();

        for (auto entity : view)
        {
            const auto& playerComponent = view.get<PlayerComponent>(entity);
            if (playerComponent.Id == partyService.GetLeaderPlayerId())
            {
                ToggleGameWeatherSystem(false);
                break;
            }
        }
    }
    else
    {
        Sky* pSky = Sky::Get();
        if (!pSky)
            return;

        TESWeather* pWeather = pSky->GetWeather();
        if (!pWeather)
        {
            m_cachedWeatherId = 0;
            return;
        }

        // Potentially sets cached weather to map weather.
        // When the player closes the map, it'll send out the proper weather on the next update.
        m_cachedWeatherId = pWeather->formID;

        // This is the map weather, should not be synced.
        if (pWeather->formID == 0xA6858)
            return;

        RequestWeatherChange request{};

        auto& modSystem = m_world.GetModSystem();
        if (!modSystem.GetServerModId(pWeather->formID, request.Id))
        {
            spdlog::error(__FUNCTION__ ": weather server ID not found, form id: {:X}", pWeather->formID);
            return;
        }

        m_transport.Send(request);
    }
}

void WeatherService::OnPartyLeftEvent(const PartyLeftEvent& acEvent) noexcept
{
    ToggleGameWeatherSystem(true);
}

// TODO: OnPlayerComponentAdded() instead? Does PlayerComponent exist already by then?
void WeatherService::OnWaitingFor3DRemoved(entt::registry& aRegistry, entt::entity aEntity) noexcept
{
    const auto* pPlayerComponent = m_world.try_get<PlayerComponent>(aEntity);
    if (!pPlayerComponent)
        return;

    const auto& partyService = m_world.GetPartyService();
    if (!partyService.IsInParty() || partyService.IsLeader())
        return;

    if (partyService.GetLeaderPlayerId() == pPlayerComponent->Id)
        ToggleGameWeatherSystem(false);
}

void WeatherService::OnPlayerComponentRemoved(entt::registry& aRegistry, entt::entity aEntity) noexcept
{
    const auto& playerComponent = m_world.get<PlayerComponent>(aEntity);

    const auto& partyService = m_world.GetPartyService();
    if (!partyService.IsInParty() || partyService.IsLeader())
        return;

    if (partyService.GetLeaderPlayerId() == playerComponent.Id)
        ToggleGameWeatherSystem(true);
}

void WeatherService::OnWeatherChange(const NotifyWeatherChange& acMessage) noexcept
{
    auto& modSystem = m_world.GetModSystem();
    const uint32_t weatherId = modSystem.GetGameId(acMessage.Id);
    TESWeather* pWeather = Cast<TESWeather>(TESForm::GetById(weatherId));

    if (!pWeather)
    {
        spdlog::error(__FUNCTION__ ": weather not found, form id: {:X}", acMessage.Id.ModId + acMessage.Id.BaseId);
        return;
    }

    Sky::Get()->ForceWeather(pWeather);

    m_cachedWeatherId = weatherId;

    m_hostSky = {};
    if (acMessage.HasSky)
    {
        m_hostSky.Valid = true;
        m_hostSky.LastWeatherId = acMessage.LastId ? modSystem.GetGameId(acMessage.LastId) : 0;
        m_hostSky.Percent = acMessage.Percent;
        m_hostSky.WindSpeed = acMessage.WindSpeed;
        m_hostSky.WindAngle = acMessage.WindAngle;
        ApplyHostSky();
    }
}

void WeatherService::ApplyHostSky() noexcept
{
    Sky* pSky = Sky::Get();
    if (!pSky || !m_hostSky.Valid || !pSky->pCurrentWeather || pSky->pCurrentWeather->formID != m_cachedWeatherId)
        return;
    // ForceWeather reset the blend to the new weather at 100% and the wind angle is re-rolled locally on a
    // weather change (ID 26229), so flags, smoke and precipitation followed a different wind than the host.
    pSky->pLastWeather = m_hostSky.LastWeatherId ? Cast<TESWeather>(TESForm::GetById(m_hostSky.LastWeatherId)) : nullptr;
    pSky->currentWeatherPct = m_hostSky.Percent;
    const bool windChanged = pSky->windSpeed != m_hostSky.WindSpeed || pSky->windAngle != m_hostSky.WindAngle;
    pSky->windSpeed = m_hostSky.WindSpeed;
    pSky->windAngle = m_hostSky.WindAngle;
    if (windChanged)
        pSky->flags |= 0x100000; // kUpdateWind: wind consumers pick up the new values
}

void WeatherService::SendSkyState(const uint32_t aWeatherId) noexcept
{
    Sky* pSky = Sky::Get();
    if (!pSky)
        return;
    SkyState state{};
    state.Valid = true;
    state.LastWeatherId = pSky->pLastWeather ? pSky->pLastWeather->formID : 0;
    state.Percent = pSky->currentWeatherPct;
    state.WindSpeed = pSky->windSpeed;
    state.WindAngle = pSky->windAngle;
    const auto now = GetTickCount64();
    const bool changed = aWeatherId != m_cachedWeatherId || state.LastWeatherId != m_sentSky.LastWeatherId ||
        state.WindAngle != m_sentSky.WindAngle || !m_sentSky.Valid;
    // During a blend the percentage moves every frame: refresh at 2 Hz, not per frame.
    const bool blending = state.Percent < 1.f && now >= m_nextSkySendMs;
    if (!changed && !blending)
        return;
    RequestWeatherChange request{};
    auto& modSystem = m_world.GetModSystem();
    if (!modSystem.GetServerModId(aWeatherId, request.Id))
    {
        spdlog::error(__FUNCTION__ ": weather server ID not found, form id: {:X}", aWeatherId);
        return;
    }
    request.HasSky = true;
    if (state.LastWeatherId && !modSystem.GetServerModId(state.LastWeatherId, request.LastId))
        request.LastId = {};
    request.Percent = state.Percent;
    request.WindSpeed = state.WindSpeed;
    request.WindAngle = state.WindAngle;
    m_transport.Send(request);
    m_cachedWeatherId = aWeatherId;
    m_sentSky = state;
    m_nextSkySendMs = now + 500;
}

void WeatherService::RunWeatherUpdates(const double acDelta) noexcept
{
    Sky* pSky = Sky::Get();
    if (!pSky)
        return;

    TESWeather* pWeather = pSky->GetWeather();
    if (!pWeather)
    {
        if (m_world.GetPartyService().IsLeader())
            m_cachedWeatherId = 0;
        else
            SetCachedWeather();

        return;
    }

    // This is the map weather, should not be synced.
    if (pWeather->formID == 0xA6858)
        return;

    if (m_world.GetPartyService().IsLeader())
    {
        // Weather id, blend and wind (checked each frame: the game has no single SetWeather entry point).
        SendSkyState(pWeather->formID);
        return;
    }
    if (pWeather->formID != m_cachedWeatherId)
        SetCachedWeather();
    ApplyHostSky();
}

void WeatherService::ToggleGameWeatherSystem(bool aToggle) noexcept
{
    if (aToggle)
        Sky::Get()->ReleaseWeatherOverride();
    else
        m_transport.Send(RequestCurrentWeather());

    m_cachedWeatherId = 0;
}

void WeatherService::SetCachedWeather() noexcept
{
    if (m_cachedWeatherId == 0)
        return;

    TESWeather* pWeather = Cast<TESWeather>(TESForm::GetById(m_cachedWeatherId));

    if (!pWeather)
    {
        spdlog::error(__FUNCTION__ ": weather not found, form id: {:X}", m_cachedWeatherId);
        return;
    }

    Sky::Get()->ForceWeather(pWeather);
}
