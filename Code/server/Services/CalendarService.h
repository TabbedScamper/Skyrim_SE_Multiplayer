#pragma once

#include <Events/PacketEvent.h>
#include <DateTime.h>
#include <Structs/GameId.h>

struct World;
struct UpdateEvent;
struct PlayerJoinEvent;
struct ServerTimeSettings;

/**
 * @brief Manages time and date of the world.
 */
class CalendarService
{
public:
    CalendarService(World& aWorld, entt::dispatcher& aDispatcher);

    // we use these types for SOL
    // this is done this way because SOL
    // provides direct support for these
    using TTime = std::pair<int, int>;
    using TDate = std::tuple<int, int, int>;

    bool SetTime(int aHour, int aMinutes, float aScale) noexcept;
    bool SetDate(int aDay, int aMonth, float aYear) noexcept;

    // returns hours, minutes
    TTime GetTime() const noexcept;
    static TTime GetRealTime() noexcept;

    // returns dd/mm/yy
    TDate GetDate() const noexcept;

    float GetTimeScale() const noexcept { return m_dateTime.m_timeModel.TimeScale; }
    bool SetTimeScale(float aScale) noexcept;
    uint64_t GetGameMs() const noexcept { return m_gameMs; }

private:
    void OnUpdate(const UpdateEvent&) noexcept;
    void OnPlayerJoin(const PlayerJoinEvent&) noexcept;
    void SendTimeResync() noexcept;
    ServerTimeSettings MakeTimeSettings(uint64_t aTick) noexcept;
    void SetGameMs(uint64_t aGameMs) noexcept;

    DateTime m_dateTime;
    uint64_t m_lastTick = 0;
    uint64_t m_lastResyncTick = 0;
    uint64_t m_gameMs = 43200000;
    double m_gameMsFrac = 0.0;
    uint32_t m_seq = 0;
    uint64_t m_serverNonce = 0;

    World& m_world;

    entt::scoped_connection m_updateConnection;
    entt::scoped_connection m_joinConnection;
};
