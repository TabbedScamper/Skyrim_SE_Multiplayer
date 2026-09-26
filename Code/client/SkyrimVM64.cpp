#include <TiltedOnlinePCH.h>
#include "TiltedOnlineApp.h"
#include "GameLoopDiagnostic.h"
#include <Misc/GameVM.h>
#include <Services/ObjectService.h>
#include <Services/CorpseRagdollService.h>
#include <Services/CreatorTogether.h>
#include <Games/Skyrim/Actor.h>
#include <Systems/InterpolationSystem.h>

extern std::unique_ptr<TiltedOnlineApp> g_appInstance;

struct Main;

TP_THIS_FUNCTION(TVMUpdate, int, GameVM, float);
TP_THIS_FUNCTION(TMainLoop, short, Main);
TP_THIS_FUNCTION(TVMDestructor, uintptr_t, void);

static TVMUpdate* VMUpdate = nullptr;
static TMainLoop* MainLoop = nullptr;
static TVMDestructor* VMDestructor = nullptr;

int TP_MAKE_THISCALL(HookVMUpdate, GameVM, float a2)
{
    RecordGameVmHookEntry();
    const bool active = apThis->inactive == 0;
    const auto entry = std::chrono::steady_clock::now();
    if (active)
        g_appInstance->Update();
    const auto afterApp = std::chrono::steady_clock::now();
    const auto result = TiltedPhoques::ThisCall(VMUpdate, apThis, a2);
    const auto afterOriginal = std::chrono::steady_clock::now();
    const auto appUs = std::chrono::duration_cast<std::chrono::microseconds>(
        afterApp - entry).count();
    const auto originalUs = std::chrono::duration_cast<std::chrono::microseconds>(
        afterOriginal - afterApp).count();
    RecordGameVmHookCall(active, static_cast<uint32_t>((std::min)(
        int64_t{UINT32_MAX}, appUs)), static_cast<uint32_t>((std::min)(
        int64_t{UINT32_MAX}, originalUs)));
    return result;
}

short TP_MAKE_THISCALL(HookMainLoop, Main)
{
    ObjectService::OnMainFrame();
    CorpseRagdollService::OnMainFrame();
    CreatorTogether::OnMainFrame();
    Actor::FlushPendingReset3D();
    InterpolationSystem::OnMainFrame();

    const auto result = TiltedPhoques::ThisCall(MainLoop, apThis);
    ObjectService::OnMainFrameEnd();
    return result;
}

uintptr_t TP_MAKE_THISCALL(HookVMDestructor, void)
{
    TP_EMPTY_HOOK_PLACEHOLDER

    return TiltedPhoques::ThisCall(VMDestructor, apThis);
}

static TiltedPhoques::Initializer s_mainHooks(
    []()
    {
        POINTER_SKYRIMSE(TMainLoop, cMainLoop, 36564);
        POINTER_SKYRIMSE(TVMUpdate, cVMUpdate, 53926);
        POINTER_SKYRIMSE(TVMDestructor, cVMDestructor, 40412);

        VMUpdate = cVMUpdate.Get();
        MainLoop = cMainLoop.Get();
        VMDestructor = cVMDestructor.Get();

        TP_HOOK(&VMUpdate, HookVMUpdate);
        TP_HOOK(&MainLoop, HookMainLoop);
        TP_HOOK(&VMDestructor, HookVMDestructor);
    });

