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
#include <Forms/TESWorldSpace.h>
#include <Misc/GameVM.h>
#include <PlayerCharacter.h>
#include <TimeManager.h>
#include <MainQueue.h>
#include <Utils/MainThread.h>
#include <atomic>
#include <cmath>
#include <cstring>

namespace { std::atomic<WeatherService*> s_weatherService{}; }

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
    s_weatherService.store(this, std::memory_order_release);
}

WeatherService::~WeatherService() noexcept
{
    s_weatherService.store(nullptr, std::memory_order_release);
}

void WeatherService::OnMainFrame() noexcept
{
    auto* service = s_weatherService.load(std::memory_order_acquire);
    if (!service)
        return;
    if (MT::CutEnabled(3))
    {
        MainQueue::Get().World.Drain(32, [service](const MainQueue::Record& record) noexcept {
            if (record.Kind != 1)
                return MainQueue::Decision::Hold;
            if (!MT::Quiet() || MT::CurrentSlot() != MT::Slot::S3_2)
            {
                MT::M3aWrongSlot.fetch_add(1, std::memory_order_relaxed);
                return MainQueue::Decision::Hold;
            }
            const auto& party = service->m_world.GetPartyService();
            const auto authority = party.GetAuthorityEpoch();
            if (record.LoadSerial != MT::LoadSerial.load(std::memory_order_acquire) ||
                record.ConnectionSerial != MT::ConnectionSerial.load(std::memory_order_acquire) ||
                record.AuthKey[0] != authority.StartEpoch ||
                record.AuthKey[1] != (static_cast<uint64_t>(authority.LeaderPlayerId) << 32 | authority.LeaderGeneration))
            {
                service->m_weatherResyncPending.store(true, std::memory_order_release);
                return MainQueue::Decision::Drop;
            }
            struct Sample { uint32_t WeatherId; SkyState Sky; };
            static_assert(sizeof(Sample) <= sizeof(record.Payload));
            Sample sample{};
            std::memcpy(&sample, record.Payload.data(), sizeof(sample));
            service->m_cachedWeatherId = sample.WeatherId;
            service->m_hostSky = sample.Sky;
            return MainQueue::Decision::Apply;
        });
        if (service->m_weatherResyncPending.exchange(false, std::memory_order_acq_rel) &&
            service->m_world.GetPartyService().IsInParty() &&
            !service->m_world.GetPartyService().IsLeader())
            service->m_transport.Send(RequestCurrentWeather());
    }
    if (service->m_releaseOverrideOnMainFrame)
    {
        service->m_releaseOverrideOnMainFrame = false;
        if (auto* sky = Sky::Get())
            sky->ReleaseWeatherOverride();
    }
    auto* vm = GameVM::Get();
    auto* player = PlayerCharacter::Get();
    if (!vm || vm->inactive || !player || !player->parentCell)
        return;
    if (service->m_sendCurrentOnMainFrame)
    {
        service->m_sendCurrentOnMainFrame = false;
        if (auto* sky = Sky::Get())
        {
            if (auto* weather = sky->GetWeather(); sky->mode != 1 && weather && weather->formID != 0xA6858)
                service->SendSkyState(weather->formID);
        }
    }
    service->RunWeatherUpdates(0.0);
}

void WeatherService::OnUpdate(const UpdateEvent&) noexcept
{
    // Sky reads and writes run from HookMainLoop before the engine's frame jobs.
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
        m_sentSky.Valid = false;
        m_sendCurrentOnMainFrame = true;
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
    SkyState sky{};
    if (acMessage.HasSky)
    {
        sky.Valid = true;
        sky.WorldSpaceId = acMessage.WorldSpaceId;
        sky.LastWeatherId = acMessage.LastId ? modSystem.GetGameId(acMessage.LastId) : 0;
        sky.Percent = acMessage.Percent;
        sky.WindSpeed = acMessage.WindSpeed;
        sky.WindAngle = acMessage.WindAngle;
    }
    if (!MT::CutEnabled(3))
    {
        m_cachedWeatherId = weatherId;
        m_hostSky = sky;
        return;
    }
    struct Sample { uint32_t WeatherId; SkyState Sky; };
    static_assert(sizeof(Sample) <= sizeof(MainQueue::Record::Payload));
    const Sample sample{weatherId, sky};
    MainQueue::Record record{};
    record.Kind = 1;
    record.OrderKey = 1;
    record.LoadSerial = MT::LoadSerial.load(std::memory_order_acquire);
    record.ConnectionSerial = MT::ConnectionSerial.load(std::memory_order_acquire);
    const auto authority = m_world.GetPartyService().GetAuthorityEpoch();
    record.AuthKey[0] = authority.StartEpoch;
    record.AuthKey[1] = static_cast<uint64_t>(authority.LeaderPlayerId) << 32 | authority.LeaderGeneration;
    std::memcpy(record.Payload.data(), &sample, sizeof(sample));
    if (!MainQueue::Get().World.Push(record))
        m_weatherResyncPending.store(true, std::memory_order_release);
}

void WeatherService::ApplyHostSky() noexcept
{
    Sky* pSky = Sky::Get();
    if (!pSky || !pSky->pClimate || !m_hostSky.Valid || !pSky->pCurrentWeather ||
        pSky->pCurrentWeather->formID != m_cachedWeatherId)
        return;
    auto* gameTime = TimeData::Get();
    if (!gameTime)
        return;
    const auto hour = gameTime->GameHour->f;
    const auto volatility = *(reinterpret_cast<const uint8_t*>(pSky->pCurrentWeather) + 0x667);
    const float transitionHours = (static_cast<float>(volatility) / 255.f) * (0.25f - 0.01f) + 0.01f;
    const auto wrap24 = [](float aHour) { return std::fmod(std::fmod(aHour, 24.f) + 24.f, 24.f); };
    pSky->pOverrideWeather = pSky->pCurrentWeather;
    const bool localBlending = pSky->pLastWeather != nullptr;
    if (m_hostSky.LastWeatherId && m_hostSky.Percent < 1.f)
        pSky->pLastWeather = Cast<TESWeather>(TESForm::GetById(m_hostSky.LastWeatherId));
    if (pSky->pLastWeather && m_hostSky.Percent < 1.f)
        pSky->weatherStartHour = wrap24(hour - m_hostSky.Percent * transitionHours);
    else if (localBlending)
        pSky->weatherStartHour = wrap24(hour - 1.01f * transitionHours);
    else
        pSky->weatherStartHour = hour;
    pSky->flags &= ~8u;
    pSky->weatherAcceleration = 0.f;
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
    if (pSky->mode == 1)
        return;
    SkyState state{};
    state.Valid = true;
    if (auto* player = PlayerCharacter::Get(); player && player->GetWorldSpace())
        m_world.GetModSystem().GetServerModId(player->GetWorldSpace()->formID, state.WorldSpaceId);
    state.LastWeatherId = pSky->pLastWeather ? pSky->pLastWeather->formID : 0;
    state.Percent = pSky->currentWeatherPct;
    state.WindSpeed = pSky->windSpeed;
    state.WindAngle = pSky->windAngle;
    const auto now = GetTickCount64();
    const bool changed = aWeatherId != m_cachedWeatherId || state.LastWeatherId != m_sentSky.LastWeatherId ||
        state.WindAngle != m_sentSky.WindAngle || state.WorldSpaceId != m_sentSky.WorldSpaceId || !m_sentSky.Valid;
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
    request.WorldSpaceId = state.WorldSpaceId;
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

    if (pSky->mode == 1)
        return;

    if (!m_world.GetPartyService().IsLeader())
    {
        GameId localWorld{};
        if (auto* player = PlayerCharacter::Get(); player && player->GetWorldSpace() &&
            !m_world.GetModSystem().GetServerModId(player->GetWorldSpace()->formID, localWorld))
            return;
        if (m_hostSky.Valid && localWorld != m_hostSky.WorldSpaceId)
            return;
    }

    const bool climateChanged = pSky->pClimate != m_lastClimate;
    m_lastClimate = pSky->pClimate;
    if (!m_world.GetPartyService().IsLeader() && m_cachedWeatherId &&
        (climateChanged || !pSky->pCurrentWeather || m_waitingForClimateWeather))
    {
        auto* weather = Cast<TESWeather>(TESForm::GetById(m_cachedWeatherId));
        if (weather && pSky->pClimate)
            pSky->pOverrideWeather = weather;
        m_waitingForClimateWeather = pSky->pCurrentWeather != weather;
        if (m_waitingForClimateWeather)
            return;
        m_forcedWeatherId = m_cachedWeatherId;
    }

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
    if (m_cachedWeatherId && (pWeather->formID != m_cachedWeatherId || m_forcedWeatherId != m_cachedWeatherId))
        SetCachedWeather();
    ApplyHostSky();
}

void WeatherService::ToggleGameWeatherSystem(bool aToggle) noexcept
{
    if (aToggle)
        m_releaseOverrideOnMainFrame = true;
    else
        m_transport.Send(RequestCurrentWeather());

    m_cachedWeatherId = 0;
    m_forcedWeatherId = 0;
    m_lastClimate = nullptr;
    m_waitingForClimateWeather = false;
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

    auto* sky = Sky::Get();
    if (!sky || !sky->pClimate || (m_forcedWeatherId == m_cachedWeatherId && sky->pCurrentWeather == pWeather))
        return;
    if (MT::CutEnabled(3) && (!MT::Quiet() || MT::CurrentSlot() != MT::Slot::S3_2))
    {
        MT::M3aWrongSlot.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    sky->ForceWeather(pWeather);
    m_forcedWeatherId = m_cachedWeatherId;
}
