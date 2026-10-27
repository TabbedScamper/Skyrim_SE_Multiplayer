#pragma once

#include <DateTime.h>
#include <Events/EventDispatcher.h>
#include <Games/Events.h>
#include <atomic>
#include <mutex>

struct ServerTimeSettings;
struct DisconnectedEvent;
struct World;
struct UpdateEvent;
struct TransportService;
struct TimeData;

/**
 * @brief Handles time sync.
 */
class CalendarService final : public BSTEventSink<TESActivateEvent>
{
public:
    CalendarService(World&, entt::dispatcher&, TransportService&);
    ~CalendarService();

    static void SimulateTime(TimeData*, float, void (*)(TimeData*, float), void*) noexcept;
    static bool BlockFollowerTimeSkip() noexcept;
    static void OnMainFrame() noexcept;

private:
    void OnTimeUpdate(const ServerTimeSettings&) noexcept;
    void HandleUpdate(const UpdateEvent&) noexcept;
    void OnDisconnected(const DisconnectedEvent&) noexcept;

    void ToggleGameClock(bool aEnable);
    void HardSetClock(TimeData*, double) noexcept;
    float TimeInterpolate(const TimeModel& aFrom, TimeModel& aTo) const;

    entt::scoped_connection m_timeUpdateConnection;
    entt::scoped_connection m_updateConnection;
    entt::scoped_connection m_disconnectedConnection;

    DateTime m_onlineTime;
    DateTime m_offlineTime;
    std::mutex m_sampleMutex;
    uint64_t m_pendingGameMs{};
    uint64_t m_pendingTick{};
    uint64_t m_pendingNonce{};
    uint32_t m_pendingSeq{};
    TimeModel m_pendingModel;
    bool m_sampleQueued{};
    uint64_t m_sampleGameMs{};
    uint64_t m_sampleTick{};
    uint64_t m_sampleNonce{};
    uint32_t m_sampleSeq{};
    bool m_hasSample{};
    std::atomic_bool m_disconnectQueued{};
    float m_fadeTimer = 0.f;
    static std::atomic_bool s_gameClockLocked;

    uint64_t m_lastTick = 0;
    uint64_t m_lastLogTick = 0;
    World& m_world;
    TransportService& m_transport;
};
