#include <Misc/BSScript.h>
#include <Services/ObjectService.h>
#include <Misc/NativeFunction.h>

#include <World.h>
#include <Events/PapyrusFunctionRegisterEvent.h>
#include <Forms/TESForm.h>
#include <Misc/GameVM.h>
#include <Actor.h>
#include <PlayerCharacter.h>
#include <Games/ActorExtension.h>
#include <Games/PapyrusFunctions.h>
#include <Misc/NativeDispatchDiagnostic.h>
#include <FunctionHook.hpp>

namespace
{
std::atomic<uint64_t> s_nativeDispatchCount{0};
std::atomic<uint64_t> s_lastNativeFunctionHash{0};
std::atomic<uint64_t> s_lastNativeDispatchTimeMs{0};
std::atomic<uint64_t> s_disablePlayerControlsCalls{0};
std::atomic<uint64_t> s_enablePlayerControlsCalls{0};
std::atomic<uint64_t> s_lastControlCallTimeMs{0};
std::atomic<bool> s_lastControlCallEnabled{false};
std::atomic<uint64_t> s_vmUpdateCount{0};
std::atomic<uint64_t> s_vmUpdateLastStartMs{0};
std::atomic<uint64_t> s_vmUpdateLastDurationUs{0};
std::atomic<uint64_t> s_vmTaskletCount{0};
std::atomic<uint64_t> s_vmTaskletLastStartMs{0};
std::atomic<uint64_t> s_vmTaskletLastDurationUs{0};
using VMUpdateFn = void (*)(BSScript::IVirtualMachine*, float);
VMUpdateFn s_realVmUpdate = nullptr;
VMUpdateFn s_realVmTasklets = nullptr;
void* s_vmHookedVtable = nullptr;

void HookVMUpdate(BSScript::IVirtualMachine* apVm, float aBudget) noexcept
{
    const auto start = std::chrono::steady_clock::now();
    s_vmUpdateLastStartMs.store(GetTickCount64(), std::memory_order_relaxed);
    s_vmUpdateCount.fetch_add(1, std::memory_order_relaxed);
    s_realVmUpdate(apVm, aBudget);
    s_vmUpdateLastDurationUs.store(static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - start).count()), std::memory_order_relaxed);
}

void HookVMTasklets(BSScript::IVirtualMachine* apVm, float aBudget) noexcept
{
    const auto start = std::chrono::steady_clock::now();
    s_vmTaskletLastStartMs.store(GetTickCount64(), std::memory_order_relaxed);
    s_vmTaskletCount.fetch_add(1, std::memory_order_relaxed);
    s_realVmTasklets(apVm, aBudget);
    s_vmTaskletLastDurationUs.store(static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - start).count()), std::memory_order_relaxed);
}

uint64_t HashNativeName(const char* apObject, const char* apName) noexcept
{
    uint64_t hash = 14695981039346656037ull;
    const auto append = [&hash](const char* apText)
    {
        if (!apText)
            return;
        for (const auto* p = reinterpret_cast<const unsigned char*>(apText); *p; ++p)
            hash = (hash ^ *p) * 1099511628211ull;
    };
    append(apObject);
    hash = (hash ^ ':') * 1099511628211ull;
    append(apName);
    return hash;
}

bool IsReadableVmRange(const void* apData, size_t aSize) noexcept
{
    if (!apData || aSize == 0)
        return false;
    MEMORY_BASIC_INFORMATION info{};
    if (!VirtualQuery(apData, &info, sizeof(info)) || info.State != MEM_COMMIT ||
        (info.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0)
        return false;
    switch (info.Protect & 0xFF)
    {
    case PAGE_READONLY:
    case PAGE_READWRITE:
    case PAGE_WRITECOPY:
    case PAGE_EXECUTE_READ:
    case PAGE_EXECUTE_READWRITE:
    case PAGE_EXECUTE_WRITECOPY:
        break;
    default:
        return false;
    }
    const auto address = reinterpret_cast<uintptr_t>(apData);
    const auto regionEnd = reinterpret_cast<uintptr_t>(info.BaseAddress) + info.RegionSize;
    return address <= regionEnd && aSize <= regionEnd - address;
}

bool IsExecutableVmAddress(const void* apData) noexcept
{
    MEMORY_BASIC_INFORMATION info{};
    if (!apData || !VirtualQuery(apData, &info, sizeof(info)) || info.State != MEM_COMMIT ||
        (info.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0)
        return false;
    const auto protection = info.Protect & 0xFF;
    return protection == PAGE_EXECUTE || protection == PAGE_EXECUTE_READ ||
        protection == PAGE_EXECUTE_READWRITE || protection == PAGE_EXECUTE_WRITECOPY;
}

std::string CopyVmText(const char* apText)
{
    if (!apText)
        return {};
    std::string result;
    result.reserve(64);
    for (size_t i = 0; i < 128; ++i)
    {
        if (!IsReadableVmRange(apText + i, 1))
            return "<unreadable>";
        const char value = apText[i];
        if (value == '\0')
            return result;
        result += value;
    }
    return result;
}

void LogControlCallVmFrame(const BSScript::StackFrame* apFrame,
    const BSScript::Stack* apStack, uint32_t aDepth)
{
    if (!IsReadableVmRange(apFrame, 0x38) || apFrame->pParent != apStack)
        return;
    auto* pFunction = apFrame->owningFunction;
    if (!IsReadableVmRange(pFunction, sizeof(void*)))
        return;
    auto* pVtable = *reinterpret_cast<void* const* const*>(pFunction);
    if (!IsReadableVmRange(pVtable, 17 * sizeof(void*)) ||
        !IsExecutableVmAddress(pVtable[1]) ||
        !IsExecutableVmAddress(pVtable[2]) ||
        !IsExecutableVmAddress(pVtable[16]))
        return;
    const auto object = CopyVmText(pFunction->GetObjectTypeName().AsAscii());
    const auto name = CopyVmText(pFunction->GetName().AsAscii());
    const auto source = CopyVmText(pFunction->GetSourceFilename().AsAscii());
    spdlog::info("Player control Papyrus frame depth={} object={} function={} source={} ip={} selfType={} selfData={:016X}",
        aDepth, object, name, source, apFrame->instructionPointer,
        apFrame->selfType, apFrame->selfData);
}
}

NativeDispatchDiagnostic GetNativeDispatchDiagnostic() noexcept
{
    return {s_nativeDispatchCount.load(std::memory_order_relaxed),
        s_lastNativeFunctionHash.load(std::memory_order_relaxed),
        s_lastNativeDispatchTimeMs.load(std::memory_order_relaxed),
        s_vmUpdateCount.load(std::memory_order_relaxed),
        s_vmUpdateLastStartMs.load(std::memory_order_relaxed),
        s_vmUpdateLastDurationUs.load(std::memory_order_relaxed),
        s_vmTaskletCount.load(std::memory_order_relaxed),
        s_vmTaskletLastStartMs.load(std::memory_order_relaxed),
        s_vmTaskletLastDurationUs.load(std::memory_order_relaxed),
        s_disablePlayerControlsCalls.load(std::memory_order_relaxed),
        s_enablePlayerControlsCalls.load(std::memory_order_relaxed),
        s_lastControlCallTimeMs.load(std::memory_order_relaxed),
        s_lastControlCallEnabled.load(std::memory_order_relaxed)};
}

void InstallVirtualMachineDiagnostic() noexcept
{
    auto* pGameVm = GameVM::Get();
    auto* pVm = pGameVm ? pGameVm->virtualMachine : nullptr;
    if (!pVm)
        return;
    auto* pVtable = *reinterpret_cast<void***>(pVm);
    if (!pVtable || s_vmHookedVtable)
        return;

    // CommonLibSSE-NG pins Update(float) and UpdateTasklets(float) to slots
    // 04/05. Observe both, but leave VM queues, stack IDs and return values
    // entirely native. Seed originals before patching so a worker can never
    // enter an installed hook with a null trampoline.
    const auto pOriginalUpdate = reinterpret_cast<VMUpdateFn>(pVtable[4]);
    const auto pOriginalTasklets = reinterpret_cast<VMUpdateFn>(pVtable[5]);
    if (!pOriginalUpdate || !pOriginalTasklets ||
        pOriginalUpdate == &HookVMUpdate || pOriginalTasklets == &HookVMTasklets)
        return;
    s_realVmUpdate = pOriginalUpdate;
    s_realVmTasklets = pOriginalTasklets;
    const auto pReplacedUpdate = TiltedPhoques::HookVTable(pVm, 4, &HookVMUpdate);
    const auto pReplacedTasklets = TiltedPhoques::HookVTable(pVm, 5, &HookVMTasklets);
    if (!pReplacedUpdate || !pReplacedTasklets)
        return;
    s_vmHookedVtable = pVtable;
}

TP_THIS_FUNCTION(TRegisterPapyrusFunction, void, BSScript::IVirtualMachine, NativeFunction*);
TP_THIS_FUNCTION(TBindEverythingToScript, void, BSScript::IVirtualMachine*);
TP_THIS_FUNCTION(TSignaturesMatch, bool, BSScript::NativeFunction, BSScript::NativeFunction*);
TP_THIS_FUNCTION(TCompareVariables, int64_t, void, BSScript::Variable*, BSScript::Variable*);
TP_THIS_FUNCTION(TNativePapyrusCall, BSScript::CallResult, BSScript::NativeFunctionBase, BSScript::Stack*, void*, BSScript::IVirtualMachine*, bool);

TRegisterPapyrusFunction* RealRegisterPapyrusFunction = nullptr;
TBindEverythingToScript* RealBindEverythingToScript = nullptr;
TSignaturesMatch* RealSignaturesMatch = nullptr;
TCompareVariables* RealCompareVariables = nullptr;
TNativePapyrusCall* RealNativePapyrusCall = nullptr;

BSScript::CallResult TP_MAKE_THISCALL(HookNativePapyrusCall, BSScript::NativeFunctionBase,
    BSScript::Stack* apStack, void* apLogger, BSScript::IVirtualMachine* apVm, bool aArg4)
{
    // MQ101's opening cards are driven by native Game Papyrus calls. Observe
    // their real timing before suppressing or replaying anything on a follower.
    const char* pName = apThis->name.AsAscii();
    const char* pObject = apThis->objectName.AsAscii();
    s_lastNativeFunctionHash.store(HashNativeName(pObject, pName), std::memory_order_relaxed);
    s_lastNativeDispatchTimeMs.store(GetTickCount64(), std::memory_order_relaxed);
    s_nativeDispatchCount.fetch_add(1, std::memory_order_relaxed);
    static std::atomic<uint32_t> s_callSamples{0};
    const auto sample = s_callSamples.fetch_add(1, std::memory_order_relaxed);
    if (sample < 16)
        spdlog::info("Papyrus native dispatch sample {}::{} tick={}", pObject ? pObject : "<null>", pName ? pName : "<null>", GetTickCount64());
    const bool isTitleCall = pName && pObject && _stricmp(pObject, "game") == 0 &&
        (_stricmp(pName, "ShowTitleSequenceMenu") == 0 ||
            _stricmp(pName, "StartTitleSequence") == 0 ||
            _stricmp(pName, "HideTitleSequenceMenu") == 0);
    if (isTitleCall)
        spdlog::info("Title sequence native enter {} tick={}", pName, GetTickCount64());
    // Scripted player-state natives: which of these a quest/scene uses to drive the player
    // (auto-walk, restraints, camera) decides what a follower must mirror.
    static std::atomic<uint32_t> s_scriptedSamples{0};
    if (ObjectService::IsRenderDiagnosticsArmed() && s_scriptedSamples.load(std::memory_order_relaxed) < 400 && pName && pObject)
    {
        static constexpr const char* s_scriptedPlayerNatives[] = {"SetPlayerAIDriven", "EvaluatePackage", "PathToReference",
            "SetDontMove", "SetRestrained", "ForceThirdPerson", "ForceFirstPerson", "PlayIdle", "MoveTo", "TranslateTo",
            "SetGhost", "SetAlpha", "KeepOffsetFromActor", "ClearKeepOffsetFromActor", "SetVehicle", "SetCameraTarget",
            "SetPlayerControls", "SetInChargen", "ShowRaceMenu"};
        for (const char* pWatched : s_scriptedPlayerNatives)
        {
            if (_stricmp(pName, pWatched) == 0)
            {
                if (s_scriptedSamples.fetch_add(1, std::memory_order_relaxed) < 400)
                    spdlog::info("Scripted player native {}::{} tick={}", pObject, pName, GetTickCount64());
                break;
            }
        }
    }
    const bool isDisableControls = pName && pObject && _stricmp(pObject, "game") == 0 &&
        _stricmp(pName, "DisablePlayerControls") == 0;
    const bool isEnableControls = pName && pObject && _stricmp(pObject, "game") == 0 &&
        _stricmp(pName, "EnablePlayerControls") == 0;
    if (isDisableControls || isEnableControls)
    {
        const auto tick = GetTickCount64();
        if (isDisableControls)
            s_disablePlayerControlsCalls.fetch_add(1, std::memory_order_relaxed);
        else
            s_enablePlayerControlsCalls.fetch_add(1, std::memory_order_relaxed);
        s_lastControlCallTimeMs.store(tick, std::memory_order_relaxed);
        s_lastControlCallEnabled.store(isEnableControls, std::memory_order_relaxed);
        // The stack walk copies VM strings a character at a time (about 90 ms per call): only when armed
        // by a test command. Unarmed, the follower's repeated camera-bob calls stalled it for seconds.
        static std::atomic<uint32_t> s_controlTraceSamples{0};
        if (ObjectService::IsRenderDiagnosticsArmed() && s_controlTraceSamples.fetch_add(1, std::memory_order_relaxed) < 256)
        {
            spdlog::info("Player control Papyrus native enter Game::{} tick={}", pName, tick);
            // The Call ABI passes a reference to BSTSmartPointer<Stack>, not
            // a direct Stack*. Only inspect live frames while the VM owns it.
            if (IsReadableVmRange(apStack, sizeof(BSScript::Stack*)))
            {
                auto* pStack = *reinterpret_cast<BSScript::Stack* const*>(apStack);
                if (IsReadableVmRange(pStack, 0x68))
                {
                    auto* pFrame = pStack->top;
                    for (uint32_t depth = 0; depth < 5 &&
                        IsReadableVmRange(pFrame, 0x38) && pFrame->pParent == pStack; ++depth)
                    {
                        LogControlCallVmFrame(pFrame, pStack, depth);
                        auto* pPrevious = pFrame->previousFrame;
                        if (pPrevious == pFrame)
                            break;
                        pFrame = pPrevious;
                    }
                }
            }
        }
    }

    const auto result = TiltedPhoques::ThisCall(RealNativePapyrusCall, apThis, apStack, apLogger, apVm, aArg4);

    if (isTitleCall)
        spdlog::info("Title sequence native leave {} tick={} result={}", pName, GetTickCount64(), static_cast<int>(result));

    return result;
}

void TP_MAKE_THISCALL(HookRegisterPapyrusFunction, BSScript::IVirtualMachine, NativeFunction* apFunction)
{
    auto& runner = World::Get().GetRunner();

    const char* pFunctionName = apFunction->functionName.AsAscii();
    const char* pTypeName = apFunction->typeName.AsAscii();
    if (pFunctionName && pTypeName && _stricmp(pTypeName, "game") == 0 &&
        (_stricmp(pFunctionName, "ShowTitleSequenceMenu") == 0 ||
            _stricmp(pFunctionName, "StartTitleSequence") == 0 ||
            _stricmp(pFunctionName, "HideTitleSequenceMenu") == 0))
        spdlog::info("Title sequence native registered {} address={}", pFunctionName, apFunction->functionAddress);

    PapyrusFunctionRegisterEvent event(apFunction->functionName.AsAscii(), apFunction->typeName.AsAscii(), apFunction->functionAddress);

    runner.Trigger(std::move(event));

    TiltedPhoques::ThisCall(RealRegisterPapyrusFunction, apThis, apFunction);
}

void TP_MAKE_THISCALL(HookBindEverythingToScript, BSScript::IVirtualMachine*)
{
    (*apThis)->BindNativeMethod(new BSScript::IsRemotePlayerFunc("IsRemotePlayer", "SkyrimTogetherUtils", PapyrusFunctions::IsRemotePlayer, BSScript::Variable::kBoolean));
    (*apThis)->BindNativeMethod(new BSScript::IsPlayerFunc("IsPlayer", "SkyrimTogetherUtils", PapyrusFunctions::IsPlayer, BSScript::Variable::kBoolean));
    (*apThis)->BindNativeMethod(new BSScript::DidLaunchSkyrimTogetherFunc("DidLaunchSkyrimTogether", "SkyrimTogetherVerifyLaunchScript", PapyrusFunctions::DidLaunchSkyrimTogether, BSScript::Variable::kBoolean));

    TiltedPhoques::ThisCall(RealBindEverythingToScript, apThis);
}

bool TP_MAKE_THISCALL(HookSignaturesMatch, BSScript::NativeFunction, BSScript::NativeFunction* apOther)
{
    /*
    if (!strcmp(apThis->GetName().AsAscii(), "IsRemotePlayer"))
        DebugBreak();
    */

    return TiltedPhoques::ThisCall(RealSignaturesMatch, apThis, apOther);
}

// This is a neat hack, but it has been disabled since it messes up other things like beastform.
// These kinds of issues should be solved with custom scripts now that we have SkyrimTogether.esp anyway.
int64_t TP_MAKE_THISCALL(HookCompareVariables, void, BSScript::Variable* apVar1, BSScript::Variable* apVar2)
{
    BSScript::Object* pObject1 = apVar1->GetObject();
    BSScript::Object* pObject2 = apVar2->GetObject();

    if (!pObject1 || !pObject2)
        return TiltedPhoques::ThisCall(RealCompareVariables, apThis, apVar1, apVar2);

    uint64_t handle1 = pObject1->GetHandle();
    uint64_t handle2 = pObject2->GetHandle();

    auto* pPolicy = GameVM::Get()->virtualMachine->GetObjectHandlePolicy();

    if (!pPolicy || !handle1 || !handle2 || !pPolicy->HandleIsType((uint32_t)Actor::Type, handle1) || !pPolicy->HandleIsType((uint32_t)Actor::Type, handle2) || !pPolicy->IsHandleObjectAvailable(handle1) || !pPolicy->IsHandleObjectAvailable(handle2))
    {
        return TiltedPhoques::ThisCall(RealCompareVariables, apThis, apVar1, apVar2);
    }

    Actor* pActor1 = pPolicy->GetObjectForHandle<Actor>(handle1);
    Actor* pActor2 = pPolicy->GetObjectForHandle<Actor>(handle2);

    if (!pActor1 || !pActor2)
        return TiltedPhoques::ThisCall(RealCompareVariables, apThis, apVar1, apVar2);

    if (pActor1 == PlayerCharacter::Get())
    {
        auto* pExtension = pActor2->GetExtension();
        if (pExtension && pExtension->IsPlayer())
            return 0;
    }
    else if (pActor2 == PlayerCharacter::Get())
    {
        auto* pExtension = pActor1->GetExtension();
        if (pExtension && pExtension->IsPlayer())
            return 0;
    }

    return TiltedPhoques::ThisCall(RealCompareVariables, apThis, apVar1, apVar2);
}

static TiltedPhoques::Initializer s_vmHooks(
    []()
    {
        POINTER_SKYRIMSE(TRegisterPapyrusFunction, s_registerPapyrusFunction, 104788);
        POINTER_SKYRIMSE(TBindEverythingToScript, s_bindEverythingToScript, 55739);
        POINTER_SKYRIMSE(TSignaturesMatch, s_signaturesMatch, 104359);
        POINTER_SKYRIMSE(TNativePapyrusCall, s_nativePapyrusCall, 104651);

        // POINTER_SKYRIMSE(TCompareVariables, s_compareVariables, 105220);

        RealRegisterPapyrusFunction = s_registerPapyrusFunction.Get();
        RealBindEverythingToScript = s_bindEverythingToScript.Get();
        RealSignaturesMatch = s_signaturesMatch.Get();
        RealNativePapyrusCall = s_nativePapyrusCall.Get();
        // RealCompareVariables = s_compareVariables.Get();

        TP_HOOK(&RealRegisterPapyrusFunction, HookRegisterPapyrusFunction);
        TP_HOOK(&RealBindEverythingToScript, HookBindEverythingToScript);
        TP_HOOK(&RealSignaturesMatch, HookSignaturesMatch);
        TP_HOOK(&RealNativePapyrusCall, HookNativePapyrusCall);
        // TP_HOOK(&RealCompareVariables, HookCompareVariables);
    });
