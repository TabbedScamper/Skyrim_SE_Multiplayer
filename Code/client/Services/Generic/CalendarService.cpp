#include <Services/CalendarService.h>

#include <Events/DisconnectedEvent.h>
#include <Events/UpdateEvent.h>
#include <Messages/ServerTimeSettings.h>
#include <World.h>
#include <Services/PartyService.h>

#include <Forms/TESObjectCELL.h>
#include <PlayerCharacter.h>
#include <TimeManager.h>
#include <Utils/MainThread.h>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <intrin.h>

constexpr float kTransitionSpeed = 5.f;
namespace {
std::atomic<CalendarService*> s_calendarService{};
std::atomic_bool s_blockFollowerTimeSkip{};
std::atomic_bool s_loadResync{};
std::atomic<uintptr_t> s_unknownClockCaller{};
std::atomic<uint32_t> s_backwardHardSetMinutes{};
std::atomic<uint32_t> s_forwardHardSetMinutes{};
std::atomic<uintptr_t> s_rawDaysResyncCaller{};
std::atomic<uint32_t> s_rawDaysResyncCount{};
const bool s_clockProtocolEnabled = [] {
    const char* value = std::getenv("SSC_TS_CLOCK");
    return !value || std::strcmp(value, "0") != 0;
}();
TP_THIS_FUNCTION(TResyncRawDays, void, TimeData);
TResyncRawDays* s_realResyncRawDays{};

void TP_MAKE_THISCALL(HookResyncRawDays, TimeData)
{
    TiltedPhoques::ThisCall(s_realResyncRawDays, apThis);
    MT::NoteLoadGeneration();
    s_loadResync.store(true, std::memory_order_release);
    s_rawDaysResyncCaller.store(reinterpret_cast<uintptr_t>(_ReturnAddress()), std::memory_order_release);
    s_rawDaysResyncCount.fetch_add(1, std::memory_order_relaxed);
}

TiltedPhoques::Initializer s_resyncHook([] {
    POINTER_SKYRIMSE(TResyncRawDays, resyncRawDays, 36317);
    s_realResyncRawDays = resyncRawDays.Get();
    TP_HOOK(&s_realResyncRawDays, HookResyncRawDays);
});
}

std::atomic_bool CalendarService::s_gameClockLocked = false;

CalendarService::CalendarService(World& aWorld, entt::dispatcher& aDispatcher, TransportService& aTransport)
    : m_world(aWorld), m_transport(aTransport)
{
    s_calendarService.store(this, std::memory_order_release);
    m_timeUpdateConnection = aDispatcher.sink<ServerTimeSettings>().connect<&CalendarService::OnTimeUpdate>(this);
    m_updateConnection = aDispatcher.sink<UpdateEvent>().connect<&CalendarService::HandleUpdate>(this);
    m_disconnectedConnection = aDispatcher.sink<DisconnectedEvent>().connect<&CalendarService::OnDisconnected>(this);
}

bool CalendarService::BlockFollowerTimeSkip() noexcept
{
    return s_blockFollowerTimeSkip.load(std::memory_order_acquire);
}

CalendarService::~CalendarService()
{
    s_blockFollowerTimeSkip.store(false, std::memory_order_release);
    s_calendarService.store(nullptr, std::memory_order_release);
}

void CalendarService::OnTimeUpdate(const ServerTimeSettings& acMessage) noexcept
{
    std::lock_guard lock(m_sampleMutex);
    m_pendingModel = acMessage.timeModel;
    m_pendingGameMs = acMessage.GameMs;
    m_pendingTick = acMessage.SampleTick;
    m_pendingSeq = acMessage.Seq;
    m_pendingNonce = acMessage.ServerNonce;
    m_sampleQueued = true;
}

void CalendarService::OnDisconnected(const DisconnectedEvent&) noexcept
{
    s_blockFollowerTimeSkip.store(false, std::memory_order_release);
    {
        std::lock_guard lock(m_sampleMutex);
        m_sampleQueued = false;
    }
    m_disconnectQueued.store(true, std::memory_order_release);
}

float CalendarService::TimeInterpolate(const TimeModel& aFrom, TimeModel& aTo) const
{
    const auto t = aTo.Time - aFrom.Time;
    if (t < 0.f)
    {
        const auto v = t + 24.f;
        // interpolate on the time difference, not the time
        const auto x = TiltedPhoques::Lerp(0.f, v, m_fadeTimer / kTransitionSpeed) + aFrom.Time;

        return TiltedPhoques::Mod(x, 24.f);
    }

    return TiltedPhoques::Lerp(aFrom.Time, aTo.Time, m_fadeTimer / kTransitionSpeed);
}

void CalendarService::ToggleGameClock(bool aEnable)
{
    auto* pGameTime = TimeData::Get();
    m_offlineTime.m_timeModel.Day = pGameTime->GameDay->f;
    m_offlineTime.m_timeModel.Month = pGameTime->GameMonth->f;
    m_offlineTime.m_timeModel.Year = pGameTime->GameYear->f;
    m_offlineTime.m_timeModel.Time = pGameTime->GameHour->f;
    m_offlineTime.m_timeModel.TimeScale = pGameTime->TimeScale->f;

    // The native tick (N36291, 0x140645D60) recomputes GameDaysPassed as GameHour / 24 + rawDaysPassed (+0x3C), which
    // the online clock never advanced: handing the clock back without this jumped days passed back to the day of
    // connecting (SkyrimAtlas weather W3, atlas audit finding 7). Same reconcile as load (N36317, 0x140646BB0): floor.
    if (aEnable && s_gameClockLocked)
        pGameTime->rawDaysPassed = std::floor(pGameTime->GameDaysPassed->f);

    s_gameClockLocked = !aEnable;
}

void CalendarService::HandleUpdate(const UpdateEvent&) noexcept
{
    const auto& party = m_world.GetPartyService();
    s_blockFollowerTimeSkip.store(party.IsInParty() && !party.IsLeader(), std::memory_order_release);
    if (s_gameClockLocked && !s_clockProtocolEnabled)
    {
        if (!m_lastTick)
            m_lastTick = m_world.GetTick();

        const auto now = m_world.GetTick();

        // we got disconnected or the client got ahead of us
        if (now < m_lastTick)
            return;

        const auto delta = now - m_lastTick;
        m_lastTick = now;

        m_onlineTime.Update(delta);
    }
}

void CalendarService::OnMainFrame() noexcept
{
    auto* service = s_calendarService.load(std::memory_order_acquire);
    if (!service)
        return;
    if (service->m_disconnectQueued.exchange(false, std::memory_order_acq_rel))
    {
        service->m_fadeTimer = 0.f;
        service->ToggleGameClock(true);
        service->m_hasSample = false;
        s_loadResync.store(false, std::memory_order_release);
    }
    bool lockClock = false;
    bool acceptedSample = false;
    {
        std::lock_guard lock(service->m_sampleMutex);
        if (service->m_sampleQueued)
        {
            if (!service->m_hasSample || service->m_sampleNonce != service->m_pendingNonce ||
                service->m_pendingSeq >= service->m_sampleSeq)
            {
                lockClock = !s_gameClockLocked;
                service->m_onlineTime.m_timeModel = service->m_pendingModel;
                service->m_sampleGameMs = service->m_pendingGameMs;
                service->m_sampleTick = service->m_pendingTick;
                service->m_sampleSeq = service->m_pendingSeq;
                service->m_sampleNonce = service->m_pendingNonce;
                service->m_hasSample = true;
                acceptedSample = true;
            }
            service->m_sampleQueued = false;
        }
    }
    if (lockClock)
        service->ToggleGameClock(false);
    if (acceptedSample && s_clockProtocolEnabled)
        spdlog::info("TimeSkip: clock sample seq={} gameMs={} tick={} nonce={:X}",
            service->m_sampleSeq, service->m_sampleGameMs, service->m_sampleTick, service->m_sampleNonce);
    if (!s_gameClockLocked)
        return;
    auto* gameTime = TimeData::Get();
    if (!gameTime)
        return;
    const auto& model = service->m_onlineTime.m_timeModel;
    gameTime->TimeScale->f = model.TimeScale;
    if (s_clockProtocolEnabled)
    {
        const auto resyncCount = s_rawDaysResyncCount.exchange(0, std::memory_order_acq_rel);
        if (resyncCount)
            spdlog::info("TimeSkip: raw-days resync count={} caller={:X}",
                resyncCount, s_rawDaysResyncCaller.load(std::memory_order_acquire));
        if (service->m_hasSample && s_loadResync.exchange(false, std::memory_order_acq_rel))
        {
            const auto now = service->m_world.GetTick();
            const auto elapsed = now > service->m_sampleTick ? now - service->m_sampleTick : 0;
            const double target = (service->m_sampleGameMs + elapsed * static_cast<double>(model.TimeScale)) / 3600000.0;
            service->HardSetClock(gameTime, target);
        }
        const auto unknown = s_unknownClockCaller.exchange(0, std::memory_order_acq_rel);
        if (unknown)
            spdlog::warn("TimeSkip: unknown Calendar::Update caller {:X}", unknown);
        const auto backwardMinutes = s_backwardHardSetMinutes.exchange(0, std::memory_order_acq_rel);
        if (backwardMinutes)
            spdlog::warn("TimeSkip: backward hard set by {:.2f} h", backwardMinutes / 60.0);
        const auto forwardMinutes = s_forwardHardSetMinutes.exchange(0, std::memory_order_acq_rel);
        if (forwardMinutes)
            spdlog::info("TimeSkip: forward hard set by {:.2f} h", forwardMinutes / 60.0);
        return;
    }
    gameTime->GameDay->f = static_cast<float>(model.Day);
    gameTime->GameMonth->f = static_cast<float>(model.Month);
    gameTime->GameYear->f = static_cast<float>(model.Year);
    gameTime->GameHour->f = model.Time;
    gameTime->rawDaysPassed = std::floor(model.DaysPassed);
    gameTime->GameDaysPassed->f = gameTime->rawDaysPassed + model.Time / 24.f;
}

void CalendarService::HardSetClock(TimeData* apTime, double aHours) noexcept
{
    const auto days = std::floor(aHours / 24.0);
    const auto hour = aHours - days * 24.0;
    apTime->rawDaysPassed = static_cast<float>(days);
    apTime->GameDaysPassed->f = static_cast<float>(aHours / 24.0);
    apTime->GameHour->f = static_cast<float>(hour);
    if (m_world.GetServerSettings().SyncPlayerCalendar)
    {
        const auto& model = m_onlineTime.m_timeModel;
        apTime->GameDay->f = static_cast<float>(model.Day);
        apTime->GameMonth->f = static_cast<float>(model.Month);
        apTime->GameYear->f = static_cast<float>(model.Year);
    }
}

void CalendarService::SimulateTime(TimeData* apTime, float aSeconds,
    void (*aOriginal)(TimeData*, float), void* apCaller) noexcept
{
    auto* service = s_calendarService.load(std::memory_order_acquire);
    if (!service || !s_gameClockLocked)
    {
        aOriginal(apTime, aSeconds);
        return;
    }
    if (!s_clockProtocolEnabled)
        return;
    const auto caller = reinterpret_cast<uintptr_t>(apCaller);
    if (caller != 0x140658ADB)
    {
        if (caller != 0x1407495DD && caller != 0x14074486D &&
            caller != 0x1401A4257 && caller != 0x1406595FF)
            s_unknownClockCaller.store(caller, std::memory_order_release);
        return;
    }
    if (!service->m_hasSample)
        return;
    const double scale = service->m_onlineTime.m_timeModel.TimeScale;
    if (scale < 1.0)
        return;
    const auto now = service->m_world.GetTick();
    const auto elapsed = now > service->m_sampleTick ? now - service->m_sampleTick : 0;
    const double target = (service->m_sampleGameMs + elapsed * scale) / 3600000.0;
    const double local = static_cast<double>(apTime->rawDaysPassed) * 24.0 + apTime->GameHour->f;
    const double difference = target - local;
    if (difference > 2.0 || difference < -2.0)
    {
        if (difference < -2.0)
            s_backwardHardSetMinutes.store(static_cast<uint32_t>(-difference * 60.0), std::memory_order_release);
        else
            s_forwardHardSetMinutes.store(static_cast<uint32_t>(difference * 60.0), std::memory_order_release);
        service->HardSetClock(apTime, target);
        return;
    }
    if (difference > 0.0)
        aOriginal(apTime, static_cast<float>(difference * 3600.0 / scale));
}
