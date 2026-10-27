#include <Services/CalendarService.h>
#include <TimeManager.h>
#include <PlayerCharacter.h>

TimeData* TimeData::Get() noexcept
{
    POINTER_SKYRIMSE(TimeData*, s_instance, 400447);
    return *(s_instance.Get());
}

using TSimulateTime = void(TimeData*, float);
static TSimulateTime* RealSimulateTime;
using TStartTimeSkip = void(PlayerCharacter*, int32_t);
static TStartTimeSkip* RealStartSleeping;
static TStartTimeSkip* RealStartWaiting;

void HookStartSleeping(PlayerCharacter* apPlayer, int32_t aHours)
{
    if (!CalendarService::BlockFollowerTimeSkip())
        RealStartSleeping(apPlayer, aHours);
}

void HookStartWaiting(PlayerCharacter* apPlayer, int32_t aHours)
{
    if (!CalendarService::BlockFollowerTimeSkip())
        RealStartWaiting(apPlayer, aHours);
}

void HookSimulateTime(TimeData* apData, float aMultiplier)
{
    CalendarService::SimulateTime(apData, aMultiplier, RealSimulateTime, _ReturnAddress());
}

static TiltedPhoques::Initializer s_loadingScreenHooks(
    []()
    {
        POINTER_SKYRIMSE(TSimulateTime, s_SimulateTime, 36291);
        RealSimulateTime = s_SimulateTime.Get();
        TP_HOOK(&RealSimulateTime, HookSimulateTime);
        POINTER_SKYRIMSE(TStartTimeSkip, startSleeping, 40414);
        POINTER_SKYRIMSE(TStartTimeSkip, startWaiting, 40415);
        RealStartSleeping = startSleeping.Get();
        RealStartWaiting = startWaiting.Get();
        TP_HOOK(&RealStartSleeping, HookStartSleeping);
        TP_HOOK(&RealStartWaiting, HookStartWaiting);
    });
