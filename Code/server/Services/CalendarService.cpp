#include <Services/CalendarService.h>

#include <GameServer.h>
#include <World.h>

#include <Events/PlayerJoinEvent.h>
#include <Events/UpdateEvent.h>

#include <Messages/ServerTimeSettings.h>

#include "Game/Player.h"
#include <cmath>
#include <random>

CalendarService::CalendarService(World& aWorld, entt::dispatcher& aDispatcher)
    : m_world(aWorld)
{
    std::random_device random;
    m_serverNonce = (static_cast<uint64_t>(random()) << 32) | random();
    if (!m_serverNonce)
        m_serverNonce = 1;
    m_updateConnection = aDispatcher.sink<UpdateEvent>().connect<&CalendarService::OnUpdate>(this);
    m_joinConnection = aDispatcher.sink<PlayerJoinEvent>().connect<&CalendarService::OnPlayerJoin>(this);
}

void CalendarService::OnUpdate(const UpdateEvent&) noexcept
{
    if (!m_lastTick)
        m_lastTick = GameServer::Get()->GetTick();

    auto now = GameServer::Get()->GetTick();

    // client got ahead, we wait
    if (now < m_lastTick)
        return;

    auto delta = now - m_lastTick;
    m_lastTick = now;
    const double advanced = static_cast<double>(delta) * m_dateTime.m_timeModel.TimeScale + m_gameMsFrac;
    if (std::isfinite(advanced) && advanced >= 0.0)
    {
        const auto whole = static_cast<uint64_t>(advanced);
        m_gameMsFrac = advanced - static_cast<double>(whole);
        SetGameMs(m_gameMs + whole);
    }
    if (!m_lastResyncTick || now - m_lastResyncTick >= 10000)
    {
        m_lastResyncTick = now;
        GameServer::Get()->SendToPlayers(MakeTimeSettings(now));
    }
}

void CalendarService::OnPlayerJoin(const PlayerJoinEvent& acEvent) noexcept
{
    // A joiner's hour and days must come from the same clock sample.
    const auto& incoming = acEvent.PlayerTime.m_timeModel;
    const double incomingHours = std::floor(incoming.DaysPassed) * 24.0 + incoming.Time;
    const auto incomingMs = std::isfinite(incomingHours) && incomingHours >= 0.0 && incomingHours < 1.0e12
        ? static_cast<uint64_t>(incomingHours * 3600000.0) : 0;
    const bool playerHasFurthestTime = incomingMs > m_gameMs;

    if (playerHasFurthestTime)
    {
        m_gameMs = incomingMs;
        m_gameMsFrac = 0.0;
        m_dateTime.m_timeModel.Day = incoming.Day;
        m_dateTime.m_timeModel.Month = incoming.Month;
        m_dateTime.m_timeModel.Year = incoming.Year;
        m_dateTime.m_timeModel.Time = static_cast<float>(std::fmod(m_gameMs / 3600000.0, 24.0));
        m_dateTime.m_timeModel.DaysPassed = static_cast<float>(m_gameMs / 86400000.0);
    }
    auto timeMsg = MakeTimeSettings(GameServer::Get()->GetTick());

    if (playerHasFurthestTime)
        GameServer::Get()->SendToPlayers(timeMsg);
    else
        acEvent.pPlayer->Send(timeMsg);
}

bool CalendarService::SetTime(int aHours, int aMinutes, float aScale) noexcept
{
    if (aHours >= 0 && aHours <= 23 && aMinutes >= 0 && aMinutes <= 59 &&
        std::isfinite(aScale) && aScale >= 0.f && aScale <= 1000.f)
    {
        m_dateTime.m_timeModel.TimeScale = aScale;
        m_gameMsFrac = 0.0;
        SetGameMs((m_gameMs / 86400000) * 86400000 +
            static_cast<uint64_t>(aHours) * 3600000 + static_cast<uint64_t>(aMinutes) * 60000);

        SendTimeResync();

        GameServer::Get()->GetWorld().GetScriptService().HandleSetTime(aHours, aMinutes, aScale);
        return true;
    }
    return false;
}

bool CalendarService::SetDate(int aDay, int aMonth, float aYear) noexcept
{
    if (aMonth >= 0 && aMonth < 12 && aYear >= 0 && aYear <= 999)
    {
        auto maxDays = m_dateTime.GetNumberOfDaysByMonthIndex(aMonth);
        if (aDay >= 0 && aDay < maxDays)
        {
            m_dateTime.m_timeModel.Day = aDay;
            m_dateTime.m_timeModel.Month = aMonth;
            m_dateTime.m_timeModel.Year = aYear;
            SendTimeResync();
            return true;
        }
    }
    return false;
}

void CalendarService::SendTimeResync() noexcept
{
    GameServer::Get()->SendToPlayers(MakeTimeSettings(GameServer::Get()->GetTick()));
}

ServerTimeSettings CalendarService::MakeTimeSettings(uint64_t aTick) noexcept
{
    ServerTimeSettings message;
    message.timeModel = m_dateTime.m_timeModel;
    message.GameMs = m_gameMs;
    message.SampleTick = aTick;
    message.Seq = ++m_seq;
    message.ServerNonce = m_serverNonce;
    return message;
}

void CalendarService::SetGameMs(uint64_t aGameMs) noexcept
{
    const auto oldDay = m_gameMs / 86400000;
    const auto newDay = aGameMs / 86400000;
    for (auto day = oldDay; day < newDay; ++day)
    {
        auto& model = m_dateTime.m_timeModel;
        if (++model.Day > DateTime::GetNumberOfDaysByMonthIndex(model.Month))
        {
            model.Day = 1;
            if (++model.Month >= 12)
            {
                model.Month = 0;
                ++model.Year;
            }
        }
    }
    m_gameMs = aGameMs;
    m_dateTime.m_timeModel.Time = static_cast<float>(std::fmod(m_gameMs / 3600000.0, 24.0));
    m_dateTime.m_timeModel.DaysPassed = static_cast<float>(m_gameMs / 86400000.0);
}

CalendarService::TTime CalendarService::GetTime() const noexcept
{
    const auto hour = floor(m_dateTime.m_timeModel.Time);
    const auto minutes = (m_dateTime.m_timeModel.Time - hour) / 17.f;

    const auto flatMinutes = static_cast<int>(ceil((minutes * 100.f) * 10.f));
    return {static_cast<int>(hour), flatMinutes};
}

CalendarService::TTime CalendarService::GetRealTime() noexcept
{
    const auto t = std::time(nullptr);
    int h = (t / 3600) % 24;
    int m = (t / 60) % 60;
    return {h, m};
}

CalendarService::TDate CalendarService::GetDate() const noexcept
{
    return {m_dateTime.m_timeModel.Day, m_dateTime.m_timeModel.Month, m_dateTime.m_timeModel.Year};
}

bool CalendarService::SetTimeScale(float aScale) noexcept
{
    if (aScale >= 0.f && aScale <= 1000.f)
    {
        m_dateTime.m_timeModel.TimeScale = aScale;

        SendTimeResync();
        return true;
    }

    return false;
}
