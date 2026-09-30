#include <TiltedOnlinePCH.h>
#include <Services/WorldStateService.h>
#include <Services/HarnessService.h>
#include <Services/FarmMode.h>

#include <Services/GameTestService.h>
#include <Services/CheckpointSaves.h>
#include <Systems/InterpolationSystem.h>
#include <Camera/PlayerCamera.h>
#include <Systems/AnimationSystem.h>
#include <Misc/NativeDispatchDiagnostic.h>
#include <Services/GameSettingsService.h>
#include <Services/OverlayService.h>
#include <Services/CameraService.h>
#include <Services/CharacterService.h>
#include <Services/PlayerService.h>
#include <Systems/FaceGenSystem.h>
#include <Services/PapyrusService.h>
#include <EquipManager.h>
#include <Services/CorpseRagdollService.h>
#include <Services/Generic/HeadTrackService.h>
#include <World.h>
#include <GameLoopDiagnostic.h>
#include <DInputHook.hpp>
#include <Games/Skyrim/BSInput/InputPollDiagnostic.h>

#include <Games/Skyrim/BSGraphics/BSGraphicsRenderer.h>
#include <Games/ActorExtension.h>
#include <Games/Misc/MenuTopicManager.h>
#include <Games/Skyrim/Interface/UI.h>
#include <Games/Skyrim/Interface/LoadingScreenProbe.h>
#include <Games/Skyrim/PlayerCharacter.h>
#include <Games/Skyrim/SaveLoad.h>
#include <Games/Skyrim/Forms/ActorValueInfo.h>
#include <Games/Skyrim/Forms/TESQuest.h>
#include <Games/Skyrim/Forms/TESNPC.h>
#include <Games/Skyrim/Forms/BGSHeadPart.h>
#include <Games/Skyrim/Forms/TESRace.h>
#include <Games/Skyrim/Forms/BGSOutfit.h>
#include <Games/Skyrim/Forms/TESObjectCELL.h>
#include <Games/Skyrim/Forms/TESWorldSpace.h>
#include <Games/Skyrim/Forms/TESPackage.h>
#include <Games/Skyrim/AI/Movement/PlayerControls.h>
#include <Games/Skyrim/Camera/PlayerCamera.h>
#include <Games/Skyrim/Camera/TESCameraState.h>
#include <Games/Skyrim/AI/AIProcess.h>
#include <Games/Skyrim/Actor.h>
#include <Games/Skyrim/ArmorAttachmentTrace.h>
#include <Forms/TESIdleForm.h>
#include <Games/Skyrim/Havok/PoseCopyAuthority.h>
#include <Services/DoorVoteService.h>
#include <Combat/CombatController.h>
#include <Games/Skyrim/NetImmerse/NiNode.h>
#include <Games/Skyrim/NetImmerse/NiTriBasedGeom.h>
#include <Games/Skyrim/NetImmerse/NiRenderedTexture.h>
#include <Games/Skyrim/NetImmerse/BSShaderProperty.h>
#include <Games/Skyrim/NetImmerse/BSMaskedShaderMaterial.h>
#include <Games/Skyrim/Misc/TintMask.h>
#include <Games/Skyrim/BSAnimationGraphManager.h>
#include <Games/Skyrim/Havok/ActorPoseDiagnosticViews.h>
#include <Games/Skyrim/Havok/AnimationGraphUpdateTrace.h>
#include <Games/Skyrim/Havok/VisualPoseMailbox.h>
#include <Games/Skyrim/Havok/hkbGenerator.h>
#include <Games/Skyrim/Havok/hkbStateMachine.h>
#include <Games/TES.h>
#include <Services/PartyService.h>
#include <Services/TransportService.h>
#include <Services/ObjectService.h>
#include <Combat/PlayerCombat.h>
#include <Services/ReviveService.h>
#include <Services/QuestService.h>
#include <Messages/PartyStartRequest.h>
#include <Components.h>
#include <Structs/AnimationGraphDescriptorManager.h>
#include <OverlayApp.hpp>
#include <OverlayRenderHandler.hpp>
#include <Games/Memory.h>
#include <Services/CreatorTogether.h>
#include <MinHook.h>

extern std::atomic<bool> g_copyNativeTracking; // AnimationSystem.cpp

// Research handoff: perf2-r1-check/reference-research.patch records the native
// call paths and prior art. This task cannot edit the shared research document.
namespace HostFrameCost
{
// 0 cull, 1 capture, 2 headtrack, 3 lips, 4 naked, 5 trigger, 6 doors, 7 players,
// 8 pose selection, 9 interest publication, 10 interest claims, 11 forced graphs.
// These are inclusive elapsed scopes, including worker waits.
std::array<std::atomic<uint64_t>, 12> s_nanoseconds{}, s_calls{}, s_maxNs{};
std::atomic_bool s_enabled{};
std::atomic<uint64_t> s_untilMs{};
std::atomic<uint64_t> s_session{};
bool s_doorTimingInstalled{};
std::atomic<uint32_t> s_triggerTimingInstalled{};

void TriggerTimingInstalled() noexcept
{
    s_triggerTimingInstalled.fetch_add(1, std::memory_order_relaxed);
}

uint64_t Session() noexcept
{
    return s_session.load(std::memory_order_relaxed);
}

std::chrono::steady_clock::time_point Begin() noexcept
{
    return s_enabled.load(std::memory_order_relaxed) ? std::chrono::steady_clock::now() :
        std::chrono::steady_clock::time_point{};
}

void Add(uint32_t aFeature, uint64_t aNanoseconds) noexcept
{
    if (aFeature >= s_nanoseconds.size() || !s_enabled.load(std::memory_order_relaxed))
        return;
    s_nanoseconds[aFeature].fetch_add(aNanoseconds, std::memory_order_relaxed);
    s_calls[aFeature].fetch_add(1, std::memory_order_relaxed);
    auto previous = s_maxNs[aFeature].load(std::memory_order_relaxed);
    while (previous < aNanoseconds && !s_maxNs[aFeature].compare_exchange_weak(
        previous, aNanoseconds, std::memory_order_relaxed)) {}
}

void End(uint32_t aFeature, std::chrono::steady_clock::time_point aStart) noexcept
{
    if (aStart != std::chrono::steady_clock::time_point{})
        Add(aFeature, std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - aStart).count());
}

void Report(World& aWorld, uint64_t aNow) noexcept
{
    static uint64_t next{}, frames{}, previousCaptureUs{}, windowStart{}, maxWorldGapUs{}, generation{};
    static GameLoopDiagnostic previousLoop{};
    const bool enabled = aNow < s_untilMs.load(std::memory_order_relaxed) &&
        aWorld.GetTransport().IsConnected();
    const bool wasEnabled = s_enabled.load(std::memory_order_relaxed);
    if (!enabled && !wasEnabled)
        return;
    const auto loop = GetGameLoopDiagnostic();
    if (enabled && !wasEnabled)
    {
        for (size_t i = 0; i < s_nanoseconds.size(); ++i)
        {
            s_nanoseconds[i].exchange(0, std::memory_order_relaxed);
            s_calls[i].exchange(0, std::memory_order_relaxed);
            s_maxNs[i].exchange(0, std::memory_order_relaxed);
        }
        previousCaptureUs = ObjectService::GetPreStepPlaybackDiagnostic().HostScanTotalUs;
        previousLoop = loop;
        windowStart = aNow;
        frames = maxWorldGapUs = 0;
        next = aNow + 5000;
        s_enabled.store(true, std::memory_order_relaxed);
        s_session.store(++generation, std::memory_order_relaxed);
        spdlog::info("Host frame profile: started wallMs={} session={} leader={} remoteCameraOverride=native-tracking-phase actionBudget=1 (inclusive elapsed scopes)",
            aNow, generation, aWorld.GetPartyService().IsLeader());
    }
    if (!enabled && !wasEnabled)
        return;
    ++frames;
    maxWorldGapUs = (std::max)(maxWorldGapUs, uint64_t{loop.WorldLastEntryGapUs});
    if (enabled && aNow < next)
        return;
    s_enabled.store(enabled, std::memory_order_relaxed);
    if (!enabled)
        s_session.store(0, std::memory_order_relaxed);
    next = aNow + 5000;
    std::array<double, 12> us{}, maxUs{};
    std::array<uint64_t, 12> calls{};
    for (size_t i = 0; i < us.size(); ++i)
    {
        us[i] = static_cast<double>(s_nanoseconds[i].exchange(0, std::memory_order_relaxed)) / (1000.0 * frames);
        maxUs[i] = s_maxNs[i].exchange(0, std::memory_order_relaxed) / 1000.0;
        calls[i] = s_calls[i].exchange(0, std::memory_order_relaxed);
    }
    // Aggregate once per report, not with extra clocks/atomics on the workers.
    for (size_t i = 9; i <= 11; ++i)
    {
        us[0] += us[i];
        maxUs[0] = (std::max)(maxUs[0], maxUs[i]);
        calls[0] += calls[i];
    }
    const auto captureUs = ObjectService::GetPreStepPlaybackDiagnostic().HostScanTotalUs;
    us[1] = static_cast<double>(captureUs >= previousCaptureUs ? captureUs - previousCaptureUs : captureUs) / frames;
    previousCaptureUs = captureUs;
    spdlog::info("Host frame cost: cull {} us, capture {} us, headtrack {} us, lips {} us, naked {} us, trigger {} us, doors {} us, remote players {} us (wallMs={} windowMs={} worldUpdates={} inclusive elapsed/world update; overlapping worker durations, NOT render critical path; capture=active physics lane only, doors=automatic distance controller including callback, excludes vote service; doorsInstalled={})",
        us[0], us[1], us[2], us[3], us[4], s_triggerTimingInstalled.load(std::memory_order_relaxed) == 2 ? us[5] : -1.0,
        s_doorTimingInstalled ? us[6] : -1.0, us[7], aNow, aNow - windowStart, frames, s_doorTimingInstalled);
    spdlog::info("Host frame scope peaks: wallMs={} cull={}/{} headtrack={}/{} lips={}/{} naked={}/{} trigger={}/{} players={}/{} poseSelection={}/{} doors={}/{} (maxUs/calls; poseSelectionMeanUs={})",
        aNow, maxUs[0], calls[0], maxUs[2], calls[2], maxUs[3], calls[3], maxUs[4], calls[4],
        maxUs[5], calls[5], maxUs[7], calls[7], maxUs[8], calls[8], maxUs[6], calls[6], us[8]);
    spdlog::info("Host frame cadence: wallMs={} maxWorldGapUs={} gapsOver50Ms={} dispatcherUs={} vmAppUs={} vmNativeUs={} (interval totals; VM/update thread, not render frame time)",
        aNow, maxWorldGapUs, loop.WorldGapsOver50Ms - previousLoop.WorldGapsOver50Ms,
        loop.WorldDispatcherTotalUs - previousLoop.WorldDispatcherTotalUs,
        loop.VmAppTotalUs - previousLoop.VmAppTotalUs, loop.VmOriginalTotalUs - previousLoop.VmOriginalTotalUs);
    spdlog::info("Host cull breakdown: wallMs={} publish={}/{}/{} claim={}/{}/{} forcedGraph={}/{}/{} (meanUsPerWorldUpdate/maxUs/calls; claim includes mutex wait; summed worker elapsed, not frame critical path)",
        aNow, us[9], maxUs[9], calls[9], us[10], maxUs[10], calls[10], us[11], maxUs[11], calls[11]);
    spdlog::info("Host frame coverage: wallMs={} triggerHooks={}/2 automaticDoorHook={} (missing coverage reports -1, not zero cost)",
        aNow, s_triggerTimingInstalled.load(std::memory_order_relaxed), s_doorTimingInstalled);
    previousLoop = loop;
    windowStart = aNow;
    frames = maxWorldGapUs = 0;
    if (!enabled)
        spdlog::info("Host frame profile: stopped wallMs={}", aNow);
}
}

namespace
{
// drop_item request (base form, count), window thread -> game thread.
std::mutex s_mainFrameDropLock;
std::pair<uint32_t, int32_t> s_mainFrameDrop{};
std::pair<uint32_t, bool> s_mainFrameDisable{}; // set_disabled request (reference, disabled), same lock
float s_mainFrameDamage{};                      // damage_player request, same lock
std::pair<uint32_t, uint32_t> s_mainFrameCombat{}; // start_combat request (attacker, target), same lock
// creator_slide call, same lock. Runs from the frame update, not a runner task: a creator rebuild fires animation
// events that queue runner tasks, and queuing from inside the runner drain aborted the game (2026-09-29 09:12).
std::function<void()> s_mainFrameCreator;
void QueueCreatorCall(std::function<void()> aCall)
{
    std::lock_guard lock(s_mainFrameDropLock);
    s_mainFrameCreator = std::move(aCall);
}

// Read-only timing at the caller of the existing automatic-door hook. 40201 /
// 140738120 is NiTimeController slot 0x27, void(this, NiUpdateData*), and calls +0x58
// only on distance crossings. 17934 / 1402848F0 installs 17933 / 1402846B0
// through 140738110. Do not hook 17933 twice or alter its arguments/decisions.
// 140EE09B0 passes the data pointer in RDX in this exe. The older CommonLib
// float signature is not the 1.7.104 call-site ABI; forward the pointer unchanged.
using TDistanceUpdate = void(void*, void*);
TDistanceUpdate* s_distanceUpdate{};
uintptr_t s_automaticDoorCallback{};

void HookDistanceUpdate(void* apController, void* apUpdateData)
{
    const bool measure = HostFrameCost::Session() &&
        *reinterpret_cast<const uintptr_t*>(static_cast<const uint8_t*>(apController) + 0x58) == s_automaticDoorCallback;
    const auto started = measure ? HostFrameCost::Begin() : std::chrono::steady_clock::time_point{};
    s_distanceUpdate(apController, apUpdateData);
    HostFrameCost::End(6, started);
}

TiltedPhoques::Initializer s_doorCostHook([]() {
    POINTER_SKYRIMSE(TDistanceUpdate, update, 40201);
    using TDoorCallback = void(void*, uint32_t, bool);
    POINTER_SKYRIMSE(TDoorCallback, callback, 17933);
    s_automaticDoorCallback = reinterpret_cast<uintptr_t>(callback.Get());
    s_distanceUpdate = update.Get();
    TiltedPhoques::FunctionHookManager::GetInstance();
    const auto target = reinterpret_cast<void*>(update.Get());
    const auto created = MH_CreateHook(target, reinterpret_cast<void*>(HookDistanceUpdate),
        reinterpret_cast<void**>(&s_distanceUpdate));
    if (created == MH_OK)
    {
        const auto enabled = MH_EnableHook(target);
        HostFrameCost::s_doorTimingInstalled = enabled == MH_OK;
        if (enabled != MH_OK)
            MH_RemoveHook(target);
    }
});

constexpr DWORD cPipeRejectRemoteClients = 0x00000008;
std::atomic<uint64_t> s_diagnosticCaptureUntilMs{};

void ArmDiagnosticCapture(uint64_t aLeadMs = 0) noexcept
{
    const auto until = GetTickCount64() + (std::min)(aLeadMs, uint64_t{30000}) + 5000;
    auto previous = s_diagnosticCaptureUntilMs.load(std::memory_order_relaxed);
    while (previous < until && !s_diagnosticCaptureUntilMs.compare_exchange_weak(
        previous, until, std::memory_order_relaxed))
    {
    }
}

std::string EscapeJson(const std::string& acValue)
{
    std::string result;
    result.reserve(acValue.size() + 16);
    for (const char value : acValue)
    {
        switch (value)
        {
        case '\\': result += "\\\\"; break;
        case '"': result += "\\\""; break;
        case '\n': result += "\\n"; break;
        case '\r': result += "\\r"; break;
        case '\t': result += "\\t"; break;
        default:
            if (static_cast<uint8_t>(value) < 0x20)
                result += fmt::format("\\u{:04x}", static_cast<uint8_t>(value));
            else
                result += value;
            break;
        }
    }
    return result;
}

std::string GetJsonString(const std::string& acJson, const char* acName)
{
    const std::string key = std::string("\"") + acName + "\"";
    auto position = acJson.find(key);
    if (position == std::string::npos)
        return {};
    position = acJson.find(':', position + key.size());
    if (position == std::string::npos)
        return {};
    position = acJson.find('"', position + 1);
    if (position == std::string::npos)
        return {};
    std::string result;
    for (++position; position < acJson.size(); ++position)
    {
        const char value = acJson[position];
        if (value == '"')
            break;
        if (value == '\\' && position + 1 < acJson.size())
        {
            const char escaped = acJson[++position];
            if (escaped == 'n') result += '\n';
            else if (escaped == 'r') result += '\r';
            else if (escaped == 't') result += '\t';
            else result += escaped;
        }
        else
            result += value;
    }
    return result;
}

uint64_t GetJsonId(const std::string& acJson)
{
    const auto key = acJson.find("\"id\"");
    if (key == std::string::npos)
        return 0;
    const auto colon = acJson.find(':', key + 4);
    if (colon == std::string::npos)
        return 0;
    return std::strtoull(acJson.c_str() + colon + 1, nullptr, 10);
}

std::string Result(uint64_t aId, const std::string& acPayload)
{
    return fmt::format("{{\"id\":{},\"ok\":true,{}}}", aId, acPayload);
}

std::string Error(uint64_t aId, const std::string& acMessage)
{
    return fmt::format("{{\"id\":{},\"ok\":false,\"error\":\"{}\"}}", aId, EscapeJson(acMessage));
}

const char* JsonBool(bool aValue)
{
    return aValue ? "true" : "false";
}

// Bridge-only capture driver. Requests and published JSON cross threads; all
// engine calls and the walking state belong exclusively to OnGameThread.
namespace IntroDriver
{
std::mutex mutex;
std::string pending, published = "\"state\":\"idle\"";
uint64_t queuedSequence{}, appliedSequence{};
bool active{}, ownsAI{};
void* ownedController{};
bool jumping{}, jumpIssued{}, jumpAirborne{};
uint64_t jumpIssuedAt{};
uint32_t jumpState = UINT32_MAX, jumpRequestedState = UINT32_MAX;
bool walkPace{};
bool harnessCombatTravel{};
bool harnessNudge{};
bool harnessArrival{};
uint64_t arrivalStillSince{};
NiPoint3 arrivalPoint{};
NiPoint3 arrivalTargetPoint{};
uint64_t jumpStart{};
float jumpHeading{};
uint32_t targetId{}, markerId{}, startCell{}, questId{};
uint16_t objectiveId{};
float radius = 64.f, distance{}, bestDistance{};
NiPoint3 submittedPoint{};
NiPoint3 progressPoint{};
uint64_t started{}, lastProgress{}, nextTick{}, clearCombatSince{};
int32_t pathId = -1;
std::string state = "idle", reason, creator = "idle";
std::string questFilter;
uint32_t harnessObjectiveRef{};
uint16_t harnessObjectiveIndex{};

float Number(const std::string& line, const char* key, float fallback)
{
    auto pos = line.find(std::string("\"") + key + "\"");
    if (pos == std::string::npos) return fallback;
    pos = line.find(':', pos);
    if (pos == std::string::npos) throw std::runtime_error("invalid number");
    ++pos;
    while (pos < line.size() && (std::isspace(static_cast<unsigned char>(line[pos])) || line[pos] == '"')) ++pos;
    char* end{};
    const float value = std::strtof(line.c_str() + pos, &end);
    if (end == line.c_str() + pos || !std::isfinite(value)) throw std::runtime_error("invalid finite number");
    return value;
}

bool Driven(PlayerCharacter* player)
{
    // ID 40586 / 1407559F0 reads and writes this bit.
    return player && (reinterpret_cast<const uint8_t*>(player)[0xBEA] & 8) != 0;
}

void* Controller(PlayerCharacter* player)
{
    // Actor getter 1406A51B0 returns the smart pointer at +150 in 1.7.104.
    void* controller{};
    if (player) std::memcpy(&controller, reinterpret_cast<uint8_t*>(player) + 0x150, sizeof(controller));
    return controller;
}

bool ControlsDriven(void* controller)
{
    if (!controller) return false;
    auto** table = *reinterpret_cast<void***>(controller);
    return reinterpret_cast<bool (*)(void*)>(table[0xF])(controller);
}

void SetMovementMode(void* controller, bool ai)
{
    // CommonLib MovementControllerNPC slots 0C/0D, IDs 41709/41710.
    // Do not set PlayerCharacter's quest AI bit: it revives MQ101's old package.
    auto** table = *reinterpret_cast<void***>(controller);
    reinterpret_cast<void (*)(void*)>(table[ai ? 0xC : 0xD])(controller);
}

void Release(PlayerCharacter* player)
{
    arrivalStillSince = 0;
    if (!ownsAI) return;
    if (player && Controller(player) == ownedController && (jumping || harnessNudge))
    {
        auto* direct = static_cast<uint8_t*>(ownedController) + 0x138;
        auto** table = *reinterpret_cast<void***>(direct);
        reinterpret_cast<void (*)(void*)>(table[8])(direct);
    }
    if (player && !Driven(player) && Controller(player) == ownedController && !ControlsDriven(ownedController))
    {
        using Stop = void(Actor*, float);
        POINTER_SKYRIMSE(Stop, stop, 37817);
        stop.Get()(player, 0.f);
        SetMovementMode(ownedController, false);
    }
    ownsAI = false;
    ownedController = nullptr;
    jumping = jumpIssued = jumpAirborne = false;
    harnessNudge = false;
    pathId = -1;
}

void Finish(PlayerCharacter* player, const char* result, const char* why)
{
    Release(player);
    active = false;
    harnessCombatTravel = false;
    harnessNudge = false;
    harnessArrival = false;
    state = result;
    reason = why;
    if (markerId)
    {
        if (auto* marker = Cast<TESObjectREFR>(TESForm::GetById(markerId)))
        {
            // The disabled, nonpersistent marker has no loaded 3D. Mark it
            // deleted through TESForm's native virtual; never call the latent
            // ObjectReference.Delete wrapper with a fabricated VM stack.
            auto** table = *reinterpret_cast<void***>(marker);
            reinterpret_cast<void (*)(TESForm*, bool)>(table[0x23])(marker, true);
        }
        markerId = 0;
    }
    spdlog::info("Intro driver: {} reason={} target={:X} distance={}", state, reason, targetId, distance);
}

float Distance(const NiPoint3& a, const NiPoint3& b)
{
    const float x = a.x - b.x, y = a.y - b.y, z = a.z - b.z;
    return std::sqrt(x*x + y*y + z*z);
}

int32_t RunPath(PlayerCharacter* player, TESObjectREFR* target)
{
    // Compose the same engine request as ID37893, with native run parameters
    // (ID37823, speed 2). ID37893 clamps its argument to 1, which only walks.
    using Construct = void*(void*);
    using Setup = void(Actor*, void**, const NiPoint3*, TESObjectCELL*, TESWorldSpace*, float, void*);
    using Parameters = void(Actor*, void**);
    using Submit = bool(Actor*, void**);
    using Worldspace = TESWorldSpace*(TESObjectREFR*);
    using Register = int32_t(void*, void*);
    POINTER_SKYRIMSE(Construct, construct, 30875);
    POINTER_SKYRIMSE(Setup, setup, 37820);
    POINTER_SKYRIMSE(Parameters, runParameters, 37823);
    POINTER_SKYRIMSE(Parameters, walkParameters, 37822);
    POINTER_SKYRIMSE(Submit, submit, 37801);
    POINTER_SKYRIMSE(Worldspace, worldspace, 19816);
    POINTER_SKYRIMSE(Register, registerPath, 91842);
    POINTER_SKYRIMSE(void*, manager, 403558);
    void* memory = Memory::Allocate(0x100);
    if (!memory) return -1;
    struct RequestRef
    {
        void* value;
        ~RequestRef()
        {
            if (value && InterlockedDecrement(reinterpret_cast<volatile LONG*>(static_cast<uint8_t*>(value) + 8)) == 0)
            {
                auto** table = *reinterpret_cast<void***>(value);
                reinterpret_cast<void (*)(void*, bool)>(table[0])(value, true);
            }
        }
    } request{construct.Get()(memory)};
    InterlockedIncrement(reinterpret_cast<volatile LONG*>(static_cast<uint8_t*>(request.value) + 8));
    // Aim inside the requested 3D completion sphere: native stopping tolerance
    // and target height can otherwise leave an ended path just outside it
    // (observed 65.3 vs 64). 35 is the native PathToReference default.
    const float nativeRadius = (std::min)(35.f, radius * 0.5f);
    setup.Get()(player, &request.value, &target->position, target->GetParentCellEx(), worldspace.Get()(target), nativeRadius, nullptr);
    (walkPace ? walkParameters.Get() : runParameters.Get())(player,
        reinterpret_cast<void**>(static_cast<uint8_t*>(request.value) + 0xF8));
    if (!submit.Get()(player, &request.value)) return -1;
    return registerPath.Get()(*manager.Get(), request.value);
}

// nullptr when the player can jump now, else the first failed check (named in the driver's refusal).
const char* JumpBlocker(PlayerCharacter* player)
{
    using InJump = bool(Actor*);
    POINTER_SKYRIMSE(InJump, inJump, 37949);
    const auto actorFlags = player->actorState.flags1;
    if (actorFlags & ((1 << 8) | (1 << 10))) return "actor state bit 8/10";
    if (actorFlags & (0xF << 14)) return "sitting or sleeping";
    if (actorFlags & (7 << 18)) return "flying state";
    if (actorFlags & (0xF << 21)) return "life state not alive";
    if (actorFlags & (7 << 25)) return "knock state";
    if (player->actorState.flags2 & (1 << 13)) return "actor state2 bit 13";
    if (inJump.Get()(player)) return "already jumping";
    using GetController = void*(Actor*);
    POINTER_SKYRIMSE(GetController, getController, 37258);
    auto* controller = static_cast<uint8_t*>(getController.Get()(player));
    if (!controller) return "no character controller";
    uint32_t flags{};
    std::memcpy(&flags, controller + 0x218, sizeof(flags));
    return (flags & (1 << 10)) ? nullptr : "controller not supported (airborne or standing on something that is not ground)";
}

bool CanJump(PlayerCharacter* player)
{
    return JumpBlocker(player) == nullptr;
}

bool ReadJumpState(PlayerCharacter* player)
{
    // 37258 returns the controller. 78280 / 141065F00 transfers wantState
    // at +21C into context.currentState at +200; these are not CanJump flags.
    using GetController = void*(Actor*);
    POINTER_SKYRIMSE(GetController, getController, 37258);
    auto* controller = player ? static_cast<uint8_t*>(getController.Get()(player)) : nullptr;
    jumpState = jumpRequestedState = UINT32_MAX;
    if (!controller) return false;
    std::memcpy(&jumpState, controller + 0x200, sizeof(jumpState));
    std::memcpy(&jumpRequestedState, controller + 0x21C, sizeof(jumpRequestedState));
    return true;
}

bool SameSpace(PlayerCharacter* player, TESObjectREFR* target)
{
    auto* a = player->GetParentCellEx();
    auto* b = target->GetParentCellEx();
    return a && b && (a == b || (a->worldspace && a->worldspace == b->worldspace));
}

TESObjectREFR* CoordinateTarget(PlayerCharacter* player, TESObjectCELL* cell, const std::string& request)
{
    NiPoint3 point{};
    point.x = Number(request, "x", NAN);
    point.y = Number(request, "y", NAN);
    point.z = Number(request, "z", NAN);
    if (!std::isfinite(point.x) || !std::isfinite(point.y) || !std::isfinite(point.z)) throw std::runtime_error("form_id or x,y,z required");
    if (Distance(player->position, point) > 8192.f) throw std::runtime_error("coordinate target exceeds local 8192-unit range");
    // A disabled XMarker in this same cell provides a native reference target.
    // Recreate it for recovery: Finish deleted the previous path's marker.
    using ObjectReference = TESObjectREFR;
    PAPYRUS_FUNCTION(TESObjectREFR*, ObjectReference, PlaceAtMe, TESForm*, int32_t, bool, bool);
    auto* target = s_pPlaceAtMe ? s_pPlaceAtMe(player, TESForm::GetById(0x3B), 1, false, true) : nullptr;
    if (target) { markerId = target->formID; target->MoveTo(cell, point); }
    return target;
}

TESObjectREFR* Objective(PlayerCharacter* player)
{
    TESObjectREFR* selected{};
    float nearest = FLT_MAX;
    // The driver follows one named quest or the two intro quests. Discover
    // those forms once in 32-entry slices, never a full quest scan per event.
    static const void* questData{};
    static uint32_t questLength{}, questCursor{};
    static std::string cachedFilter;
    static std::array<uint32_t, 2> resolved{};
    auto* mods = ModManager::Get();
    if (!mods) return nullptr;
    if (questData != mods->quests.data || questLength != mods->quests.length || cachedFilter != questFilter)
    {
        questData = mods->quests.data; questLength = mods->quests.length;
        cachedFilter = questFilter; questCursor = 0; resolved = {};
    }
    for (unsigned budget = 0; questCursor < questLength && budget < 32; ++budget)
    {
        auto* quest = mods->quests[questCursor++];
        if (!quest) continue;
        const std::string_view name = quest->idName.AsAscii();
        if (name == (questFilter.empty() ? "MQ101" : questFilter)) resolved[0] = quest->formID;
        if (questFilter.empty() && name == "MQ101DragonAttack") resolved[1] = quest->formID;
        if (resolved[0] && (!questFilter.empty() || resolved[1])) { questCursor = questLength; break; }
    }
    if (questCursor < questLength) return nullptr;
    unsigned objectiveBudget = 64, targetBudget = 128;
    for (auto id : resolved)
    {
        auto* quest = id ? Cast<TESQuest>(TESForm::GetById(id)) : nullptr;
        if (!quest || quest->IsStopped() || !quest->IsActive()) continue;
        const std::string_view name = quest->idName.AsAscii();
        if (!questFilter.empty() ? name != questFilter : (name != "MQ101" && name != "MQ101DragonAttack")) continue;
        for (auto* objective : quest->objectives)
        {
            if (!objectiveBudget--) throw std::runtime_error("objective_lookup_budget_exceeded");
            if (!objective || objective->state != 1) continue;
            void** targets{};
            uint32_t count{};
            std::memcpy(&targets, objective->pad10, sizeof(targets));
            std::memcpy(&count, objective->pad10 + 8, sizeof(count));
            if (count > 128) continue;
            using GetTarget = uint32_t*(void*, uint32_t*, bool, const TESQuest*);
            POINTER_SKYRIMSE(GetTarget, getTarget, 25284);
            for (uint32_t i = 0; targets && i < count; ++i)
            {
                if (!targetBudget--) throw std::runtime_error("objective_target_budget_exceeded");
                if (!targets[i]) continue;
                uint32_t handle{};
                getTarget.Get()(targets[i], &handle, false, quest);
                auto* ref = TESObjectREFR::GetByHandle(handle);
                if (!ref || ref == player || !SameSpace(player, ref)) continue;
                // Match native objective enumeration 1403DD6B0: resolve the
                // alias, set quest condition context, then evaluate target+8.
                // A displayed objective can retain several inactive targets.
                alignas(8) std::array<uint8_t, 0x38> check{};
                std::memcpy(check.data(), &ref, sizeof(ref));
                using Context = void(void*, TESForm*);
                using Evaluate = bool(void*, void*);
                POINTER_SKYRIMSE(Context, setContext, 29872);
                POINTER_SKYRIMSE(Evaluate, evaluate, 29889);
                setContext.Get()(check.data(), quest);
                if (!evaluate.Get()(static_cast<uint8_t*>(targets[i]) + 8, check.data())) continue;
                // The ordered host scenario selects a fork only among native,
                // displayed, condition-valid targets. Ordinary driving is unchanged.
                if (harnessObjectiveRef && HarnessService::IsEnabled() && HarnessService::OwnsDriver() &&
                    objective->stageId == harnessObjectiveIndex && ref->formID != harnessObjectiveRef) continue;
                const float d = Distance(player->position, ref->position);
                if (d >= nearest) continue;
                selected = ref;
                nearest = d;
                questId = quest->formID;
                objectiveId = objective->stageId;
            }
        }
    }
    return selected;
}

void Tick()
{
    const auto now = GetTickCount64();
    std::string request;
    { std::scoped_lock lock(mutex); request.swap(pending); }
    if (request.empty() && now < nextTick) return;
    nextTick = now + 100;
    auto* player = PlayerCharacter::Get();
    auto* ui = UI::Get();
    auto* cell = player ? player->GetParentCellEx() : nullptr;
    const bool creatorOpen = ui && ui->GetMenuOpen(BSFixedString("RaceSex Menu"));
    const bool loading = !ui || ui->GetMenuOpen(BSFixedString("Loading Menu")) || ui->GetMenuOpen(BSFixedString("Main Menu"));
    POINTER_SKYRIMSE(void*, controlMap, 400863);
    uint32_t controls{};
    if (auto* map = *controlMap.Get()) std::memcpy(&controls, static_cast<uint8_t*>(map) + 0x120, sizeof(controls));
    try
    {
        const auto command = GetJsonString(request, "command");
        if (command == "creator_finish")
        {
            const auto name = GetJsonString(request, "name");
            if (!creatorOpen || !player) creator = "failed: creator not open";
            else if (CreatorTogether::IsDone()) creator = "already_done: existing name retained";
            else if (ui->GetMenuOpen(BSFixedString("MessageBoxMenu"))) creator = "failed: dismiss existing message box first";
            else
            {
                auto* menu = ui->FindMenuByName(BSFixedString("RaceSex Menu"));
                using ChangeName = void(void*, const char*);
                POINTER_SKYRIMSE(ChangeName, changeName, 52415);
                if (!menu) creator = "failed: menu unavailable";
                else
                {
                    changeName.Get()(menu, name.c_str());
                    creator = CreatorTogether::IsDone() ? "done_held" : "close_requested";
                    spdlog::info("Intro driver: creator_finish native ChangeName name={} held={}", name, CreatorTogether::IsDone());
                }
            }
        }
        else if (command == "walk_cancel") Finish(player, "cancelled", "requested");
        else if (command == "jump_toward" || command == "walk_nudge")
        {
            const bool nudge = command == "walk_nudge";
            if (nudge && !(HarnessService::IsEnabled() && HarnessService::OwnsDriver()))
                throw std::runtime_error("walk_nudge requires active test harness");
            if (active) Finish(player, "cancelled", "replaced by jump_toward");
            auto form = GetJsonString(request, "form_id");
            auto* target = form.empty() ? nullptr : Cast<TESObjectREFR>(TESForm::GetById(std::stoul(form, nullptr, 16)));
            if (nudge && form.empty() && player && cell && !loading && !creatorOpen)
                target = CoordinateTarget(player, cell, request);
            // Harness travel may jump during the dragon attack (the player is in combat then), like walk_to.
            const bool combatAllowed = nudge || (GetJsonString(request, "allow_combat") == "true" &&
                HarnessService::IsEnabled() && HarnessService::OwnsDriver());
            const char* refusal = !player || !cell || !player->GetNiNode() ? "player not loaded" :
                loading || creatorOpen ? "loading or creator open" :
                !target || !SameSpace(player, target) ? "target missing or in another space" :
                Driven(player) || !ControlsDriven(Controller(player)) ? "player AI driven by another system" :
                !(controls & 1) || !(controls & (1 << 10)) ? "movement or jumping controls disabled" :
                !combatAllowed && player->IsInCombat() ? "player in combat" :
                player->actorState.IsDeadState() ? "player dead" :
                player->GetNativeMountFormId() ? "player mounted" :
                !CanJump(player) ? "player cannot jump now (airborne or settling)" : nullptr;
            const std::string blocker = refusal && player && !CanJump(player) ? std::string(" [") + JumpBlocker(player) + "]" : std::string();
            if (refusal)
            {
                const auto message = fmt::format("jump requires an eligible free player and a local target: {}{}", refusal, blocker);
                Finish(player, "failed", message.c_str());
            }
            else if (reinterpret_cast<uint8_t*>(Controller(player))[0x1C6])
                Finish(player, "failed", "native direct movement already owned");
            else if (Distance(player->position, target->position) > 1200.f)
                Finish(player, "failed", "jump target exceeds 1200-unit range");
            else
            {
                targetId = target->formID;
                startCell = cell->formID;
                started = lastProgress = jumpStart = now;
                progressPoint = player->position;
                ownedController = Controller(player);
                SetMovementMode(ownedController, true);
                ownsAI = active = true;
                jumping = !nudge;
                harnessNudge = nudge;
                radius = nudge ? 35.f : std::clamp(Number(request, "radius", 64.f), 8.f, 256.f);
                walkPace = nudge;
                jumpIssued = jumpAirborne = false;
                jumpIssuedAt = 0;
                ReadJumpState(player);
                auto* direct = static_cast<uint8_t*>(ownedController) + 0x138;
                auto** table = *reinterpret_cast<void***>(direct);
                NiPoint3 direction{};
                jumpHeading = std::atan2(target->position.x - player->position.x, target->position.y - player->position.y);
                // The direct handler consumes actor-relative movement angles,
                // like PlayerControls, not the planner's world-facing angles.
                direction.z = std::remainder(jumpHeading - player->rotation.z, 6.283185307f);
                reinterpret_cast<void (*)(void*, const NiPoint3*)>(table[2])(direct, &direction);
                reinterpret_cast<void (*)(void*, float)>(table[3])(direct, nudge ? 1.f : 2.f);
                spdlog::info("Intro driver: {} target={:X} worldHeading={} facing={} relativeHeading={} speed={} position={},{},{}", nudge ? "walk nudge" : "jump runup", targetId,
                    jumpHeading, player->rotation.z, direction.z, nudge ? 1.f : 2.f, player->position.x, player->position.y, player->position.z);
                state = nudge ? "walk_nudge" : "jump_runup";
                reason.clear();
            }
        }
        else if (command == "walk_to" || command == "follow_objective")
        {
            questFilter = GetJsonString(request, "quest");
            harnessObjectiveRef = 0;
            harnessObjectiveIndex = 0;
            if (command == "follow_objective" && HarnessService::IsEnabled() && HarnessService::OwnsDriver())
            {
                const auto preferred = GetJsonString(request, "objective_ref");
                if (!preferred.empty())
                {
                    const auto index = Number(request, "objective_id", -1.f);
                    if (index < 0 || index > UINT16_MAX || std::floor(index) != index)
                        throw std::runtime_error("objective_id must be 0..65535");
                    harnessObjectiveIndex = static_cast<uint16_t>(index);
                    harnessObjectiveRef = static_cast<uint32_t>(std::stoul(preferred, nullptr, 16));
                }
            }
            const auto pace = GetJsonString(request, "pace");
            if (!pace.empty() && pace != "run" && pace != "walk") throw std::runtime_error("pace must be run or walk");
            const bool requestedWalk = pace == "walk";
            if (command == "walk_to" && active) Finish(player, "cancelled", "replaced by walk_to");
            if (active && requestedWalk != walkPace) Finish(player, "cancelled", "requested pace changed");
            if (command == "follow_objective" && active && player)
            {
                if (auto* next = Objective(player); next && next->formID != targetId)
                    Finish(player, "cancelled", "active objective changed");
            }
            // Repeated follow requests are idempotent while a path is running.
            // Re-resolve on the next leg, preserving the chosen fork for this leg.
            harnessCombatTravel = HarnessService::IsEnabled() && HarnessService::OwnsDriver() &&
                GetJsonString(request, "allow_combat") == "true";
            if (!active)
            {
                walkPace = requestedWalk;
                reason.clear();
                questId = objectiveId = 0;
                targetId = 0;
                radius = Number(request, "radius", 64.f);
                if (radius < 8.f || radius > 1024.f) throw std::runtime_error("radius must be 8..1024");
                if (!player || !cell || !player->currentProcess || !player->GetNiNode() || loading || creatorOpen)
                    Finish(player, "waiting", "player or world unavailable");
                else if (Driven(player) || !ControlsDriven(Controller(player)) || !(controls & 1))
                    Finish(player, "waiting", "vanilla or co-op still owns player controls");
                else
                {
                    TESObjectREFR* target{};
                    if (command == "follow_objective") target = Objective(player);
                    else if (auto form = GetJsonString(request, "form_id"); !form.empty())
                        target = Cast<TESObjectREFR>(TESForm::GetById(std::stoul(form, nullptr, 16)));
                    else
                        target = CoordinateTarget(player, cell, request);
                    if (!target) Finish(player, "waiting", "no resolved active objective or reference");
                    else if (!SameSpace(player, target)) Finish(player, "failed", "target is across cells; approach a local load door first");
                    else
                    {
                        targetId = target->formID;
                        startCell = cell->formID;
                        bestDistance = distance = Distance(player->position, target->position);
                        progressPoint = player->position;
                        started = lastProgress = now;
                        harnessArrival = HarnessService::IsEnabled() && HarnessService::OwnsDriver();
                        arrivalStillSince = 0;
                        clearCombatSince = 0;
                        active = true;
                        state = "starting";
                    }
                }
            }
        }
        if (active)
        {
            auto* target = Cast<TESObjectREFR>(TESForm::GetById(targetId));
            if (!player || !cell || loading || creatorOpen || !player->currentProcess)
                Finish(player, "interrupted", "world or menu transition");
            else if (cell->formID != startCell && !cell->worldspace)
                Finish(player, "transitioned", "native interior cell transition");
            else if (!target || !SameSpace(player, target)) Finish(player, "failed", "target unloaded or changed space");
            else if (player->actorState.IsDeadState()) Finish(player, "failed", "player died");
            else if (Driven(player)) Finish(player, "interrupted", "script acquired quest AI control");
            else if (harnessArrival && (!(controls & 1) || !HarnessService::IsEnabled() || !HarnessService::OwnsDriver()))
                Finish(player, "interrupted", "harness arrival lost control authorization");
            else if (ownsAI && (Controller(player) != ownedController || ControlsDriven(ownedController)))
                Finish(player, "interrupted", "movement controller replaced or released");
            else if (now - started > 180000) Finish(player, "failed", "180-second path timeout");
            else
            {
                distance = Distance(player->position, target->position);
                bool settledArrival = false;
                if (harnessArrival && ownsAI && !jumping && !harnessNudge && distance <= radius)
                {
                    if (!arrivalStillSince || Distance(player->position, arrivalPoint) > 2.f ||
                        Distance(target->position, arrivalTargetPoint) > 2.f)
                    {
                        arrivalStillSince = now;
                        arrivalPoint = player->position;
                        arrivalTargetPoint = target->position;
                    }
                    settledArrival = now - arrivalStillSince >= 1000;
                }
                else arrivalStillSince = 0;
                if (jumping || harnessNudge)
                {
                    auto* direct = static_cast<uint8_t*>(ownedController) + 0x138;
                    auto** table = *reinterpret_cast<void***>(direct);
                    NiPoint3 direction{};
                    direction.z = std::remainder(jumpHeading - player->rotation.z, 6.283185307f);
                    reinterpret_cast<void (*)(void*, const NiPoint3*)>(table[2])(direct, &direction);
                    if (harnessNudge)
                    {
                        if (!HarnessService::IsEnabled() || !HarnessService::OwnsDriver() || !(controls & 1))
                            Finish(player, "interrupted", "harness nudge lost control authorization");
                        else if (distance <= radius || now - jumpStart >= 1200)
                            Finish(player, "nudge_finished", "bounded collision-enabled walking interval ended");
                    }
                    else if (player->IsInCombat() || !(controls & 1)) Finish(player, "interrupted", "jump interrupted by combat or controls");
                    else if (!jumpIssued && now - jumpStart >= 250)
                    {
                        if (!CanJump(player)) Finish(player, "failed", "native jump eligibility lost during runup");
                        else
                        {
                            using Jump = void(Actor*);
                            POINTER_SKYRIMSE(Jump, jump, 37257);
                            jump.Get()(player);
                            jumpIssued = true;
                            jumpIssuedAt = now;
                            ReadJumpState(player);
                            state = "jumping";
                            spdlog::info("Intro driver: native jump toward {:X} controllerState={} requestedState={}", targetId, jumpState, jumpRequestedState);
                            // 78262 synchronously writes wantState=1 when reached,
                            // but 78280 consumes it. This sample is diagnostic;
                            // actual target proximity qualifies the harness step.
                        }
                    }
                    else if (jumpIssued)
                    {
                        if (!ReadJumpState(player)) Finish(player, "failed", "native_jump_controller_unavailable");
                        else
                        {
                            // Native state types: OnGround=0, Jumping=1, InAir=2.
                            // State transitions are not a collision manifold sample.
                            if (jumpState == 1 || jumpState == 2) jumpAirborne = true;
                            const bool harnessApproach = HarnessService::IsEnabled() && HarnessService::OwnsDriver();
                            if (harnessApproach && jumpAirborne && jumpState == 0 && distance > radius)
                                state = "jump_landing_approach";
                            // A roof-edge landing can precede the floor-level target.
                            // Keep collision-enabled travel bounded and require actual proximity.
                            if (jumpAirborne && jumpState == 0 && now - jumpIssuedAt >= 250 &&
                                (!harnessApproach || distance <= radius))
                                Finish(player, "jump_landed", "native controller returned to OnGround");
                            else if (now - jumpIssuedAt >= (harnessApproach ? 4000 : 2000))
                                Finish(player, "jump_finished", jumpAirborne ?
                                    "native movement interval ended; gravity and collision remain active" :
                                    "native movement interval ended; air state not observed in samples");
                        }
                    }
                }
                else if (distance <= radius && (!harnessArrival || distance <= (std::min)(35.f, radius * 0.5f) || settledArrival))
                    Finish(player, "arrived", !harnessArrival ? "within radius" : settledArrival ? "arrived_settled" : "arrived_inner");
                else if (player->IsInCombat() && !(harnessCombatTravel && HarnessService::IsEnabled() && HarnessService::OwnsDriver()))
                {
                    Release(player);
                    clearCombatSince = 0;
                    lastProgress = now;
                    state = "waiting_combat";
                }
                else if (!(controls & 1)) Finish(player, "interrupted", "script or co-op movement hold");
                else
                {
                    // PathToReference copies the destination. Complete each
                    // engine leg before pursuing an actor that has moved away.
                    if (ownsAI && Distance(player->position, submittedPoint) <= radius &&
                        Distance(submittedPoint, target->position) > radius)
                    {
                        Release(player);
                        clearCombatSince = now;
                        bestDistance = distance;
                        lastProgress = now;
                        state = "retargeting";
                    }
                    if (!clearCombatSince) clearCombatSince = now;
                    if (!ownsAI && now - clearCombatSince >= 1000)
                    {
                        if (Driven(player) || !ControlsDriven(Controller(player))) Finish(player, "interrupted", "another system acquired AI control");
                        else
                        {
                            ownedController = Controller(player);
                            SetMovementMode(ownedController, true);
                            ownsAI = true;
                            submittedPoint = target->position;
                            pathId = RunPath(player, target);
                            arrivalStillSince = 0;
                            state = "walking";
                            lastProgress = now;
                            if (pathId == -1) Finish(player, "failed", "native path request rejected");
                            else spdlog::info("Intro driver: native path={} target={:X} distance={} radius={} pace={}", pathId, targetId, distance, radius, walkPace ? "walk" : "run");
                        }
                    }
                    if (Distance(player->position, progressPoint) >= 16.f)
                    {
                        progressPoint = player->position;
                        lastProgress = now;
                    }
                    // Look where a person walking there would: eye level toward the target, tilted only
                    // for height difference (stairs, ramps), eased so the view never snaps. Path steering
                    // turns heading only and would otherwise keep any earlier look-up/down pitch.
                    // Measured 2026-09-30 13:11:46: starting the native path pulls the player's pitch from -3 to 84
                    // degrees within 120 ms (not our code). Hold our own pitch from level and write it every tick.
                    static bool s_pitchHeld{};
                    static float s_heldPitch{};
                    static uint64_t s_lastPitchMs{};
                    if (active && ownsAI)
                    {
                        if (!s_pitchHeld) { s_pitchHeld = true; s_heldPitch = 0.f; s_lastPitchMs = now; }
                        // About 90 degrees per second, by elapsed time (the driver tick rate varies).
                        const float maxStep = std::clamp((now - s_lastPitchMs) * 0.0016f, 0.f, 0.1f);
                        s_lastPitchMs = now;
                        const float horizontal = std::hypot(target->position.x - player->position.x, target->position.y - player->position.y);
                        const float wanted = std::clamp(std::atan2(player->position.z - target->position.z, std::max(horizontal, 64.f)), -0.5f, 0.5f);
                        if (std::isfinite(wanted))
                            s_heldPitch += std::clamp(wanted - s_heldPitch, -maxStep, maxStep);
                        if (std::abs(player->rotation.x - s_heldPitch) > 0.001f)
                            player->SetRotation(s_heldPitch, player->rotation.y, player->rotation.z);
                    }
                    else s_pitchHeld = false;
                    if (active && now - lastProgress > 20000) Finish(player, "failed", "no path progress for 20 seconds");
                }
            }
        }
    }
    catch (const std::exception& e) { Finish(player, "failed", e.what()); }
    // Owner saw test players' views snap straight up or down: log any big pitch jump with the driver state.
    if (player)
    {
        static float s_lastPitch{};
        const float pitch = player->rotation.x;
        if (std::isfinite(pitch) && std::abs(pitch - s_lastPitch) > 0.35f)
            spdlog::info("Intro driver: pitch jump {:.0f} -> {:.0f} deg state={} active={} ownsAI={} aiDriven={} target={:X}",
                s_lastPitch * 57.2958f, pitch * 57.2958f, state, active, ownsAI, Driven(player), targetId);
        s_lastPitch = pitch;
    }
    const auto json = fmt::format("\"state\":\"{}\",\"reason\":\"{}\",\"active\":{},\"ownsAI\":{},\"aiDriven\":{},\"creator\":\"{}\",\"creatorOpen\":{},\"creatorDone\":{},\"movementEnabled\":{},\"cellId\":{},\"targetId\":{},\"questId\":{},\"objectiveId\":{},\"distance\":{},\"radius\":{},\"pathId\":{},\"elapsedMs\":{},\"sampleMs\":{}",
        state, EscapeJson(reason), JsonBool(active), JsonBool(ownsAI), JsonBool(Driven(player)), EscapeJson(creator), JsonBool(creatorOpen), JsonBool(CreatorTogether::IsDone()), JsonBool((controls & 1) != 0), cell ? cell->formID : 0, targetId, questId, objectiveId, distance, radius, pathId, active ? now - started : 0, now);
    {
        std::scoped_lock lock(mutex);
        // A window request can arrive while this tick processes its predecessor.
        if (!request.empty()) ++appliedSequence;
        published = json + fmt::format(",\"appliedSequence\":{},\"pace\":\"{}\",\"combat\":{},\"harnessCombatTravel\":{},\"harnessNudge\":{},\"travelReady\":{}", appliedSequence, walkPace ? "walk" : "run", JsonBool(player && player->IsInCombat()), JsonBool(harnessCombatTravel), JsonBool(harnessNudge),
            JsonBool(player && cell && player->currentProcess && player->GetNiNode() && !loading && !creatorOpen &&
                !Driven(player) && ControlsDriven(Controller(player)) && (controls & 1)));
        published += fmt::format(",\"jumpControllerState\":{},\"jumpRequestedState\":{},\"jumpIssuedAtMs\":{}", jumpState, jumpRequestedState, jumpIssuedAt);
    }
}
}

bool HandlerEnabled(const PlayerInputHandler* apHandler)
{
    return apHandler && apHandler->isEnabled;
}

constexpr uint64_t cFnvOffsetBasis = 14695981039346656037ull;
constexpr uint64_t cFnvPrime = 1099511628211ull;

void HashWord(uint64_t& arHash, uint32_t aWord) noexcept
{
    for (uint32_t shift = 0; shift < 32; shift += 8)
        arHash = (arHash ^ ((aWord >> shift) & 0xFFu)) * cFnvPrime;
}

struct SceneActionDiagnosticView
{
    void* Vtable{};
    uint32_t ActorId{};
    uint16_t StartPhase{};
    uint16_t EndPhase{};
    uint32_t Flags{};
    uint8_t Unknown14[4]{};
    uint32_t ActionId{};
    uint32_t Unknown1C{};
};
static_assert(offsetof(SceneActionDiagnosticView, ActionId) == 0x18);
static_assert(sizeof(SceneActionDiagnosticView) == 0x20);

// Pages already proven readable during the current audit snapshot. ReadNative queried
// VirtualQuery for every field and every string byte, so one world audit made hundreds of
// thousands of syscalls and held the game thread 2 s (host) to 4.6 s (follower). Only active
// inside ReadablePageCacheScope and cleared per snapshot, so a page is validated once per audit.
struct ReadablePageCache
{
    bool Active{};
    std::array<uintptr_t, 256> Pages{};
    uint32_t Count{};
    uint32_t Next{};
};
thread_local ReadablePageCache s_readablePages;

struct ReadablePageCacheScope
{
    ReadablePageCacheScope() noexcept { s_readablePages = {}; s_readablePages.Active = true; }
    ~ReadablePageCacheScope() noexcept { s_readablePages = {}; }
};

bool IsCachedReadablePage(uintptr_t aPage) noexcept
{
    for (uint32_t i = 0; i < s_readablePages.Count; ++i)
        if (s_readablePages.Pages[i] == aPage)
            return true;
    return false;
}

bool IsReadableRange(const void* apData, size_t aSize) noexcept
{
    if (!apData || aSize == 0)
        return false;

    const auto begin = reinterpret_cast<uintptr_t>(apData);
    if (begin > std::numeric_limits<uintptr_t>::max() - aSize)
        return false;
    const auto end = begin + aSize;

    constexpr uintptr_t cPageMask = ~uintptr_t{0xFFF};
    const bool cached = s_readablePages.Active;
    if (cached && ((end - 1) & cPageMask) - (begin & cPageMask) <= 0x1000 &&
        IsCachedReadablePage(begin & cPageMask) && IsCachedReadablePage((end - 1) & cPageMask))
        return true;

    auto cursor = begin;
    while (cursor < end)
    {
        MEMORY_BASIC_INFORMATION information{};
        if (!VirtualQuery(reinterpret_cast<const void*>(cursor), &information, sizeof(information)))
            return false;
        if (information.State != MEM_COMMIT ||
            (information.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0)
            return false;
        switch (information.Protect & 0xFF)
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

        const auto regionBegin = reinterpret_cast<uintptr_t>(information.BaseAddress);
        if (regionBegin > std::numeric_limits<uintptr_t>::max() - information.RegionSize)
            return false;
        const auto regionEnd = regionBegin + information.RegionSize;
        if (regionEnd <= cursor)
            return false;
        cursor = (std::min)(end, regionEnd);
    }
    if (cached)
    {
        for (auto page = begin & cPageMask; page <= ((end - 1) & cPageMask); page += 0x1000)
        {
            if (IsCachedReadablePage(page))
                continue;
            s_readablePages.Pages[s_readablePages.Next] = page;
            s_readablePages.Next = (s_readablePages.Next + 1) % s_readablePages.Pages.size();
            s_readablePages.Count = (std::min)(s_readablePages.Count + 1,
                static_cast<uint32_t>(s_readablePages.Pages.size()));
        }
    }
    return true;
}

template <class T> bool ReadNative(const void* apData, T& arValue) noexcept
{
    if (!IsReadableRange(apData, sizeof(T)))
        return false;
    std::memcpy(&arValue, apData, sizeof(T));
    return true;
}

uint32_t ReadProcessHandle(const AIProcess* apProcess, size_t aOffset) noexcept
{
    uint32_t handle{};
    if (apProcess)
        ReadNative(reinterpret_cast<const uint8_t*>(apProcess) + aOffset, handle);
    return handle;
}

uint32_t ResolveHandleFormId(uint32_t aHandle) noexcept
{
    if (!aHandle || aHandle == UINT32_MAX)
        return 0;
    const auto* pReference = TESObjectREFR::GetByHandle(aHandle);
    return pReference ? pReference->formID : 0;
}

bool TryReadPackedGraphInput(const AnimationGraphDescriptor& acDescriptor,
    const AnimationVariables& acVariables, uint32_t aIndex,
    uint32_t& arRawValue) noexcept
{
    const auto boolean = std::find(acDescriptor.BooleanLookUpTable.begin(),
        acDescriptor.BooleanLookUpTable.end(), aIndex);
    if (boolean != acDescriptor.BooleanLookUpTable.end())
    {
        const auto offset = std::distance(acDescriptor.BooleanLookUpTable.begin(), boolean);
        if (offset >= static_cast<ptrdiff_t>(acVariables.Booleans.size()))
            return false;
        arRawValue = acVariables.Booleans[offset] ? 1u : 0u;
        return true;
    }
    const auto integer = std::find(acDescriptor.IntegerLookupTable.begin(),
        acDescriptor.IntegerLookupTable.end(), aIndex);
    if (integer != acDescriptor.IntegerLookupTable.end())
    {
        const auto offset = std::distance(acDescriptor.IntegerLookupTable.begin(), integer);
        if (offset >= static_cast<ptrdiff_t>(acVariables.Integers.size()))
            return false;
        arRawValue = acVariables.Integers[offset];
        return true;
    }
    const auto floating = std::find(acDescriptor.FloatLookupTable.begin(),
        acDescriptor.FloatLookupTable.end(), aIndex);
    if (floating == acDescriptor.FloatLookupTable.end())
        return false;
    const auto offset = std::distance(acDescriptor.FloatLookupTable.begin(), floating);
    if (offset >= static_cast<ptrdiff_t>(acVariables.Floats.size()))
        return false;
    std::memcpy(&arRawValue, &acVariables.Floats[offset], sizeof(arRawValue));
    return true;
}

std::string ReadNativeString(const char* apText, bool& arReadable, size_t aLimit = 256)
{
    arReadable = false;
    if (!apText)
    {
        arReadable = true;
        return {};
    }

    std::string result;
    result.reserve(64);
    for (size_t i = 0; i < aLimit; ++i)
    {
        char value{};
        if (!ReadNative(apText + i, value))
            return result;
        if (value == '\0')
        {
            arReadable = true;
            return result;
        }
        result.push_back(value);
    }
    return result;
}

// On-demand only. CommonLib NiAVObject/BSGeometry/NiSkinInstance/BipedAnim layouts,
// checked against 1.7.104: 14073A760 (Actor+268 biped), 140EFC810 (geometry skin
// +130 and bounds +E4), 140F09CB0 (skin data/root/bone transforms), 140217E30
// (42 biped entries, stride 78). No equip, update, cull, or reload calls here.
std::string DescribeActorVisuals(Actor* apActor)
{
    const auto readAt = [](const void* apBase, size_t aOffset, auto& arValue)
    {
        return apBase && ReadNative(static_cast<const uint8_t*>(apBase) + aOffset, arValue);
    };
    const auto floats = [](const auto& acValues)
    {
        std::string result = "[";
        for (const auto value : acValues)
        {
            if (result.size() > 1)
                result += ",";
            result += std::isfinite(value) ? fmt::format("{}", value) : "null";
        }
        return result + "]";
    };
    const auto formId = [&](const void* apForm)
    {
        uint32_t result{};
        readAt(apForm, 0x14, result);
        return result;
    };
    auto* pRoot = apActor->GetNiNode();
    std::string nodes = "[";
    std::vector<const NiAVObject*> visited;
    bool truncated = false;
    bool treeReadable = pRoot != nullptr;
    std::function<void(NiAVObject*, int, bool)> walk = [&](NiAVObject* apNode, int aDepth, bool aParentHidden)
    {
        if (!apNode || std::find(visited.begin(), visited.end(), apNode) != visited.end())
            return;
        if (aDepth > 24 || visited.size() >= 256)
        {
            truncated = true;
            return;
        }
        visited.push_back(apNode);
        const char* pName{};
        uint32_t flags{};
        std::array<float, 4> bound{}, world{};
        const bool readable = readAt(apNode, 0x10, pName) && readAt(apNode, 0xF4, flags) &&
            readAt(apNode, 0xE4, bound) && readAt(apNode, 0xA0, world);
        bool nameReadable{};
        const auto name = ReadNativeString(pName, nameReadable);
        const bool hidden = aParentHidden || (flags & 1) != 0;
        if (nodes.size() > 1)
            nodes += ",";
        nodes += fmt::format("{{\"address\":\"{:X}\",\"name\":\"{}\",\"depth\":{},\"readable\":{},"
            "\"flags\":{},\"appCulled\":{},\"inheritedAppCulled\":{},\"notVisible\":{},\"world\":{},\"bound\":{}",
            reinterpret_cast<uintptr_t>(apNode), EscapeJson(name), aDepth, JsonBool(readable && nameReadable),
            flags, JsonBool((flags & 1) != 0), JsonBool(hidden), JsonBool((flags & (1u << 20)) != 0),
            floats(world), floats(bound));
        if (!readable)
        {
            treeReadable = false;
            nodes += "}";
            return;
        }
        // NiRTTI has a name and parent pointer. Do not interpret NiNode memory
        // as a BSGeometry, or legacy NiGeometry skin offsets as BSGeometry ones.
        struct RttiView { const char* Name; const void* Parent; };
        const void* pType = apNode->GetRTTI();
        bool geometry = false;
        for (int i = 0; pType && i < 16; ++i)
        {
            RttiView type{};
            if (!ReadNative(pType, type))
                break;
            bool typeReadable{};
            if (ReadNativeString(type.Name, typeReadable) == "BSGeometry" && typeReadable)
            {
                geometry = true;
                break;
            }
            pType = type.Parent;
        }
        if (geometry)
        {
            const void* pSkin{}, *pShader{}, *pRenderer{}, *pData{}, *pPartition{}, *pSkinRoot{}, *pMatrices{};
            uint32_t boneCount{}, matrixCount{}, frame{};
            const void* const* pTransforms{};
            bool skinReadable = readAt(apNode, 0x130, pSkin) && readAt(apNode, 0x128, pShader) &&
                readAt(apNode, 0x138, pRenderer);
            if (pSkin)
                skinReadable = skinReadable && readAt(pSkin, 0x10, pData) && readAt(pSkin, 0x18, pPartition) &&
                    readAt(pSkin, 0x20, pSkinRoot) && readAt(pSkin, 0x30, pTransforms) &&
                    readAt(pSkin, 0x38, frame) && readAt(pSkin, 0x3C, matrixCount) &&
                    readAt(pSkin, 0x48, pMatrices) && readAt(pData, 0x58, boneCount);
            uint32_t nullTransforms{}, sampledTransforms{};
            if (pSkin && skinReadable && pTransforms && boneCount <= 512)
            {
                for (; sampledTransforms < boneCount; ++sampledTransforms)
                {
                    const void* pTransform{};
                    if (!ReadNative(pTransforms + sampledTransforms, pTransform))
                        break;
                    nullTransforms += pTransform == nullptr;
                }
            }
            nodes += fmt::format(",\"skin\":{{\"present\":{},\"readable\":{},\"address\":\"{:X}\","
                "\"data\":\"{:X}\",\"partition\":\"{:X}\",\"root\":\"{:X}\",\"boneCount\":{},"
                "\"sampledTransforms\":{},\"nullTransforms\":{},\"matrixCount\":{},\"matrices\":\"{:X}\","
                "\"frame\":{},\"shader\":\"{:X}\",\"renderer\":\"{:X}\"}}",
                JsonBool(pSkin != nullptr), JsonBool(skinReadable), reinterpret_cast<uintptr_t>(pSkin),
                reinterpret_cast<uintptr_t>(pData), reinterpret_cast<uintptr_t>(pPartition),
                reinterpret_cast<uintptr_t>(pSkinRoot), boneCount, sampledTransforms, nullTransforms,
                matrixCount, reinterpret_cast<uintptr_t>(pMatrices), frame,
                reinterpret_cast<uintptr_t>(pShader), reinterpret_cast<uintptr_t>(pRenderer));
        }
        nodes += "}";
        if (auto* pNode = apNode->AsNode())
        {
            const auto count = (std::min)(pNode->children.length, uint16_t{256});
            truncated |= pNode->children.length > count;
            for (uint16_t i = 0; i < count; ++i)
            {
                NiAVObject* pChild{};
                if (pNode->children.data && ReadNative(pNode->children.data + i, pChild))
                    walk(pChild, aDepth + 1, hidden);
                else
                    treeReadable = false;
            }
        }
    };
    bool parentHidden = false;
    auto* pParent = pRoot ? pRoot->parent : nullptr;
    for (int depth = 0; pParent && depth < 32; ++depth)
    {
        uint32_t flags{};
        if (!readAt(pParent, 0xF4, flags))
            break;
        parentHidden |= (flags & 1) != 0;
        NiAVObject* pNext{};
        if (!readAt(pParent, 0x30, pNext) || pNext == pParent)
            break;
        pParent = pNext;
    }
    walk(pRoot, 0, parentHidden);
    nodes += "]";
    static_assert(offsetof(Actor, actorWeightData) == 0x268);
    const void* pBiped{};
    const bool bipedReadable = ReadNative(&apActor->actorWeightData, pBiped);
    const NiAVObject* pBipedRoot{};
    const bool bipedRootReadable = readAt(pBiped, 8, pBipedRoot);
    const bool bipedRootInActorTree = pBipedRoot && std::find(visited.begin(), visited.end(), pBipedRoot) != visited.end();
    std::string slots = "[";
    for (uint32_t i = 0; pBiped && i < 42; ++i)
    {
        const auto* pEntry = static_cast<const uint8_t*>(pBiped) + 0x10 + i * 0x78;
        const void* pItem{}, *pAddon{}, *pClone{};
        uint8_t skinned{};
        const bool readable = readAt(pEntry, 0, pItem) && readAt(pEntry, 8, pAddon) &&
            readAt(pEntry, 0x20, pClone) && readAt(pEntry, 0x68, skinned);
        if (readable && !pItem && !pClone)
            continue;
        if (slots.size() > 1)
            slots += ",";
        const void* pCloneParent{};
        const bool cloneParentReadable = readAt(pClone, 0x30, pCloneParent);
        const bool cloneInActorTree = pClone &&
            std::find(visited.begin(), visited.end(), static_cast<const NiAVObject*>(pClone)) != visited.end();
        slots += fmt::format("{{\"index\":{},\"readable\":{},\"item\":{},\"addon\":{},"
            "\"clone\":\"{:X}\",\"skinned\":{},\"cloneParent\":\"{:X}\",\"cloneParentReadable\":{},\"cloneInActorTree\":{}}}",
            i, JsonBool(readable), formId(pItem), formId(pAddon), reinterpret_cast<uintptr_t>(pClone), JsonBool(skinned != 0),
            reinterpret_cast<uintptr_t>(pCloneParent), JsonBool(cloneParentReadable), JsonBool(cloneInActorTree));
    }
    const void* pBipedAfter{};
    const bool bipedUnchanged = ReadNative(&apActor->actorWeightData, pBipedAfter) && pBipedAfter == pBiped;
    const bool rootUnchanged = apActor->GetNiNode() == pRoot;
    return fmt::format("{{\"sampleTimeMs\":{},\"bipedReadable\":{},\"biped\":\"{:X}\","
        "\"bipedRoot\":\"{:X}\",\"bipedRootReadable\":{},\"bipedRootInActorTree\":{},"
        "\"slots\":{}],\"nodes\":{},\"truncated\":{},\"treeReadable\":{},\"treeComplete\":{},"
        "\"rootUnchanged\":{},\"bipedUnchanged\":{},\"nonAtomic\":true}}",
        GetTickCount64(), JsonBool(bipedReadable),
        reinterpret_cast<uintptr_t>(pBiped), reinterpret_cast<uintptr_t>(pBipedRoot), JsonBool(bipedRootReadable),
        JsonBool(bipedRootInActorTree), slots, nodes, JsonBool(truncated), JsonBool(treeReadable),
        JsonBool(treeReadable && !truncated), JsonBool(rootUnchanged), JsonBool(bipedUnchanged));
}

void HashBytes(uint64_t& arHash, const void* apData, size_t aSize) noexcept
{
    const auto* pBytes = static_cast<const uint8_t*>(apData);
    for (size_t i = 0; i < aSize; ++i)
    {
        arHash ^= pBytes[i];
        arHash *= cFnvPrime;
    }
}

template <class T> void HashValue(uint64_t& arHash, const T& acValue) noexcept
{
    HashBytes(arHash, std::addressof(acValue), sizeof(T));
}

bool HashQuantizedFloats(uint64_t& arHash, const float* apValues, size_t aCount) noexcept
{
    for (size_t i = 0; i < aCount; ++i)
    {
        const double value = apValues[i];
        if (!std::isfinite(value) || std::abs(value) > 100000000.0)
            return false;
        const auto quantized = static_cast<int64_t>(std::llround(value * 1000.0));
        HashValue(arHash, quantized);
    }
    return true;
}

template <class T>
bool ValidateHavokArray(const ActorPoseDiagnosticViews::HavokArray<T>& acArray,
    int32_t aMaximum) noexcept
{
    constexpr uint32_t cCapacityMask = 0x3FFFFFFF;
    if (acArray.size < 0 || acArray.size > aMaximum)
        return false;
    const auto capacity = static_cast<uint32_t>(acArray.capacityAndFlags) & cCapacityMask;
    if (capacity < static_cast<uint32_t>(acArray.size))
        return false;
    if (acArray.size == 0)
        return true;
    return acArray.data && IsReadableRange(acArray.data,
        static_cast<size_t>(acArray.size) * sizeof(T));
}

struct GraphManagerRef
{
    BSAnimationGraphManager* pointer{};
    ~GraphManagerRef()
    {
        if (pointer)
            pointer->Release();
    }
};

// Sample the actual local graph and rendered bone transforms. Network pose
// checksums only establish packet delivery; they cannot diagnose a frozen
// follower graph or a horse whose bones never reach the scene graph.
struct NativeActorAnimationSample
{
    bool GraphReady{};
    uint32_t GraphCount{};
    uint32_t GraphIndex{};
    uint32_t StateId{};
    float TimeInState{};
    bool RootClonePresent{};
    bool RootCloneSameAsTemplate{};
    bool BehaviorActive{};
    bool BehaviorLinked{};
    bool CloneStateReadable{};
    bool CloneStateActive{};
    int32_t CloneStateId{-1};
    int32_t ClonePreviousStateId{-1};
    float CloneTimeInState{};
    uint32_t PoseCount{};
    uint64_t PoseChecksum{};
    uint32_t RenderBoneCount{};
    uint64_t RenderBoneChecksum{};
    uint64_t RenderWorldBoneChecksum{};
    uint32_t GraphVariableCount{};
    uint64_t GraphVariableChecksum{};
    std::vector<uint32_t> GraphVariables;
    uint32_t GraphVariableNameCount{};
    uint32_t GraphVariableInfoCount{};
    std::vector<std::string> GraphVariableNames;
};

NativeActorAnimationSample SampleNativeActorAnimation(Actor* apActor,
    bool aIncludeVariableNames = false)
{
    using namespace ActorPoseDiagnosticViews;
    NativeActorAnimationSample result;
    GraphManagerRef manager;
    if (!apActor || !apActor->animationGraphHolder.GetBSAnimationGraph(&manager.pointer) ||
        !manager.pointer)
        return result;
    BSScopedLock<BSRecursiveLock> graphLock(manager.pointer->lock);
    const auto count = manager.pointer->animationGraphs.size;
    const auto index = manager.pointer->animationGraphIndex;
    result.GraphCount = count;
    result.GraphIndex = index;
    if (!count || count > 32 || index >= count)
        return result;
    AnimationGraph graph{};
    if (!ReadNative(manager.pointer->animationGraphs.Get(index), graph))
        return result;
    result.GraphReady = true;
    if (graph.behaviorGraph && aIncludeVariableNames)
    {
        // Read the active graph's own string table. The player has separate
        // third- and first-person graphs, so graph-0 descriptors cannot name
        // graph-1 slots. Keep this diagnostic bounded and read-only.
        const auto readSafe = [](const void* apSource, void* apTarget,
            size_t aSize) noexcept {
            SIZE_T copied{};
            return apSource && apTarget && aSize &&
                ReadProcessMemory(GetCurrentProcess(), apSource, apTarget,
                    aSize, &copied) && copied == aSize;
        };
        const auto* pBehavior = reinterpret_cast<const uint8_t*>(graph.behaviorGraph);
        void* pGraphData{};
        void* pStringData{};
        HavokArray<const char*> names{};
        HavokArray<uint8_t> infos{};
        constexpr uint32_t cCapacityMask = 0x3FFFFFFF;
        if (readSafe(pBehavior + 0x88, &pGraphData, sizeof(pGraphData)) &&
            pGraphData && readSafe(
                reinterpret_cast<const uint8_t*>(pGraphData) + 0x78,
                &pStringData, sizeof(pStringData)) && pStringData &&
            readSafe(reinterpret_cast<const uint8_t*>(pGraphData) + 0x20,
                &infos, sizeof(infos)) &&
            readSafe(reinterpret_cast<const uint8_t*>(pStringData) + 0x30,
                &names, sizeof(names)) &&
            names.size > 0 && names.size <= 512 &&
            infos.size >= 0 && infos.size <= 512 &&
            (static_cast<uint32_t>(names.capacityAndFlags) & cCapacityMask) >=
                static_cast<uint32_t>(names.size) &&
            (static_cast<uint32_t>(infos.capacityAndFlags) & cCapacityMask) >=
                static_cast<uint32_t>(infos.size))
        {
            result.GraphVariableNameCount = static_cast<uint32_t>(names.size);
            result.GraphVariableInfoCount = static_cast<uint32_t>(infos.size);
            std::vector<const char*> namePointers(names.size);
            if (readSafe(names.data, namePointers.data(),
                    namePointers.size() * sizeof(const char*)))
            {
                result.GraphVariableNames.reserve(names.size);
                for (auto* pName : namePointers)
                {
                    pName = reinterpret_cast<const char*>(
                        reinterpret_cast<uintptr_t>(pName) & ~uintptr_t{1});
                    if (!pName)
                        break;
                    std::array<char, 96> buffer{};
                    if (!readSafe(pName, buffer.data(), buffer.size()))
                        break;
                    const auto* pEnd = static_cast<const char*>(std::memchr(
                        buffer.data(), '\0', buffer.size()));
                    if (!pEnd)
                        break;
                    result.GraphVariableNames.emplace_back(buffer.data(),
                        static_cast<size_t>(pEnd - buffer.data()));
                }
            }
        }
        void* pVariables{};
        void* pValues{};
        uint32_t variableCount{};
        if (ReadNative(reinterpret_cast<const uint8_t*>(graph.behaviorGraph) +
                0xD8, pVariables) && pVariables &&
            ReadNative(reinterpret_cast<const uint8_t*>(pVariables) + 0x10,
                pValues) &&
            ReadNative(reinterpret_cast<const uint8_t*>(pVariables) + 0x18,
                variableCount) && variableCount > 0 && variableCount <= 512 &&
            IsReadableRange(pValues, sizeof(uint32_t) * variableCount))
        {
            result.GraphVariableCount = variableCount;
            result.GraphVariables.assign(static_cast<const uint32_t*>(pValues),
                static_cast<const uint32_t*>(pValues) +
                    std::min<uint32_t>(variableCount,
                        aIncludeVariableNames ? 512u : 256u));
            uint64_t checksum = cFnvOffsetBasis;
            HashBytes(checksum, pValues, sizeof(uint32_t) * variableCount);
            result.GraphVariableChecksum = checksum;
        }
    }
    const auto poseCount = graph.characterInstance.numPoseLocal;
    if (poseCount > 0 && poseCount <= 1024 &&
        IsReadableRange(graph.characterInstance.poseLocal,
            static_cast<size_t>(poseCount) * sizeof(QsTransform)))
    {
        uint64_t checksum = cFnvOffsetBasis;
        bool valid = true;
        for (int32_t i = 0; i < poseCount && valid; ++i)
        {
            const auto& transform = graph.characterInstance.poseLocal[i];
            valid = HashQuantizedFloats(checksum, transform.translation, 4) &&
                HashQuantizedFloats(checksum, transform.rotation, 4) &&
                HashQuantizedFloats(checksum, transform.scale, 4);
        }
        if (valid)
        {
            result.PoseCount = poseCount;
            result.PoseChecksum = checksum;
        }
    }
    if (graph.behaviorGraph)
    {
        BehaviorGraph behavior{};
        StateMachine state{};
        const bool behaviorReadable = ReadNative(graph.behaviorGraph, behavior);
        auto* pState = behaviorReadable && behavior.rootGenerator &&
            IsReadableRange(behavior.rootGenerator, sizeof(void*)) ?
            Cast<hkbStateMachine>(reinterpret_cast<hkbGenerator*>(
                behavior.rootGenerator)) : nullptr;
        if (pState && ReadNative(pState, state))
        {
            result.StateId = state.currentStateID;
            result.TimeInState = state.timeInState;
        }
        if (behaviorReadable)
        {
            result.BehaviorActive = behavior.isActive;
            result.BehaviorLinked = behavior.isLinked;
            result.RootClonePresent = behavior.rootGeneratorClone != nullptr;
            result.RootCloneSameAsTemplate = behavior.rootGeneratorClone &&
                behavior.rootGeneratorClone == behavior.rootGenerator;
            uintptr_t templateVtable{};
            uintptr_t cloneVtable{};
            if (behavior.isActive && behavior.isLinked && pState &&
                behavior.rootGeneratorClone &&
                IsReadableRange(behavior.rootGeneratorClone,
                    sizeof(StateMachine)) &&
                ReadNative(behavior.rootGenerator, templateVtable) &&
                ReadNative(behavior.rootGeneratorClone, cloneVtable) &&
                templateVtable && cloneVtable == templateVtable)
            {
                StateMachine clone{};
                if (ReadNative(behavior.rootGeneratorClone, clone) &&
                    std::isfinite(clone.timeInState) &&
                    clone.timeInState >= 0.f && clone.timeInState < 1e7f &&
                    clone.currentStateID >= -1 &&
                    clone.currentStateID < 4096)
                {
                    result.CloneStateReadable = true;
                    result.CloneStateActive = clone.isActive;
                    result.CloneStateId = clone.currentStateID;
                    result.ClonePreviousStateId = clone.previousStateID;
                    result.CloneTimeInState = clone.timeInState;
                }
            }
        }
    }
    const auto renderCount = graph.boneNodes.length;
    if (renderCount > 0 && renderCount <= 1024 &&
        graph.boneNodes.capacity >= renderCount &&
        IsReadableRange(graph.boneNodes.data,
            static_cast<size_t>(renderCount) * sizeof(BoneNodeEntry)))
    {
        uint64_t checksum = cFnvOffsetBasis;
        uint64_t worldChecksum = cFnvOffsetBasis;
        uint32_t readable = 0;
        uint32_t worldReadable = 0;
        for (uint32_t i = 0; i < renderCount; ++i)
        {
            BoneNodeEntry entry{};
            NiTransform local{};
            if (!ReadNative(graph.boneNodes.data + i, entry) || !entry.node ||
                !ReadNative(reinterpret_cast<const uint8_t*>(entry.node) +
                    offsetof(NiAVObject, local), local))
                continue;
            if (!HashQuantizedFloats(checksum, &local.rotate.entry[0][0], 9) ||
                !HashQuantizedFloats(checksum, &local.translate.x, 3) ||
                !HashQuantizedFloats(checksum, &local.scale, 1))
                continue;
            ++readable;
            NiTransform world{};
            if (ReadNative(reinterpret_cast<const uint8_t*>(entry.node) +
                    offsetof(NiAVObject, world), world) &&
                HashQuantizedFloats(worldChecksum, &world.rotate.entry[0][0], 9) &&
                HashQuantizedFloats(worldChecksum, &world.translate.x, 3) &&
                HashQuantizedFloats(worldChecksum, &world.scale, 1))
                ++worldReadable;
        }
        result.RenderBoneCount = readable;
        if (readable)
            result.RenderBoneChecksum = checksum;
        if (worldReadable == readable && worldReadable)
            result.RenderWorldBoneChecksum = worldChecksum;
    }
    return result;
}

void AppendActorPoseDiagnostic(std::string& arSnapshot, Actor* apActor, const char* acpSource)
{
    using namespace ActorPoseDiagnosticViews;

    const auto* pExtension = apActor ? apActor->GetExtension() : nullptr;
    arSnapshot += fmt::format(
        "{{\"formId\":{},\"source\":\"{}\",\"dead\":{},\"bleedingOut\":{},"
        "\"graphDescriptor\":{}",
        apActor ? apActor->formID : 0, acpSource, JsonBool(apActor && apActor->IsDead()),
        JsonBool(apActor && apActor->actorState.IsBleedingOut()),
        pExtension ? pExtension->GraphDescriptorHash : 0);

    GraphManagerRef manager;
    const bool graphReady = apActor &&
        apActor->animationGraphHolder.GetBSAnimationGraph(&manager.pointer) && manager.pointer;
    arSnapshot += fmt::format(",\"graphReady\":{}", JsonBool(graphReady));
    if (!graphReady)
    {
        arSnapshot += ",\"validation\":{\"graphReadable\":false,\"reason\":\"manager-unavailable\"}}";
        return;
    }

    BSScopedLock<BSRecursiveLock> graphLock(manager.pointer->lock);
    const uint32_t graphCount = manager.pointer->animationGraphs.size;
    const uint32_t graphIndex = manager.pointer->animationGraphIndex;
    arSnapshot += fmt::format(",\"graphCount\":{},\"activeGraphIndex\":{}", graphCount, graphIndex);
    if (graphCount == 0 || graphCount > 32 || graphIndex >= graphCount)
    {
        arSnapshot += ",\"validation\":{\"graphReadable\":false,\"reason\":\"graph-index-invalid\"}}";
        return;
    }

    const auto* pNativeGraph = manager.pointer->animationGraphs.Get(graphIndex);
    AnimationGraph graph{};
    if (!ReadNative(pNativeGraph, graph))
    {
        arSnapshot += ",\"validation\":{\"graphReadable\":false,\"reason\":\"graph-range-unreadable\"}}";
        return;
    }

    bool projectNameReadable = false;
    const auto projectName = ReadNativeString(graph.projectName, projectNameReadable);
    BehaviorGraph characterBehavior{};
    const bool characterBehaviorReadable = graph.characterInstance.behaviorGraph &&
        ReadNative(graph.characterInstance.behaviorGraph, characterBehavior);
    arSnapshot += fmt::format(
        ",\"graph\":{{\"projectName\":\"{}\",\"projectNameReadable\":{},"
        "\"behaviorGraphPresent\":{},\"characterBehaviorGraphPresent\":{},"
        "\"characterBehaviorGraphSameAsGraph\":{},"
        "\"characterBehaviorReadable\":{},\"characterBehaviorActive\":{},"
        "\"characterBehaviorLinked\":{},\"characterRootClonePresent\":{},"
        "\"rootNodePresent\":{},\"physicsWorldPresent\":{},\"currentLod\":{},"
        "\"numTracksInLod\":{},\"poseLocalCount\":{},\"reportedAnimBoneCount\":{}}}",
        EscapeJson(projectName), JsonBool(projectNameReadable), JsonBool(graph.behaviorGraph != nullptr),
        JsonBool(graph.characterInstance.behaviorGraph != nullptr),
        JsonBool(graph.characterInstance.behaviorGraph == graph.behaviorGraph),
        JsonBool(characterBehaviorReadable),
        JsonBool(characterBehaviorReadable && characterBehavior.isActive),
        JsonBool(characterBehaviorReadable && characterBehavior.isLinked),
        JsonBool(characterBehaviorReadable && characterBehavior.rootGeneratorClone),
        JsonBool(graph.rootNode != nullptr),
        JsonBool(graph.physicsWorld != nullptr), graph.characterInstance.currentLOD,
        graph.characterInstance.numTracksInLOD, graph.characterInstance.numPoseLocal,
        graph.numAnimBones);

    const auto localPoseCount = graph.characterInstance.numPoseLocal;
    const bool localPoseReadable = localPoseCount > 0 && localPoseCount <= 1024 &&
        graph.characterInstance.poseLocal &&
        IsReadableRange(graph.characterInstance.poseLocal,
            static_cast<size_t>(localPoseCount) * sizeof(QsTransform));
    uint64_t localPoseChecksum = cFnvOffsetBasis;
    if (localPoseReadable)
    {
        for (int32_t i = 0; i < localPoseCount; ++i)
        {
            const auto& transform = graph.characterInstance.poseLocal[i];
            HashValue(localPoseChecksum, i);
            if (!HashQuantizedFloats(localPoseChecksum, transform.translation, 4) ||
                !HashQuantizedFloats(localPoseChecksum, transform.rotation, 4) ||
                !HashQuantizedFloats(localPoseChecksum, transform.scale, 4))
            {
                localPoseChecksum = 0;
                break;
            }
        }
    }
    arSnapshot += fmt::format(",\"evaluatedLocalPose\":{{\"readable\":{},\"count\":{},\"checksum\":{},\"transforms\":[",
        JsonBool(localPoseReadable && localPoseChecksum != 0),
        localPoseReadable ? localPoseCount : 0, localPoseReadable ? localPoseChecksum : 0);
    if (localPoseReadable && localPoseChecksum != 0)
    {
        for (int32_t i = 0; i < localPoseCount; ++i)
        {
            if (i != 0)
                arSnapshot += ',';
            const auto& transform = graph.characterInstance.poseLocal[i];
            arSnapshot += fmt::format(
                "{{\"t\":[{},{},{}],\"q\":[{},{},{},{}],\"s\":[{},{},{}]}}",
                transform.translation[0], transform.translation[1], transform.translation[2],
                transform.rotation[0], transform.rotation[1], transform.rotation[2],
                transform.rotation[3], transform.scale[0], transform.scale[1],
                transform.scale[2]);
        }
    }
    arSnapshot += "]}";

    BehaviorGraph behavior{};
    const bool behaviorReadable = graph.behaviorGraph && ReadNative(graph.behaviorGraph, behavior);
    StateMachine stateMachine{};
    bool stateMachineReadable = false;
    if (behaviorReadable && behavior.rootGenerator &&
        IsReadableRange(behavior.rootGenerator, sizeof(void*)))
    {
        auto* pStateMachine = Cast<hkbStateMachine>(
            reinterpret_cast<hkbGenerator*>(behavior.rootGenerator));
        stateMachineReadable = pStateMachine && ReadNative(pStateMachine, stateMachine);
    }
    StateMachine cloneStateMachine{};
    bool cloneStateMachineReadable = false;
    uintptr_t templateVtable{};
    uintptr_t cloneVtable{};
    if (stateMachineReadable)
        ReadNative(behavior.rootGenerator, templateVtable);
    if (stateMachineReadable && behavior.isActive && behavior.isLinked &&
        behavior.rootGeneratorClone &&
        IsReadableRange(behavior.rootGeneratorClone, sizeof(StateMachine)) &&
        ReadNative(behavior.rootGeneratorClone, cloneVtable) &&
        templateVtable && cloneVtable == templateVtable &&
        ReadNative(behavior.rootGeneratorClone, cloneStateMachine) &&
        std::isfinite(cloneStateMachine.timeInState) &&
        cloneStateMachine.timeInState >= 0.f &&
        cloneStateMachine.timeInState < 1e7f &&
        cloneStateMachine.currentStateID >= -1 &&
        cloneStateMachine.currentStateID < 4096)
        cloneStateMachineReadable = true;

    bool stateNameReadable = false;
    const auto stateMachineName = stateMachineReadable ?
        ReadNativeString(stateMachine.name, stateNameReadable) : std::string{};
    arSnapshot += fmt::format(
        ",\"graphState\":{{\"behaviorReadable\":{},\"active\":{},\"linked\":{},"
        "\"updateActiveNodes\":{},\"stateOrTransitionChanged\":{},"
        "\"rootStateMachineReadable\":{},\"rootStateMachineName\":\"{}\","
        "\"rootStateMachineNameReadable\":{},\"currentStateId\":{},"
        "\"previousStateId\":{},\"timeInState\":{},\"stateMachineActive\":{},"
        "\"stateMachineTransitionChanged\":{},"
        "\"clonePresent\":{},\"cloneReadable\":{},"
        "\"cloneCurrentStateId\":{},\"clonePreviousStateId\":{},"
        "\"cloneTimeInState\":{},\"cloneActive\":{}}}",
        JsonBool(behaviorReadable), JsonBool(behaviorReadable && behavior.isActive),
        JsonBool(behaviorReadable && behavior.isLinked),
        JsonBool(behaviorReadable && behavior.updateActiveNodes),
        JsonBool(behaviorReadable && behavior.stateOrTransitionChanged), JsonBool(stateMachineReadable),
        EscapeJson(stateMachineName), JsonBool(stateNameReadable),
        stateMachineReadable ? stateMachine.currentStateID : -1,
        stateMachineReadable ? stateMachine.previousStateID : -1,
        stateMachineReadable && std::isfinite(stateMachine.timeInState) ? stateMachine.timeInState : 0.f,
        JsonBool(stateMachineReadable && stateMachine.isActive),
        JsonBool(stateMachineReadable && stateMachine.stateOrTransitionChanged),
        JsonBool(behaviorReadable && behavior.rootGeneratorClone),
        JsonBool(cloneStateMachineReadable),
        cloneStateMachineReadable ? cloneStateMachine.currentStateID : -1,
        cloneStateMachineReadable ? cloneStateMachine.previousStateID : -1,
        cloneStateMachineReadable ? cloneStateMachine.timeInState : 0.f,
        JsonBool(cloneStateMachineReadable && cloneStateMachine.isActive));

    // Havok's active-node list, not rootGeneratorClone alone, identifies the
    // state machines currently participating in this character's behavior.
    ActiveNodeList activeNodeList{};
    const bool activeNodeListReadable = behaviorReadable && behavior.activeNodes &&
        ReadNative(behavior.activeNodes, activeNodeList) &&
        activeNodeList.size >= 0 && activeNodeList.size <= 1024 &&
        (activeNodeList.size == 0 ||
            IsReadableRange(activeNodeList.data,
                static_cast<size_t>(activeNodeList.size) * sizeof(ActiveNodeInfo)));
    const auto scannedActiveNodes = activeNodeListReadable ?
        std::min(activeNodeList.size, 256) : 0;
    arSnapshot += fmt::format(
        ",\"activeBehaviorNodes\":{{\"listReadable\":{},\"count\":{},"
        "\"scanned\":{},\"stateMachineTypeScope\":\"root-vtable\","
        "\"rootStateMachineTypeAvailable\":{},"
        "\"stateMachines\":[",
        JsonBool(activeNodeListReadable),
        activeNodeListReadable ? activeNodeList.size : 0, scannedActiveNodes,
        JsonBool(templateVtable != 0));
    uint32_t activeStateMachines = 0;
    for (int32_t i = 0; i < scannedActiveNodes && activeStateMachines < 32; ++i)
    {
        ActiveNodeInfo info{};
        const auto* pInfo = static_cast<const ActiveNodeInfo*>(activeNodeList.data) + i;
        if (!ReadNative(pInfo, info) || !info.nodeTemplate || !info.nodeClone ||
            !templateVtable)
            continue;
        uintptr_t nodeTemplateVtable{};
        uintptr_t nodeCloneVtable{};
        if (!ReadNative(info.nodeTemplate, nodeTemplateVtable) ||
            !ReadNative(info.nodeClone, nodeCloneVtable) ||
            nodeTemplateVtable != templateVtable ||
            nodeCloneVtable != templateVtable)
            continue;
        StateMachine activeState{};
        if (!ReadNative(info.nodeClone, activeState) ||
            !std::isfinite(activeState.timeInState) ||
            activeState.timeInState < 0.f || activeState.timeInState >= 1e7f ||
            activeState.currentStateID < -1 || activeState.currentStateID >= 4096)
            continue;
        bool activeNameReadable = false;
        const auto activeName = ReadNativeString(activeState.name, activeNameReadable);
        if (activeStateMachines++)
            arSnapshot += ',';
        arSnapshot += fmt::format(
            "{{\"index\":{},\"nodeId\":{},\"name\":\"{}\",\"nameReadable\":{},"
            "\"currentStateId\":{},\"previousStateId\":{},\"timeInState\":{},"
            "\"active\":{},\"nodeByte84\":{},\"nodeByte85\":{}}}",
            i, activeState.nodeID, EscapeJson(activeName), JsonBool(activeNameReadable),
            activeState.currentStateID, activeState.previousStateID,
            activeState.timeInState, JsonBool(activeState.isActive),
            info.byte84, info.byte85);
    }
    arSnapshot += fmt::format("],\"stateMachineCount\":{}}}", activeStateMachines);

    uint64_t boneChecksum = cFnvOffsetBasis;
    uint32_t validBoneCount = 0;
    uint32_t invalidBoneCount = 0;
    const bool boneArrayValid = graph.boneNodes.length <= 1024 &&
        graph.boneNodes.capacity >= graph.boneNodes.length &&
        (graph.boneNodes.length == 0 ||
            (graph.boneNodes.data && IsReadableRange(graph.boneNodes.data,
                static_cast<size_t>(graph.boneNodes.length) * sizeof(BoneNodeEntry))));
    if (boneArrayValid)
    {
        for (uint32_t i = 0; i < graph.boneNodes.length; ++i)
        {
            BoneNodeEntry entry{};
            NiTransform transform{};
            if (!ReadNative(graph.boneNodes.data + i, entry))
            {
                ++invalidBoneCount;
                continue;
            }
            const auto* pWorld = entry.node ?
                reinterpret_cast<const uint8_t*>(entry.node) + offsetof(NiAVObject, world) : nullptr;
            if (!pWorld || !ReadNative(pWorld, transform))
            {
                ++invalidBoneCount;
                continue;
            }

            uint64_t sampleHash = boneChecksum;
            HashValue(sampleHash, i);
            if (!HashQuantizedFloats(sampleHash, &transform.rotate.entry[0][0], 9) ||
                !HashQuantizedFloats(sampleHash, &transform.translate.x, 3) ||
                !HashQuantizedFloats(sampleHash, &transform.scale, 1))
            {
                ++invalidBoneCount;
                continue;
            }
            boneChecksum = sampleHash;
            ++validBoneCount;
        }
    }
    arSnapshot += fmt::format(
        ",\"skeleton\":{{\"boneArrayValid\":{},\"boneCount\":{},\"validBoneCount\":{},"
        "\"invalidBoneCount\":{},\"checksum\":\"{:016x}\",\"quantization\":0.001}}",
        JsonBool(boneArrayValid), boneArrayValid ? graph.boneNodes.length : 0,
        validBoneCount, invalidBoneCount, boneChecksum);

    // Keep root and sampled bone transforms in the one-shot probe. Matching
    // locals do not imply matching world transforms when the actor root or
    // the graph's world-update phase differs between peers.
    NiTransform rootLocal{};
    NiTransform rootWorld{};
    const bool rootReadable = graph.rootNode &&
        ReadNative(reinterpret_cast<const uint8_t*>(graph.rootNode) +
            offsetof(NiAVObject, local), rootLocal) &&
        ReadNative(reinterpret_cast<const uint8_t*>(graph.rootNode) +
            offsetof(NiAVObject, world), rootWorld);
    arSnapshot += fmt::format(
        ",\"renderRoot\":{{\"readable\":{},\"localT\":[{},{},{}],"
        "\"worldT\":[{},{},{}]}}",
        JsonBool(rootReadable), rootLocal.translate.x, rootLocal.translate.y,
        rootLocal.translate.z, rootWorld.translate.x,
        rootWorld.translate.y, rootWorld.translate.z);
    arSnapshot += ",\"renderLocalSamples\":[";
    std::string worldSamples;
    worldSamples += ",\"renderWorldSamples\":[";
    bool firstRenderLocal = true;
    bool firstRenderWorld = true;
    constexpr uint32_t cSampleIndices[] =
        {0, 5, 10, 20, 30, 40, 50, 60, 70, 80, 90, 98};
    if (boneArrayValid)
    {
        for (const auto index : cSampleIndices)
        {
            if (index >= graph.boneNodes.length)
                continue;
            BoneNodeEntry entry{};
            NiTransform local{};
            if (!ReadNative(graph.boneNodes.data + index, entry) ||
                !entry.node ||
                !ReadNative(reinterpret_cast<const uint8_t*>(entry.node) +
                    offsetof(NiAVObject, local), local))
                continue;
            if (!firstRenderLocal)
                arSnapshot += ',';
            firstRenderLocal = false;
            arSnapshot += fmt::format(
                "{{\"index\":{},\"t\":[{},{},{}],\"r\":[{},{},{},{},{},{},{},{},{}],\"s\":{}}}",
                index, local.translate.x, local.translate.y,
                local.translate.z, local.rotate.entry[0][0],
                local.rotate.entry[0][1], local.rotate.entry[0][2],
                local.rotate.entry[1][0], local.rotate.entry[1][1],
                local.rotate.entry[1][2], local.rotate.entry[2][0],
                local.rotate.entry[2][1], local.rotate.entry[2][2],
                local.scale);
            NiTransform world{};
            if (!ReadNative(reinterpret_cast<const uint8_t*>(entry.node) +
                    offsetof(NiAVObject, world), world))
                continue;
            if (!firstRenderWorld)
                worldSamples += ',';
            firstRenderWorld = false;
            worldSamples += fmt::format(
                "{{\"index\":{},\"t\":[{},{},{}],\"r\":[{},{},{},{},{},{},{},{},{}],\"s\":{}}}",
                index, world.translate.x, world.translate.y,
                world.translate.z, world.rotate.entry[0][0],
                world.rotate.entry[0][1], world.rotate.entry[0][2],
                world.rotate.entry[1][0], world.rotate.entry[1][1],
                world.rotate.entry[1][2], world.rotate.entry[2][0],
                world.rotate.entry[2][1], world.rotate.entry[2][2],
                world.scale);
        }
    }
    arSnapshot += ']';
    worldSamples += ']';
    arSnapshot += worldSamples;

    RagdollDriver driver{};
    const bool driverReadable = graph.characterInstance.ragdollDriver &&
        ReadNative(graph.characterInstance.ragdollDriver, driver);
    RagdollInstance ragdoll{};
    const bool ragdollReadable = driverReadable && driver.ragdoll &&
        ReadNative(driver.ragdoll, ragdoll);
    const bool bodyArrayValid = ragdollReadable && ValidateHavokArray(ragdoll.rigidBodies, 256);
    const bool constraintArrayValid = ragdollReadable &&
        ValidateHavokArray(ragdoll.constraints, 512);
    const bool boneMapValid = ragdollReadable &&
        ValidateHavokArray(ragdoll.boneToRigidBodyMap, 1024);

    uint64_t bodyChecksum = cFnvOffsetBasis;
    uint64_t boneMapChecksum = cFnvOffsetBasis;
    uint32_t validBodyCount = 0;
    uint32_t invalidBodyCount = 0;
    uint32_t bodiesInWorld = 0;
    uint32_t dynamicBodies = 0;
    uint32_t keyframedBodies = 0;
    uint32_t fixedBodies = 0;
    std::string bodySamples = "[";
    uint32_t sampledBodyCount = 0;
    if (bodyArrayValid)
    {
        for (int32_t i = 0; i < ragdoll.rigidBodies.size; ++i)
        {
            void* pBody{};
            RigidBody body{};
            if (!ReadNative(ragdoll.rigidBodies.data + i, pBody) || !pBody ||
                !ReadNative(pBody, body))
            {
                ++invalidBodyCount;
                continue;
            }

            uint64_t sampleHash = bodyChecksum;
            HashValue(sampleHash, i);
            HashValue(sampleHash, body.motionType);
            const bool inWorld = body.world != nullptr;
            HashValue(sampleHash, inWorld);
            if (!HashQuantizedFloats(sampleHash, body.transform, std::size(body.transform)) ||
                !HashQuantizedFloats(sampleHash, body.linearVelocity, std::size(body.linearVelocity)) ||
                !HashQuantizedFloats(sampleHash, body.angularVelocity, std::size(body.angularVelocity)))
            {
                ++invalidBodyCount;
                continue;
            }

            bodyChecksum = sampleHash;
            ++validBodyCount;
            bodiesInWorld += inWorld ? 1u : 0u;
            // Skyrim's hkpMotion types 2/3 (sphere/box inertia) and 6
            // (thin box) are simulated too, not just type 1.
            dynamicBodies += (body.motionType == 1 || body.motionType == 2 ||
                body.motionType == 3 || body.motionType == 6) ? 1u : 0u;
            keyframedBodies += body.motionType == 4 ? 1u : 0u;
            fixedBodies += body.motionType == 5 ? 1u : 0u;
            if (sampledBodyCount < 32)
            {
                if (sampledBodyCount != 0)
                    bodySamples += ',';
                bodySamples += fmt::format(
                    "{{\"index\":{},\"uid\":{},\"motionType\":{},\"inWorld\":{},"
                    "\"transform\":[{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{}],"
                    "\"linearVelocity\":[{},{},{}],\"angularVelocity\":[{},{},{}]}}",
                    i, body.uid, body.motionType, JsonBool(inWorld),
                    body.transform[0], body.transform[1], body.transform[2], body.transform[3],
                    body.transform[4], body.transform[5], body.transform[6], body.transform[7],
                    body.transform[8], body.transform[9], body.transform[10], body.transform[11],
                    body.transform[12], body.transform[13], body.transform[14], body.transform[15],
                    body.linearVelocity[0], body.linearVelocity[1], body.linearVelocity[2],
                    body.angularVelocity[0], body.angularVelocity[1], body.angularVelocity[2]);
                ++sampledBodyCount;
            }
        }
    }
    bodySamples += ']';
    if (boneMapValid)
    {
        for (int32_t i = 0; i < ragdoll.boneToRigidBodyMap.size; ++i)
        {
            int32_t bodyIndex{};
            if (!ReadNative(ragdoll.boneToRigidBodyMap.data + i, bodyIndex))
            {
                boneMapChecksum = 0;
                break;
            }
            HashValue(boneMapChecksum, bodyIndex);
        }
    }

    uint64_t transitionChecksum = cFnvOffsetBasis;
    const bool ragdollPresent = ragdollReadable;
    const bool graphTransitionChanged = behaviorReadable && behavior.stateOrTransitionChanged;
    const bool stateTransitionChanged = stateMachineReadable && stateMachine.stateOrTransitionChanged;
    const bool dead = apActor && apActor->IsDead();
    const bool bleedingOut = apActor && apActor->actorState.IsBleedingOut();
    HashValue(transitionChecksum, ragdollPresent);
    HashValue(transitionChecksum, bodiesInWorld);
    HashValue(transitionChecksum, dynamicBodies);
    HashValue(transitionChecksum, keyframedBodies);
    HashValue(transitionChecksum, fixedBodies);
    HashValue(transitionChecksum, graphTransitionChanged);
    HashValue(transitionChecksum, stateTransitionChanged);
    HashValue(transitionChecksum, dead);
    HashValue(transitionChecksum, bleedingOut);

    arSnapshot += fmt::format(
        ",\"ragdoll\":{{\"driverPresent\":{},\"driverReadable\":{},\"present\":{},"
        "\"bodyArrayValid\":{},\"bodyCount\":{},\"validBodyCount\":{},"
        "\"invalidBodyCount\":{},\"constraintArrayValid\":{},\"constraintCount\":{},"
        "\"boneMapValid\":{},\"boneMapCount\":{},\"bodiesInWorld\":{},"
        "\"dynamicBodies\":{},\"keyframedBodies\":{},\"fixedBodies\":{},"
        "\"bodyChecksum\":\"{:016x}\",\"boneMapChecksum\":\"{:016x}\","
        "\"bodySamples\":{}}}",
        JsonBool(graph.characterInstance.ragdollDriver != nullptr), JsonBool(driverReadable),
        JsonBool(ragdollPresent), JsonBool(bodyArrayValid),
        bodyArrayValid ? ragdoll.rigidBodies.size : 0, validBodyCount, invalidBodyCount,
        JsonBool(constraintArrayValid), constraintArrayValid ? ragdoll.constraints.size : 0,
        JsonBool(boneMapValid), boneMapValid ? ragdoll.boneToRigidBodyMap.size : 0,
        bodiesInWorld, dynamicBodies, keyframedBodies, fixedBodies, bodyChecksum,
        boneMapChecksum, bodySamples);
    arSnapshot += fmt::format(
        ",\"transitionObservables\":{{\"signature\":\"{:016x}\","
        "\"graphTransitionChanged\":{},\"stateMachineTransitionChanged\":{},"
        "\"ragdollBodiesAttached\":{},\"dead\":{},\"bleedingOut\":{}}}",
        transitionChecksum, JsonBool(graphTransitionChanged), JsonBool(stateTransitionChanged),
        JsonBool(bodiesInWorld != 0), JsonBool(dead), JsonBool(bleedingOut));
    arSnapshot += fmt::format(
        ",\"validation\":{{\"layoutSource\":\"commonlibsse-ng-b93280e\","
        "\"runtimeCandidate\":\"1.7.104\",\"graphReadable\":true,"
        "\"boneArrayValid\":{},\"ragdollArraysValid\":{},\"bodySamplesValid\":{}}}}}",
        JsonBool(boneArrayValid),
        JsonBool(!ragdollReadable || (bodyArrayValid && constraintArrayValid && boneMapValid)),
        JsonBool(!bodyArrayValid || invalidBodyCount == 0));
}
}

struct GameTestService::HitchSnapshotData
{
    uint64_t WorldTick{};
    uint64_t SampleTimeMs{};
    GameLoopDiagnostic FrameTiming{};
    ObjectService::PreStepPlaybackDiagnostic Physics{};
    ObjectService::WorldUpdateDiagnostic WorldPhysics{};
    CharacterService::LocalPoseProductionDiagnostic PoseProduction{};
    std::array<ReferenceMotionStats, 2> CartMotionStats{};
    std::array<HitchCartSample, 64> CartHistory{};
    uint32_t CartNext{};
    uint32_t CartCount{};
    std::array<HitchMotionEvent, 64> MotionEvents{};
    uint32_t MotionNext{};
    uint32_t MotionCount{};
};

bool GameTestService::IsDiagnosticCaptureArmed() noexcept
{
    const auto until = s_diagnosticCaptureUntilMs.load(std::memory_order_relaxed);
    return until && GetTickCount64() < until;
}

GameTestService::GameTestService(World& aWorld) noexcept
    : m_world(aWorld)
    , m_pipeThread([this]() { PipeMain(); })
{
    if (auto* pEvents = EventDispatcherManager::Get())
    {
        pEvents->triggerEnterEvent.RegisterSink(this);
        pEvents->triggerLeaveEvent.RegisterSink(this);
    }
    spdlog::info("In-game test bridge starting: farm={} pid={} capability={}", FarmMode::Enabled(), GetCurrentProcessId(), FarmMode::BuildMarker);
}

GameTestService::~GameTestService() noexcept
{
    if (auto* pEvents = EventDispatcherManager::Get())
    {
        pEvents->triggerEnterEvent.UnRegisterSink(this);
        pEvents->triggerLeaveEvent.UnRegisterSink(this);
    }
    m_stopping = true;
    if (m_pipeThread.joinable())
        CancelSynchronousIo(static_cast<HANDLE>(m_pipeThread.native_handle()));
    // Wake a blocking ConnectNamedPipe during orderly shutdown.
    if (const HANDLE pipe = CreateFileW(FarmMode::Pipe(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
            OPEN_EXISTING, 0, nullptr); pipe != INVALID_HANDLE_VALUE)
        CloseHandle(pipe);
    if (m_pipeThread.joinable())
        m_pipeThread.join();
}

void GameTestService::RecordTriggerEvent(bool aEnter, uint32_t aTriggerFormId,
    uint32_t aActorFormId) noexcept
{
    std::lock_guard lock(m_triggerMutex);
    auto& event = m_triggerEvents[m_triggerNext];
    event = {++m_triggerSequence, GetTickCount64(), aTriggerFormId,
        aActorFormId, aEnter};
    m_triggerNext = (m_triggerNext + 1) % m_triggerEvents.size();
    m_triggerCount = std::min(m_triggerCount + 1, m_triggerEvents.size());
}

BSTEventResult GameTestService::OnEvent(const TESTriggerEnterEvent* apEvent,
    const EventDispatcher<TESTriggerEnterEvent>*)
{
    if (apEvent)
        RecordTriggerEvent(true, apEvent->pTrigger ? apEvent->pTrigger->formID : 0,
            apEvent->pActionRef ? apEvent->pActionRef->formID : 0);
    return BSTEventResult::kOk;
}

BSTEventResult GameTestService::OnEvent(const TESTriggerLeaveEvent* apEvent,
    const EventDispatcher<TESTriggerLeaveEvent>*)
{
    if (apEvent)
        RecordTriggerEvent(false, apEvent->pTrigger ? apEvent->pTrigger->formID : 0,
            apEvent->pActionRef ? apEvent->pActionRef->formID : 0);
    return BSTEventResult::kOk;
}

void GameTestService::WakeWindowThread() noexcept
{
    if (auto* pWindow = BSGraphics::GetMainWindow(); pWindow && pWindow->hWnd)
        PostMessageW(pWindow->hWnd, cGameTestWakeMessage, 0, 0);
}

void GameTestService::PipeMain() noexcept
{
    while (!m_stopping)
    {
        const HANDLE pipe = CreateNamedPipeW(FarmMode::Pipe(), PIPE_ACCESS_DUPLEX,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | cPipeRejectRemoteClients,
            1, 64 * 1024, 64 * 1024, 0, nullptr);
        if (pipe == INVALID_HANDLE_VALUE)
        {
            Sleep(1000);
            continue;
        }

        const bool connected = ConnectNamedPipe(pipe, nullptr) != FALSE || GetLastError() == ERROR_PIPE_CONNECTED;
        if (!connected || m_stopping)
        {
            CloseHandle(pipe);
            continue;
        }

        std::string buffered;
        char chunk[4096];
        DWORD bytesRead = 0;
        bool requestServed = false;
        while (!m_stopping && !requestServed && ReadFile(pipe, chunk, sizeof(chunk), &bytesRead, nullptr) && bytesRead)
        {
            buffered.append(chunk, bytesRead);
            for (auto newline = buffered.find('\n'); newline != std::string::npos; newline = buffered.find('\n'))
            {
                auto line = buffered.substr(0, newline);
                buffered.erase(0, newline + 1);
                if (!line.empty() && line.back() == '\r')
                    line.pop_back();
                if (line.empty())
                    continue;

                auto request = std::make_shared<Request>();
                request->Line = std::move(line);
                {
                    std::scoped_lock lock(m_queueMutex);
                    m_requests.push_back(request);
                }
                WakeWindowThread();

                std::unique_lock lock(request->Mutex);
                if (!request->Completed.wait_for(lock, 15s, [&]() { return request->Complete || m_stopping; }))
                    request->Response = Error(GetJsonId(request->Line), "window-thread timeout");
                request->Response += '\n';
                DWORD written = 0;
                if (!WriteFile(pipe, request->Response.data(), static_cast<DWORD>(request->Response.size()), &written, nullptr))
                    break;
                // The client is blocked reading this response, so flushing here
                // guarantees delivery before the one-shot server disconnects.
                FlushFileBuffers(pipe);
                // One request per connection keeps a disconnected or idle
                // diagnostic client from monopolizing the sole local pipe.
                requestServed = true;
                break;
            }
        }
        DisconnectNamedPipe(pipe);
        CloseHandle(pipe);
    }
}

void GameTestService::OnWindowThread() noexcept
{
    std::deque<std::shared_ptr<Request>> requests;
    {
        std::scoped_lock lock(m_queueMutex);
        requests.swap(m_requests);
    }
    for (const auto& request : requests)
    {
        const auto response = Execute(request->Line);
        {
            std::scoped_lock lock(request->Mutex);
            request->Response = response;
            request->Complete = true;
        }
        request->Completed.notify_one();
    }
}

void GameTestService::OnGameThread() noexcept
{
    {
        float damage{};
        {
            std::lock_guard lock(s_mainFrameDropLock);
            damage = std::exchange(s_mainFrameDamage, 0.f);
        }
        if (auto* pPlayer = PlayerCharacter::Get(); pPlayer && damage > 0.f)
        {
            PlayerCombat::ApplyDamage(pPlayer, nullptr, damage, false);
            spdlog::info("Test damage_player: {} -> health {} bleeding out {}", damage,
                pPlayer->GetActorValue(ActorValueInfo::kHealth), pPlayer->actorState.IsBleedingOut());
        }
    }
    {
        std::function<void()> creatorCall;
        {
            std::lock_guard lock(s_mainFrameDropLock);
            creatorCall = std::exchange(s_mainFrameCreator, nullptr);
        }
        if (creatorCall)
            creatorCall();
    }
    {
        std::pair<uint32_t, uint32_t> combat{};
        {
            std::lock_guard lock(s_mainFrameDropLock);
            combat = std::exchange(s_mainFrameCombat, {});
        }
        auto* pAttacker = combat.first ? Cast<Actor>(TESForm::GetById(combat.first)) : nullptr;
        auto* pTarget = combat.second ? Cast<Actor>(TESForm::GetById(combat.second)) : nullptr;
        auto* pVM = GameVM::Get() ? GameVM::Get()->virtualMachine : nullptr;
        if (pAttacker && pTarget && pVM)
        {
            // Papyrus Actor.StartCombat (54768 / 140A03090): (VM, stack id, self, target).
            using TStartCombat = void(void*, uint32_t, Actor*, Actor*);
            POINTER_SKYRIMSE(TStartCombat, startCombat, 54768);
            startCombat.Get()(pVM, 0, pAttacker, pTarget);
            spdlog::info("Test start_combat: {:X} -> {:X}", pAttacker->formID, pTarget->formID);
        }
    }
    {
        std::pair<uint32_t, bool> request{};
        {
            std::lock_guard lock(s_mainFrameDropLock);
            request = std::exchange(s_mainFrameDisable, {});
        }
        if (auto* pRef = request.first ? Cast<TESObjectREFR>(TESForm::GetById(request.first)) : nullptr)
        {
            if (request.second)
                pRef->Disable();
            else
            {
                using ObjectReference = TESObjectREFR;
                PAPYRUS_FUNCTION(void, ObjectReference, EnableNoWait, bool);
                if (s_pEnableNoWait)
                    s_pEnableNoWait(pRef, false);
            }
            spdlog::info("Test set_disabled: {:X} -> {} (now disabled={})", request.first, request.second, pRef->IsDisabled());
        }
    }
    {
        // drop_item, queued by the window thread.
        // Two phases: Papyrus AddItem hands the inventory change to the task queue, so the item is only in the
        // inventory a few frames later; the drop follows 500 ms after the add.
        static std::pair<uint32_t, int32_t> s_adding{};
        static uint64_t s_dropAt{};
        std::pair<uint32_t, int32_t> drop{};
        {
            std::lock_guard lock(s_mainFrameDropLock);
            if (s_mainFrameDrop.first)
            {
                s_adding = std::exchange(s_mainFrameDrop, {});
                s_dropAt = 0;
            }
        }
        auto* pPlayer = PlayerCharacter::Get();
        if (s_adding.first && pPlayer && !s_dropAt)
        {
            using ObjectReference = TESObjectREFR;
            PAPYRUS_FUNCTION(void, ObjectReference, AddItem, TESForm*, int32_t, bool);
            auto* pAdd = Cast<TESBoundObject>(TESForm::GetById(s_adding.first));
            if (s_pAddItem && pAdd && s_adding.second > 0)
                s_pAddItem(pPlayer, pAdd, s_adding.second, true);
            s_dropAt = GetTickCount64() + 500;
        }
        else if (s_adding.first && s_dropAt && GetTickCount64() >= s_dropAt)
        {
            drop = std::exchange(s_adding, {});
            s_dropAt = 0;
        }
        auto* pBase = drop.first ? Cast<TESBoundObject>(TESForm::GetById(drop.first)) : nullptr;
        if (pBase && pPlayer && drop.second > 0)
        {
            // Papyrus DropObject returns its reference through a hidden return slot that PAPYRUS_FUNCTION does not
            // model (the misread pointer crashed the game, 2026-09-28 19:21). Use the native Actor::DropObject entry
            // (40454, through our hook, as the inventory menu does), 100 u in front of the player.
            {
                NiPoint3 location = pPlayer->position;
                location.x += std::sin(pPlayer->rotation.z) * 100.f;
                location.y += std::cos(pPlayer->rotation.z) * 100.f;
                location.z += 60.f;
                NiPoint3 rotation{};
                using TDrop = void*(Actor*, uint32_t*, TESBoundObject*, ExtraDataList*, int32_t, NiPoint3*, NiPoint3*);
                POINTER_SKYRIMSE(TDrop, dropEntry, 40454);
                uint32_t handle{};
                dropEntry.Get()(pPlayer, &handle, pBase, nullptr, drop.second, &location, &rotation);
                auto* pDropped = TESObjectREFR::GetByHandle(handle);
                spdlog::info("Test drop: {:X} x{} -> {:X}", drop.first, drop.second, pDropped ? pDropped->formID : 0);
            }
        }
    }
    DrainArmorAttachmentTrace();
    InstallVirtualMachineDiagnostic();
    if (!HarnessService::OwnsDriver()) IntroDriver::Tick();
    if (const uint32_t formId = m_testCorpseDisplaceFormId.exchange(0,
            std::memory_order_acq_rel))
    {
        auto* pActor = Cast<Actor>(TESForm::GetById(formId));
        auto* pExtension = pActor ? pActor->GetExtension() : nullptr;
        auto* pCell = pActor ? pActor->GetParentCellEx() : nullptr;
        if (!m_world.GetPartyService().IsInParty() ||
            m_world.GetPartyService().IsLeader() || !pActor ||
            !pExtension || !pExtension->IsRemote() ||
            pExtension->IsPlayer() || !pActor->actorState.IsDeadState() ||
            !pActor->GetNiNode() || !pCell ||
            pActor->GetNativeMountState().InteractionExtra)
        {
            spdlog::warn("Corpse displacement test rejected actor={:X}", formId);
        }
        else
        {
            NiPoint3 target = pActor->position;
            target.x += 320.f;
            pActor->MoveTo(pCell, target);
            spdlog::info("Corpse displacement test actor={:X} cell={:X} target=({},{},{})",
                formId, pCell->formID, target.x, target.y, target.z);
        }
    }
    if (m_nativeCreatorConfirmRequested.exchange(false, std::memory_order_acq_rel))
    {
        auto* pUI = UI::Get();
        const bool selected = pUI && pUI->SelectCharacterConfirmationForTest();
        {
            std::scoped_lock lock(m_snapshotMutex);
            m_nativeCreatorConfirmSucceeded = selected;
            m_nativeCreatorConfirmComplete = true;
        }
        spdlog::info("Native character confirmation test selection {}",
            selected ? "accepted" : "rejected");
    }
    const auto now = GetTickCount64();
    const auto* pTitleUI = UI::Get();
    HostFrameCost::Report(m_world, now);
    const bool titleMenuOpen = pTitleUI && pTitleUI->GetMenuOpen(BSFixedString("TitleSequence Menu"));
    if (titleMenuOpen != m_titleSequenceMenuOpen)
    {
        m_titleSequenceMenuOpen = titleMenuOpen;
        m_lastTitleSequenceTransitionMs = now;
        ++m_titleSequenceMenuTransitions;
        spdlog::info("Title sequence menu {} tick={} transition={} leader={}",
            titleMenuOpen ? "opened" : "closed", now, m_titleSequenceMenuTransitions,
            m_world.GetPartyService().IsLeader());
    }
    // Sample at the game update cadence, not the slower cached-snapshot
    // cadence. This catches a one-frame physics snap even when an SSH-based
    // two-PC comparison happens to land between snaps.
    constexpr uint32_t cartIds[]{0x000B9DF3, 0x000BB970};
    constexpr uint32_t horseIds[]{0x000B9DF2, 0x000BB971};
    const auto frameTiming = GetGameLoopDiagnostic();
    auto recordMotion = [this, now, &frameTiming](uint32_t aFormId,
        uint32_t aDeltaMs, float aStep, const TESObjectREFR* apReference)
    {
        auto& event = m_hitchMotionEvents[m_hitchMotionNext];
        event = {now, m_world.GetTick(), aFormId,
            frameTiming.WorldLastEntryGapUs,
            frameTiming.VmLastEntryGapUs, frameTiming.VmLastAppUs,
            frameTiming.VmLastOriginalUs, frameTiming.WorldLastGameTestUs,
            aDeltaMs, aStep,
            {apReference ? apReference->position.x : 0.f,
                apReference ? apReference->position.y : 0.f,
                apReference ? apReference->position.z : 0.f}};
        const uint32_t horseId = aFormId == 0x000B9DF3 ? 0x000B9DF2 :
            (aFormId == 0x000BB970 ? 0x000BB971 : 0);
        auto* pHorse = horseId ? Cast<Actor>(TESForm::GetById(horseId)) : nullptr;
        if (pHorse && pHorse->loadedState)
        {
            event.HorsePresent = true;
            event.HorsePosition[0] = pHorse->position.x;
            event.HorsePosition[1] = pHorse->position.y;
            event.HorsePosition[2] = pHorse->position.z;
        }
        m_hitchMotionNext = (m_hitchMotionNext + 1) % m_hitchMotionEvents.size();
        m_hitchMotionCount = (std::min)(m_hitchMotionCount + 1,
            static_cast<uint32_t>(m_hitchMotionEvents.size()));
    };
    bool recordedGap = false;
    for (size_t i = 0; i < std::size(cartIds); ++i)
    {
        auto& stats = m_introCartMotionStats[i];
        auto* pCart = Cast<TESObjectREFR>(TESForm::GetById(cartIds[i]));
        if (!m_world.GetPartyService().IsInParty() || !pCart || !pCart->loadedState ||
            !std::isfinite(pCart->position.x) || !std::isfinite(pCart->position.y) ||
            !std::isfinite(pCart->position.z))
        {
            stats = {};
            continue;
        }
        if (stats.LastMs && now > stats.LastMs && now - stats.LastMs <= 100)
        {
            const float dx = pCart->position.x - stats.Position[0];
            const float dy = pCart->position.y - stats.Position[1];
            const float dz = pCart->position.z - stats.Position[2];
            const float step = std::sqrt(dx * dx + dy * dy + dz * dz);
            stats.PeakStep = (std::max)(stats.PeakStep, step);
            stats.PeakSpeed = (std::max)(stats.PeakSpeed, step * 1000.f / static_cast<float>(now - stats.LastMs));
            if (step >= 75.f && now - stats.LastMs <= 50)
                ++stats.LargeSteps;
            if (step >= 75.f || frameTiming.WorldLastEntryGapUs >= 50000)
            {
                recordMotion(cartIds[i], static_cast<uint32_t>(now - stats.LastMs),
                    step, pCart);
                recordedGap = recordedGap || frameTiming.WorldLastEntryGapUs >= 50000;
            }
        }
        else if (stats.LastMs && now > stats.LastMs &&
            frameTiming.WorldLastEntryGapUs >= 50000)
        {
            const float dx = pCart->position.x - stats.Position[0];
            const float dy = pCart->position.y - stats.Position[1];
            const float dz = pCart->position.z - stats.Position[2];
            recordMotion(cartIds[i], static_cast<uint32_t>((std::min)(
                uint64_t{UINT32_MAX}, now - stats.LastMs)),
                std::sqrt(dx * dx + dy * dy + dz * dz), pCart);
            recordedGap = true;
        }
        stats.Position[0] = pCart->position.x;
        stats.Position[1] = pCart->position.y;
        stats.Position[2] = pCart->position.z;
        stats.LastMs = now;
        ++stats.Samples;
    }
    if (frameTiming.WorldLastEntryGapUs >= 50000 && !recordedGap)
        recordMotion(0, 0, 0.f, nullptr);
    if (now >= m_nextHitchSnapshotMs)
    {
        m_nextHitchSnapshotMs = now + 100;
        auto& cartSample = m_hitchCartHistory[m_hitchCartNext];
        cartSample = {};
        cartSample.WorldTick = m_world.GetTick();
        for (size_t i = 0; i < std::size(cartIds); ++i)
        {
            const auto& stats = m_introCartMotionStats[i];
            cartSample.Present[i] = stats.Samples != 0;
            std::copy(std::begin(stats.Position), std::end(stats.Position),
                cartSample.Position[i].begin());
            if (auto* pCart = Cast<TESObjectREFR>(TESForm::GetById(cartIds[i])); pCart && pCart->loadedState)
                cartSample.Rotation[i] = {pCart->rotation.x, pCart->rotation.y, pCart->rotation.z};
            auto* pHorse = Cast<Actor>(TESForm::GetById(horseIds[i]));
            if (pHorse && pHorse->loadedState)
            {
                cartSample.HorsePresent[i] = true;
                if (auto* horseRoot = pHorse->GetNiNode())
                    cartSample.HorseNodeZ[i] = horseRoot->world.translate.z;
                if (const auto* pProcess = pHorse->currentProcess)
                {
                    ReadNative(reinterpret_cast<const uint8_t*>(pProcess) + 0x137, cartSample.HorseLevel[i]);
                    cartSample.HorseController[i] = pProcess->middleProcess &&
                        *reinterpret_cast<void* const*>(reinterpret_cast<const uint8_t*>(pProcess->middleProcess) + 0x250);
                    if (cartSample.HorseController[i])
                    {
                        const auto* controller = *reinterpret_cast<const uint8_t* const*>(
                            reinterpret_cast<const uint8_t*>(pProcess->middleProcess) + 0x250);
                        ReadNative(controller + 0x218, cartSample.HorseControllerFlags[i]);
                        ReadNative(controller + 0x200, cartSample.HorseControllerState[i]);
                        ReadNative(controller + 0x1A0, cartSample.HorseSupported[i]);
                        ReadNative(controller + 0x244, cartSample.CtrlFallTime[i]);
                        ReadNative(controller + 0x240, cartSample.CtrlFallStart[i]);
                        ReadNative(controller + 0x188, cartSample.CtrlDeltaZ[i]);
                        {
                            alignas(16) float pos[4]{};
                            using GetPositionFn = void(const void*, float*, bool);
                            auto** vtable = *reinterpret_cast<GetPositionFn** const*>(controller);
                            vtable[2](controller, pos, false);
                            cartSample.CtrlZ[i] = pos[2] * 69.99125f;
                        }
                        const uint8_t* support{};
                        if (ReadNative(controller + 0x2B0, support) && support)
                        {
                            cartSample.SupportBody[i] = reinterpret_cast<uint64_t>(support);
                            uint8_t motion{};
                            float z{};
                            if (ReadNative(support + 0x160, motion))
                                cartSample.SupportMotion[i] = motion;
                            if (ReadNative(support + 0x170 + 14 * sizeof(float), z))
                                cartSample.SupportZ[i] = z * 69.99125f;
                            // Cart root body: node -> collisionObject -> +0x20 wrapper -> +0x10 hkpRigidBody.
                            if (auto* pCart = Cast<TESObjectREFR>(TESForm::GetById(cartIds[i])); pCart && pCart->GetNiNode())
                            {
                                const uint8_t* wrapper{};
                                const void* body{};
                                if (pCart->GetNiNode()->collisionObject &&
                                    ReadNative(reinterpret_cast<const uint8_t*>(pCart->GetNiNode()->collisionObject) + 0x20, wrapper) &&
                                    wrapper && ReadNative(wrapper + 0x10, body))
                                    cartSample.SupportIsCart[i] = body == support;
                            }
                        }
                    }
                }
                cartSample.HorsePosition[i] = {pHorse->position.x,
                    pHorse->position.y, pHorse->position.z};
            }
        }
        if (auto* camera = PlayerCamera::Get(); camera && camera->cameraNode)
        {
            const auto& w = camera->cameraNode->world;
            const float fx = w.rotate.entry[0][1], fy = w.rotate.entry[1][1], fz = w.rotate.entry[2][1];
            const float fl = std::sqrt(fx * fx + fy * fy + fz * fz);
            cartSample.CameraFov = camera->GetWorldFov();
            auto angleTo = [&](const NiPoint3& p) -> float
            {
                const float dx = p.x - w.translate.x, dy = p.y - w.translate.y, dz = p.z - w.translate.z;
                const float dl = std::sqrt(dx * dx + dy * dy + dz * dz);
                if (fl < 1e-4f || dl < 1e-4f)
                    return -1.f;
                const float c = (std::clamp)((fx * dx + fy * dy + fz * dz) / (fl * dl), -1.f, 1.f);
                return std::acos(c) * 57.29578f;
            };
            for (size_t i = 0; i < std::size(cartIds); ++i)
            {
                if (auto* pCart = Cast<TESObjectREFR>(TESForm::GetById(cartIds[i])))
                    cartSample.CartViewAngle[i] = angleTo(pCart->position);
                if (auto* pHorse = Cast<Actor>(TESForm::GetById(horseIds[i])))
                    cartSample.HorseViewAngle[i] = angleTo(pHorse->position);
            }
        }
        {
            const auto probe = ObjectService::GetReferencePhaseDiagnostic();
            cartSample.ProbeInputZ = probe.SetPositionInputZ;
            cartSample.ProbeCalls = probe.SetPositionCalls;
        }
        m_hitchCartNext = (m_hitchCartNext + 1) % m_hitchCartHistory.size();
        m_hitchCartCount = (std::min)(m_hitchCartCount + 1,
            static_cast<uint32_t>(m_hitchCartHistory.size()));
        auto data = std::make_shared<HitchSnapshotData>();
        data->WorldTick = m_world.GetTick();
        data->SampleTimeMs = now;
        data->FrameTiming = frameTiming;
        data->Physics = ObjectService::GetPreStepPlaybackDiagnostic();
        data->WorldPhysics = ObjectService::GetWorldUpdateDiagnostic();
        data->PoseProduction =
            m_world.GetCharacterService().GetLocalPoseProductionDiagnostic();
        data->CartMotionStats = m_introCartMotionStats;
        data->CartHistory = m_hitchCartHistory;
        data->CartNext = m_hitchCartNext;
        data->CartCount = m_hitchCartCount;
        data->MotionEvents = m_hitchMotionEvents;
        data->MotionNext = m_hitchMotionNext;
        data->MotionCount = m_hitchMotionCount;
        // Publish one immutable numeric sample. The requesting window thread
        // formats it after releasing the lock, independently of full snapshots.
        std::scoped_lock lock(m_snapshotMutex);
        m_hitchSnapshot = std::move(data);
    }
    bool captureRequested = false;
    {
        std::scoped_lock lock(m_snapshotScheduleMutex);
        const auto tick = m_world.GetTick();
        if (m_snapshotTargetTick && m_snapshotTargetTick > tick &&
            m_snapshotTargetTick - tick <= 5000)
            ArmDiagnosticCapture();
        if (m_snapshotRelativeDueWallMs)
        {
            const bool sameEpoch = m_world.GetTransport().GetAuthorityEpoch() ==
                m_snapshotTargetAuthorityEpoch;
            if ((sameEpoch && m_world.GetTick() >= m_snapshotTargetTick) ||
                GetTickCount64() >= m_snapshotRelativeDueWallMs)
            {
                captureRequested = true;
                m_snapshotRelativeDueWallMs = 0;
                m_snapshotTargetTick = 0;
            }
        }
        else if (m_snapshotTargetTick)
        {
            const auto currentEpoch = m_world.GetTransport().GetAuthorityEpoch();
            if (currentEpoch != m_snapshotTargetAuthorityEpoch)
            {
                spdlog::info("Canceled game snapshot across authority epoch {} -> {}",
                    m_snapshotTargetAuthorityEpoch, currentEpoch);
                m_snapshotTargetTick = 0;
            }
            else if (m_world.GetTick() >= m_snapshotTargetTick)
            {
                captureRequested = true;
                m_snapshotTargetTick = 0;
            }
        }
    }
    // The full native-actor audit can hold the game thread for seconds. Never
    // run it implicitly at startup; a caller must explicitly schedule it.
    if (!captureRequested)
        return;

    ArmDiagnosticCapture();
    try
    {
        ReadablePageCacheScope readableCache;
        std::string snapshot = "{";
        std::array<uint32_t, 7> snapshotPhaseUs{};
        auto phaseStarted = std::chrono::steady_clock::now();
        auto markSnapshotPhase = [&](size_t aIndex)
        {
            const auto ended = std::chrono::steady_clock::now();
            snapshotPhaseUs[aIndex] = static_cast<uint32_t>((std::min)(
                int64_t{UINT32_MAX},
                std::chrono::duration_cast<std::chrono::microseconds>(
                    ended - phaseStarted).count()));
            phaseStarted = ended;
        };
        const auto worldTick = m_world.GetTick();
        snapshot += fmt::format("\"sampleTimeMs\":{},\"worldTick\":{}", now, worldTick);
        snapshot += fmt::format(",\"nativeCrashGuard\":{{\"nullKnockExplosionSkips\":{}}}",
            Actor::GetNullKnockExplosionSkips());
        const auto death = m_world.ctx().at<PlayerService>().GetDeathDiagnostic();
        snapshot += fmt::format(
            ",\"playerDeath\":{{\"respawns\":{},\"lastRespawnMs\":{},"
            "\"postRespawnKnockAttempts\":{},"
            "\"postRespawnKnocksApplied\":{},"
            "\"lastPostRespawnKnockMs\":{},"
            "\"skipNextPostRespawnKnock\":{},"
            "\"lastKnockSkipped\":{},"
            "\"lastBleedingOutAtKnock\":{},"
            "\"lastHad3DAtKnock\":{},"
            "\"lastHadProcessAtKnock\":{}}}",
            death.Respawns, death.LastRespawnMs,
            death.PostRespawnKnockAttempts,
            death.PostRespawnKnocksApplied,
            death.LastPostRespawnKnockMs,
            JsonBool(death.SkipNextPostRespawnKnock),
            JsonBool(death.LastKnockSkipped),
            JsonBool(death.LastBleedingOutAtKnock),
            JsonBool(death.LastHad3DAtKnock),
            JsonBool(death.LastHadProcessAtKnock));
        const auto watchedGraph = AnimationGraphUpdateTrace::GetWatchedPoseSample();
        snapshot += fmt::format(",\"graphUpdateTrace\":{{\"registered\":{},"
            "\"totalCalls\":{},\"lastPostCallMs\":{},"
            "\"watchedFormId\":{},\"watchedPoseBoneCount\":{},"
            "\"watchedRenderBoneCount\":{},\"watchedValidRenderNodeCount\":{},"
            "\"watchedThreadId\":{},"
            "\"watchedLastSampleMs\":{},\"watchedPoseChecksum\":{},"
            "\"watchedRenderChecksum\":{},\"watchedRenderWorldChecksum\":{},"
            "\"watchedSamples\":{},"
            "\"watchedPoseChanges\":{},\"watchedRenderChanges\":{},"
            "\"watchedRenderWorldChanges\":{},"
            "\"watchedDurationUs\":{}}}",
            JsonBool(AnimationGraphUpdateTrace::IsHookRegistered()),
            AnimationGraphUpdateTrace::GetTotalCalls(),
            AnimationGraphUpdateTrace::GetLastPostCallMs(),
            watchedGraph.FormId, watchedGraph.PoseBoneCount,
            watchedGraph.RenderBoneCount, watchedGraph.ValidRenderNodeCount,
            watchedGraph.ThreadId,
            watchedGraph.LastSampleMs, watchedGraph.PoseChecksum,
            watchedGraph.RenderLocalChecksum, watchedGraph.RenderWorldChecksum,
            watchedGraph.Samples,
            watchedGraph.PoseChanges, watchedGraph.RenderLocalChanges,
            watchedGraph.RenderWorldChanges,
            watchedGraph.DurationUs);
        snapshot += fmt::format(",\"presentationDelayMs\":{}",
            m_world.GetCharacterService().GetPresentationDelayMs());
        const auto setVehicleNative = m_world.ctx().at<PapyrusService>().Get(
            "Actor", "SetVehicle");
        const auto gameBase = reinterpret_cast<uintptr_t>(GetModuleHandleW(L"SkyrimSE.exe"));
        const auto nativeAddress = reinterpret_cast<uintptr_t>(setVehicleNative);
        snapshot += fmt::format(",\"nativeVehicleBinding\":{{\"registered\":{},"
            "\"gameRva\":{}}}", JsonBool(setVehicleNative != nullptr),
            gameBase && nativeAddress >= gameBase && nativeAddress - gameBase < 0x10000000
                ? nativeAddress - gameBase : 0);
        snapshot += fmt::format(",\"nativeVehicleTrial\":{{\"riderId\":{},"
            "\"calls\":{},\"immediateSeats\":{},\"immediateHandle\":{}}}",
            m_world.GetCharacterService().GetVehicleTrialRiderId(),
            m_world.GetCharacterService().GetVehicleTrialCalls(),
            m_world.GetCharacterService().GetVehicleTrialImmediateSeats(),
            m_world.GetCharacterService().GetVehicleTrialImmediateHandle());
        snapshot += fmt::format(",\"remoteProcessTrialTicks\":{}",
            Actor::GetRemoteProcessTrialTicks());
        const auto targetTrial = CombatController::GetTargetAuthorityTrialDiagnostics();
        snapshot += fmt::format(
            ",\"combatTargetAuthorityTrial\":{{\"actorFormId\":{},"
            "\"calls\":{},\"overrides\":{},\"lastRequestedFormId\":{},"
            "\"lastPresentedFormId\":{},\"lastNativeFormId\":{},"
            "\"lastCallerRva\":{}}}",
            targetTrial.ActorFormId, targetTrial.Calls,
            targetTrial.Overrides, targetTrial.LastRequestedFormId,
            targetTrial.LastPresentedFormId, targetTrial.LastNativeFormId,
            targetTrial.LastCallerRva);
        const auto mountDiagnostic = m_world.GetCharacterService().GetMountDiagnostic();
        snapshot += fmt::format(",\"mountDiagnostic\":{{\"pending\":{},"
            "\"notifications\":{},\"waitedFor3D\":{},\"applied\":{},\"seated\":{},"
            "\"rejected\":{},\"lastRiderId\":{},\"lastMountId\":{}}}",
            mountDiagnostic.Pending, mountDiagnostic.Notifications,
            mountDiagnostic.WaitedFor3D, mountDiagnostic.Applied,
            mountDiagnostic.Seated,
            mountDiagnostic.Rejected, mountDiagnostic.LastRiderId,
            mountDiagnostic.LastMountId);
        snapshot += ",\"mountRelations\":[";
        bool firstMountRelation = true;
        for (const auto& relation : m_world.GetCharacterService().GetPendingMountRelations())
        {
            if (!firstMountRelation)
                snapshot += ',';
            firstMountRelation = false;
            snapshot += fmt::format(
                "{{\"riderId\":{},\"mountId\":{},\"riderFormId\":{},"
                "\"mountFormId\":{},\"nativeMountFormId\":{},"
                "\"nativeVehicleHandle\":{},"
                "\"horseExtra\":{},\"horseHandle\":{},"
                "\"interactionExtra\":{},\"interactionPointerPresent\":{},"
                "\"interactionActorHandle\":{},\"interactionTargetHandle\":{},"
                "\"riderHas3D\":{},\"mountHas3D\":{},"
                "\"wasSeated\":{},\"attempts\":{}}}",
                relation.RiderId, relation.MountId, relation.RiderFormId,
                relation.MountFormId, relation.NativeMountFormId,
                relation.NativeVehicleHandle,
                JsonBool(relation.HorseExtra), relation.HorseHandle,
                JsonBool(relation.InteractionExtra),
                JsonBool(relation.InteractionPointerPresent),
                relation.InteractionActorHandle, relation.InteractionTargetHandle,
                JsonBool(relation.RiderHas3D), JsonBool(relation.MountHas3D),
                JsonBool(relation.WasSeated), relation.Attempts);
        }
        snapshot += ']';
        const auto visualInspection = VisualPoseMailbox::GetInspection();
        snapshot += fmt::format(",\"visualPoseMailbox\":{{\"published\":{},"
            "\"accepted\":{},\"stale\":{},\"rejectedGraph\":{},"
            "\"inspected\":{},\"lastFormId\":{},\"lastEpoch\":{},"
            "\"lastSourceAgeMs\":{},\"lastEligibleBones\":{},"
            "\"lastMaxElementErrorMilli\":{},\"lastDurationUs\":{},"
            "\"applyEnabled\":{},\"applyFormId\":{},"
            "\"appliedFrames\":{},\"appliedBones\":{},"
            "\"oldFrameApplySkips\":{},\"timelineMisses\":{},"
            "\"interpolatedFrames\":{},\"lastInterpolationSpanMs\":{},"
            "\"writeFailures\":{},"
            "\"lastReadbackErrorMilli\":{},\"rootSamples\":{},"
            "\"lastRootLatestErrorMilli\":{},"
            "\"lastRootPresentationErrorMilli\":{},"
            "\"rootDiagnosticFormId\":{}}}",
            visualInspection.Published, visualInspection.Accepted,
            visualInspection.Stale, visualInspection.RejectedGraph,
            visualInspection.Inspected, visualInspection.LastFormId,
            visualInspection.LastEpoch, visualInspection.LastSourceAgeMs,
            visualInspection.LastEligibleBones,
            visualInspection.LastMaxElementErrorMilli,
            visualInspection.LastDurationUs,
            JsonBool(visualInspection.ApplyEnabled),
            visualInspection.ApplyFormId,
            visualInspection.AppliedFrames, visualInspection.AppliedBones,
            visualInspection.OldFrameApplySkips,
            visualInspection.TimelineMisses,
            visualInspection.InterpolatedFrames,
            visualInspection.LastInterpolationSpanMs,
            visualInspection.WriteFailures,
            visualInspection.LastReadbackErrorMilli,
            visualInspection.RootSamples,
            visualInspection.LastRootLatestErrorMilli,
            visualInspection.LastRootPresentationErrorMilli,
            visualInspection.RootDiagnosticFormId);
        const auto bodyPlayback = ObjectService::GetBodyPlaybackDiagnostic();
        snapshot += fmt::format(
            ",\"bodyPlaybackProbe\":{{\"selectedFormId\":{},\"attempts\":{},"
            "\"succeeded\":{},\"staleSkips\":{},\"lastSourceAgeMs\":{},"
            "\"lastDurationUs\":{},\"lastPreError\":{},\"lastPostError\":{},"
            "\"lastStep\":{}}}",
            bodyPlayback.SelectedFormId, bodyPlayback.Attempts,
            bodyPlayback.Succeeded, bodyPlayback.StaleSkips,
            bodyPlayback.LastSourceAgeMs, bodyPlayback.LastDurationUs,
            bodyPlayback.LastPreError, bodyPlayback.LastPostError,
            bodyPlayback.LastStep);
        const auto referencePhase = ObjectService::GetReferencePhaseDiagnostic();
        snapshot += fmt::format(
            ",\"referencePhaseProbe\":{{\"selectedFormId\":{},\"hookInstalled\":{},"
            "\"update3DCalls\":{},\"moveHavokCalls\":{},\"lastThreadId\":{},"
            "\"lastMethod\":{},\"lastDurationUs\":{},\"lastReferenceDelta\":{},"
            "\"lastNodeDelta\":{},\"lastBodyDelta\":{},\"lastRefBodyError\":{},"
            "\"lastRefNodeError\":{},\"setPositionCalls\":{},"
            "\"setPositionRemoteSuppressedCalls\":{},"
            "\"setPositionRemoteOverrideCalls\":{},"
            "\"setPositionCallerRva\":{},\"setPositionThreadId\":{},"
            "\"setPositionFormType\":{},\"setPositionSourceIsNodeWorld\":{},"
            "\"setPositionInput\":[{},{},{}],"
            "\"setPositionPreReferenceError\":{}}}",
            referencePhase.SelectedFormId, JsonBool(referencePhase.HookInstalled),
            referencePhase.Update3DCalls, referencePhase.MoveHavokCalls,
            referencePhase.LastThreadId, referencePhase.LastMethod,
            referencePhase.LastDurationUs, referencePhase.LastReferenceDelta,
            referencePhase.LastNodeDelta, referencePhase.LastBodyDelta,
            referencePhase.LastRefBodyError, referencePhase.LastRefNodeError,
            referencePhase.SetPositionCalls,
            referencePhase.SetPositionRemoteSuppressedCalls,
            referencePhase.SetPositionRemoteOverrideCalls,
            referencePhase.SetPositionCallerRva,
            referencePhase.SetPositionThreadId, referencePhase.SetPositionFormType,
            JsonBool(referencePhase.SetPositionSourceIsNodeWorld),
            referencePhase.SetPositionInputX, referencePhase.SetPositionInputY,
            referencePhase.SetPositionInputZ,
            referencePhase.SetPositionPreReferenceError);
        const auto nodePhase = ObjectService::GetRenderNodePhaseDiagnostic();
        snapshot += fmt::format(
            ",\"renderNodePhaseProbe\":{{\"hookInstalled\":{},"
            "\"downwardCalls\":{},\"worldDataCalls\":{},"
            "\"transformBoundsCalls\":{},\"lastMethod\":{},"
            "\"lastThreadId\":{},\"lastDurationUs\":{},"
            "\"lastLocalDelta\":{},\"lastWorldDelta\":{},"
            "\"lastReferenceDelta\":{},\"lastRefNodeError\":{},"
            "\"worldDataCallerRva\":{},\"transformBoundsCallerRva\":{},"
            "\"worldDataReferenceDelta\":{},"
            "\"transformBoundsReferenceDelta\":{},"
            "\"worldDataTargetRva\":{},\"transformBoundsTargetRva\":{}}}",
            JsonBool(nodePhase.HookInstalled), nodePhase.DownwardCalls,
            nodePhase.WorldDataCalls, nodePhase.TransformBoundsCalls,
            nodePhase.LastMethod, nodePhase.LastThreadId,
            nodePhase.LastDurationUs, nodePhase.LastLocalDelta,
            nodePhase.LastWorldDelta, nodePhase.LastReferenceDelta,
            nodePhase.LastRefNodeError, nodePhase.WorldDataCallerRva,
            nodePhase.TransformBoundsCallerRva,
            nodePhase.WorldDataReferenceDelta,
            nodePhase.TransformBoundsReferenceDelta,
            nodePhase.WorldDataTargetRva,
            nodePhase.TransformBoundsTargetRva);
        const auto collisionSync = ObjectService::GetCollisionSyncDiagnostic();
        snapshot += fmt::format(
            ",\"collisionSyncProbe\":{{\"selectedCalls\":{},"
            "\"lastCallerRva\":{},"
            "\"lastThreadId\":{},\"lastDurationUs\":{},"
            "\"lastNodeDelta\":{},\"lastReferenceDelta\":{},"
            "\"lastBodyDelta\":{},\"lastPreNodeReferenceError\":{},"
            "\"lastPostNodeReferenceError\":{}}}",
            collisionSync.SelectedCalls, collisionSync.LastCallerRva,
            collisionSync.LastThreadId,
            collisionSync.LastDurationUs, collisionSync.LastNodeDelta,
            collisionSync.LastReferenceDelta, collisionSync.LastBodyDelta,
            collisionSync.LastPreNodeReferenceError,
            collisionSync.LastPostNodeReferenceError);
        const auto collisionWorld = ObjectService::GetCollisionWorldDiagnostic();
        snapshot += fmt::format(
            ",\"collisionWorldProbe\":{{\"selectedCalls\":{},"
            "\"lastCallerRva\":{},\"lastThreadId\":{},"
            "\"lastDurationUs\":{},\"lastInput\":[{},{},{}],"
            "\"lastInputBodyError\":{},\"lastInputNodeError\":{},"
            "\"lastPostNodeInputError\":{},"
            "\"lastPostReferenceInputError\":{}}}",
            collisionWorld.SelectedCalls, collisionWorld.LastCallerRva,
            collisionWorld.LastThreadId, collisionWorld.LastDurationUs,
            collisionWorld.LastInputX, collisionWorld.LastInputY,
            collisionWorld.LastInputZ, collisionWorld.LastInputBodyError,
            collisionWorld.LastInputNodeError,
            collisionWorld.LastPostNodeInputError,
            collisionWorld.LastPostReferenceInputError);
        const auto worldUpdate = ObjectService::GetWorldUpdateDiagnostic();
        snapshot += fmt::format(
            ",\"worldUpdateProbe\":{{\"calls\":{},\"lastThreadId\":{},"
            "\"lastDurationUs\":{},\"nativeStepCalls\":{},"
            "\"nativeStepLastThreadId\":{},\"nativeStepLastDurationUs\":{},"
            "\"selectedBodySteps\":{},\"selectedBodyChangedSteps\":{},"
            "\"lastSelectedBodyStepDelta\":{},"
            "\"peakSelectedBodyStepDelta\":{},"
            "\"selectedBodyStepsOver75Units\":{},"
            "\"peakSelectedBodyStepTimeMs\":{},"
            "\"peakSelectedBodyStepDt\":{},"
            "\"peakSelectedBodyPreLinearSpeed\":{},"
            "\"peakSelectedBodyPostLinearSpeed\":{},"
            "\"peakSelectedBodyPreAngularSpeed\":{},"
            "\"peakSelectedBodyPostAngularSpeed\":{},"
            "\"peakSelectedBodyMotionType\":{},"
            "\"peakSelectedBodyTargetApplied\":{},"
            "\"peakSelectedBodyTargetAgeMs\":{},"
            "\"peakSelectedBodyVelocityAfterWrite\":{},"
            "\"selectedBodyCacheRefreshes\":{},"
            "\"selectedStepWorldMatches\":{},"
            "\"selectedStepBodyReads\":{},"
            "\"selectedCollisionWorldDuringUpdate\":{},"
            "\"selectedCollisionWorldOutsideUpdate\":{},"
            "\"lastSelectedCollisionWorldAfterUpdateUs\":{},"
            "\"lastSelectedCollisionWorldAfterStepUs\":{}}}",
            worldUpdate.Calls, worldUpdate.LastThreadId,
            worldUpdate.LastDurationUs,
            worldUpdate.NativeStepCalls,
            worldUpdate.NativeStepLastThreadId,
            worldUpdate.NativeStepLastDurationUs,
            worldUpdate.SelectedBodySteps,
            worldUpdate.SelectedBodyChangedSteps,
            worldUpdate.LastSelectedBodyStepDelta,
            worldUpdate.PeakSelectedBodyStepDelta,
            worldUpdate.SelectedBodyStepsOver75Units,
            worldUpdate.PeakSelectedBodyStepTimeMs,
            worldUpdate.PeakSelectedBodyStepDt,
            worldUpdate.PeakSelectedBodyPreLinearSpeed,
            worldUpdate.PeakSelectedBodyPostLinearSpeed,
            worldUpdate.PeakSelectedBodyPreAngularSpeed,
            worldUpdate.PeakSelectedBodyPostAngularSpeed,
            worldUpdate.PeakSelectedBodyMotionType,
            JsonBool(worldUpdate.PeakSelectedBodyTargetApplied),
            worldUpdate.PeakSelectedBodyTargetAgeMs,
            worldUpdate.PeakSelectedBodyVelocityAfterWrite,
            worldUpdate.SelectedBodyCacheRefreshes,
            worldUpdate.SelectedStepWorldMatches,
            worldUpdate.SelectedStepBodyReads,
            worldUpdate.SelectedCollisionWorldDuringUpdate,
            worldUpdate.SelectedCollisionWorldOutsideUpdate,
            worldUpdate.LastSelectedCollisionWorldAfterUpdateUs,
            worldUpdate.LastSelectedCollisionWorldAfterStepUs);
        const auto preStepPlayback = ObjectService::GetPreStepPlaybackDiagnostic();
        snapshot += fmt::format(
            ",\"preStepPlaybackProbe\":{{\"selectedFormId\":{},\"mode\":{},"
            "\"publishedTargets\":{},\"attempts\":{},\"applied\":{},"
            "\"staleSkips\":{},\"lastSourceAgeMs\":{},"
            "\"lastPreError\":{},\"lastPostError\":{},"
            "\"lastVelocityCorrection\":{},\"poseWrites\":{},"
            "\"lastPoseStep\":{},\"hostScans\":{},"
            "\"hostPacketsSent\":{},\"hostUpdatesQueued\":{},"
            "\"hostBodyOnlyUpdates\":{},\"hostSelectedObserved\":{},"
            "\"hostSelectedQueued\":{},\"followerPacketsReceived\":{},"
            "\"followerSelectedReceived\":{},"
            "\"lastSelectedTransitAgeMs\":{},"
            "\"lastHostScanDurationUs\":{},"
            "\"lastHostReferencesVisited\":{},"
            "\"lastHostUpdatesQueued\":{}}}",
            preStepPlayback.SelectedFormId,
            preStepPlayback.Mode,
            preStepPlayback.PublishedTargets, preStepPlayback.Attempts,
            preStepPlayback.Applied, preStepPlayback.StaleSkips,
            preStepPlayback.LastSourceAgeMs, preStepPlayback.LastPreError,
            preStepPlayback.LastPostError,
            preStepPlayback.LastVelocityCorrection,
            preStepPlayback.PoseWrites, preStepPlayback.LastPoseStep,
            preStepPlayback.HostScans, preStepPlayback.HostPacketsSent,
            preStepPlayback.HostUpdatesQueued,
            preStepPlayback.HostBodyOnlyUpdates,
            preStepPlayback.HostSelectedObserved,
            preStepPlayback.HostSelectedQueued,
            preStepPlayback.FollowerPacketsReceived,
            preStepPlayback.FollowerSelectedReceived,
            preStepPlayback.LastSelectedTransitAgeMs,
            preStepPlayback.LastHostScanDurationUs,
            preStepPlayback.LastHostReferencesVisited,
            preStepPlayback.LastHostUpdatesQueued);

        const auto& party = m_world.GetPartyService();
        const auto& transport = m_world.GetTransport();
        snapshot += fmt::format(
            ",\"session\":{{\"online\":{},\"localPlayerId\":{},\"inParty\":{},\"leader\":{},"
            "\"leaderPlayerId\":{},\"memberCount\":{},\"campaignId\":\"{}\","
            "\"campaignRevision\":{},\"authorityEpoch\":{}}}",
            JsonBool(transport.IsOnline()), transport.GetLocalPlayerId(), JsonBool(party.IsInParty()),
            JsonBool(party.IsLeader()), party.GetLeaderPlayerId(), party.GetPartyMembers().size(),
            EscapeJson(transport.GetCampaignId().c_str()), transport.GetCampaignRevision(),
            transport.GetAuthorityEpoch());

        const auto nativeDispatch = GetNativeDispatchDiagnostic();
        const auto gameLoop = GetGameLoopDiagnostic();
        snapshot += fmt::format(
            ",\"gameLoopTiming\":{{\"vmHookCalls\":{},"
            "\"vmActiveCalls\":{},\"vmInactiveCalls\":{},"
            "\"vmAppTotalUs\":{},\"vmOriginalTotalUs\":{},"
            "\"vmLastAppUs\":{},\"vmLastOriginalUs\":{},"
            "\"vmLastEntryGapUs\":{},\"vmMaxEntryGapUs\":{},"
            "\"vmMaxAppUs\":{},\"vmMaxOriginalUs\":{},"
            "\"worldCalls\":{},\"worldPreUpdateTotalUs\":{},"
            "\"worldRunnerTotalUs\":{},"
            "\"worldDispatcherTotalUs\":{},"
            "\"worldGameTestTotalUs\":{},"
            "\"worldLastGameTestUs\":{},\"worldMaxGameTestUs\":{},"
            "\"worldLastEntryGapUs\":{},"
            "\"worldMaxDispatcherUs\":{},"
            "\"worldMaxEntryGapUs\":{},"
            "\"worldGapsOver50Ms\":{},"
            "\"worldGapsOver100Ms\":{},"
            "\"worldGapsOver250Ms\":{}}}",
            gameLoop.VmHookCalls, gameLoop.VmActiveCalls,
            gameLoop.VmInactiveCalls, gameLoop.VmAppTotalUs,
            gameLoop.VmOriginalTotalUs, gameLoop.VmLastAppUs,
            gameLoop.VmLastOriginalUs, gameLoop.VmLastEntryGapUs,
            gameLoop.VmMaxEntryGapUs, gameLoop.VmMaxAppUs,
            gameLoop.VmMaxOriginalUs, gameLoop.WorldCalls,
            gameLoop.WorldPreUpdateTotalUs, gameLoop.WorldRunnerTotalUs,
            gameLoop.WorldDispatcherTotalUs,
            gameLoop.WorldGameTestTotalUs,
            gameLoop.WorldLastGameTestUs, gameLoop.WorldMaxGameTestUs,
            gameLoop.WorldLastEntryGapUs,
            gameLoop.WorldMaxDispatcherUs,
            gameLoop.WorldMaxEntryGapUs,
            gameLoop.WorldGapsOver50Ms,
            gameLoop.WorldGapsOver100Ms,
            gameLoop.WorldGapsOver250Ms);
        snapshot += fmt::format(
            ",\"papyrusNativeDispatch\":{{\"count\":{},\"lastFunctionHash\":{},\"lastTimeMs\":{},"
            "\"vmUpdateCount\":{},\"vmUpdateLastStartMs\":{},\"vmUpdateLastDurationUs\":{},"
            "\"vmTaskletCount\":{},\"vmTaskletLastStartMs\":{},\"vmTaskletLastDurationUs\":{},"
            "\"disablePlayerControlsCalls\":{},\"enablePlayerControlsCalls\":{},"
            "\"lastControlCallTimeMs\":{},\"lastControlCallEnabled\":{}}}",
            nativeDispatch.Count, nativeDispatch.LastFunctionHash, nativeDispatch.LastTimeMs,
            nativeDispatch.VmUpdateCount, nativeDispatch.VmUpdateLastStartMs,
            nativeDispatch.VmUpdateLastDurationUs, nativeDispatch.VmTaskletCount,
            nativeDispatch.VmTaskletLastStartMs, nativeDispatch.VmTaskletLastDurationUs,
            nativeDispatch.DisablePlayerControlsCalls, nativeDispatch.EnablePlayerControlsCalls,
            nativeDispatch.LastControlCallTimeMs, JsonBool(nativeDispatch.LastControlCallEnabled));

        if (auto* pPlayer = PlayerCharacter::Get())
        {
            auto* pCell = pPlayer->GetParentCellEx();
            auto* pWorldspace = pPlayer->GetWorldSpace();
            auto* pPackage = pPlayer->currentProcess ? pPlayer->currentProcess->package : nullptr;
            snapshot += fmt::format(
                ",\"player\":{{\"present\":true,\"formId\":{},\"cellId\":{},\"worldspaceId\":{},"
                "\"position\":[{},{},{}],\"rotation\":[{},{},{}],\"dead\":{},\"bleedingOut\":{},"
                "\"inCombat\":{},\"weaponDrawn\":{},\"weaponFullyDrawn\":{},\"dialogueHandle\":{},"
                "\"combatHandle\":{},\"killerHandle\":{},\"packageFormId\":{},\"movementType\":{},"
                "\"health\":{},\"magicka\":{},\"stamina\":{},\"speed\":{},"
                "\"actorStateFlags1\":{},\"actorStateFlags2\":{}}}",
                pPlayer->formID, pCell ? pCell->formID : 0, pWorldspace ? pWorldspace->formID : 0,
                pPlayer->position.x, pPlayer->position.y, pPlayer->position.z,
                pPlayer->rotation.x, pPlayer->rotation.y, pPlayer->rotation.z,
                JsonBool(pPlayer->IsDead()), JsonBool(pPlayer->actorState.IsBleedingOut()),
                JsonBool(pPlayer->IsInCombat()), JsonBool(pPlayer->actorState.IsWeaponDrawn()),
                JsonBool(pPlayer->actorState.IsWeaponFullyDrawn()), pPlayer->dialogueHandle,
                pPlayer->combatHandle, pPlayer->killerHandle, pPackage ? pPackage->formID : 0,
                pPlayer->currentProcess ? pPlayer->currentProcess->movementType : -1,
                pPlayer->GetActorValue(ActorValueInfo::kHealth),
                pPlayer->GetActorValue(ActorValueInfo::kMagicka),
                pPlayer->GetActorValue(ActorValueInfo::kStamina), pPlayer->GetSpeed(),
                pPlayer->actorState.flags1, pPlayer->actorState.flags2);

            const auto playerInventory = pPlayer->GetActorInventory();
            uint64_t inventoryChecksum = 1469598103934665603ull;
            for (const auto& entry : playerInventory.Entries)
            {
                HashValue(inventoryChecksum, entry.BaseId.ModId);
                HashValue(inventoryChecksum, entry.BaseId.BaseId);
                HashValue(inventoryChecksum, entry.Count);
                HashValue(inventoryChecksum, entry.ExtraWorn);
                HashValue(inventoryChecksum, entry.ExtraWornLeft);
                HashValue(inventoryChecksum, entry.IsQuestItem);
            }
            snapshot += fmt::format(
                ",\"playerInventory\":{{\"entryCount\":{},\"checksum\":{},\"entries\":[",
                playerInventory.Entries.size(), inventoryChecksum);
            bool firstInventoryEntry = true;
            for (size_t i = 0; i < playerInventory.Entries.size() && i < 128; ++i)
            {
                const auto& entry = playerInventory.Entries[i];
                if (!firstInventoryEntry)
                    snapshot += ',';
                firstInventoryEntry = false;
                snapshot += fmt::format(
                    "{{\"modId\":{},\"baseId\":{},\"count\":{},\"worn\":{},\"questItem\":{}}}",
                    entry.BaseId.ModId, entry.BaseId.BaseId, entry.Count,
                    JsonBool(entry.IsWorn()), JsonBool(entry.IsQuestItem));
            }
            snapshot += "]}";

            if (const auto* pExtension = pPlayer->GetExtension())
            {
                const auto& action = pExtension->LatestAnimation;
                snapshot += fmt::format(
                    ",\"playerAnimation\":{{\"graphReady\":{},\"graphDescriptor\":{},"
                    "\"reconciliationStage\":{},\"tick\":{},\"actionId\":{},\"targetId\":{},"
                    "\"idleId\":{},\"type\":{},\"state1\":{},\"state2\":{},\"event\":\"{}\"}}",
                    JsonBool(pPlayer->animationGraphHolder.IsReady()), pExtension->GraphDescriptorHash,
                    static_cast<uint32_t>(pExtension->Reconciliation), action.Tick, action.ActionId,
                    action.TargetId, action.IdleId, action.Type, action.State1, action.State2,
                    EscapeJson(action.EventName.c_str()));
            }

            // One-shot player graph audit: the stationary follower camera can
            // move even when cinematic camera playback is inactive. Capture
            // the native behavior inputs that can drive first-person motion,
            // using the same bounded, locked reader as the NPC pose audit.
            const auto playerNativeAnimation = SampleNativeActorAnimation(pPlayer, true);
            snapshot += fmt::format(
                ",\"playerNativeAnimation\":{{\"graphReady\":{},"
                "\"graphCount\":{},\"graphIndex\":{},"
                "\"stateId\":{},\"timeInState\":{},"
                "\"behaviorActive\":{},\"behaviorLinked\":{},"
                "\"rootClonePresent\":{},\"rootCloneSameAsTemplate\":{},"
                "\"cloneStateReadable\":{},"
                "\"cloneStateActive\":{},\"cloneStateId\":{},"
                "\"clonePreviousStateId\":{},\"cloneTimeInState\":{},"
                "\"poseCount\":{},\"poseChecksum\":{},"
                "\"renderBoneCount\":{},\"renderBoneChecksum\":{},"
                "\"renderWorldBoneChecksum\":{},"
                "\"graphVariableCount\":{},\"graphVariableChecksum\":{},"
                "\"sampledCount\":{},\"values\":[",
                JsonBool(playerNativeAnimation.GraphReady),
                playerNativeAnimation.GraphCount,
                playerNativeAnimation.GraphIndex,
                playerNativeAnimation.StateId,
                std::isfinite(playerNativeAnimation.TimeInState) ?
                    playerNativeAnimation.TimeInState : 0.f,
                JsonBool(playerNativeAnimation.BehaviorActive),
                JsonBool(playerNativeAnimation.BehaviorLinked),
                JsonBool(playerNativeAnimation.RootClonePresent),
                JsonBool(playerNativeAnimation.RootCloneSameAsTemplate),
                JsonBool(playerNativeAnimation.CloneStateReadable),
                JsonBool(playerNativeAnimation.CloneStateActive),
                playerNativeAnimation.CloneStateId,
                playerNativeAnimation.ClonePreviousStateId,
                std::isfinite(playerNativeAnimation.CloneTimeInState) ?
                    playerNativeAnimation.CloneTimeInState : 0.f,
                playerNativeAnimation.PoseCount,
                playerNativeAnimation.PoseChecksum,
                playerNativeAnimation.RenderBoneCount,
                playerNativeAnimation.RenderBoneChecksum,
                playerNativeAnimation.RenderWorldBoneChecksum,
                playerNativeAnimation.GraphVariableCount,
                playerNativeAnimation.GraphVariableChecksum,
                playerNativeAnimation.GraphVariables.size());
            for (size_t i = 0; i < playerNativeAnimation.GraphVariables.size(); ++i)
            {
                if (i != 0)
                    snapshot += ',';
                snapshot += fmt::format("{}", playerNativeAnimation.GraphVariables[i]);
            }
            snapshot += fmt::format("],\"nameCount\":{},\"infoCount\":{},"
                "\"sampledNameCount\":{},\"names\":[",
                playerNativeAnimation.GraphVariableNameCount,
                playerNativeAnimation.GraphVariableInfoCount,
                playerNativeAnimation.GraphVariableNames.size());
            for (size_t i = 0; i < playerNativeAnimation.GraphVariableNames.size(); ++i)
            {
                if (i != 0)
                    snapshot += ',';
                snapshot += fmt::format("\"{}\"",
                    EscapeJson(playerNativeAnimation.GraphVariableNames[i]));
            }
            snapshot += "]}";
        }
        else
            snapshot += ",\"player\":{\"present\":false}";

        if (auto* pControls = PlayerControls::GetInstance())
        {
            snapshot += fmt::format(
                ",\"controls\":{{\"present\":true,\"blocked\":{},\"movement\":{},\"look\":{},"
                "\"sprint\":{},\"readyWeapon\":{},\"activate\":{},\"jump\":{},\"shout\":{},"
                "\"attackBlock\":{},\"sneak\":{},\"togglePov\":{},\"autoMove\":{},"
                "\"running\":{},\"povScriptMode\":{},\"remapMode\":{},"
                "\"moveInput\":[{},{}],\"lookInput\":[{},{}]}}",
                JsonBool(pControls->bBlockPlayerInput), JsonBool(HandlerEnabled(pControls->pMovementHandler)),
                JsonBool(HandlerEnabled(pControls->pLookHandler)), JsonBool(HandlerEnabled(pControls->pSprintHandler)),
                JsonBool(HandlerEnabled(pControls->pReadyWeaponHandler)), JsonBool(HandlerEnabled(pControls->pActivateHandler)),
                JsonBool(HandlerEnabled(pControls->pJumpHandler)), JsonBool(HandlerEnabled(pControls->shoutHandler)),
                JsonBool(HandlerEnabled(pControls->attackBlockHandler)), JsonBool(HandlerEnabled(pControls->sneakHandler)),
                JsonBool(HandlerEnabled(pControls->togglePOVHandler)), JsonBool(pControls->Data.bAutoMove),
                JsonBool(pControls->Data.bRunning), JsonBool(pControls->Data.povScriptMode),
                JsonBool(pControls->Data.remapMode), pControls->Data.MoveInputVec.x,
                pControls->Data.MoveInputVec.y, pControls->Data.LookInputVec.x,
                pControls->Data.LookInputVec.y);
        }
        else
            snapshot += ",\"controls\":{\"present\":false}";

        // ControlMap::enabledControls is independent of PlayerControls'
        // handler enable flags. Skyrim AE's ToggleControls (ID 68545, named
        // EnableOtherEvent by this project's legacy wrapper) writes
        // the effective mask at +0x120; Store/LoadStoredControls use +0x124.
        // The +0x118 CommonLibSSE-NG declaration does not fit this AE exe.
        uint32_t controlMapEnabledFlags{};
        uint32_t controlMapStoredFlags{};
        const auto* pControlMap = BSInputEnableManager::Get();
        const bool controlMapReadable = pControlMap &&
            ReadNative(reinterpret_cast<const uint8_t*>(pControlMap) + 0x120,
                       controlMapEnabledFlags) &&
            ReadNative(reinterpret_cast<const uint8_t*>(pControlMap) + 0x124,
                       controlMapStoredFlags);
        snapshot += fmt::format(
            ",\"controlMap\":{{\"present\":{},\"enabledFlags\":{},"
            "\"storedFlags\":{},\"storedValid\":{},"
            "\"movement\":{},\"looking\":{},\"activate\":{},"
            "\"menu\":{},\"console\":{},\"povSwitch\":{},"
            "\"fighting\":{},\"sneaking\":{},\"mainFour\":{},"
            "\"wheelZoom\":{},\"jumping\":{},\"vats\":{}}}",
            JsonBool(controlMapReadable),
            controlMapReadable ? fmt::format("{}", controlMapEnabledFlags) : "null",
            controlMapReadable ? fmt::format("{}", controlMapStoredFlags) : "null",
            JsonBool(controlMapReadable && controlMapStoredFlags != 0x80000000u),
            JsonBool(controlMapReadable && (controlMapEnabledFlags & (1u << 0)) != 0),
            JsonBool(controlMapReadable && (controlMapEnabledFlags & (1u << 1)) != 0),
            JsonBool(controlMapReadable && (controlMapEnabledFlags & (1u << 2)) != 0),
            JsonBool(controlMapReadable && (controlMapEnabledFlags & (1u << 3)) != 0),
            JsonBool(controlMapReadable && (controlMapEnabledFlags & (1u << 4)) != 0),
            JsonBool(controlMapReadable && (controlMapEnabledFlags & (1u << 5)) != 0),
            JsonBool(controlMapReadable && (controlMapEnabledFlags & (1u << 6)) != 0),
            JsonBool(controlMapReadable && (controlMapEnabledFlags & (1u << 7)) != 0),
            JsonBool(controlMapReadable && (controlMapEnabledFlags & (1u << 8)) != 0),
            JsonBool(controlMapReadable && (controlMapEnabledFlags & (1u << 9)) != 0),
            JsonBool(controlMapReadable && (controlMapEnabledFlags & (1u << 10)) != 0),
            JsonBool(controlMapReadable && (controlMapEnabledFlags & (1u << 11)) != 0));

        if (auto* pCamera = PlayerCamera::Get())
        {
            const auto* pCameraRoot = pCamera->cameraNode;
            snapshot += fmt::format(
                ",\"camera\":{{\"present\":true,\"firstPerson\":{},\"hasState\":{},"
                "\"position\":[{},{},{}],\"rotation\":[{},{}],\"zoom\":{},"
                "\"rootPresent\":{},\"rootWorldPosition\":[{},{},{}],"
                "\"rootWorldRotation\":[{},{},{},{},{},{},{},{},{}]}}",
                JsonBool(pCamera->IsFirstPerson()), JsonBool(pCamera->state != nullptr),
                pCamera->pos.x, pCamera->pos.y, pCamera->pos.z, pCamera->rotX, pCamera->rotZ, pCamera->zoom,
                JsonBool(pCameraRoot != nullptr),
                pCameraRoot ? pCameraRoot->world.translate.x : 0.f,
                pCameraRoot ? pCameraRoot->world.translate.y : 0.f,
                pCameraRoot ? pCameraRoot->world.translate.z : 0.f,
                pCameraRoot ? pCameraRoot->world.rotate.entry[0][0] : 0.f,
                pCameraRoot ? pCameraRoot->world.rotate.entry[0][1] : 0.f,
                pCameraRoot ? pCameraRoot->world.rotate.entry[0][2] : 0.f,
                pCameraRoot ? pCameraRoot->world.rotate.entry[1][0] : 0.f,
                pCameraRoot ? pCameraRoot->world.rotate.entry[1][1] : 0.f,
                pCameraRoot ? pCameraRoot->world.rotate.entry[1][2] : 0.f,
                pCameraRoot ? pCameraRoot->world.rotate.entry[2][0] : 0.f,
                pCameraRoot ? pCameraRoot->world.rotate.entry[2][1] : 0.f,
                pCameraRoot ? pCameraRoot->world.rotate.entry[2][2] : 0.f);

            // CommonLibSSE-NG 1.7 candidate FirstPersonState layout. Read the
            // native spring and pitch fields only in a requested snapshot;
            // these can distinguish persistent state motion from graph pose.
            NiPoint3 lastPosition{};
            NiPoint3 springVelocity{};
            NiPoint3 dampeningOffset{};
            float currentPitchOffset{};
            float targetPitchOffset{};
            uint8_t cameraOverride{};
            uint8_t cameraPitchOverride{};
            uint32_t nativeStateId{0xFFFFFFFFu};
            void* pFirstPersonCameraObject{};
            NiPoint3 firstPersonObjectWorld{};
            NiPoint3 player3DRootWorld{};
            const auto* pStateBytes = reinterpret_cast<const uint8_t*>(pCamera->state);
            const bool firstPersonStateReadable = pStateBytes &&
                ReadNative(pStateBytes + 0x18, nativeStateId) &&
                nativeStateId == 0 &&
                ReadNative(pStateBytes + 0x30, lastPosition) &&
                ReadNative(pStateBytes + 0x3C, springVelocity) &&
                ReadNative(pStateBytes + 0x48, dampeningOffset) &&
                ReadNative(pStateBytes + 0x74, currentPitchOffset) &&
                ReadNative(pStateBytes + 0x78, targetPitchOffset) &&
                ReadNative(pStateBytes + 0x84, cameraOverride) &&
                ReadNative(pStateBytes + 0x85, cameraPitchOverride) &&
                std::isfinite(lastPosition.x) && std::isfinite(lastPosition.y) &&
                std::isfinite(lastPosition.z) &&
                std::isfinite(springVelocity.x) &&
                std::isfinite(springVelocity.y) &&
                std::isfinite(springVelocity.z) &&
                std::isfinite(dampeningOffset.x) &&
                std::isfinite(dampeningOffset.y) &&
                std::isfinite(dampeningOffset.z) &&
                std::isfinite(currentPitchOffset) &&
                std::isfinite(targetPitchOffset);
            const bool cameraObjectReadable = firstPersonStateReadable &&
                ReadNative(pStateBytes + 0x58, pFirstPersonCameraObject) &&
                pFirstPersonCameraObject &&
                ReadNative(reinterpret_cast<const uint8_t*>(
                    pFirstPersonCameraObject) + offsetof(NiAVObject, world) +
                    offsetof(NiTransform, translate), firstPersonObjectWorld) &&
                std::isfinite(firstPersonObjectWorld.x) &&
                std::isfinite(firstPersonObjectWorld.y) &&
                std::isfinite(firstPersonObjectWorld.z);
            if (cameraObjectReadable)
            {
                if (auto* pLocalPlayer = PlayerCharacter::Get())
                    AnimationGraphUpdateTrace::WatchPlayerCameraObject(
                        &pLocalPlayer->animationGraphHolder, pCamera->state,
                        pFirstPersonCameraObject);
            }
            auto* pPlayer3D = PlayerCharacter::Get() ?
                PlayerCharacter::Get()->GetNiNode() : nullptr;
            const bool player3DRootReadable = pPlayer3D &&
                ReadNative(&pPlayer3D->world.translate, player3DRootWorld) &&
                std::isfinite(player3DRootWorld.x) &&
                std::isfinite(player3DRootWorld.y) &&
                std::isfinite(player3DRootWorld.z);
            snapshot += fmt::format(
                ",\"firstPersonNativeState\":{{\"readable\":{},"
                "\"lastPosition\":[{},{},{}],"
                "\"springVelocity\":[{},{},{}],"
                "\"dampeningOffset\":[{},{},{}],"
                "\"currentPitchOffset\":{},\"targetPitchOffset\":{},"
                "\"cameraOverride\":{},\"cameraPitchOverride\":{},"
                "\"cameraObjectReadable\":{},\"cameraObjectWorld\":[{},{},{}],"
                "\"player3DRootReadable\":{},\"player3DRootWorld\":[{},{},{}]}}",
                JsonBool(firstPersonStateReadable),
                firstPersonStateReadable ? lastPosition.x : 0.f,
                firstPersonStateReadable ? lastPosition.y : 0.f,
                firstPersonStateReadable ? lastPosition.z : 0.f,
                firstPersonStateReadable ? springVelocity.x : 0.f,
                firstPersonStateReadable ? springVelocity.y : 0.f,
                firstPersonStateReadable ? springVelocity.z : 0.f,
                firstPersonStateReadable ? dampeningOffset.x : 0.f,
                firstPersonStateReadable ? dampeningOffset.y : 0.f,
                firstPersonStateReadable ? dampeningOffset.z : 0.f,
                firstPersonStateReadable ? currentPitchOffset : 0.f,
                firstPersonStateReadable ? targetPitchOffset : 0.f,
                JsonBool(firstPersonStateReadable && cameraOverride != 0),
                JsonBool(firstPersonStateReadable && cameraPitchOverride != 0),
                JsonBool(cameraObjectReadable),
                cameraObjectReadable ? firstPersonObjectWorld.x : 0.f,
                cameraObjectReadable ? firstPersonObjectWorld.y : 0.f,
                cameraObjectReadable ? firstPersonObjectWorld.z : 0.f,
                JsonBool(player3DRootReadable),
                player3DRootReadable ? player3DRootWorld.x : 0.f,
                player3DRootReadable ? player3DRootWorld.y : 0.f,
                player3DRootReadable ? player3DRootWorld.z : 0.f);
        }
        else
            snapshot += ",\"camera\":{\"present\":false}";

        const auto cameraAuthority = m_world.ctx().at<CameraService>().GetDiagnostic();
        snapshot += fmt::format(
            ",\"cameraAuthority\":{{\"inputGated\":{},"
            "\"hasHostSnapshot\":{},\"localStateId\":{},"
            "\"hostStateId\":{},\"lastHostTick\":{},"
            "\"receivedPackets\":{},\"nativePostUpdates\":{},"
            "\"lastNativeThreadId\":{},\"hookedVtables\":{}}}",
            JsonBool(cameraAuthority.InputGated),
            JsonBool(cameraAuthority.HasHostSnapshot),
            cameraAuthority.LocalStateId, cameraAuthority.HostStateId,
            cameraAuthority.LastHostTick, cameraAuthority.ReceivedPackets,
            cameraAuthority.NativePostUpdates,
            cameraAuthority.LastNativeThreadId,
            cameraAuthority.HookedVtables);

        const auto cameraTrace = m_world.ctx().at<CameraService>().GetNativeUpdateTrace();
        snapshot += ",\"nativeCameraUpdateTrace\":[";
        for (size_t i = 0; i < cameraTrace.Count; ++i)
        {
            const auto& sample = cameraTrace.Samples[i];
            if (i != 0)
                snapshot += ',';
            snapshot += fmt::format(
                "{{\"timeMs\":{},\"stateId\":{},\"before\":[{},{},{}],"
                "\"after\":[{},{},{}],\"beforeLocal\":[{},{},{}],"
                "\"afterLocal\":[{},{},{}],\"parentStable\":{},"
                "\"beforeParentWorld\":[{},{},{}],"
                "\"afterParentWorld\":[{},{},{}],"
                "\"firstPersonStateReadable\":{},"
                "\"pitchBefore\":{},\"pitchAfter\":{},"
                "\"targetPitchBefore\":{},\"targetPitchAfter\":{},"
                "\"firstPersonObjectStable\":{},"
                "\"objectBefore\":[{},{},{}],"
                "\"objectAfter\":[{},{},{}],"
                "\"graphPitchReadable\":{},"
                "\"graphPitchBefore\":{},\"graphPitchAfter\":{}}}",
                sample.TimeMs, sample.StateId,
                sample.Before[0], sample.Before[1], sample.Before[2],
                sample.After[0], sample.After[1], sample.After[2],
                sample.BeforeLocal[0], sample.BeforeLocal[1], sample.BeforeLocal[2],
                sample.AfterLocal[0], sample.AfterLocal[1], sample.AfterLocal[2],
                JsonBool(sample.ParentStable),
                sample.BeforeParentWorld[0], sample.BeforeParentWorld[1],
                sample.BeforeParentWorld[2],
                sample.AfterParentWorld[0], sample.AfterParentWorld[1],
                sample.AfterParentWorld[2],
                JsonBool(sample.FirstPersonStateReadable),
                sample.PitchBefore, sample.PitchAfter,
                sample.TargetPitchBefore, sample.TargetPitchAfter,
                JsonBool(sample.FirstPersonObjectStable),
                sample.ObjectBefore[0], sample.ObjectBefore[1],
                sample.ObjectBefore[2],
                sample.ObjectAfter[0], sample.ObjectAfter[1],
                sample.ObjectAfter[2],
                JsonBool(sample.GraphPitchReadable),
                sample.GraphPitchBefore, sample.GraphPitchAfter);
        }
        snapshot += ']';

        const auto graphCameraTrace =
            AnimationGraphUpdateTrace::GetPlayerCameraObjectTrace();
        snapshot += fmt::format(
            ",\"playerCameraObjectGraphWatchGeneration\":{},"
            "\"playerCameraObjectGraphTrace\":[",
            graphCameraTrace.WatchGeneration);
        for (size_t i = 0; i < graphCameraTrace.Count; ++i)
        {
            const auto& sample = graphCameraTrace.Samples[i];
            if (i != 0)
                snapshot += ',';
            snapshot += fmt::format(
                "{{\"startMs\":{},\"endMs\":{},\"threadId\":{},"
                "\"watchGeneration\":{},"
                "\"cameraSequenceBefore\":{},\"cameraSequenceAfter\":{},"
                "\"valid\":{},\"localBefore\":[{},{},{}],"
                "\"localAfter\":[{},{},{}],"
                "\"worldBefore\":[{},{},{}],"
                "\"worldAfter\":[{},{},{}]}}",
                sample.StartMs, sample.EndMs, sample.ThreadId,
                sample.WatchGeneration,
                sample.CameraSequenceBefore, sample.CameraSequenceAfter,
                JsonBool(sample.Valid),
                sample.LocalBefore[0], sample.LocalBefore[1],
                sample.LocalBefore[2],
                sample.LocalAfter[0], sample.LocalAfter[1],
                sample.LocalAfter[2],
                sample.WorldBefore[0], sample.WorldBefore[1],
                sample.WorldBefore[2],
                sample.WorldAfter[0], sample.WorldAfter[1],
                sample.WorldAfter[2]);
        }
        snapshot += ']';

        snapshot += ",\"menus\":[";
        if (auto* pUI = UI::Get())
        {
            bool firstMenu = true;
            for (auto* pMenu : pUI->menuStack)
            {
                if (!pMenu)
                    continue;
                auto* pName = pUI->LookupMenuNameByInstance(pMenu);
                if (!pName)
                    continue;
                if (!firstMenu)
                    snapshot += ',';
                snapshot += fmt::format("\"{}\"", EscapeJson(pName->AsAscii()));
                firstMenu = false;
            }
        }
        snapshot += ']';

        snapshot += fmt::format(
            ",\"titleSequence\":{{\"menuOpen\":{},\"transitions\":{},\"lastTransitionMs\":{}}}",
            JsonBool(m_titleSequenceMenuOpen), m_titleSequenceMenuTransitions,
            m_lastTitleSequenceTransitionMs);

        if (auto* pUI = UI::Get())
        {
            snapshot += fmt::format(
                ",\"ui\":{{\"loading\":{},\"dialogue\":{},\"pausesGame\":{},\"allowSaving\":{},"
                "\"disablePauseMenu\":{},\"modal\":{},\"visible\":{},\"closingAllMenus\":{},"
                "\"dontHideCursor\":{}}}",
                JsonBool(pUI->GetMenuOpen(BSFixedString("Loading Menu"))),
                JsonBool(pUI->GetMenuOpen(BSFixedString("Dialogue Menu"))), pUI->numPausesGame,
                pUI->numAllowSaving, pUI->numDisablePauseMenu, JsonBool(pUI->modal),
                JsonBool(pUI->menuSystemVisible), JsonBool(pUI->closingAllMenus),
                pUI->numDontHideCursorWhenTopmost);
        }
        else
            snapshot += ",\"ui\":{\"present\":false}";

        const auto& loading = LoadingScreenProbe::Get().Sample();
        snapshot += fmt::format(
            ",\"loadingPresentation\":{{\"epoch\":{},\"loadingMenuPresent\":{},"
            "\"candidateListReadable\":{},\"mistMenuPresent\":{},\"mistStateReadable\":{},"
            "\"showMist\":{},\"showLoadScreen\":{},\"modelPresent\":{},"
            "\"modelHasUserData\":{},\"cameraPathPresent\":{},\"cameraFov\":{},"
            "\"angleZ\":{},\"selectedIdentityResolved\":{},\"selectedFormId\":{},"
            "\"eligibleFormIds\":[",
            loading.Epoch, JsonBool(loading.LoadingMenuPresent),
            JsonBool(loading.CandidateListReadable), JsonBool(loading.MistMenuPresent),
            JsonBool(loading.MistStateReadable), JsonBool(loading.ShowMist),
            JsonBool(loading.ShowLoadScreen), JsonBool(loading.LoadScreenModelPresent),
            JsonBool(loading.LoadScreenModelHasUserData), JsonBool(loading.CameraPathPresent),
            loading.CameraFov, loading.AngleZ, JsonBool(loading.SelectedIdentityResolved),
            loading.SelectedFormId);
        for (size_t i = 0; i < loading.EligibleFormIds.size(); ++i)
        {
            if (i != 0)
                snapshot += ',';
            snapshot += std::to_string(loading.EligibleFormIds[i]);
        }
        snapshot += "]}";

        if (auto* pDialogue = MenuTopicManager::Get())
        {
            const auto* pSpeaker = TESObjectREFR::GetByHandle(pDialogue->speaker.handle.iBits);
            snapshot += fmt::format(
                ",\"dialogue\":{{\"menuOpen\":{},\"speakerHandle\":{},\"speakerFormId\":{},"
                "\"hasOptions\":{}}}",
                JsonBool(pDialogue->menuOpen), pDialogue->speaker.handle.iBits,
                pSpeaker ? pSpeaker->formID : 0,
                JsonBool(pDialogue->pOptions != nullptr));
        }
        else
            snapshot += ",\"dialogue\":{\"present\":false}";

        if (auto* pTes = TES::Get())
        {
            snapshot += fmt::format(
                ",\"world\":{{\"centerGrid\":[{},{}],\"currentGrid\":[{},{}],"
                "\"interiorCellId\":{}}}",
                pTes->centerGridX, pTes->centerGridY, pTes->currentGridX, pTes->currentGridY,
                pTes->interiorCell ? pTes->interiorCell->formID : 0);
        }
        if (auto* pProcesses = ProcessLists::Get())
        {
            snapshot += fmt::format(
                ",\"actorProcessing\":{{\"highCount\":{},\"highHandles\":{},"
                "\"middleHighHandles\":{},\"middleLowHandles\":{},\"lowHandles\":{}}}",
                pProcesses->numberHighActors, pProcesses->highActorHandleArray.length,
                pProcesses->middleHighActorHandleArray.length, pProcesses->middleLowActorHandleArray.length,
                pProcesses->lowActorHandleArray.length);
        }

        // Source-pinned, read-only animation/ragdoll probe. Native graph and
        // bone reads are expensive, so keep this diagnostic bounded enough
        // that observation does not materially change scene timing.
        constexpr size_t cPoseProbeActorLimit = 12;
        const auto poseProbeStart = std::chrono::steady_clock::now();
        auto poseTargetTick = m_poseProbeTargetTick.load(std::memory_order_relaxed);
        const bool capturePose = poseTargetTick != 0 && worldTick >= poseTargetTick &&
            m_poseProbeTargetTick.compare_exchange_strong(poseTargetTick, 0,
                std::memory_order_relaxed);
        markSnapshotPhase(0);
        snapshot += ",\"actorPoseDiagnostics\":[";
        bool firstPoseDiagnostic = true;
        Set<uint32_t> sampledActorIds;
        auto appendPoseDiagnostic = [&](Actor* apActor, const char* acpSource)
        {
            if (!apActor || sampledActorIds.contains(apActor->formID) ||
                sampledActorIds.size() >= cPoseProbeActorLimit)
                return;
            sampledActorIds.insert(apActor->formID);
            if (!firstPoseDiagnostic)
                snapshot += ',';
            firstPoseDiagnostic = false;
            AppendActorPoseDiagnostic(snapshot, apActor, acpSource);
        };
        if (capturePose)
        {
            const auto selectedFormId = m_poseProbeFormId.load(
                std::memory_order_acquire);
            if (selectedFormId)
                appendPoseDiagnostic(Cast<Actor>(TESForm::GetById(selectedFormId)),
                    "selected");
            else
            {
                appendPoseDiagnostic(PlayerCharacter::Get(), "player");
                if (auto* pProcesses = ProcessLists::Get())
                {
                    const auto& handles = pProcesses->highActorHandleArray;
                    const bool handlesReadable = handles.length <= 2048 &&
                        handles.capacity >= handles.length &&
                        (handles.length == 0 ||
                            (handles.data && IsReadableRange(handles.data,
                                static_cast<size_t>(handles.length) * sizeof(uint32_t))));
                    if (handlesReadable)
                    {
                        for (uint32_t i = 0; i < handles.length &&
                            sampledActorIds.size() < cPoseProbeActorLimit; ++i)
                        {
                            auto* pReference = TESObjectREFR::GetByHandle(handles.data[i]);
                            appendPoseDiagnostic(pReference ? Cast<Actor>(pReference) : nullptr, "high");
                        }
                    }
                }
            }
        }
        snapshot += ']';
        if (capturePose)
            m_lastPoseSampleTick.store(worldTick, std::memory_order_relaxed);
        snapshot += fmt::format(",\"actorPoseProbeDurationUs\":{}",
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - poseProbeStart).count());
        snapshot += fmt::format(",\"actorPoseSampleTick\":{},\"actorPoseLastSampleTick\":{}",
            capturePose ? worldTick : 0, m_lastPoseSampleTick.load(std::memory_order_relaxed));

        if (const auto* pWindow = BSGraphics::GetMainWindow(); pWindow && pWindow->hWnd)
        {
            RECT client{};
            RECT window{};
            RECT clip{};
            GetClientRect(pWindow->hWnd, &client);
            GetWindowRect(pWindow->hWnd, &window);
            const bool cursorClipped = GetClipCursor(&clip) != FALSE &&
                (clip.left != 0 || clip.top != 0 || clip.right != GetSystemMetrics(SM_CXSCREEN) ||
                    clip.bottom != GetSystemMetrics(SM_CYSCREEN));
            snapshot += fmt::format(
                ",\"window\":{{\"foreground\":{},\"minimized\":{},\"visible\":{},"
                "\"client\":[{},{}],\"rect\":[{},{},{},{}],\"cursorClipped\":{}}}",
                JsonBool(GetForegroundWindow() == pWindow->hWnd), JsonBool(IsIconic(pWindow->hWnd) != FALSE),
                JsonBool(IsWindowVisible(pWindow->hWnd) != FALSE), client.right - client.left,
                client.bottom - client.top, window.left, window.top, window.right, window.bottom,
                JsonBool(cursorClipped));
        }

        markSnapshotPhase(1);
        // Dynamic Havok references (the Helgen carts in particular) are not
        // actors and therefore do not appear in networkEntities. Expose their
        // live reference transforms so paired captures can identify divergent
        // rigid bodies by form ID without guessing from the rendered image.
        snapshot += ",\"nearbyReferences\":[";
        bool firstReference = true;
        uint32_t referenceCount = 0;
        auto* pLocalPlayer = PlayerCharacter::Get();
        if (pLocalPlayer && pLocalPlayer->parentCell && pLocalPlayer->parentCell->refData.refArray)
        {
            const auto& references = pLocalPlayer->parentCell->refData;
            for (uint32_t i = 0; i < references.capacity && referenceCount < 256; ++i)
            {
                auto* pReference = references.refArray[i].Get();
                if (!pReference || pReference == pLocalPlayer || !pReference->baseForm ||
                    !pReference->loadedState || Cast<Actor>(pReference))
                    continue;

                const auto delta = pReference->position - pLocalPlayer->position;
                if (glm::dot(delta, delta) > 30000.f * 30000.f)
                    continue;

                if (!firstReference)
                    snapshot += ',';
                firstReference = false;
                ++referenceCount;
                ObjectService::RemotePhysicsDiagnostic authority{};
                const bool hasAuthority = m_world.ctx().at<ObjectService>().GetRemotePhysicsDiagnostic(
                    pReference->formID, authority);
                snapshot += '{';
                snapshot += fmt::format(
                    "\"formId\":{},\"baseId\":{},\"formType\":{},\"position\":[{},{},{}],"
                    "\"rotation\":[{},{},{}],\"hasHostPhysics\":{},"
                    "\"hostPhysicsPosition\":[{},{},{}],\"hostPhysicsTick\":{},"
                    "\"hostPhysicsAgeMs\":{},\"hostPhysicsBodyDriven\":{}",
                    pReference->formID, pReference->baseForm->formID,
                    static_cast<uint32_t>(pReference->baseForm->formType), pReference->position.x,
                    pReference->position.y, pReference->position.z, pReference->rotation.x,
                    pReference->rotation.y, pReference->rotation.z, JsonBool(hasAuthority),
                    authority.Position.x, authority.Position.y, authority.Position.z,
                    authority.Tick, authority.AgeMs, JsonBool(authority.BodyDriven));
                snapshot += '}';
            }
        }
        snapshot += ']';

        // A first-N cell walk can omit the gate and its trigger entirely.
        // Sort all nearby interactive references by distance for a bounded,
        // generic read-only door/activator view, not an MQ101 form-ID patch.
        snapshot += ",\"nearbyInteractiveReferences\":[";
        if (pLocalPlayer && pLocalPlayer->parentCell &&
            pLocalPlayer->parentCell->refData.refArray)
        {
            struct InteractiveCandidate
            {
                TESObjectREFR* Reference{};
                float DistanceSquared{};
            };
            std::vector<InteractiveCandidate> candidates;
            const auto& references = pLocalPlayer->parentCell->refData;
            const auto scanLimit = std::min<uint32_t>(references.capacity, 50000);
            for (uint32_t i = 0; i < scanLimit; ++i)
            {
                auto* pReference = references.refArray[i].Get();
                if (!pReference || !pReference->baseForm)
                    continue;
                const auto type = pReference->baseForm->formType;
                if (type != FormType::Door && type != FormType::Activator)
                    continue;
                const auto delta = pReference->position - pLocalPlayer->position;
                const float distanceSquared = glm::dot(delta, delta);
                if (distanceSquared <= 8000.f * 8000.f)
                    candidates.push_back({pReference, distanceSquared});
            }
            std::sort(candidates.begin(), candidates.end(),
                [](const auto& left, const auto& right) {
                    return left.DistanceSquared < right.DistanceSquared;
                });
            const bool openStateAvailable =
                m_world.ctx().at<PapyrusService>().Get(
                    "ObjectReference", "GetOpenState") != nullptr;
            const size_t limit = std::min<size_t>(candidates.size(), 96);
            for (size_t i = 0; i < limit; ++i)
            {
                const auto* pReference = candidates[i].Reference;
                const bool isDoor = pReference->baseForm->formType == FormType::Door;
                const bool openStateReadable = isDoor && pReference->loadedState &&
                    openStateAvailable;
                const auto openState = openStateReadable ?
                    const_cast<TESObjectREFR*>(pReference)->GetOpenState() :
                    TESObjectREFR::kNone;
                if (i != 0)
                    snapshot += ',';
                snapshot += fmt::format(
                    "{{\"formId\":{},\"baseId\":{},\"baseType\":{},"
                    "\"distance\":{},\"position\":[{},{},{}],"
                    "\"loaded\":{},\"disabled\":{},\"openStateReadable\":{},"
                    "\"openState\":{}}}",
                    pReference->formID, pReference->baseForm->formID,
                    static_cast<uint32_t>(pReference->baseForm->formType),
                    std::sqrt(candidates[i].DistanceSquared),
                    pReference->position.x, pReference->position.y,
                    pReference->position.z, JsonBool(pReference->loadedState != nullptr),
                    JsonBool(pReference->IsDisabled()), JsonBool(openStateReadable),
                    static_cast<uint32_t>(openState));
            }
        }
        snapshot += ']';

        // Scan active scenes across every quest, not only the optional watched
        // quest list. This makes the parity probe useful outside MQ101.
        snapshot += ",\"activeScenes\":[";
        bool firstActiveScene = true;
        uint32_t activeSceneCount = 0;
        if (auto* pModManager = ModManager::Get())
        {
            for (auto* pQuest : pModManager->quests)
            {
                if (!pQuest || !pQuest->IsEnabled() || activeSceneCount >= 64)
                    continue;
                const auto& scenes = pQuest->scenes;
                if (!scenes.data || scenes.length > 256 || scenes.length > scenes.capacity ||
                    !IsReadableRange(scenes.data, sizeof(BGSScene*) * scenes.length))
                    continue;
                for (uint32_t sceneIndex = 0; sceneIndex < scenes.length && activeSceneCount < 64; ++sceneIndex)
                {
                    const auto* pScene = scenes.data[sceneIndex];
                    if (!pScene || !IsReadableRange(pScene, sizeof(BGSScene)) || !pScene->isPlaying)
                        continue;
                    if (!firstActiveScene)
                        snapshot += ',';
                    firstActiveScene = false;
                    ++activeSceneCount;
                    uint64_t actionSignature = cFnvOffsetBasis;
                    uint32_t readableActions = 0;
                    const auto& actions = pScene->actions;
                    if (actions.data && actions.length <= 128 && actions.length <= actions.capacity &&
                        IsReadableRange(actions.data, sizeof(void*) * actions.length))
                    {
                        for (uint32_t actionIndex = 0; actionIndex < actions.length; ++actionIndex)
                        {
                            SceneActionDiagnosticView action{};
                            if (!ReadNative(actions.data[actionIndex], action))
                                continue;
                            HashWord(actionSignature, actionIndex);
                            HashWord(actionSignature, action.ActorId);
                            HashWord(actionSignature, action.StartPhase);
                            HashWord(actionSignature, action.EndPhase);
                            HashWord(actionSignature, action.Flags);
                            ++readableActions;
                        }
                    }
                    snapshot += fmt::format(
                        "{{\"questId\":{},\"questEditorId\":\"{}\","
                        "\"sceneId\":{},\"rawPhaseWord\":{},"
                        "\"actionCount\":{},\"readableActions\":{},\"actionSignature\":{}}}",
                        pQuest->formID, EscapeJson(pQuest->idName.AsAscii()),
                        pScene->formID, pScene->rawPhaseWord,
                        actions.length, readableActions, actionSignature);
                }
            }
        }
        snapshot += ']';

        markSnapshotPhase(2);
        // Targeted, read-only probe for the two vanilla MQ101 cart references.
        // This does not alter their script, animation, or Havok state.
        snapshot += ",\"introCartReferences\":[";
        for (size_t i = 0; i < std::size(cartIds); ++i)
        {
            if (i != 0)
                snapshot += ',';
            auto* pCart = Cast<TESObjectREFR>(TESForm::GetById(cartIds[i]));
            auto* pNode = pCart ? pCart->GetNiNode() : nullptr;
            BSAnimationGraphManager* pGraphManager = nullptr;
            const bool hasGraph = pCart && pCart->animationGraphHolder.GetBSAnimationGraph(&pGraphManager) && pGraphManager;
            // CommonLibSSE-NG's pinned bhkNiCollisionObject layout places its
            // bhkWorldObject pointer at +0x20; bhkRefObject then owns the
            // hkpRigidBody pointer at +0x10. This remains read-only and is
            // guarded at every indirection on the live runtime.
            uint32_t collisionFlags = 0;
            std::string collisionType;
            std::string collisionParentType;
            bool collisionTypeReadable = false;
            void* pBodyWrapper = nullptr;
            void* pHavokBody = nullptr;
            ActorPoseDiagnosticViews::RigidBody havokBody{};
            bool bodyReadable = false;
            if (pNode && pNode->collisionObject)
            {
                struct NativeNiRttiView
                {
                    const char* Name{};
                    const void* Parent{};
                };
                const auto* pRtti = reinterpret_cast<NiObject*>(
                    pNode->collisionObject)->GetRTTI();
                NativeNiRttiView rtti{};
                if (pRtti && ReadNative(pRtti, rtti))
                {
                    collisionType = ReadNativeString(rtti.Name,
                        collisionTypeReadable, 64);
                    NativeNiRttiView parent{};
                    if (rtti.Parent && ReadNative(rtti.Parent, parent))
                    {
                        bool parentReadable = false;
                        collisionParentType = ReadNativeString(parent.Name,
                            parentReadable, 64);
                        if (!parentReadable)
                            collisionParentType.clear();
                    }
                    if (!collisionTypeReadable)
                        collisionType.clear();
                }
                const auto collisionAddress = reinterpret_cast<uintptr_t>(pNode->collisionObject);
                if (collisionAddress <= std::numeric_limits<uintptr_t>::max() - 0x28 &&
                    ReadNative(reinterpret_cast<const void*>(collisionAddress + 0x18), collisionFlags) &&
                    ReadNative(reinterpret_cast<const void*>(collisionAddress + 0x20), pBodyWrapper) &&
                    pBodyWrapper)
                {
                    const auto wrapperAddress = reinterpret_cast<uintptr_t>(pBodyWrapper);
                    if (wrapperAddress <= std::numeric_limits<uintptr_t>::max() - 0x18 &&
                        ReadNative(reinterpret_cast<const void*>(wrapperAddress + 0x10), pHavokBody) &&
                        pHavokBody)
                        bodyReadable = ReadNative(pHavokBody, havokBody) && havokBody.motionType <= 7 &&
                            std::isfinite(havokBody.linearVelocity[0]) &&
                            std::isfinite(havokBody.linearVelocity[1]) &&
                            std::isfinite(havokBody.linearVelocity[2]) &&
                            std::isfinite(havokBody.transform[12]) &&
                            std::isfinite(havokBody.transform[13]) &&
                            std::isfinite(havokBody.transform[14]);
                }
            }
            snapshot += fmt::format(
                "{{\"formId\":{},\"present\":{},\"loaded\":{},\"node\":{},\"animationGraph\":{},"
                "\"baseId\":{},\"formType\":{},\"cellId\":{},\"position\":[{},{},{}],"
                "\"nodeWorldPosition\":[{},{},{}],\"nodeLocalPosition\":[{},{},{}],"
                "\"collisionObjectPresent\":{},\"collisionFlags\":{},"
                "\"collisionTypeReadable\":{},\"collisionType\":\"{}\","
                "\"collisionParentType\":\"{}\","
                "\"havokBodyReadable\":{},\"havokWorldPresent\":{},\"havokMotionType\":{},"
                "\"havokTransformPosition\":[{},{},{}],"
                "\"havokLinearVelocity\":[{},{},{}],\"motionSamples\":{},"
                "\"peakFrameStep\":{},\"peakFrameSpeed\":{},\"largeFrameSteps\":{}}}",
                cartIds[i], JsonBool(pCart != nullptr), JsonBool(pCart && pCart->loadedState),
                JsonBool(pNode != nullptr), JsonBool(hasGraph),
                pCart && pCart->baseForm ? pCart->baseForm->formID : 0,
                pCart && pCart->baseForm ? static_cast<uint32_t>(pCart->baseForm->formType) : 0,
                pCart ? pCart->GetCellId() : 0,
                pCart ? pCart->position.x : 0.f,
                pCart ? pCart->position.y : 0.f,
                pCart ? pCart->position.z : 0.f,
                pNode ? pNode->world.translate.x : 0.f,
                pNode ? pNode->world.translate.y : 0.f,
                pNode ? pNode->world.translate.z : 0.f,
                pNode ? pNode->local.translate.x : 0.f,
                pNode ? pNode->local.translate.y : 0.f,
                pNode ? pNode->local.translate.z : 0.f,
                JsonBool(pNode && pNode->collisionObject), collisionFlags,
                JsonBool(collisionTypeReadable), EscapeJson(collisionType),
                EscapeJson(collisionParentType),
                JsonBool(bodyReadable), JsonBool(bodyReadable && havokBody.world),
                bodyReadable ? havokBody.motionType : 0,
                bodyReadable ? havokBody.transform[12] : 0.f,
                bodyReadable ? havokBody.transform[13] : 0.f,
                bodyReadable ? havokBody.transform[14] : 0.f,
                bodyReadable ? havokBody.linearVelocity[0] : 0.f,
                bodyReadable ? havokBody.linearVelocity[1] : 0.f,
                bodyReadable ? havokBody.linearVelocity[2] : 0.f,
                m_introCartMotionStats[i].Samples, m_introCartMotionStats[i].PeakStep,
                m_introCartMotionStats[i].PeakSpeed, m_introCartMotionStats[i].LargeSteps);
        }
        snapshot += ']';

        snapshot += ",\"recentHitchMotionEvents\":[";
        for (uint32_t i = 0; i < m_hitchMotionCount; ++i)
        {
            const auto index = (m_hitchMotionNext + m_hitchMotionEvents.size() -
                m_hitchMotionCount + i) % m_hitchMotionEvents.size();
            const auto& event = m_hitchMotionEvents[index];
            if (i)
                snapshot += ',';
            snapshot += fmt::format(
                "{{\"timeMs\":{},\"worldTick\":{},\"formId\":{},\"worldGapUs\":{},"
                "\"vmGapUs\":{},\"priorVmAppUs\":{},"
                "\"priorVmOriginalUs\":{},\"priorGameTestUs\":{},"
                "\"cartDeltaMs\":{},\"cartStep\":{},"
                "\"position\":[{},{},{}]}}",
                event.TimeMs, event.WorldTick, event.FormId, event.WorldGapUs,
                event.VmGapUs, event.PriorVmAppUs,
                event.PriorVmOriginalUs, event.PriorGameTestUs,
                event.CartDeltaMs, event.CartStep,
                event.Position[0], event.Position[1], event.Position[2]);
        }
        snapshot += ']';

        markSnapshotPhase(3);
        snapshot += ",\"networkEntities\":[";
        bool firstEntity = true;
        size_t emittedEntities = 0;
        std::unordered_map<uint32_t, uint32_t> networkActorIds;
        const auto presentationNow = m_world.GetTransport().GetClock().GetCurrentTick();
        const auto presentationDelay = static_cast<uint64_t>(
            m_world.GetCharacterService().GetPresentationDelayMs());
        const auto presentationTick = presentationNow > presentationDelay ?
            presentationNow - presentationDelay : 0;
        const auto entityView = m_world.view<FormIdComponent>();
        for (const auto entity : entityView)
        {
            if (emittedEntities++ >= 256)
                break;
            const auto& form = entityView.get<FormIdComponent>(entity);
            const auto* pLocal = m_world.try_get<LocalComponent>(entity);
            const auto* pRemote = m_world.try_get<RemoteComponent>(entity);
            const auto* pNetworkPlayer = m_world.try_get<PlayerComponent>(entity);
            const auto* pLocalAnimation = m_world.try_get<LocalAnimationComponent>(entity);
            const auto* pRemoteAnimation = m_world.try_get<RemoteAnimationComponent>(entity);
            const auto* pInterpolation = m_world.try_get<InterpolationComponent>(entity);
            const auto* pVisual = pLocalAnimation ?
                &pLocalAnimation->LastSentVisualBones :
                (pRemoteAnimation ? &pRemoteAnimation->VisualBones : nullptr);
            auto* pForm = TESForm::GetById(form.Id);
            auto* pActor = Cast<Actor>(pForm);
            if (pActor)
                networkActorIds.emplace(form.Id, pLocal ? pLocal->Id :
                    (pRemote ? pRemote->Id : 0));
            auto* pRootNode = pActor ? pActor->GetNiNode() : nullptr;
            // Limit inventory scans: this snapshot is taken on the game thread.
            const auto wornArmorCount = pActor && emittedEntities <= 48 ?
                pActor->GetWornArmor().Entries.size() : 0;
            // The extra scan is only needed for the suspicious zero-armor
            // cases; normal snapshots retain their existing cost.
            const auto inventoryEntryCount = pActor && emittedEntities <= 48 &&
                wornArmorCount == 0 ? pActor->GetActorInventory().Entries.size() : 0;
            const auto* pNpcBase = pActor ? Cast<TESNPC>(pActor->baseForm) : nullptr;
            const uint32_t defaultOutfitId = pNpcBase && pNpcBase->defaultOutfit ?
                pNpcBase->defaultOutfit->formID : 0;
            const auto nativeMountState = pActor && emittedEntities <= 48 ?
                pActor->GetNativeMountState() : Actor::NativeMountState{};
            if (pActor && ((pRemoteAnimation &&
                    pRemoteAnimation->EvaluatedPose.Bones.size() == 99) ||
                (pLocalAnimation &&
                    pLocalAnimation->LastSentPose.Bones.size() == 99)))
                AnimationGraphUpdateTrace::WatchHolder(
                    &pActor->animationGraphHolder, form.Id);
            const auto graphTrace = AnimationGraphUpdateTrace::GetHolderSample(
                pActor ? &pActor->animationGraphHolder : nullptr);
            const auto nativeAnimation = SampleNativeActorAnimation(pActor);
            const auto* pExtension = pActor ? pActor->GetExtension() : nullptr;
            const auto* pPackage = pActor && pActor->currentProcess ?
                pActor->currentProcess->package : nullptr;
            auto* pCombatTarget = pActor ? pActor->GetCombatTarget() : nullptr;
            auto* pActorCell = pActor ? pActor->GetParentCellEx() : nullptr;
            const auto* pProcess = pActor ? pActor->currentProcess : nullptr;
            const auto followHandle = ReadProcessHandle(pProcess, 0x110);
            const auto aiTargetHandle = ReadProcessHandle(pProcess, 0x114);
            // The published AE getter crashed this installed runtime during
            // loading; do not call it until its ABI is verified locally.
            const uint32_t headtrackHandle = 0;
            uint8_t processLevel{};
            if (pProcess)
                ReadNative(reinterpret_cast<const uint8_t*>(pProcess) + 0x137,
                    processLevel);
            if (!firstEntity)
                snapshot += ',';
            firstEntity = false;
            snapshot += fmt::format(
                "{{\"formId\":{},\"playerId\":{},\"authority\":\"{}\",\"networkId\":{},"
                "\"ownershipEpoch\":{},\"waitingFor3D\":{},\"waitingForAssignment\":{},"
                "\"has3D\":{},\"hasAIProcess\":{},\"rootChildCount\":{},\"wornArmorCount\":{},\"inventoryEntryCount\":{},\"inventoryEntriesSampled\":{},\"defaultOutfitId\":{},\"nativeMountFormId\":{},"
                "\"horseExtra\":{},\"horseHandle\":{},"
                "\"interactionExtra\":{},\"interactionPointerPresent\":{},"
                "\"interactionActorHandle\":{},\"interactionTargetHandle\":{},"
                "\"animationQueued\":{},\"animationReplayQueued\":{},"
                "\"poseSourceTick\":{},\"poseBoneCount\":{},\"poseChecksum\":{},"
                "\"visualSourceTick\":{},\"visualBoneCount\":{},\"visualChecksum\":{},"
                "\"visualRootPresent\":{},\"visualRootWorldT\":[{},{},{}],"
                "\"graphPostCallMs\":{},\"graphPostAgeMs\":{},"
                "\"graphPostThreadId\":{},\"cellId\":{},\"position\":[{},{},{}],"
                "\"authorityCellId\":{},\"authorityWorldSpaceId\":{},"
                "\"authorityMovementTick\":{},\"authorityStableSinceTick\":{},"
                "\"corpseCorrectionAttempts\":{},\"corpseCorrectionAttemptsForTarget\":{},"
                "\"lastCorpseCorrectionTick\":{},"
                "\"authorityPosition\":[{},{},{}],"
                "\"packageFormId\":{},\"combatTargetFormId\":{},"
                "\"processLevel\":{},\"aiFollowHandle\":{},\"aiFollowFormId\":{},"
                "\"aiTargetHandle\":{},\"aiTargetFormId\":{},"
                "\"headtrackReadable\":false,\"headtrackHandle\":{},\"headtrackFormId\":{},"
                "\"dialogueHandle\":{},\"dialogueTargetFormId\":{},"
                "\"lastActionId\":{},\"lastActionTargetId\":{},\"lastActionEvent\":\"{}\","
                "\"lastActionTick\":{},\"lastActionType\":{},"
                "\"lastActionState1\":{},\"lastActionState2\":{},"
                "\"ownerProcessedTick\":{},\"ownerProcessedActionId\":{},"
                "\"ownerProcessedEvent\":\"{}\","
                "\"remoteRanTick\":{},\"remoteRanActionId\":{},"
                "\"remoteRanEvent\":\"{}\","
                "\"remoteProcessedTick\":{},\"remoteProcessedActionId\":{},"
                "\"nativeGraphReady\":{},\"nativeStateId\":{},\"nativeTimeInState\":{},"
                "\"nativeCloneStateReadable\":{},\"nativeCloneStateId\":{},"
                "\"nativeCloneTimeInState\":{},"
                "\"nativePoseCount\":{},\"nativePoseChecksum\":{},"
                "\"renderBoneCount\":{},\"renderBoneChecksum\":{},\"renderWorldBoneChecksum\":{},"
                "\"visualGeometrySampled\":{},\"wornArmorSampled\":{},"
                "\"rootChildArrayLength\":{},"
                "\"rootChildArrayCapacity\":{},\"rootChildren\":[",
                form.Id, pNetworkPlayer ? pNetworkPlayer->Id : 0,
                pLocal ? "local" : (pRemote ? "remote" : "unassigned"),
                pLocal ? pLocal->Id : (pRemote ? pRemote->Id : 0),
                pLocal ? pLocal->OwnershipEpoch : (pRemote ? pRemote->OwnershipEpoch : 0),
                JsonBool(m_world.all_of<WaitingFor3D>(entity)),
                JsonBool(m_world.all_of<WaitingForAssignmentComponent>(entity)),
                JsonBool(pRootNode != nullptr),
                JsonBool(pActor && pActor->currentProcess != nullptr),
                pRootNode ? pRootNode->children.length : 0,
                wornArmorCount,
                inventoryEntryCount,
                JsonBool(pActor && emittedEntities <= 48 && wornArmorCount == 0),
                defaultOutfitId,
                pActor ? pActor->GetNativeMountFormId() : 0,
                JsonBool(nativeMountState.HorseExtra), nativeMountState.HorseHandle,
                JsonBool(nativeMountState.InteractionExtra),
                JsonBool(nativeMountState.InteractionPointerPresent),
                nativeMountState.InteractionActorHandle,
                nativeMountState.InteractionTargetHandle,
                pLocalAnimation ? pLocalAnimation->Actions.size() : 0,
                pRemoteAnimation ? pRemoteAnimation->TimePoints.size() : 0,
                pLocalAnimation ? pLocalAnimation->LastSentPose.SourceTick :
                    (pRemoteAnimation ? pRemoteAnimation->EvaluatedPose.SourceTick : 0),
                pLocalAnimation ? pLocalAnimation->LastSentPose.Bones.size() :
                    (pRemoteAnimation ? pRemoteAnimation->EvaluatedPose.Bones.size() : 0),
                pLocalAnimation ? pLocalAnimation->LastSentPose.Checksum() :
                    (pRemoteAnimation ? pRemoteAnimation->EvaluatedPose.Checksum() : 0),
                pVisual ? pVisual->SourceTick : 0,
                pVisual ? pVisual->Bones.size() : 0,
                pVisual ? pVisual->Checksum() : 0,
                JsonBool(pVisual && pVisual->RootWorld.Present),
                pVisual ? pVisual->RootWorld.Translation[0] : 0.f,
                pVisual ? pVisual->RootWorld.Translation[1] : 0.f,
                pVisual ? pVisual->RootWorld.Translation[2] : 0.f,
                graphTrace.LastPostCallMs,
                graphTrace.LastPostCallMs && now >= graphTrace.LastPostCallMs ?
                    now - graphTrace.LastPostCallMs : 0,
                graphTrace.ThreadId,
                pActorCell ? pActorCell->formID : 0,
                pActor ? pActor->position.x : 0.f,
                pActor ? pActor->position.y : 0.f,
                pActor ? pActor->position.z : 0.f,
                pInterpolation && pInterpolation->AuthorityCellId ?
                    m_world.GetModSystem().GetGameId(pInterpolation->AuthorityCellId) : 0,
                pInterpolation && pInterpolation->AuthorityWorldSpaceId ?
                    m_world.GetModSystem().GetGameId(pInterpolation->AuthorityWorldSpaceId) : 0,
                pInterpolation ? pInterpolation->AuthorityTick : 0,
                pInterpolation ? pInterpolation->AuthorityStableSinceTick : 0,
                pInterpolation ? pInterpolation->CorpseCorrectionAttempts : 0,
                pInterpolation ? pInterpolation->CorpseCorrectionAttemptsForTarget : 0,
                pInterpolation ? pInterpolation->LastCorpseCorrectionTick : 0,
                pInterpolation ? pInterpolation->AuthorityPosition.x : 0.f,
                pInterpolation ? pInterpolation->AuthorityPosition.y : 0.f,
                pInterpolation ? pInterpolation->AuthorityPosition.z : 0.f,
                pPackage ? pPackage->formID : 0,
                pCombatTarget ? pCombatTarget->formID : 0,
                processLevel, followHandle, ResolveHandleFormId(followHandle),
                aiTargetHandle, ResolveHandleFormId(aiTargetHandle),
                headtrackHandle, ResolveHandleFormId(headtrackHandle),
                pActor ? pActor->dialogueHandle : 0,
                pActor ? ResolveHandleFormId(pActor->dialogueHandle) : 0,
                pExtension ? pExtension->LatestAnimation.ActionId : 0,
                pExtension ? pExtension->LatestAnimation.TargetId : 0,
                pExtension ? EscapeJson(pExtension->LatestAnimation.EventName.c_str()) : "",
                pExtension ? pExtension->LatestAnimation.Tick : 0,
                pExtension ? pExtension->LatestAnimation.Type : 0,
                pExtension ? pExtension->LatestAnimation.State1 : 0,
                pExtension ? pExtension->LatestAnimation.State2 : 0,
                pLocalAnimation ? pLocalAnimation->LastProcessedAction.Tick : 0,
                pLocalAnimation ? pLocalAnimation->LastProcessedAction.ActionId : 0,
                pLocalAnimation ? EscapeJson(
                    pLocalAnimation->LastProcessedAction.EventName.c_str()) : "",
                pRemoteAnimation ? pRemoteAnimation->LastRanAction.Tick : 0,
                pRemoteAnimation ? pRemoteAnimation->LastRanAction.ActionId : 0,
                pRemoteAnimation ? EscapeJson(
                    pRemoteAnimation->LastRanAction.EventName.c_str()) : "",
                pRemoteAnimation ? pRemoteAnimation->LastProcessedAction.Tick : 0,
                pRemoteAnimation ? pRemoteAnimation->LastProcessedAction.ActionId : 0,
                JsonBool(nativeAnimation.GraphReady), nativeAnimation.StateId,
                nativeAnimation.TimeInState,
                JsonBool(nativeAnimation.CloneStateReadable),
                nativeAnimation.CloneStateId,
                std::isfinite(nativeAnimation.CloneTimeInState) ?
                    nativeAnimation.CloneTimeInState : 0.f,
                nativeAnimation.PoseCount,
                nativeAnimation.PoseChecksum, nativeAnimation.RenderBoneCount,
                nativeAnimation.RenderBoneChecksum,
                nativeAnimation.RenderWorldBoneChecksum,
                JsonBool(emittedEntities <= 48), JsonBool(emittedEntities <= 48),
                pRootNode ? pRootNode->children.length : 0,
                pRootNode ? pRootNode->children.capacity : 0);

            // A root node alone does not prove the actor's body geometry has
            // attached. Keep this one-shot probe bounded and read-only so a
            // missing body can be distinguished from a pose or outfit issue.
            constexpr uint16_t cMaxRootChildren = 16;
            const auto* pChildren = pRootNode ? &pRootNode->children : nullptr;
            const bool childrenReadable = pChildren &&
                pChildren->length <= 256 &&
                (pChildren->length == 0 ||
                    (pChildren->data && IsReadableRange(pChildren->data,
                        sizeof(NiAVObject*) * pChildren->length)));
            if (childrenReadable && emittedEntities <= 48)
            {
                for (uint16_t index = 0;
                    index < std::min(pChildren->length, cMaxRootChildren); ++index)
                {
                    if (index != 0)
                        snapshot += ',';
                    NiAVObject* pChild{};
                    const bool childReadable = ReadNative(pChildren->data + index, pChild) &&
                        pChild && IsReadableRange(pChild, sizeof(NiAVObject));
                    const char* pName{};
                    uint32_t flags{};
                    float localScale{};
                    bool nameReadable = false;
                    if (childReadable)
                    {
                        ReadNative(reinterpret_cast<const uint8_t*>(pChild) + 0x10, pName);
                        ReadNative(reinterpret_cast<const uint8_t*>(pChild) +
                            offsetof(NiAVObject, flags), flags);
                        ReadNative(reinterpret_cast<const uint8_t*>(pChild) +
                            offsetof(NiAVObject, local) + offsetof(NiTransform, scale), localScale);
                    }
                    const auto name = ReadNativeString(pName, nameReadable, 80);
                    NiNode* pNestedNode = childReadable ? pChild->AsNode() : nullptr;
                    if (pNestedNode && !IsReadableRange(pNestedNode, sizeof(NiNode)))
                        pNestedNode = nullptr;
                    const auto* pNestedChildren = pNestedNode ? &pNestedNode->children : nullptr;
                    const bool nestedReadable = pNestedChildren &&
                        pNestedChildren->length <= 128 &&
                        (pNestedChildren->length == 0 ||
                            (pNestedChildren->data && IsReadableRange(
                                pNestedChildren->data,
                                sizeof(NiAVObject*) * pNestedChildren->length)));
                    uint16_t nestedPresentCount = 0;
                    std::string nestedNames;
                    if (nestedReadable)
                    {
                        constexpr uint16_t cMaxNestedNames = 16;
                        uint16_t emittedNestedNames = 0;
                        for (uint16_t nestedIndex = 0;
                            nestedIndex < pNestedChildren->length; ++nestedIndex)
                        {
                            NiAVObject* pNestedChild{};
                            if (!ReadNative(pNestedChildren->data + nestedIndex, pNestedChild) ||
                                !pNestedChild || !IsReadableRange(pNestedChild, sizeof(NiAVObject)))
                                continue;
                            ++nestedPresentCount;
                            if (emittedNestedNames >= cMaxNestedNames)
                                continue;
                            const char* pNestedName{};
                            bool nestedNameReadable = false;
                            ReadNative(reinterpret_cast<const uint8_t*>(pNestedChild) + 0x10,
                                pNestedName);
                            const auto nestedName = ReadNativeString(pNestedName,
                                nestedNameReadable, 80);
                            if (emittedNestedNames++ != 0)
                                nestedNames += ',';
                            nestedNames += fmt::format("\"{}\"", EscapeJson(nestedName));
                        }
                    }
                    snapshot += fmt::format(
                        "{{\"index\":{},\"present\":{},\"name\":\"{}\","
                        "\"nameReadable\":{},\"hidden\":{},\"scale\":{},"
                        "\"nestedNode\":{},\"nestedReadable\":{},"
                        "\"nestedChildCount\":{},\"nestedPresentCount\":{},"
                        "\"nestedNames\":[{}]}}",
                        index, JsonBool(childReadable), EscapeJson(name),
                        JsonBool(nameReadable), JsonBool(childReadable && (flags & 1u)),
                        childReadable && std::isfinite(localScale) ? localScale : 0.f,
                        JsonBool(pNestedNode != nullptr), JsonBool(nestedReadable),
                        pNestedChildren ? pNestedChildren->length : 0,
                        nestedPresentCount, nestedNames);
                }
            }
            snapshot += ']';
            snapshot += fmt::format(
                ",\"graphVariables\":{{\"count\":{},\"checksum\":{},"
                "\"sampledCount\":{},\"values\":[",
                nativeAnimation.GraphVariableCount,
                nativeAnimation.GraphVariableChecksum,
                nativeAnimation.GraphVariables.size());
            for (size_t i = 0; i < nativeAnimation.GraphVariables.size(); ++i)
            {
                if (i != 0)
                    snapshot += ',';
                snapshot += fmt::format("{}", nativeAnimation.GraphVariables[i]);
            }
            snapshot += "]}";
            snapshot += fmt::format(
                ",\"actionPipeline\":{{\"dispatchStage\":{},"
                "\"ownerQueued\":{},\"ownerLastSentTick\":{},"
                "\"ownerLastSentEvent\":\"{}\",\"ownerLastSentTargetId\":{},"
                "\"followerQueued\":{},\"followerLastReceivedTick\":{},"
                "\"followerLastReceivedEvent\":\"{}\","
                "\"followerLastReceivedTargetId\":{},"
                "\"followerLastReplayResult\":{}}}",
                pExtension ? pExtension->LatestAnimationDispatch : 0,
                pLocalAnimation ? pLocalAnimation->Actions.size() : 0,
                pLocalAnimation ? pLocalAnimation->LastSentAction.Tick : 0,
                pLocalAnimation ? EscapeJson(
                    pLocalAnimation->LastSentAction.EventName.c_str()) : "",
                pLocalAnimation ? pLocalAnimation->LastSentAction.TargetId : 0,
                pRemoteAnimation ? pRemoteAnimation->TimePoints.size() : 0,
                pRemoteAnimation ? pRemoteAnimation->LastReceivedAction.Tick : 0,
                pRemoteAnimation ? EscapeJson(
                    pRemoteAnimation->LastReceivedAction.EventName.c_str()) : "",
                pRemoteAnimation ? pRemoteAnimation->LastReceivedAction.TargetId : 0,
                JsonBool(pRemoteAnimation && pRemoteAnimation->LastRanActionResult));
            snapshot += fmt::format(
                ",\"combatTargetPipeline\":{{\"desiredServerId\":{},"
                "\"sourceTick\":{},\"lastReceivedTick\":{},"
                "\"queued\":{},\"lastApplyTick\":{},"
                "\"lastDesiredFormId\":{},\"lastBeforeFormId\":{},"
                "\"lastAfterFormId\":{},\"nativeHandle\":{},"
                "\"nativeFormId\":{}}}",
                pRemoteAnimation ? pRemoteAnimation->DesiredCombatTargetServerId : 0xFFFFFFFFu,
                pRemoteAnimation ? pRemoteAnimation->DesiredCombatTargetTick : 0,
                pRemoteAnimation ? pRemoteAnimation->LastReceivedCombatTargetTick : 0,
                pRemoteAnimation ? pRemoteAnimation->CombatTargetTimePoints.size() : 0,
                pRemoteAnimation ? pRemoteAnimation->LastCombatTargetApplyTick : 0,
                pRemoteAnimation ? pRemoteAnimation->LastCombatTargetApplyDesiredFormId : 0,
                pRemoteAnimation ? pRemoteAnimation->LastCombatTargetApplyBeforeFormId : 0,
                pRemoteAnimation ? pRemoteAnimation->LastCombatTargetApplyAfterFormId : 0,
                pActor && pActor->pCombatController ?
                    pActor->pCombatController->targetHandle : 0,
                pActor && pActor->pCombatController ? ResolveHandleFormId(
                    pActor->pCombatController->targetHandle) : 0);
            const InterpolationComponent::TimePoint* pMovementPoint{};
            if (pInterpolation && pInterpolation->TimePoints.size() >= 2)
            {
                const auto first = pInterpolation->TimePoints.begin();
                const auto second = std::next(first);
                pMovementPoint = presentationTick >= second->Tick ?
                    &*second : &*first;
            }
            const auto* pDescriptor = pExtension ?
                AnimationGraphDescriptorManager::Get().GetDescriptor(
                    pExtension->GraphDescriptorHash) : nullptr;
            snapshot += fmt::format(
                ",\"movementGraphInputs\":{{\"sourceTick\":{},\"presentationTick\":{},"
                "\"descriptorFound\":{},\"values\":[",
                pMovementPoint ? pMovementPoint->Tick : 0, presentationTick,
                JsonBool(pDescriptor != nullptr));
            if (pMovementPoint && pDescriptor)
            {
                constexpr uint32_t cDecisionIndices[] = {
                    0, 1, 2, 3, 6, 8, 13, 40, 47, 48, 53, 127, 155,
                    178, 184, 221, 229};
                bool firstInput = true;
                for (const auto index : cDecisionIndices)
                {
                    uint32_t rawValue{};
                    if (!TryReadPackedGraphInput(*pDescriptor,
                            pMovementPoint->Variables, index, rawValue))
                        continue;
                    if (!firstInput)
                        snapshot += ',';
                    firstInput = false;
                    snapshot += fmt::format("{{\"index\":{},\"raw\":{}}}",
                        index, rawValue);
                }
            }
            snapshot += "]}";
            const auto mailbox = pActor ? VisualPoseMailbox::GetHolderDiagnostics(
                &pActor->animationGraphHolder, pActor) :
                VisualPoseMailbox::HolderDiagnostics{};
            snapshot += fmt::format(
                ",\"mailbox\":{{\"slotIndex\":{},\"slotOwnerEvictions\":{},"
                "\"slotInspectMisses\":{},\"slotOwnerMatches\":{},"
                "\"frameMatchesHolder\":{},\"frameMatchesActor\":{},"
                "\"statsHolderMatches\":{},\"ownerPublishCount\":{},"
                "\"ownerInspectCount\":{},\"ownerApplyCount\":{},"
                "\"lastPublishAgeMs\":{},\"formId\":{},\"ownershipEpoch\":{},"
                "\"receiptAgeMs\":{},\"latestSourceTick\":{},\"historyCount\":{},"
                "\"presentationTick\":{},\"presentationBracketed\":{},"
                "\"lastInspectAgeMs\":{},\"lastEligibleBones\":{},"
                "\"lastWrittenBones\":{},\"lastSkipReason\":{},"
                "\"lastAppliedSourceTick\":{}}}}}",
                mailbox.SlotIndex, mailbox.SlotOwnerEvictions,
                mailbox.SlotInspectMisses, JsonBool(mailbox.SlotOwnerMatches),
                JsonBool(mailbox.FrameMatchesHolder), JsonBool(mailbox.FrameMatchesActor),
                JsonBool(mailbox.StatsHolderMatches), mailbox.OwnerPublishCount,
                mailbox.OwnerInspectCount, mailbox.OwnerApplyCount,
                mailbox.LastPublishAgeMs, mailbox.FormId, mailbox.OwnershipEpoch,
                mailbox.ReceiptAgeMs, mailbox.LatestSourceTick, mailbox.HistoryCount,
                mailbox.PresentationTick, JsonBool(mailbox.PresentationBracketed),
                mailbox.LastInspectAgeMs, mailbox.LastEligibleBones,
                mailbox.LastWrittenBones, static_cast<uint32_t>(mailbox.LastSkipReason),
                mailbox.LastAppliedSourceTick);
        }
        snapshot += ']';

        markSnapshotPhase(4);
        // A network-entity walk misses actors that were never registered with
        // the session. Inventory the player's loaded cell independently so
        // those NPCs are visible as explicit coverage gaps in paired audits.
        snapshot += ",\"nearbyActorAudit\":[";
        auto* pAuditPlayer = PlayerCharacter::Get();
        if (pAuditPlayer)
        {
            auto* pAuditCell = pAuditPlayer->GetParentCellEx();
            if (pAuditCell && pAuditCell->refData.refArray)
            {
                const auto& references = pAuditCell->refData;
                const auto scanLimit = std::min<uint32_t>(references.capacity, 50000);
                uint32_t actorCount = 0;
                for (uint32_t i = 0; i < scanLimit && actorCount < 256; ++i)
                {
                    auto* pReference = references.refArray[i].Get();
                    auto* pActor = pReference ? Cast<Actor>(pReference) : nullptr;
                    if (!pActor || !pActor->loadedState)
                        continue;
                    const auto delta = pActor->position - pAuditPlayer->position;
                    if (glm::dot(delta, delta) > 8000.f * 8000.f)
                        continue;
                    if (actorCount++ != 0)
                        snapshot += ',';
                    const auto nativeAnimation = SampleNativeActorAnimation(pActor);
                    const auto* pExtension = pActor->GetExtension();
                    const auto* pPackage = pActor->currentProcess ?
                        pActor->currentProcess->package : nullptr;
                    auto* pCombatTarget = pActor->GetCombatTarget();
                    const auto* pProcess = pActor->currentProcess;
                    const auto followHandle = ReadProcessHandle(pProcess, 0x110);
                    const auto aiTargetHandle = ReadProcessHandle(pProcess, 0x114);
                    const uint32_t headtrackHandle = 0;
                    const auto networkIt = networkActorIds.find(pActor->formID);
                    snapshot += fmt::format(
                        "{{\"formId\":{},\"baseId\":{},\"networked\":{},\"networkId\":{},"
                        "\"position\":[{},{},{}],\"dead\":{},\"bleedingOut\":{},"
                        "\"has3D\":{},\"packageFormId\":{},\"combatTargetFormId\":{},"
                        "\"aiFollowFormId\":{},\"aiTargetFormId\":{},"
                        "\"headtrackReadable\":false,\"headtrackFormId\":{},"
                        "\"dialogueTargetFormId\":{},"
                        "\"lastActionId\":{},\"lastActionTargetId\":{},\"lastActionEvent\":\"{}\","
                        "\"graphDescriptor\":{},\"nativeGraphReady\":{},\"nativeStateId\":{},"
                        "\"nativeTimeInState\":{},\"nativeCloneStateReadable\":{},"
                        "\"nativeCloneStateId\":{},\"nativeCloneTimeInState\":{},"
                        "\"nativePoseCount\":{},\"nativePoseChecksum\":{},"
                        "\"renderBoneCount\":{},\"renderBoneChecksum\":{},"
                        "\"renderWorldBoneChecksum\":{},"
                        "\"graphVariableCount\":{},\"graphVariableChecksum\":{}}}",
                        pActor->formID, pActor->baseForm ? pActor->baseForm->formID : 0,
                        JsonBool(networkIt != networkActorIds.end()),
                        networkIt != networkActorIds.end() ? networkIt->second : 0,
                        pActor->position.x, pActor->position.y, pActor->position.z,
                        JsonBool(pActor->IsDead()),
                        JsonBool(pActor->actorState.IsBleedingOut()),
                        JsonBool(pActor->GetNiNode() != nullptr),
                        pPackage ? pPackage->formID : 0,
                        pCombatTarget ? pCombatTarget->formID : 0,
                        ResolveHandleFormId(followHandle),
                        ResolveHandleFormId(aiTargetHandle),
                        ResolveHandleFormId(headtrackHandle),
                        ResolveHandleFormId(pActor->dialogueHandle),
                        pExtension ? pExtension->LatestAnimation.ActionId : 0,
                        pExtension ? pExtension->LatestAnimation.TargetId : 0,
                        pExtension ? EscapeJson(pExtension->LatestAnimation.EventName.c_str()) : "",
                        pExtension ? pExtension->GraphDescriptorHash : 0,
                        JsonBool(nativeAnimation.GraphReady), nativeAnimation.StateId,
                        nativeAnimation.TimeInState,
                        JsonBool(nativeAnimation.CloneStateReadable),
                        nativeAnimation.CloneStateId,
                        std::isfinite(nativeAnimation.CloneTimeInState) ?
                            nativeAnimation.CloneTimeInState : 0.f,
                        nativeAnimation.PoseCount,
                        nativeAnimation.PoseChecksum, nativeAnimation.RenderBoneCount,
                        nativeAnimation.RenderBoneChecksum,
                        nativeAnimation.RenderWorldBoneChecksum,
                        nativeAnimation.GraphVariableCount,
                        nativeAnimation.GraphVariableChecksum);
                }
            }
        }
        snapshot += ']';

        markSnapshotPhase(5);
        // Probe one specified actor even when it is outside the player's
        // loaded cell. This is especially useful for scene aliases (for
        // example a dragon approaching from outside the normal audit radius).
        const auto targetActorId = m_poseProbeFormId.load(std::memory_order_acquire);
        auto* pTargetActor = targetActorId ?
            Cast<Actor>(TESForm::GetById(targetActorId)) : nullptr;
        if (pTargetActor)
        {
            const auto* pTargetCell = pTargetActor->GetParentCellEx();
            const auto* pExtension = pTargetActor->GetExtension();
            const auto* pPackage = pTargetActor->currentProcess ?
                pTargetActor->currentProcess->package : nullptr;
            snapshot += fmt::format(
                ",\"targetActor\":{{\"formId\":{},\"cellId\":{},"
                "\"cellAttached\":{},\"loaded\":{},\"has3D\":{},"
                "\"remote\":{},\"dead\":{},\"packageFormId\":{},"
                "\"position\":[{},{},{}],\"lastActionEvent\":\"{}\"}}",
                pTargetActor->formID, pTargetCell ? pTargetCell->formID : 0,
                JsonBool(pTargetCell && pTargetCell->IsAttached()),
                JsonBool(pTargetActor->loadedState != nullptr),
                JsonBool(pTargetActor->GetNiNode() != nullptr),
                JsonBool(pExtension && pExtension->IsRemote()),
                JsonBool(pTargetActor->IsDead()),
                pPackage ? pPackage->formID : 0,
                pTargetActor->position.x, pTargetActor->position.y,
                pTargetActor->position.z,
                pExtension ? EscapeJson(pExtension->LatestAnimation.EventName.c_str()) : "");
        }
        Set<std::string> watchedQuests;
        {
            std::scoped_lock lock(m_snapshotMutex);
            watchedQuests = m_watchedQuests;
        }
        snapshot += ",\"quests\":[";
        bool firstQuest = true;
        const PapyrusFunction<bool, BGSScene, uint32_t> isSceneActionComplete(
            m_world.ctx().at<PapyrusService>().Get("Scene", "IsActionComplete"));
        const bool actionCompletionReadable = isSceneActionComplete && GameVM::Get() &&
            GameVM::Get()->virtualMachine;
        uint32_t completionCalls = 0;
        if (auto* pModManager = ModManager::Get())
        {
            for (auto* pQuest : pModManager->quests)
            {
                if (!pQuest || !watchedQuests.contains(pQuest->idName.AsAscii()))
                    continue;
                if (!firstQuest)
                    snapshot += ',';
                firstQuest = false;
                snapshot += fmt::format(
                    "{{\"editorId\":\"{}\",\"formId\":{},\"currentStage\":{},\"flags\":{},"
                    "\"state\":{},\"enabled\":{},\"active\":{},\"stopped\":{},\"doneStages\":[",
                    EscapeJson(pQuest->idName.AsAscii()), pQuest->formID, pQuest->currentStage, pQuest->flags,
                    static_cast<uint8_t>(pQuest->getState()), JsonBool(pQuest->IsEnabled()),
                    JsonBool(pQuest->IsActive()), JsonBool(pQuest->IsStopped()));
                bool firstStage = true;
                for (auto* pStage : pQuest->stages)
                {
                    if (!pStage || !pStage->IsDone())
                        continue;
                    if (!firstStage)
                        snapshot += ',';
                    snapshot += fmt::format("{}", pStage->stageIndex);
                    firstStage = false;
                }
                snapshot += "],\"objectives\":[";
                bool firstObjective = true;
                size_t objectiveCount = 0;
                for (auto* pObjective : pQuest->objectives)
                {
                    if (!pObjective || objectiveCount++ >= 128)
                        break;
                    if (!firstObjective)
                        snapshot += ',';
                    firstObjective = false;
                    snapshot += fmt::format("{{\"stageId\":{},\"state\":{}}}",
                        pObjective->stageId, pObjective->state);
                }
                snapshot += "],\"scenes\":[";
                bool firstScene = true;
                const auto& sceneArray = pQuest->scenes;
                if (sceneArray.data && sceneArray.length <= 128 &&
                    sceneArray.length <= sceneArray.capacity &&
                    IsReadableRange(sceneArray.data, sizeof(BGSScene*) * sceneArray.length))
                {
                    static Map<uint32_t, uint64_t> sceneStates;
                    for (uint32_t sceneIndex = 0; sceneIndex < sceneArray.length; ++sceneIndex)
                    {
                        const auto* pScene = sceneArray.data[sceneIndex];
                        if (!pScene || !IsReadableRange(pScene, 0xC0))
                            continue;
                        const auto sceneState = (static_cast<uint64_t>(pScene->isPlaying) << 32) |
                            pScene->rawPhaseWord;
                        auto [it, inserted] = sceneStates.try_emplace(pScene->formID, sceneState);
                        if (!inserted && it->second != sceneState)
                        {
                            spdlog::info("Native scene transition quest={:X} scene={:X} playing={} rawPhase={} tick={} leader={}",
                                pQuest->formID, pScene->formID, pScene->isPlaying, pScene->rawPhaseWord,
                                now, party.IsLeader());
                            sceneStates.insert_or_assign(pScene->formID, sceneState);
                        }
                        if (!firstScene)
                            snapshot += ',';
                        firstScene = false;
                        snapshot += fmt::format(
                            "{{\"formId\":{},\"playing\":{},\"rawPhaseWord\":{},\"phaseCount\":{},\"phaseEligibleActions\":[",
                            pScene->formID, JsonBool(pScene->isPlaying), pScene->rawPhaseWord,
                            pScene->phases.length);
                        bool firstAction = true;
                        const auto& actions = pScene->actions;
                        if (pScene->isPlaying && pScene->rawPhaseWord < pScene->phases.length &&
                            actions.data && actions.length <= 128 && actions.length <= actions.capacity &&
                            IsReadableRange(actions.data, sizeof(void*) * actions.length))
                        {
                            for (uint32_t actionIndex = 0; actionIndex < actions.length; ++actionIndex)
                            {
                                SceneActionDiagnosticView action{};
                                if (!ReadNative(actions.data[actionIndex], action) ||
                                    action.StartPhase > pScene->rawPhaseWord ||
                                    action.EndPhase < pScene->rawPhaseWord)
                                    continue;
                                if (!firstAction)
                                    snapshot += ',';
                                firstAction = false;
                                // Only watched quests reach this bounded
                                // action loop; resolve their active aliases
                                // so a phase mismatch can identify the actor
                                // whose local scene action is holding it.
                                auto* pAliasRef = pQuest->GetAliasedRef(action.ActorId);
                                auto* pAliasActor = Cast<Actor>(pAliasRef);
                                const auto* pAliasExtension = pAliasActor ?
                                    pAliasActor->GetExtension() : nullptr;
                                const auto* pAliasPackage = pAliasActor &&
                                    pAliasActor->currentProcess ?
                                    pAliasActor->currentProcess->package : nullptr;
                                const bool queryCompletion = actionCompletionReadable &&
                                    completionCalls < 32;
                                const bool actionComplete = queryCompletion ?
                                    isSceneActionComplete(pScene, action.ActionId) : false;
                                completionCalls += queryCompletion ? 1u : 0u;
                                snapshot += fmt::format(
                                    "{{\"index\":{},\"actionId\":{},\"actorId\":{},"
                                    "\"startPhase\":{},\"endPhase\":{},\"flags\":{},"
                                    "\"actionComplete\":{},\"actionStatusByte\":{},"
                                    "\"aliasFormId\":{},\"aliasRemote\":{},\"aliasDead\":{},\"aliasDisabled\":{},"
                                    "\"aliasPackageFormId\":{}}}",
                                    actionIndex, action.ActionId, action.ActorId,
                                    action.StartPhase, action.EndPhase, action.Flags,
                                    queryCompletion ? JsonBool(actionComplete) : "null",
                                    action.Unknown14[0],
                                    pAliasRef ? pAliasRef->formID : 0,
                                    JsonBool(pAliasExtension && pAliasExtension->IsRemote()),
                                    JsonBool(pAliasActor && pAliasActor->IsDead()),
                                    JsonBool(pAliasRef && pAliasRef->IsDisabled()),
                                    pAliasPackage ? pAliasPackage->formID : 0);
                            }
                        }
                        snapshot += "]}";
                    }
                }
                snapshot += "]}";
            }
        }
        snapshot += ']';

        snapshot += ",\"questEvents\":[";
        bool firstEvent = true;
        for (const auto& event : m_world.GetQuestService().GetRecentDebugEvents())
        {
            if (!firstEvent)
                snapshot += ',';
            snapshot += fmt::format(
                "{{\"sequence\":{},\"timeMs\":{},\"kind\":\"{}\",\"formId\":{},\"stage\":{},"
                "\"scopedOverride\":{},\"inParty\":{},\"leader\":{}}}",
                event.Sequence, event.TimeMs, EscapeJson(event.Kind.c_str()), event.FormId, event.Stage,
                JsonBool(event.ScopedOverride), JsonBool(event.InParty), JsonBool(event.Leader));
            firstEvent = false;
        }
        markSnapshotPhase(6);
        snapshot += ']';
        std::array<TriggerDiagnostic, 64> triggerEvents{};
        size_t triggerEventCount = 0;
        {
            std::lock_guard lock(m_triggerMutex);
            triggerEventCount = m_triggerCount;
            const size_t start = (m_triggerNext + m_triggerEvents.size() -
                m_triggerCount) % m_triggerEvents.size();
            for (size_t i = 0; i < triggerEventCount; ++i)
                triggerEvents[i] = m_triggerEvents[(start + i) % m_triggerEvents.size()];
        }
        snapshot += ",\"triggerEvents\":[";
        for (size_t i = 0; i < triggerEventCount; ++i)
        {
            const auto& event = triggerEvents[i];
            if (i != 0)
                snapshot += ',';
            snapshot += fmt::format(
                "{{\"sequence\":{},\"timeMs\":{},\"enter\":{},"
                "\"triggerFormId\":{},\"actorFormId\":{}}}",
                event.Sequence, event.TimeMs, JsonBool(event.Enter),
                event.TriggerFormId, event.ActorFormId);
        }
        snapshot += ']';
        snapshot += fmt::format(
            ",\"snapshotProfileUs\":{{\"setup\":{},\"actorPose\":{},"
            "\"referencesAndScenes\":{},\"cartAndTrace\":{},"
            "\"networkActors\":{},\"nearbyActors\":{},\"quests\":{}}}",
            snapshotPhaseUs[0], snapshotPhaseUs[1], snapshotPhaseUs[2],
            snapshotPhaseUs[3], snapshotPhaseUs[4], snapshotPhaseUs[5],
            snapshotPhaseUs[6]);
        snapshot += '}';

        std::scoped_lock lock(m_snapshotMutex);
        m_gameSnapshot = std::move(snapshot);
        m_gameSnapshotTimeMs = now;
        if (capturePose)
            m_lastPoseSnapshot = m_gameSnapshot;
        m_recentGameSnapshots.push_back({worldTick, now, m_gameSnapshot});
        while (m_recentGameSnapshots.size() > 80)
            m_recentGameSnapshots.pop_front();
    }
    catch (const std::exception& exception)
    {
        spdlog::warn("Game test snapshot failed: {}", exception.what());
    }
    catch (...)
    {
        spdlog::warn("Game test snapshot failed with native exception");
    }
}

std::string GameTestService::GetHitchSnapshot() const
{
    std::shared_ptr<const HitchSnapshotData> data;
    {
        std::scoped_lock lock(m_snapshotMutex);
        data = m_hitchSnapshot;
    }
    if (!data)
        return "null";
    constexpr uint32_t cartIds[]{0x000B9DF3, 0x000BB970};
    const auto& frameTiming = data->FrameTiming;
    const auto& physics = data->Physics;
    const auto& worldPhysics = data->WorldPhysics;
    const auto& poseProduction = data->PoseProduction;
    std::string hitch = fmt::format(
        "{{\"worldTick\":{},\"sampleTimeMs\":{},"
        "\"worldLastGapUs\":{},\"worldMaxGapUs\":{},"
        "\"worldMaxGameTestUs\":{},\"vmMaxAppUs\":{},"
        "\"vmMaxOriginalUs\":{},\"hostPhysicsPackets\":{},"
        "\"hostPhysicsUpdates\":{},\"hostBodyOnlyUpdates\":{},"
        "\"followerPhysicsPackets\":{},\"hostLastScanUs\":{},"
        "\"hostScanCount\":{},\"hostScanTotalUs\":{},"
        "\"hostScanMaxUs\":{},\"hostScanLastReferencesVisited\":{},"
        "\"hostScanLastCandidateCount\":{},"
        "\"hostKnownRefreshLastUs\":{},\"hostKnownRefreshMaxUs\":{},\"hostKnownRefreshTotalUs\":{},"
        "\"hostCurrentDiscoveryLastUs\":{},\"hostCurrentDiscoveryMaxUs\":{},\"hostCurrentDiscoveryTotalUs\":{},"
        "\"hostGridDiscoveryLastUs\":{},\"hostGridDiscoveryMaxUs\":{},\"hostGridDiscoveryTotalUs\":{},"
        "\"hostPruneLastUs\":{},\"hostPruneMaxUs\":{},\"hostPruneTotalUs\":{},"
        "\"poseSelectedBatches\":{},\"poseSelectedActors\":{},"
        "\"poseSelectedTotalUs\":{},\"poseSelectedMaxActorUs\":{},"
        "\"poseSelectedLastBatchUs\":{},\"poseSelectedLastBatchActors\":{},"
        "\"selectedBodyPeakStep\":{},"
        "\"selectedBodyStepsOver75\":{},"
        "\"selectedBodyPeakTimeMs\":{},"
        "\"selectedBodyPeakDt\":{},"
        "\"selectedBodyPeakPreLinearSpeed\":{},"
        "\"selectedBodyPeakPostLinearSpeed\":{},"
        "\"selectedBodyPeakPreAngularSpeed\":{},"
        "\"selectedBodyPeakPostAngularSpeed\":{},"
        "\"selectedBodyPeakMotionType\":{},"
        "\"selectedBodyPeakTargetApplied\":{},"
        "\"selectedBodyPeakTargetAgeMs\":{},"
        "\"selectedBodyPeakVelocityAfterWrite\":{},"
        "\"carts\":[",
        data->WorldTick, data->SampleTimeMs, frameTiming.WorldLastEntryGapUs,
        frameTiming.WorldMaxEntryGapUs,
        frameTiming.WorldMaxGameTestUs, frameTiming.VmMaxAppUs,
        frameTiming.VmMaxOriginalUs, physics.HostPacketsSent,
        physics.HostUpdatesQueued, physics.HostBodyOnlyUpdates,
        physics.FollowerPacketsReceived, physics.LastHostScanDurationUs,
        physics.HostScans, physics.HostScanTotalUs, physics.HostScanMaxUs,
        physics.HostLastReferencesVisited, physics.HostLastCandidateCount,
        physics.HostKnownRefreshLastUs, physics.HostKnownRefreshMaxUs,
        physics.HostKnownRefreshTotalUs,
        physics.HostCurrentDiscoveryLastUs, physics.HostCurrentDiscoveryMaxUs,
        physics.HostCurrentDiscoveryTotalUs,
        physics.HostGridDiscoveryLastUs, physics.HostGridDiscoveryMaxUs,
        physics.HostGridDiscoveryTotalUs,
        physics.HostPruneLastUs, physics.HostPruneMaxUs,
        physics.HostPruneTotalUs,
        poseProduction.Batches, poseProduction.Actors,
        poseProduction.TotalUs, poseProduction.MaxActorUs,
        poseProduction.LastBatchUs, poseProduction.LastBatchActors,
        worldPhysics.PeakSelectedBodyStepDelta,
        worldPhysics.SelectedBodyStepsOver75Units,
        worldPhysics.PeakSelectedBodyStepTimeMs,
        worldPhysics.PeakSelectedBodyStepDt,
        worldPhysics.PeakSelectedBodyPreLinearSpeed,
        worldPhysics.PeakSelectedBodyPostLinearSpeed,
        worldPhysics.PeakSelectedBodyPreAngularSpeed,
        worldPhysics.PeakSelectedBodyPostAngularSpeed,
        worldPhysics.PeakSelectedBodyMotionType,
        JsonBool(worldPhysics.PeakSelectedBodyTargetApplied),
        worldPhysics.PeakSelectedBodyTargetAgeMs,
        worldPhysics.PeakSelectedBodyVelocityAfterWrite);
    for (size_t i = 0; i < std::size(cartIds); ++i)
    {
        if (i)
            hitch += ',';
        const auto& stats = data->CartMotionStats[i];
        hitch += fmt::format(
            "{{\"formId\":{},\"samples\":{},\"peakStep\":{},"
            "\"largeSteps\":{},\"position\":[{},{},{}]}}",
            cartIds[i], stats.Samples, stats.PeakStep, stats.LargeSteps,
            stats.Position[0], stats.Position[1], stats.Position[2]);
    }
    hitch += "],\"history\":[";
    for (uint32_t i = 0; i < data->CartCount; ++i)
    {
        const auto index = (data->CartNext + data->CartHistory.size() -
            data->CartCount + i) % data->CartHistory.size();
        const auto& sample = data->CartHistory[index];
        if (i)
            hitch += ',';
        hitch += fmt::format(
            "{{\"tick\":{},\"present\":[{},{}],"
            "\"position\":[[{},{},{}],[{},{},{}]],"
            "\"horsePresent\":[{},{}],"
            "\"horsePosition\":[[{},{},{}],[{},{},{}]],"
            "\"rotation\":[[{},{},{}],[{},{},{}]],"
            "\"horseLevel\":[{},{}],\"horseController\":[{},{}],"
            "\"horseFlags\":[{},{}],\"horseState\":[{},{}],\"horseSupported\":[{},{}],"
            "\"supportMotion\":[{},{}],\"supportZ\":[{},{}],\"supportIsCart\":[{},{}],\"supportBody\":[{},{}],"
            "\"horseNodeZ\":[{},{}],\"probeInputZ\":{},\"probeCalls\":{},"
            "\"cartView\":[{},{}],\"horseView\":[{},{}],\"fov\":{},"
            "\"ctrlZ\":[{},{}],\"ctrlFallTime\":[{},{}],\"ctrlFallStart\":[{},{}],\"ctrlDeltaZ\":[{},{}]}}",
            sample.WorldTick, JsonBool(sample.Present[0]),
            JsonBool(sample.Present[1]),
            sample.Position[0][0], sample.Position[0][1],
            sample.Position[0][2], sample.Position[1][0],
            sample.Position[1][1], sample.Position[1][2],
            JsonBool(sample.HorsePresent[0]),
            JsonBool(sample.HorsePresent[1]),
            sample.HorsePosition[0][0], sample.HorsePosition[0][1],
            sample.HorsePosition[0][2], sample.HorsePosition[1][0],
            sample.HorsePosition[1][1], sample.HorsePosition[1][2],
            sample.Rotation[0][0], sample.Rotation[0][1], sample.Rotation[0][2],
            sample.Rotation[1][0], sample.Rotation[1][1], sample.Rotation[1][2],
            sample.HorseLevel[0], sample.HorseLevel[1],
            JsonBool(sample.HorseController[0]), JsonBool(sample.HorseController[1]),
            sample.HorseControllerFlags[0], sample.HorseControllerFlags[1],
            sample.HorseControllerState[0], sample.HorseControllerState[1],
            sample.HorseSupported[0], sample.HorseSupported[1],
            sample.SupportMotion[0], sample.SupportMotion[1], sample.SupportZ[0], sample.SupportZ[1],
            JsonBool(sample.SupportIsCart[0]), JsonBool(sample.SupportIsCart[1]),
            sample.SupportBody[0], sample.SupportBody[1],
            sample.HorseNodeZ[0], sample.HorseNodeZ[1], sample.ProbeInputZ, sample.ProbeCalls,
            sample.CartViewAngle[0], sample.CartViewAngle[1], sample.HorseViewAngle[0], sample.HorseViewAngle[1],
            sample.CameraFov,
            sample.CtrlZ[0], sample.CtrlZ[1], sample.CtrlFallTime[0], sample.CtrlFallTime[1],
            sample.CtrlFallStart[0], sample.CtrlFallStart[1], sample.CtrlDeltaZ[0], sample.CtrlDeltaZ[1]);
    }
    hitch += "],\"events\":[";
    for (uint32_t i = 0; i < data->MotionCount; ++i)
    {
        const auto index = (data->MotionNext + data->MotionEvents.size() -
            data->MotionCount + i) % data->MotionEvents.size();
        const auto& event = data->MotionEvents[index];
        if (i)
            hitch += ',';
        hitch += fmt::format(
            "{{\"timeMs\":{},\"worldTick\":{},\"formId\":{},"
            "\"worldGapUs\":{},\"vmGapUs\":{},"
            "\"priorVmAppUs\":{},\"priorVmOriginalUs\":{},"
            "\"priorGameTestUs\":{},\"cartDeltaMs\":{},"
            "\"cartStep\":{},\"position\":[{},{},{}],"
            "\"horsePresent\":{},\"horsePosition\":[{},{},{}]}}",
            event.TimeMs, event.WorldTick, event.FormId,
            event.WorldGapUs, event.VmGapUs,
            event.PriorVmAppUs, event.PriorVmOriginalUs,
            event.PriorGameTestUs, event.CartDeltaMs, event.CartStep,
            event.Position[0], event.Position[1], event.Position[2],
            JsonBool(event.HorsePresent), event.HorsePosition[0],
            event.HorsePosition[1], event.HorsePosition[2]);
    }
    hitch += "]}";
    return hitch;
}

std::string GameTestService::GetCachedGameSnapshot() const noexcept
{
    std::scoped_lock lock(m_snapshotMutex);
    return m_gameSnapshot;
}

std::string GameTestService::HarnessDriverTick(const std::string& aRequest)
{
    if (!aRequest.empty())
    {
        std::scoped_lock lock(IntroDriver::mutex);
        if (!IntroDriver::pending.empty()) throw std::runtime_error("driver already has a pending action");
        IntroDriver::pending = aRequest;
        ++IntroDriver::queuedSequence;
    }
    IntroDriver::Tick();
    std::scoped_lock lock(IntroDriver::mutex);
    return "{" + IntroDriver::published + "}";
}

std::string GameTestService::Execute(const std::string& acLine) noexcept
{
    const uint64_t id = GetJsonId(acLine);
    const auto command = GetJsonString(acLine, "command");
    try
    {
        if (command == "ping")
            return Result(id, fmt::format("\"pid\":{},\"protocol\":2", GetCurrentProcessId()));

        if (command == "farm_connect" || command == "farm_create_party" || command == "farm_state")
        {
            if (!FarmMode::Enabled() || !HarnessService::IsEnabled())
                return Error(id, "farm requires a harness build, SSC_FARM_TOKEN, SSC_FARM_ROOT and enabled harness.json");
            if (command == "farm_connect")
            {
                if (m_world.GetTransport().IsOnline()) return Error(id, "already connected");
                const auto portText = GetJsonString(acLine, "port");
                if (portText.empty() || portText.find_first_not_of("0123456789") != std::string::npos)
                    return Error(id, "decimal loopback port required");
                const auto port = std::stoul(portText);
                if (port < 1024 || port > 65535) return Error(id, "invalid loopback port");
                auto* settings = INISettingCollection::Get();
                auto* savePath = settings ? settings->GetSetting("sLocalSavePath:General") : nullptr;
                auto* characterPath = settings ? settings->GetSetting("sLocalCharacterDataPath:General") : nullptr;
                if (!savePath || !characterPath) return Error(id, "native save path settings unavailable");
                // AL109356/109357 read this string on every path construction.
                // AL75619 (Utility.SetINIString's setter) owns allocation and the
                // lowercase s -> managed S transition; never lend it a CRT buffer.
                using SetString = Setting*(Setting*, const char*);
                POINTER_SKYRIMSE(SetString, setString, 75619);
                setString.Get()(savePath, FarmMode::SaveRelative().c_str());
                setString.Get()(characterPath, (FarmMode::SaveRelative() + "Character\\").c_str());
                if (!savePath->data || FarmMode::SaveRelative() != reinterpret_cast<const char*>(savePath->data) ||
                    !characterPath->data || FarmMode::SaveRelative() + "Character\\" != reinterpret_cast<const char*>(characterPath->data))
                    return Error(id, "native save isolation failed");
                m_world.GetTransport().SetServerPassword(GetJsonString(acLine, "password"));
                m_world.GetTransport().Connect(fmt::format("127.0.0.1:{}", port).c_str());
            }
            else if (command == "farm_create_party")
            {
                if (!m_world.GetTransport().IsOnline() || m_world.GetPartyService().IsInParty())
                    return Error(id, "requires authenticated client without a party");
                m_world.GetPartyService().CreateParty();
            }
            auto* settings = INISettingCollection::Get();
            auto* savePath = settings ? settings->GetSetting("sLocalSavePath:General") : nullptr;
            auto* characterPath = settings ? settings->GetSetting("sLocalCharacterDataPath:General") : nullptr;
            const bool isolated = savePath && savePath->data &&
                FarmMode::SaveRelative() == reinterpret_cast<const char*>(savePath->data) &&
                characterPath && characterPath->data &&
                FarmMode::SaveRelative() + "Character\\" == reinterpret_cast<const char*>(characterPath->data);
            auto* active = settings ? settings->GetSetting("bAlwaysActive:General") : nullptr;
            auto* window = BSGraphics::GetMainWindow();
            RECT client{}, outer{};
            if (window && window->hWnd) { GetClientRect(window->hWnd, &client); GetWindowRect(window->hWnd, &outer); }
            spdlog::default_logger()->flush();
            return Result(id, fmt::format("\"pid\":{},\"playerId\":{},\"farm\":true,\"token\":\"{}\",\"saveIsolated\":{},"
                "\"alwaysActive\":{},\"width\":{},\"height\":{},\"x\":{},\"y\":{},\"foreground\":{}",
                GetCurrentProcessId(), m_world.GetTransport().GetLocalPlayerId(), FarmMode::Token(), isolated,
                active && (active->data & 0xFF), client.right, client.bottom, outer.left, outer.top,
                window && window->hWnd && GetForegroundWindow() == window->hWnd));
        }

        if (command == "harness_start" || command == "harness_status" || command == "harness_stop")
            return Result(id, "\"harness\":" + m_world.ctx().at<HarnessService>().Command(acLine));

        if (command == "intro_status")
        {
            std::scoped_lock lock(IntroDriver::mutex);
            return Result(id, IntroDriver::published + fmt::format(",\"queuedSequence\":{},\"pending\":{}",
                IntroDriver::queuedSequence, JsonBool(IntroDriver::queuedSequence != IntroDriver::appliedSequence)));
        }
        if (command == "creator_finish" || command == "walk_to" || command == "follow_objective" || command == "walk_cancel" || command == "jump_toward")
        {
            if (command == "creator_finish")
            {
                const auto name = GetJsonString(acLine, "name");
                if (name.empty() || name.size() > 64 || name.find_first_of("\r\n\t") != std::string::npos)
                    return Error(id, "name must contain 1..64 bytes without control characters");
            }
            std::scoped_lock lock(IntroDriver::mutex);
            if (!IntroDriver::pending.empty()) return Error(id, "intro command already pending");
            IntroDriver::pending = acLine;
            return Result(id, fmt::format("\"queued\":true,\"sequence\":{},\"statusCommand\":\"intro_status\"",
                ++IntroDriver::queuedSequence));
        }

        if (command == "capabilities")
            return Result(id, "\"protocol\":2,\"commands\":[\"ping\",\"capabilities\",\"snapshot\","
                "\"game_snapshot\",\"hitch_snapshot\",\"request_game_snapshot\",\"cancel_game_snapshot\",\"game_snapshot_at\",\"game_pose_snapshot\",\"watch_quest\",\"set_pose_probe\",\"set_pose_probe_actor\",\"test_displace_remote_corpse\",\"capture_bundle\",\"screenshot\",\"open_options\","
                "\"set_visual_pose_apply\",\"set_visual_root_diagnostic\",\"set_native_vehicle_trial\",\"set_remote_process_trial\",\"set_presentation_delay\",\"profile_host_frames\",\"set_camera_position_probe\",\"set_skip_next_post_respawn_knock\","
                "\"open_coop\",\"party_state\",\"set_session_open\",\"join_friend\",\"set_ready\",\"start_new_campaign\",\"start_continue_campaign\",\"create_test_checkpoint\",\"test_checkpoint_status\","
                "\"close_options\",\"controller\",\"race_menu_key\",\"gameplay_key\",\"race_menu_state\",\"confirm_character_native\",\"confirm_character_native_status\","
                "\"creator_finish\",\"walk_to\",\"follow_objective\",\"walk_cancel\",\"jump_toward\",\"intro_status\","
                "\"toggle_window\",\"confirm_display\",\"setting\",\"world_reference_state\"]");

        if (command == "watch_quest")
        {
            const auto editorId = GetJsonString(acLine, "editorId");
            if (editorId.empty())
                return Error(id, "editorId is required");
            std::scoped_lock lock(m_snapshotMutex);
            m_watchedQuests.insert(editorId);
            return Result(id, fmt::format("\"editorId\":\"{}\",\"watched\":true", EscapeJson(editorId)));
        }

        if (command == "request_game_snapshot")
        {
            const auto tickText = GetJsonString(acLine, "tick");
            const auto delayText = GetJsonString(acLine, "delay_ms");
            if ((!tickText.empty() &&
                    tickText.find_first_not_of("0123456789") != std::string::npos) ||
                (!delayText.empty() &&
                    delayText.find_first_not_of("0123456789") != std::string::npos))
                return Error(id, "tick and delay_ms must be decimal strings");
            const auto delay = delayText.empty() ? uint64_t{1} :
                std::strtoull(delayText.c_str(), nullptr, 10);
            if (delay > 30000)
                return Error(id, "delay_ms exceeds 30000");
            uint64_t nowTick{};
            uint64_t target{};
            {
                std::scoped_lock lock(m_snapshotScheduleMutex);
                nowTick = m_world.GetTick();
                target = tickText.empty() ? nowTick + delay :
                    std::strtoull(tickText.c_str(), nullptr, 10);
                if (!target)
                    return Error(id, "target tick must be nonzero");
                if (m_snapshotRelativeDueWallMs || m_snapshotTargetTick)
                    return Error(id, fmt::format(
                        "snapshot already scheduled at tick {}", m_snapshotTargetTick));
                m_snapshotTargetAuthorityEpoch =
                    m_world.GetTransport().GetAuthorityEpoch();
                m_snapshotTargetTick = target;
                if (tickText.empty())
                    m_snapshotRelativeDueWallMs = GetTickCount64() + delay;
            }
            ArmDiagnosticCapture(target > nowTick ? target - nowTick : 0);
            ObjectService::ArmRenderDiagnostics();
            return Result(id, fmt::format("\"currentTick\":{},\"targetTick\":{},\"relative\":{}",
                nowTick, target, JsonBool(tickText.empty())));
        }
        if (command == "cancel_game_snapshot")
        {
            std::scoped_lock lock(m_snapshotScheduleMutex);
            const bool hadPending = m_snapshotRelativeDueWallMs || m_snapshotTargetTick;
            m_snapshotRelativeDueWallMs = 0;
            m_snapshotTargetTick = 0;
            s_diagnosticCaptureUntilMs.store(0, std::memory_order_relaxed);
            AnimationGraphUpdateTrace::WatchHolder(nullptr, 0);
            return Result(id, fmt::format("\"cancelled\":{}", JsonBool(hadPending)));
        }
        if (command == "game_snapshot")
        {
            bool pending{};
            bool relative{};
            {
                std::scoped_lock scheduleLock(m_snapshotScheduleMutex);
                pending = m_snapshotRelativeDueWallMs || m_snapshotTargetTick;
                relative = m_snapshotRelativeDueWallMs != 0;
            }
            std::scoped_lock lock(m_snapshotMutex);
            const auto age = m_gameSnapshotTimeMs ? GetTickCount64() - m_gameSnapshotTimeMs : 0;
            return Result(id, fmt::format("\"ageMs\":{},\"capturePending\":{},\"relative\":{},\"game\":{}",
                age, JsonBool(pending), JsonBool(relative), m_gameSnapshot));
        }
        if (command == "hitch_snapshot")
        {
            const auto hitch = GetHitchSnapshot();
            if (hitch == "null")
                return Error(id, "no hitch snapshot available");
            return Result(id, fmt::format("\"game\":{}", hitch));
        }
        if (command == "game_pose_snapshot")
        {
            std::scoped_lock lock(m_snapshotMutex);
            if (m_lastPoseSnapshot == "null")
                return Error(id, "no pose snapshot available");
            return Result(id, fmt::format("\"game\":{}", m_lastPoseSnapshot));
        }
        if (command == "game_snapshot_at")
        {
            const auto tickText = GetJsonString(acLine, "tick");
            if (tickText.empty() || tickText.find_first_not_of("0123456789") != std::string::npos)
                return Error(id, "tick must be a decimal string");
            const auto targetTick = std::strtoull(tickText.c_str(), nullptr, 10);
            std::scoped_lock lock(m_snapshotMutex);
            if (m_recentGameSnapshots.empty())
                return Error(id, "no game snapshots available");
            const auto nearest = std::min_element(m_recentGameSnapshots.begin(),
                m_recentGameSnapshots.end(), [targetTick](const auto& left, const auto& right)
                {
                    const auto leftError = left.WorldTick > targetTick ?
                        left.WorldTick - targetTick : targetTick - left.WorldTick;
                    const auto rightError = right.WorldTick > targetTick ?
                        right.WorldTick - targetTick : targetTick - right.WorldTick;
                    return leftError < rightError;
                });
            const auto errorMs = nearest->WorldTick > targetTick ?
                nearest->WorldTick - targetTick : targetTick - nearest->WorldTick;
            if (errorMs > 200)
                return Error(id, "no snapshot within 200 ms of requested tick");
            return Result(id, fmt::format(
                "\"requestedTick\":{},\"sampleTick\":{},\"errorMs\":{},\"game\":{}",
                targetTick, nearest->WorldTick, errorMs, nearest->Json));
        }
        if (command == "set_pose_probe")
        {
            const bool enabled = GetJsonString(acLine, "enabled") == "true";
            uint64_t targetTick = 0;
            if (enabled)
            {
                const auto tickText = GetJsonString(acLine, "tick");
                if (!tickText.empty() && tickText.find_first_not_of("0123456789") != std::string::npos)
                    return Error(id, "tick must be a decimal string");
                targetTick = tickText.empty() ? 1 : std::strtoull(tickText.c_str(), nullptr, 10);
                if (targetTick == 0)
                    return Error(id, "tick must be nonzero when enabling pose capture");
            }
            m_poseProbeTargetTick.store(targetTick, std::memory_order_relaxed);
            if (enabled)
            {
                const auto nowTick = m_world.GetTick();
                ArmDiagnosticCapture(targetTick > nowTick ? targetTick - nowTick : 0);
            }
            return Result(id, fmt::format("\"enabled\":{},\"targetTick\":{}",
                JsonBool(enabled), targetTick));
        }

        if (command == "set_pose_probe_actor")
        {
            const auto formText = GetJsonString(acLine, "form_id");
            if (formText.empty() ||
                formText.find_first_not_of("0123456789") != std::string::npos)
                return Error(id, "form_id must be a decimal string; zero restores ambient capture");
            const auto formId = std::strtoull(formText.c_str(), nullptr, 10);
            if (formId > std::numeric_limits<uint32_t>::max())
                return Error(id, "form_id exceeds uint32 range");
            m_poseProbeFormId.store(static_cast<uint32_t>(formId),
                std::memory_order_release);
            return Result(id, fmt::format("\"formId\":{}", formId));
        }

        if (command == "test_displace_remote_corpse")
        {
            const auto formText = GetJsonString(acLine, "form_id");
            if (formText.empty() ||
                formText.find_first_not_of("0123456789") != std::string::npos)
                return Error(id, "form_id must be a nonzero decimal string");
            const auto formId = std::strtoull(formText.c_str(), nullptr, 10);
            if (!formId || formId > std::numeric_limits<uint32_t>::max())
                return Error(id, "form_id out of range");
            uint32_t pending = 0;
            if (!m_testCorpseDisplaceFormId.compare_exchange_strong(pending,
                    static_cast<uint32_t>(formId), std::memory_order_acq_rel))
                return Error(id, "corpse displacement test already pending");
            return Result(id, fmt::format("\"queuedFormId\":{}", formId));
        }

        if (command == "set_visual_pose_apply")
        {
            const auto enabledText = GetJsonString(acLine, "enabled");
            if (enabledText != "true" && enabledText != "false")
                return Error(id, "enabled must be the string true or false");
            VisualPoseMailbox::SetApplyEnabled(enabledText == "true");
            return Result(id, fmt::format("\"enabled\":{}",
                JsonBool(VisualPoseMailbox::IsApplyEnabled())));
        }

        if (command == "set_visual_pose_apply_form")
        {
            const auto formText = GetJsonString(acLine, "form_id");
            if (formText.empty() ||
                formText.find_first_not_of("0123456789") != std::string::npos)
                return Error(id, "form_id must be a decimal form ID; zero disables selection");
            const auto formId = std::strtoull(formText.c_str(), nullptr, 10);
            if (formId > std::numeric_limits<uint32_t>::max())
                return Error(id, "form_id exceeds uint32 range");
            VisualPoseMailbox::SetApplyFormId(static_cast<uint32_t>(formId));
            return Result(id, fmt::format("\"formId\":{}", formId));
        }

        if (command == "set_native_vehicle_trial")
        {
            const auto riderText = GetJsonString(acLine, "rider_id");
            if (riderText.empty() ||
                riderText.find_first_not_of("0123456789") != std::string::npos)
                return Error(id, "rider_id must be a decimal server ID; zero disables the trial");
            const auto riderId = std::strtoull(riderText.c_str(), nullptr, 10);
            if (riderId > std::numeric_limits<uint32_t>::max())
                return Error(id, "rider_id exceeds uint32 range");
            m_world.GetCharacterService().SetVehicleTrialRiderId(
                static_cast<uint32_t>(riderId));
            return Result(id, fmt::format("\"riderId\":{}", riderId));
        }

        if (command == "set_combat_target_trial")
        {
            const auto actorText = GetJsonString(acLine, "actor_id");
            const auto targetText = GetJsonString(acLine, "target_id");
            const auto valid = [](const std::string& value) {
                return !value.empty() &&
                    value.find_first_not_of("0123456789") == std::string::npos;
            };
            if (!valid(actorText) || !valid(targetText))
                return Error(id, "actor_id and target_id must be decimal server IDs");
            const auto actorId = std::strtoull(actorText.c_str(), nullptr, 10);
            const auto targetId = std::strtoull(targetText.c_str(), nullptr, 10);
            if (actorId > std::numeric_limits<uint32_t>::max() ||
                targetId > std::numeric_limits<uint32_t>::max() || !actorId)
                return Error(id, "actor_id is invalid or a form ID exceeds uint32 range");
            auto* pActor = Utils::GetByServerId<Actor>(
                static_cast<uint32_t>(actorId));
            if (!pActor || !Utils::GetLocalOwnershipToken(pActor->formID) ||
                !pActor->pCombatController)
                return Error(id, "actor is not locally owned or has no combat controller");
            auto* pTarget = targetId ? Utils::GetByServerId<Actor>(
                static_cast<uint32_t>(targetId)) : nullptr;
            if (targetId && !pTarget)
                return Error(id, "target actor is not loaded");
            pActor->SetCombatTargetEx(pTarget);
            return Result(id, fmt::format("\"actorId\":{},\"targetId\":{},"
                "\"actorFormId\":{},\"targetFormId\":{}",
                actorId, targetId, pActor->formID,
                pTarget ? pTarget->formID : 0));
        }

        if (command == "set_combat_target_authority_trial")
        {
            const auto actorText = GetJsonString(acLine, "actor_form_id");
            if (actorText.empty() ||
                actorText.find_first_not_of("0123456789") != std::string::npos)
                return Error(id, "actor_form_id must be a decimal form ID; zero disables");
            const auto actorId = std::strtoull(actorText.c_str(), nullptr, 10);
            if (actorId > std::numeric_limits<uint32_t>::max())
                return Error(id, "actor_form_id exceeds uint32 range");
            CombatController::SetTargetAuthorityTrialActor(
                static_cast<uint32_t>(actorId));
            return Result(id, fmt::format("\"actorFormId\":{}", actorId));
        }

        if (command == "set_remote_process_trial")
        {
            const auto riderText = GetJsonString(acLine, "rider_form_id");
            const auto mountText = GetJsonString(acLine, "mount_form_id");
            const auto valid = [](const std::string& value) {
                return !value.empty() &&
                    value.find_first_not_of("0123456789") == std::string::npos;
            };
            if (!valid(riderText) || !valid(mountText))
                return Error(id, "rider_form_id and mount_form_id must be decimal strings; zero disables");
            const auto riderId = std::strtoull(riderText.c_str(), nullptr, 10);
            const auto mountId = std::strtoull(mountText.c_str(), nullptr, 10);
            if (riderId > std::numeric_limits<uint32_t>::max() ||
                mountId > std::numeric_limits<uint32_t>::max())
                return Error(id, "form ID exceeds uint32 range");
            Actor::SetRemoteProcessTrial(static_cast<uint32_t>(riderId),
                static_cast<uint32_t>(mountId));
            return Result(id, fmt::format("\"riderFormId\":{},\"mountFormId\":{}",
                riderId, mountId));
        }

        if (command == "set_visual_root_diagnostic")
        {
            const auto formText = GetJsonString(acLine, "form_id");
            if (formText.empty() ||
                formText.find_first_not_of("0123456789") != std::string::npos)
                return Error(id, "form_id must be a decimal string; zero disables the diagnostic");
            const auto formId = std::strtoull(formText.c_str(), nullptr, 10);
            if (formId > std::numeric_limits<uint32_t>::max())
                return Error(id, "form_id exceeds uint32 range");
            VisualPoseMailbox::SetRootDiagnosticFormId(static_cast<uint32_t>(formId));
            return Result(id, fmt::format("\"formId\":{}", formId));
        }

        if (command == "profile_host_frames")
        {
            const auto durationText = GetJsonString(acLine, "seconds");
            if (durationText.empty() || durationText.size() > 3 ||
                durationText.find_first_not_of("0123456789") != std::string::npos)
                return Error(id, "seconds must be a decimal string (0-120); zero stops profiling");
            const auto duration = std::strtoul(durationText.c_str(), nullptr, 10);
            if (duration > 120)
                return Error(id, "seconds must be 0-120");
            HostFrameCost::s_untilMs.store(duration ? GetTickCount64() + duration * 1000 : 0,
                std::memory_order_relaxed);
            return Result(id, fmt::format("\"seconds\":{},\"hostOnly\":false", duration));
        }

        if (command == "set_presentation_delay")
        {
            const auto delayText = GetJsonString(acLine, "delay_ms");
            if (delayText.empty() ||
                delayText.find_first_not_of("0123456789") != std::string::npos)
                return Error(id, "delay_ms must be a decimal string (50-500); zero restores 300");
            const auto requested = std::strtoull(delayText.c_str(), nullptr, 10);
            const auto delay = requested == 0 ? 300 : requested;
            if (delay < 50 || delay > 500)
                return Error(id, "delay_ms must be 50-500 or zero to restore 300");
            m_world.GetCharacterService().SetPresentationDelayMs(
                static_cast<uint32_t>(delay));
            return Result(id, fmt::format("\"delayMs\":{}", delay));
        }

        if (command == "set_camera_position_probe")
        {
            const auto enabledText = GetJsonString(acLine, "enabled");
            if (enabledText != "true" && enabledText != "false")
                return Error(id, "enabled must be the string true or false");
            auto& cameraService = m_world.ctx().at<CameraService>();
            cameraService.SetPositionProbeEnabled(enabledText == "true");
            return Result(id, fmt::format("\"enabled\":{}",
                JsonBool(cameraService.IsPositionProbeEnabled())));
        }

        if (command == "set_skip_next_post_respawn_knock")
        {
            const auto enabledText = GetJsonString(acLine, "enabled");
            if (enabledText != "true" && enabledText != "false")
                return Error(id, "enabled must be the string true or false");
            auto& playerService = m_world.ctx().at<PlayerService>();
            playerService.SetSkipNextPostRespawnKnock(enabledText == "true");
            return Result(id, fmt::format("\"enabled\":{}",
                JsonBool(playerService.GetDeathDiagnostic().SkipNextPostRespawnKnock)));
        }


        if (command == "set_body_playback_probe")
        {
            const auto formText = GetJsonString(acLine, "form_id");
            if (formText.empty() ||
                formText.find_first_not_of("0123456789") != std::string::npos)
                return Error(id, "form_id must be a decimal string; zero disables the probe");
            const auto formId = std::strtoull(formText.c_str(), nullptr, 10);
            if (formId > std::numeric_limits<uint32_t>::max())
                return Error(id, "form_id exceeds uint32 range");
            ObjectService::SetBodyPlaybackProbe(static_cast<uint32_t>(formId));
            return Result(id, fmt::format("\"selectedFormId\":{}",
                ObjectService::GetBodyPlaybackDiagnostic().SelectedFormId));
        }

        if (command == "set_pre_step_body_playback_probe")
        {
            const auto formText = GetJsonString(acLine, "form_id");
            if (formText.empty() ||
                formText.find_first_not_of("0123456789") != std::string::npos)
                return Error(id, "form_id must be a decimal string; zero disables the probe");
            const auto formId = std::strtoull(formText.c_str(), nullptr, 10);
            if (formId > std::numeric_limits<uint32_t>::max())
                return Error(id, "form_id exceeds uint32 range");
            const auto modeText = GetJsonString(acLine, "mode");
            if (formId && !modeText.empty() && modeText != "velocity" &&
                modeText != "pose" && modeText != "kinematic" &&
                modeText != "hard_kinematic")
                return Error(id, "mode must be velocity, pose, kinematic, or hard_kinematic");
            const uint32_t mode = modeText == "hard_kinematic" ? 4 :
                modeText == "kinematic" ? 3 :
                (modeText == "pose" ? 2 : 1);
            ObjectService::SetPreStepBodyPlaybackProbe(
                static_cast<uint32_t>(formId), mode);
            const auto diagnostic = ObjectService::GetPreStepPlaybackDiagnostic();
            return Result(id, fmt::format("\"selectedFormId\":{},\"mode\":{}",
                diagnostic.SelectedFormId, diagnostic.Mode));
        }

        if (command == "set_reference_phase_probe")
        {
            const auto formText = GetJsonString(acLine, "form_id");
            if (formText.empty() ||
                formText.find_first_not_of("0123456789") != std::string::npos)
                return Error(id, "form_id must be a decimal string; zero disables the probe");
            const auto formId = std::strtoull(formText.c_str(), nullptr, 10);
            if (formId > std::numeric_limits<uint32_t>::max())
                return Error(id, "form_id exceeds uint32 range");
            ObjectService::SetReferencePhaseProbe(static_cast<uint32_t>(formId));
            return Result(id, fmt::format("\"selectedFormId\":{}",
                ObjectService::GetReferencePhaseDiagnostic().SelectedFormId));
        }

        if (command == "world_reference_state")
        {
            const auto formText = GetJsonString(acLine, "form_id");
            if (formText.empty() || formText.find_first_not_of("0123456789abcdefABCDEF") != std::string::npos)
                return Error(id, "form_id must be a hexadecimal string");
            const auto value = std::strtoull(formText.c_str(), nullptr, 16);
            if (!value || value > UINT32_MAX) return Error(id, "form_id out of range");
            // The window callback only reads a mailbox; the native main-loop
            // phase samples the reference. Poll pending=true with the same ID.
            return Result(id, WorldStateService::Diagnostic(static_cast<uint32_t>(value)));
        }

        if (command == "native_reference_address")
        {
            const auto formText = GetJsonString(acLine, "form_id");
            if (formText.empty() ||
                formText.find_first_not_of("0123456789") != std::string::npos)
                return Error(id, "form_id must be a decimal string");
            const auto formId = std::strtoull(formText.c_str(), nullptr, 10);
            if (formId == 0 || formId > std::numeric_limits<uint32_t>::max())
                return Error(id, "form_id out of range");
            auto* pReference = Cast<TESObjectREFR>(TESForm::GetById(
                static_cast<uint32_t>(formId)));
            if (!pReference || !pReference->loadedState)
                return Error(id, "reference unavailable or not loaded");
            return Result(id, fmt::format(
                "\"processId\":{},\"formId\":{},"
                "\"referenceAddress\":\"{}\","
                "\"positionXAddress\":\"{}\"",
                GetCurrentProcessId(), pReference->formID,
                reinterpret_cast<uintptr_t>(pReference),
                reinterpret_cast<uintptr_t>(&pReference->position.x)));
        }

        if (command == "capture_bundle")
        {
            const auto path = m_world.GetGameSettingsService().CaptureTestScreenshot();
            if (path.empty())
                return Error(id, "screenshot failed");
            const auto statePath = path.parent_path() / (path.stem().string() + ".game.json");
            std::ofstream state(statePath, std::ios::binary);
            state << GetCachedGameSnapshot();
            if (!state)
                return Error(id, "game snapshot write failed");
            return Result(id, fmt::format("\"screenshotPath\":\"{}\",\"gameStatePath\":\"{}\"",
                EscapeJson(path.string()), EscapeJson(statePath.string())));
        }

        if (command == "open_options")
        {
            auto& overlay = m_world.GetOverlayService();
            overlay.SetActive(true);
            if (auto* pApp = overlay.GetOverlayApp())
                pApp->ExecuteAsync("showTitleOptions");
            m_world.GetGameSettingsService().RequestSettings();
            return Result(id, "\"action\":\"open_options\"");
        }
        if (command == "open_coop")
        {
            auto& overlay = m_world.GetOverlayService();
            overlay.SetActive(true);
            if (auto* pApp = overlay.GetOverlayApp())
                pApp->ExecuteAsync("showTitleLobby");
            m_world.GetSteamLobbyService().QueueRefreshLobbyState();
            return Result(id, "\"action\":\"open_coop\"");
        }
        if (command == "party_state")
        {
            const auto& party = m_world.GetPartyService();
            return Result(id, fmt::format(
                "\"inParty\":{},\"leader\":{},\"memberCount\":{},\"readyCount\":{},\"sessionState\":{},\"startEpoch\":{},\"online\":{}",
                JsonBool(party.IsInParty()), JsonBool(party.IsLeader()), party.GetPartyMembers().size(),
                party.GetReadyPlayerCount(), party.GetSessionState(), party.GetStartEpoch(), JsonBool(m_world.GetTransport().IsOnline())));
        }
        if (command == "create_test_checkpoint")
            return Error(id, "direct save is disabled after a paired cinematic hang; use gameplay_key quicksave after control handoff");
        if (command == "test_checkpoint_status")
            return Error(id, "direct save is disabled after a paired cinematic hang");
        // Every physics body in a reference's 3D tree (carts: body, wheels, harness...).
        // Testing ground setup: leave the intro on this PC. Stops MQ101 (0003372B) and undoes what it
        // leaves on the player: controls disabled, character-creation mode (no saving), AI driven,
        // restrained. Papyrus natives called with their (VM, stack, self or static tag, args) form.
        if (command == "skip_intro")
        {
            struct Game
            {
            };
            using Quest = TESQuest;
            auto* pPlayer = PlayerCharacter::Get();
            auto* pIntro = Cast<TESQuest>(TESForm::GetById(0x0003372B));
            if (!pPlayer)
                return Error(id, "player not found");
            // Each native only if it was found by name (a missing one was a call to address 0).
            std::string missing;
            // keep_quest: release this player only (a separated test away from the intro scene); stopping the intro
            // on one PC of a session would reach the others through quest sync.
            const bool keepQuest = GetJsonString(acLine, "keep_quest") == "true";
            PAPYRUS_FUNCTION(void, Quest, Stop);
            if (!s_pStop)
                missing += "Quest.Stop ";
            else if (pIntro && !keepQuest)
                s_pStop(pIntro);
            // Game.SetInChargen is a static native with no address in the registration hook (papyrus_natives.tsv):
            // call its implementation 55576 (VM, stack id, static tag, disableSaving, disableWaiting, showMessage).
            {
                using TSetInChargen = void(void*, uint32_t, void*, bool, bool, bool);
                POINTER_SKYRIMSE(TSetInChargen, setInChargen, 55576);
                if (auto* pVM = GameVM::Get() ? GameVM::Get()->virtualMachine : nullptr)
                    setInChargen.Get()(pVM, 0, nullptr, false, false, false);
                else
                    missing += "Game.SetInChargen ";
            }
            // Game's static natives register without a direct address (papyrus_natives.tsv); call their
            // implementations (VM, stack id, static tag, arguments): EnablePlayerControls 55455
            // (0x140A23FC0), SetPlayerAIDriven 55577 (0x140A2AE50).
            {
                using TEnableControls = void(void*, uint32_t, void*, bool, bool, bool, bool, bool, bool, bool, bool, int32_t);
                using TSetAIDriven = void(void*, uint32_t, void*, bool);
                POINTER_SKYRIMSE(TEnableControls, enableControls, 55455);
                POINTER_SKYRIMSE(TSetAIDriven, setAIDriven, 55577);
                auto* pVM = GameVM::Get() ? GameVM::Get()->virtualMachine : nullptr;
                if (!pVM)
                    missing += "VM ";
                else
                {
                    enableControls.Get()(pVM, 0, nullptr, true, true, true, true, true, true, true, true, 0);
                    setAIDriven.Get()(pVM, 0, nullptr, false);
                }
            }
            PAPYRUS_FUNCTION(void, Actor, SetRestrained, bool);
            if (!s_pSetRestrained)
                missing += "Actor.SetRestrained ";
            else
                s_pSetRestrained(pPlayer, false);
            return Result(id, fmt::format("\"introStopped\":{},\"missing\":\"{}\"", JsonBool(pIntro != nullptr && !keepQuest),
                missing));
        }
        // Testing ground (docs: C:\Tools\skyrim_re\testground.ps1). Move this PC's player to a
        // persistent reference (a map marker), offset sideways so players do not overlap.
        if (command == "teleport_player")
        {
            // A worldspace and a position (the marker's, read from the plugin): the marker reference
            // itself is only loaded while its cell is.
            const auto worldText = GetJsonString(acLine, "world");
            const auto offsetText = GetJsonString(acLine, "offset");
            auto* pWorldSpace = worldText.empty() ? nullptr : Cast<TESWorldSpace>(TESForm::GetById(std::stoul(worldText, nullptr, 16)));
            auto* pPlayer = PlayerCharacter::Get();
            // No worldspace: a move inside the player's current cell (interiors included; never another cell).
            if (worldText.empty() && pPlayer && pPlayer->parentCell && !GetJsonString(acLine, "x").empty())
            {
                NiPoint3 target{};
                target.x = std::stof(GetJsonString(acLine, "x"));
                target.y = std::stof(GetJsonString(acLine, "y"));
                target.z = std::stof(GetJsonString(acLine, "z"));
                auto* pCell = pPlayer->parentCell;
                QueueCreatorCall([pCell, target]() {
                    if (auto* pLocal = PlayerCharacter::Get(); pLocal && pLocal->parentCell == pCell)
                        pLocal->MoveTo(pCell, target);
                });
                return Result(id, fmt::format("\"cell\":\"{:X}\",\"x\":{:.0f},\"y\":{:.0f},\"z\":{:.0f},\"sameCell\":true",
                    pCell->formID, target.x, target.y, target.z));
            }
            if (!pWorldSpace || !pPlayer || GetJsonString(acLine, "x").empty())
                return Error(id, "worldspace, position or player missing");
            NiPoint3 target{};
            target.x = std::stof(GetJsonString(acLine, "x")) + (offsetText.empty() ? 0.f : std::stof(offsetText));
            target.y = std::stof(GetJsonString(acLine, "y"));
            target.z = std::stof(GetJsonString(acLine, "z"));
            auto* pCell = ModManager::Get()->GetCellFromCoordinates(static_cast<int32_t>(std::floor(target.x / 4096.f)),
                static_cast<int32_t>(std::floor(target.y / 4096.f)), pWorldSpace, true);
            if (!pCell)
                return Error(id, "exterior cell not found");
            pPlayer->MoveTo(pCell, target);
            return Result(id, fmt::format("\"cell\":\"{:X}\",\"x\":{:.0f},\"y\":{:.0f},\"z\":{:.0f}", pCell->formID,
                target.x, target.y, target.z));
        }
        // Place an actor (a base form) in front of this PC's player.
        if (command == "spawn_actor")
        {
            const auto baseText = GetJsonString(acLine, "base");
            const auto distanceText = GetJsonString(acLine, "distance");
            auto* pBase = baseText.empty() ? nullptr : TESForm::GetById(std::stoul(baseText, nullptr, 16));
            auto* pPlayer = PlayerCharacter::Get();
            if (!pBase || !pPlayer)
                return Error(id, "base form or player not found");
            using ObjectReference = TESObjectREFR;
            PAPYRUS_FUNCTION(TESObjectREFR*, ObjectReference, PlaceAtMe, TESForm*, int32_t, bool, bool);
            auto* pPlaced = s_pPlaceAtMe(pPlayer, pBase, 1, false, false);
            if (!pPlaced)
                return Error(id, "PlaceAtMe returned nothing");
            const float distance = distanceText.empty() ? 300.f : std::stof(distanceText);
            NiPoint3 target = pPlayer->position;
            target.x += std::sin(pPlayer->rotation.z) * distance;
            target.y += std::cos(pPlayer->rotation.z) * distance;
            // Or at an explicit point (the same spot on every PC, e.g. at another player's feet).
            if (!GetJsonString(acLine, "x").empty())
            {
                target.x = std::stof(GetJsonString(acLine, "x"));
                target.y = std::stof(GetJsonString(acLine, "y"));
                target.z = std::stof(GetJsonString(acLine, "z"));
            }
            pPlaced->MoveTo(pPlayer->parentCell, target);
            return Result(id, fmt::format("\"form_id\":\"{:X}\"", pPlaced->formID));
        }
        // Drop an item from this PC's player (added first) the way the inventory menu does. Queued to the main frame
        // (GameTestService::RunMainFrameRequests): calling the drop on this window thread crashed the game twice
        // (2026-09-28 19:03 and 19:09). Poll nearby_refs for the result.
        // Enable or disable a reference the way quest scripts do (Papyrus EnableNoWait / TESObjectREFR::Disable), on
        // the game thread. Measures the world-state pipeline end to end.
        if (command == "set_disabled")
        {
            const auto formText = GetJsonString(acLine, "form_id");
            if (formText.empty())
                return Error(id, "form_id missing");
            std::lock_guard lock(s_mainFrameDropLock);
            s_mainFrameDisable = {static_cast<uint32_t>(std::stoul(formText, nullptr, 16)), GetJsonString(acLine, "disabled") != "false"};
            return Result(id, "\"queued\":true");
        }
        // Revive tests: face a point (heading toward x,y), hold Activate, damage this PC's player (game thread).
        // Cheap per-frame-safe camera sample: camera root and player root world positions, the camera's view
        // direction and one quest's stage. For probes that run inside a scene (a full game snapshot costs ~0.2 s).
        if (command == "smooth_stamp")
        {
            extern std::atomic<bool> g_smoothSnapshotStamp;
            if (const auto enabled = GetJsonString(acLine, "enabled"); !enabled.empty())
                g_smoothSnapshotStamp = enabled != "false";
            return Result(id, fmt::format("\"enabled\":{}", g_smoothSnapshotStamp.load()));
        }
        if (command == "camera_sample")
        {
            auto* player = PlayerCharacter::Get();
            auto* camera = PlayerCamera::Get();
            if (!player || !player->GetNiNode() || !camera || !camera->cameraNode)
                return Error(id, "player or camera not loaded");
            const auto& c = camera->cameraNode->world;
            const auto& p = player->GetNiNode()->world.translate;
            const auto questText = GetJsonString(acLine, "quest");
            auto* quest = questText.empty() ? nullptr : Cast<TESQuest>(TESForm::GetById(std::stoul(questText, nullptr, 16)));
            return Result(id, fmt::format("\"camera\":[{},{},{}],\"view\":[{},{},{}],\"body\":[{},{},{}],\"stage\":{}",
                c.translate.x, c.translate.y, c.translate.z, c.rotate.entry[0][1], c.rotate.entry[1][1], c.rotate.entry[2][1],
                p.x, p.y, p.z, quest ? quest->currentStage : -1));
        }
        if (command == "face_point")
        {
            auto* player = PlayerCharacter::Get();
            if (!player || GetJsonString(acLine, "x").empty())
                return Error(id, "player or point missing");
            const float dx = std::stof(GetJsonString(acLine, "x")) - player->position.x;
            const float dy = std::stof(GetJsonString(acLine, "y")) - player->position.y;
            const float z = std::atan2(dx, dy);
            // Look at the point like a player would: at its height when "z" is given (eyes about
            // 120 u up), otherwise straight ahead, never keeping an earlier look up or down.
            const auto pointZ = GetJsonString(acLine, "z");
            const float x = pointZ.empty() ? 0.f :
                std::clamp(std::atan2(player->position.z + 120.f - std::stof(pointZ), std::max(std::hypot(dx, dy), 1.f)), -1.2f, 1.2f);
            player->SetRotation(x, player->rotation.y, z);
            return Result(id, fmt::format("\"yaw\":{},\"pitch\":{}", z, x));
        }
        if (command == "revive_hold")
        {
            ReviveService::SetTestHold(GetJsonString(acLine, "enabled") != "false");
            return Result(id, fmt::format("\"held\":{}", JsonBool(GetJsonString(acLine, "enabled") != "false")));
        }
        if (command == "appearance_state")
        {
            // Character creation slider spill check: this player's and every remote player copy's NPC base, its
            // face morph storage (pointer + value hash), weight, head data and head parts. Shared pointers between two
            // characters mean one player's slider writes land on the other.
            const auto describe = [](Actor* apActor) -> std::string
            {
                auto* pNpc = apActor ? Cast<TESNPC>(apActor->baseForm) : nullptr;
                if (!pNpc)
                    return "null";
                uint64_t hash = 1469598103934665603ull;
                if (pNpc->faceMorphs)
                {
                    const auto* bytes = reinterpret_cast<const uint8_t*>(pNpc->faceMorphs);
                    for (size_t i = 0; i < sizeof(TESNPC::FaceMorphs); ++i)
                        hash = (hash ^ bytes[i]) * 1099511628211ull;
                }
                std::string parts = "[";
                for (uint8_t i = 0; pNpc->headparts && i < pNpc->headpartsCount; ++i)
                    parts += fmt::format("{}\"{:X}\"", i ? "," : "", pNpc->headparts[i] ? pNpc->headparts[i]->formID : 0);
                parts += "]";
                return fmt::format("{{\"actor\":\"{:X}\",\"npc\":\"{:X}\",\"npcPtr\":\"{}\",\"faceMorphs\":\"{}\",\"morphHash\":\"{:X}\","
                    "\"morph0\":{:.3f},\"weight\":{:.2f},\"headData\":\"{}\",\"headpartsPtr\":\"{}\",\"headparts\":{},\"faceNPC\":\"{:X}\","
                    "\"race\":\"{:X}\",\"name\":\"{}\"}}",
                    apActor->formID, pNpc->formID, fmt::ptr(pNpc), fmt::ptr(pNpc->faceMorphs), hash,
                    pNpc->faceMorphs ? pNpc->faceMorphs->option[0] : -1.f, pNpc->weight, fmt::ptr(pNpc->headData),
                    fmt::ptr(pNpc->headparts), parts, pNpc->faceNPC ? pNpc->faceNPC->formID : 0,
                    pNpc->raceForm.race ? pNpc->raceForm.race->formID : 0,
                    EscapeJson(pNpc->fullName.value.data ? pNpc->fullName.value.AsAscii() : ""));
            };
            // Every lighting material on a character's 3D (feature 4 face tint, 5 skin tint): shared materials between
            // characters carry one player's creator tint writes onto the other.
            const auto materials = [](Actor* apActor, std::map<void*, int>& aFeatures)
            {
                std::function<void(NiAVObject*, int)> walk = [&](NiAVObject* apNode, int aDepth)
                {
                    if (!apNode || aDepth > 24)
                        return;
                    if (auto* pGeom = apNode->CastToNiTriBasedGeom())
                    {
                        auto* pProperty = reinterpret_cast<BSShaderProperty*>(pGeom->effect.object);
                        if (pProperty && pProperty->material)
                        {
                            const auto getFeature = reinterpret_cast<uint32_t (*)(void*)>((*reinterpret_cast<void***>(pProperty->material))[6]);
                            aFeatures[pProperty->material] = static_cast<int>(getFeature(pProperty->material));
                        }
                    }
                    if (auto* pNode = apNode->AsNode())
                        for (uint16_t i = 0; i < pNode->children.length; ++i)
                            walk(pNode->children.data[i], aDepth + 1);
                };
                if (apActor)
                    walk(apActor->GetNiNode(), 0);
            };
            std::map<void*, int> selfMaterials;
            materials(PlayerCharacter::Get(), selfMaterials);
            const auto skinColors = [](const std::map<void*, int>& aMaterials)
            {
                std::string list;
                for (const auto& [material, feature] : aMaterials)
                    if (feature == 5)
                    {
                        const auto* color = reinterpret_cast<const float*>(static_cast<const uint8_t*>(material) + 0xA0);
                        list += fmt::format("{}\"{:.3f},{:.3f},{:.3f}\"", list.empty() ? "" : ",", color[0], color[1], color[2]);
                    }
                return "[" + list + "]";
            };
            std::string otherSkin = "[]";
            uint64_t tintHash = 1469598103934665603ull;
            uint32_t firstTint = 0;
            if (auto* pSelf = PlayerCharacter::Get())
            {
                const auto& tints = pSelf->GetTints();
                for (uint32_t i = 0; i < tints.length; ++i)
                    if (tints[i])
                    {
                        const uint64_t values[] = {tints[i]->color, static_cast<uint64_t>(tints[i]->alpha * 1000.f), tints[i]->type};
                        for (const auto value : values)
                            tintHash = (tintHash ^ value) * 1099511628211ull;
                        if (i == 0)
                            firstTint = tints[i]->color;
                    }
            }
            // Face tint texture and its pixel buffer (NiRenderedTexture::buffer): the local player's face renders into
            // the global render target 0xF (52396 / 14096BE70); a copy whose texture reads that buffer shows this
            // player's creator tints.
            const auto faceTexturesOf = [](const std::map<void*, int>& aMaterials)
            {
                std::string list;
                for (const auto& [material, feature] : aMaterials)
                    if (feature == 4)
                    {
                        auto* pTexture = static_cast<NiRenderedTexture*>(static_cast<BSMaskedShaderMaterial*>(material)->renderedTexture.object);
                        // texture/buffer/pixel hash (the pixels, read back from the GPU: what is actually drawn).
                        list += fmt::format("{}\"{}/{}/{:X}\"", list.empty() ? "" : ",", fmt::ptr(pTexture),
                            fmt::ptr(pTexture ? static_cast<void*>(pTexture->buffer) : nullptr),
                            FaceGenSystem::HashTintTexture(material));
                    }
                return list;
            };
            const std::string faceTextures = faceTexturesOf(selfMaterials);
            std::string otherFace;
            std::string others = "[";
            int count = 0;
            uint32_t shared = 0;
            std::string sharedList;
            auto view = m_world.view<FormIdComponent, PlayerComponent>();
            for (auto entity : view)
            {
                auto* pActor = Cast<Actor>(TESForm::GetById(view.get<FormIdComponent>(entity).Id));
                if (!pActor || pActor == PlayerCharacter::Get())
                    continue;
                others += (count++ ? "," : "") + describe(pActor);
                std::map<void*, int> theirs;
                materials(pActor, theirs);
                otherSkin = skinColors(theirs);
                otherFace = faceTexturesOf(theirs);
                for (const auto& [material, feature] : theirs)
                    if (selfMaterials.count(material))
                    {
                        ++shared;
                        sharedList += fmt::format("{}\"{}:{}\"", sharedList.empty() ? "" : ",", fmt::ptr(material), feature);
                    }
            }
            std::string selfList;
            for (const auto& [material, feature] : selfMaterials)
                if (feature == 4 || feature == 5)
                    selfList += fmt::format("{}\"{}:{}\"", selfList.empty() ? "" : ",", fmt::ptr(material), feature);
            return Result(id, fmt::format("\"self\":{},\"others\":{}],\"selfTintHash\":\"{:X}\",\"selfFirstTint\":\"{:06X}\",\"selfFaceTexture\":[{}],\"otherFaceTexture\":[{}],\"selfSkin\":{},\"otherSkin\":{},\"selfTintMaterials\":[{}],\"sharedMaterials\":{},\"shared\":[{}],\"creatorLeaks\":{},\"appliedLooks\":{},\"partyNames\":[{}]",
                describe(PlayerCharacter::Get()), others, tintHash, firstTint, faceTextures, otherFace, skinColors(selfMaterials), otherSkin,
                selfList, shared, sharedList, CreatorTogether::LeakedFrames(), CreatorTogether::AppliedLooks(),
                [this]() {
                    std::string names;
                    for (const auto& [playerId, name] : m_world.GetPartyService().GetPlayers())
                        names += fmt::format("{}\"{}:{}\"", names.empty() ? "" : ",", playerId, EscapeJson(name.c_str()));
                    return names;
                }()));
        }
        if (command == "checkpoint_now")
        {
            // A named test checkpoint on this PC: queues the engine save Papyrus Game.RequestSave uses (the
            // checkpoint service's own path) and copies it to SSC_<name>.ess once written. Run on every PC at the same
            // moment for a matched set. Saving must be allowed (see set_in_chargen).
            const auto name = GetJsonString(acLine, "name");
            if (name.empty() || name.size() > 48 ||
                !std::all_of(name.begin(), name.end(), [](char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; }))
                return Error(id, "name must be 1-48 letters, digits or underscores");
            World::Get().GetRunner().Queue([name]() { CheckpointSaves::Begin(String(name.c_str())); });
            return Result(id, fmt::format("\"queued\":\"{}\"", name));
        }
        if (command == "set_in_chargen")
        {
            // Game.SetInChargen (55576): the intro disables saving and waiting until the creator is done. Test
            // checkpoints inside the intro clear it for one save, then restore it. Runs on the game thread.
            const bool disableSaving = GetJsonString(acLine, "saving") == "false";
            const bool disableWaiting = GetJsonString(acLine, "waiting") == "false";
            World::Get().GetRunner().Queue([disableSaving, disableWaiting]() {
                using TSetInChargen = void(void*, uint32_t, void*, bool, bool, bool);
                POINTER_SKYRIMSE(TSetInChargen, setInChargen, 55576);
                if (auto* pVM = GameVM::Get() ? GameVM::Get()->virtualMachine : nullptr)
                {
                    setInChargen.Get()(pVM, 0, nullptr, disableSaving, disableWaiting, false);
                    spdlog::info("Test set_in_chargen: saving disabled {}, waiting disabled {}", disableSaving, disableWaiting);
                }
            });
            return Result(id, fmt::format("\"disableSaving\":{},\"disableWaiting\":{}", JsonBool(disableSaving),
                JsonBool(disableWaiting)));
        }
        if (command == "creator_poke")
        {
            // Write to this player's own face morph storage and weight, as a creator slider does, to see which
            // characters change with it (shared storage = slider spill).
            auto* pPlayer = PlayerCharacter::Get();
            auto* pNpc = pPlayer ? Cast<TESNPC>(pPlayer->baseForm) : nullptr;
            if (!pNpc)
                return Error(id, "no player base");
            // Every slider-backed value (owner: "mess with every slider"): all face morphs and presets, the weight,
            // and every tint layer's color and strength (skin tone, warpaint, dirt...). "only" limits it to one group:
            // morphs, presets, weight, tints.
            const auto only = GetJsonString(acLine, "only");
            if (pNpc->faceMorphs && (only.empty() || only == "morphs"))
                for (auto& option : pNpc->faceMorphs->option)
                    option = option > 0.f ? -0.8f : 0.8f;
            if (pNpc->faceMorphs && (only.empty() || only == "presets"))
                for (auto& preset : pNpc->faceMorphs->presets)
                    preset = (preset + 1) % 4;
            if (only.empty() || only == "weight")
                pNpc->weight = pNpc->weight >= 50.f ? 10.f : 90.f;
            if (only.empty() || only == "tints")
            {
                // Only the layers already shown, as the creator's sliders do (switching every layer on overflowed the
                // native 16-layer tint pass).
                const auto& tints = pPlayer->GetTints();
                for (uint32_t i = 0; i < tints.length; ++i)
                    if (tints[i] && tints[i]->alpha > 0.f)
                    {
                        tints[i]->color = tints[i]->color == 0x0000FF ? 0x00FF00 : 0x0000FF;
                        tints[i]->alpha = tints[i]->alpha > 0.5f ? 0.3f : 0.9f;
                    }
            }
            return Result(id, fmt::format("\"faceMorphs\":\"{}\",\"morph0\":{:.3f},\"weight\":{:.1f}", fmt::ptr(pNpc->faceMorphs),
                pNpc->faceMorphs ? pNpc->faceMorphs->option[0] : -1.f, pNpc->weight));
        }
        if (command == "creator_slide")
        {
            // Drives the RaceSex menu's own slider callbacks (registered in 140968C50), exactly as moving a slider
            // does, so the native apply code runs (skin tone, face tint texture, weight rebuild). Without "index" it
            // lists the sliders: slider array = menu+0x140[menu+0x198] entry (0x28) at menu+0x188, data +8, size +0x18;
            // each slider is 0x138: min +0, max +4, callback name +0x20, tint type +0x124, value +0x130. With "index"
            // it queues that slider's callback on the game thread with a new value (or "value").
            auto* pUI = UI::Get();
            auto* pMenu = pUI && pUI->GetMenuOpen(BSFixedString("RaceSex Menu"))
                ? reinterpret_cast<uint8_t*>(pUI->FindMenuByName(BSFixedString("RaceSex Menu"))) : nullptr;
            if (!pMenu)
                return Error(id, "RaceSex Menu is not open");
            const auto sliderArray = [](uint8_t* apMenu) -> std::pair<uint8_t*, uint32_t> {
                auto* pLists = *reinterpret_cast<uint8_t**>(apMenu + 0x140 + *reinterpret_cast<uint32_t*>(apMenu + 0x198) * 0x18);
                if (!pLists)
                    return {nullptr, 0};
                auto* pEntry = pLists + *reinterpret_cast<uint32_t*>(apMenu + 0x188) * 0x28;
                return {*reinterpret_cast<uint8_t**>(pEntry + 8), *reinterpret_cast<uint32_t*>(pEntry + 0x18)};
            };
            // "callback" with "a0"/"a1": call ChangeRace (52351, a0 = race index), ChangeSex (52352, a0 = 0/1) or
            // ChangeHeadPreset (52359, a0 value, a1 slider) directly; these rebuild the whole character.
            const auto callbackText = GetJsonString(acLine, "callback");
            if (!callbackText.empty())
            {
                static const std::unordered_map<std::string, uint32_t> s_direct{
                    {"ChangeRace", 52351}, {"ChangeSex", 52352}, {"ChangeHeadPreset", 52359}};
                const auto direct = s_direct.find(callbackText);
                if (direct == s_direct.end())
                    return Error(id, "callback must be ChangeRace, ChangeSex or ChangeHeadPreset");
                const auto a0Text = GetJsonString(acLine, "a0");
                const auto a1Text = GetJsonString(acLine, "a1");
                const double a0 = a0Text.empty() ? 0.0 : std::stod(a0Text);
                const double a1 = a1Text.empty() ? 0.0 : std::stod(a1Text);
                const uint32_t handlerId = direct->second;
                QueueCreatorCall([handlerId, a0, a1, callbackText]() {
                    auto* pUI = UI::Get();
                    auto* pMenu = pUI && pUI->GetMenuOpen(BSFixedString("RaceSex Menu"))
                        ? pUI->FindMenuByName(BSFixedString("RaceSex Menu")) : nullptr;
                    if (!pMenu)
                        return;
                    struct FakeValue { uint64_t objectInterface; uint32_t type; uint32_t pad; double number; };
                    struct FakeArgs { uint8_t responseId[0x18]; void* pHandler; void* pMovie; FakeValue* pArgs; uint32_t count; };
                    FakeValue values[2]{{0, 5, 0, a0}, {0, 5, 0, a1}};
                    FakeArgs args{};
                    args.pHandler = pMenu;
                    args.pArgs = values;
                    args.count = 2;
                    using THandler = void (*)(FakeArgs*);
                    VersionDbPtr<void> pHandler(handlerId);
                    spdlog::info("Test creator_slide: {}({}, {})", callbackText, a0, a1);
                    reinterpret_cast<THandler>(pHandler.GetPtr())(&args);
                });
                return Result(id, fmt::format("\"queued\":\"{}\"", callbackText));
            }
            const auto [pSliders, count] = sliderArray(pMenu);
            if (!pSliders)
                return Error(id, "no slider array");
            const auto indexText = GetJsonString(acLine, "index");
            if (indexText.empty())
            {
                std::string list = "[";
                for (uint32_t i = 0; i < count && i < 128; ++i)
                {
                    auto* pSlider = pSliders + i * 0x138;
                    char name[64]{};
                    strncpy_s(name, reinterpret_cast<const char*>(pSlider + 0x20), _TRUNCATE);
                    list += fmt::format("{}{{\"i\":{},\"cb\":\"{}\",\"min\":{:.2f},\"max\":{:.2f},\"tint\":{},\"value\":{:.2f}}}",
                        i ? "," : "", i, name, *reinterpret_cast<float*>(pSlider), *reinterpret_cast<float*>(pSlider + 4),
                        *reinterpret_cast<int32_t*>(pSlider + 0x124), *reinterpret_cast<float*>(pSlider + 0x130));
                }
                return Result(id, fmt::format("\"count\":{},\"sliders\":{}]", count, list));
            }
            const uint32_t index = std::stoul(indexText);
            if (index >= count)
                return Error(id, "index out of range");
            const auto valueText = GetJsonString(acLine, "value");
            QueueCreatorCall([index, valueText]() {
                auto* pUI = UI::Get();
                auto* pMenu = pUI && pUI->GetMenuOpen(BSFixedString("RaceSex Menu"))
                    ? reinterpret_cast<uint8_t*>(pUI->FindMenuByName(BSFixedString("RaceSex Menu"))) : nullptr;
                if (!pMenu)
                    return;
                auto* pLists = *reinterpret_cast<uint8_t**>(pMenu + 0x140 + *reinterpret_cast<uint32_t*>(pMenu + 0x198) * 0x18);
                auto* pEntry = pLists + *reinterpret_cast<uint32_t*>(pMenu + 0x188) * 0x28;
                if (index >= *reinterpret_cast<uint32_t*>(pEntry + 0x18))
                    return;
                auto* pSlider = *reinterpret_cast<uint8_t**>(pEntry + 8) + index * 0x138;
                const std::string callback(reinterpret_cast<const char*>(pSlider + 0x20));
                static const std::unordered_map<std::string, uint32_t> s_handlers{
                    {"ChangeWeight", 52353}, {"ChangeFaceDetails", 52354}, {"ChangeMorph", 52355}, {"ChangeDoubleMorph", 52356},
                    {"ChangeHeadPart", 52357}, {"ChangePreset", 52358}, {"ChangeHeadPreset", 52359},
                    {"ChangeHairColorPreset", 52360}, {"ChangeTintingMask", 52361}, {"ChangeMask", 52362}, {"ChangeMaskColor", 52363}};
                const auto handler = s_handlers.find(callback);
                if (handler == s_handlers.end())
                {
                    spdlog::info("Test creator_slide: slider {} callback '{}' not driven", index, callback);
                    return;
                }
                const float minimum = *reinterpret_cast<float*>(pSlider);
                const float maximum = *reinterpret_cast<float*>(pSlider + 4);
                const float current = *reinterpret_cast<float*>(pSlider + 0x130);
                float value = valueText.empty() ? 0.f : std::stof(valueText);
                if (valueText.empty())
                {
                    // Whole steps for index sliders (presets, tint colors, masks), a far point for continuous ones.
                    value = current + 1.f <= maximum ? std::floor(current) + 1.f : minimum;
                    if (callback == "ChangeWeight" || callback == "ChangeMorph" || callback == "ChangeDoubleMorph")
                        value = current > (minimum + maximum) / 2.f ? minimum : maximum;
                }
                // Scaleform FxDelegateArgs: handler +0x18, GFxValue args +0x28 (0x18 each, number at +0x10), count +0x30.
                struct FakeValue { uint64_t objectInterface; uint32_t type; uint32_t pad; double number; };
                struct FakeArgs { uint8_t responseId[0x18]; void* pHandler; void* pMovie; FakeValue* pArgs; uint32_t count; };
                FakeValue values[2]{{0, 5, 0, value}, {0, 5, 0, static_cast<double>(index)}};
                FakeArgs args{};
                args.pHandler = pMenu;
                args.pArgs = values;
                args.count = 2;
                using THandler = void(FakeArgs*);
                VersionDbPtr<THandler> pHandler(handler->second);
                spdlog::info("Test creator_slide: slider {} {} tint {} {:.2f} -> {:.2f}", index, callback,
                    *reinterpret_cast<int32_t*>(pSlider + 0x124), current, value);
                pHandler.Get()(&args);
            });
            return Result(id, fmt::format("\"queued\":{}", index));
        }
        if (command == "facegen_restore")
        {
            // Control runs only: "false" stops redrawing the local player's tints after a copy's tint job.
            FaceGenSystem::RestoreLocalTints.store(GetJsonString(acLine, "enabled") != "false");
            return Result(id, fmt::format("\"restore\":{}", JsonBool(FaceGenSystem::RestoreLocalTints.load())));
        }
        if (command == "papyrus_has")
        {
            // names: "Type.Function,..." -> which are missing from PapyrusService's runtime table (a name-bound call to
            // a missing one returns a default).
            const auto names = GetJsonString(acLine, "names");
            const auto& service = World::Get().ctx().at<PapyrusService>();
            std::string missing;
            size_t start = 0;
            while (start < names.size())
            {
                auto end = names.find(',', start);
                if (end == std::string::npos)
                    end = names.size();
                const auto entry = names.substr(start, end - start);
                const auto dot = entry.find('.');
                if (dot != std::string::npos &&
                    !service.Get(String(entry.substr(0, dot).c_str()), String(entry.substr(dot + 1).c_str())))
                    missing += fmt::format("{}\"{}\"", missing.empty() ? "" : ",", entry);
                start = end + 1;
            }
            return Result(id, fmt::format("\"registered\":{},\"missing\":[{}]", service.RegisteredCount(), missing));
        }
        if (command == "remote_entities")
        {
            // Every remote character entity: server id, cached form, whether that form exists and is flagged a
            // remote player, waiting for 3D, the interpolated position, and player id if any.
            std::string list = "[";
            int count = 0;
            auto view = m_world.view<RemoteComponent>();
            for (auto entity : view)
            {
                const auto& remote = view.get<RemoteComponent>(entity);
                auto* pActor = Cast<Actor>(TESForm::GetById(remote.CachedRefId));
                const auto* pInterp = m_world.try_get<InterpolationComponent>(entity);
                const auto* pPlayer = m_world.try_get<PlayerComponent>(entity);
                list += fmt::format("{}{{\"server\":\"{:X}\",\"form\":\"{:X}\",\"exists\":{},\"remotePlayer\":{},\"formComponent\":{},"
                    "\"waiting3D\":{},\"player\":{},\"pos\":[{:.0f},{:.0f},{:.0f}]}}",
                    count++ ? "," : "", remote.Id, remote.CachedRefId, JsonBool(pActor != nullptr),
                    JsonBool(pActor && pActor->GetExtension() && pActor->GetExtension()->IsRemotePlayer()),
                    JsonBool(m_world.all_of<FormIdComponent>(entity)), JsonBool(m_world.all_of<WaitingFor3D>(entity)),
                    pPlayer ? static_cast<int64_t>(pPlayer->Id) : -1,
                    pInterp ? pInterp->Position.x : 0.f, pInterp ? pInterp->Position.y : 0.f, pInterp ? pInterp->Position.z : 0.f);
            }
            return Result(id, fmt::format("\"count\":{},\"entities\":{}]", count, list));
        }
        if (command == "ritual_hold")
        {
            ReviveService::SetTestShout(GetJsonString(acLine, "enabled") != "false");
            return Result(id, fmt::format("\"held\":{}", JsonBool(GetJsonString(acLine, "enabled") != "false")));
        }
        if (command == "revive_state")
            return Result(id, fmt::format("\"revive\":{},\"unresolvedPapyrus\":{},\"papyrusRegistered\":{}", ReviveService::DescribeTest(), g_unresolvedPapyrusNatives.load(), World::Get().ctx().at<PapyrusService>().RegisteredCount()));
        if (command == "revive_bleed")
        {
            const auto valueText = GetJsonString(acLine, "value");
            const float value = valueText.empty() ? 0.5f : std::stof(valueText);
            ReviveService::SetTestBleed(value);
            return Result(id, fmt::format("\"bleed\":{}", value));
        }
        if (command == "death_drop_now")
        {
            // The death path's weapon drop (0x140677C90 / 37320, through our hook) on an actor, as a death would call it.
            const auto formText = GetJsonString(acLine, "form_id");
            const uint32_t formId = formText.empty() ? 0 : std::stoul(formText, nullptr, 16);
            QueueCreatorCall([formId]() {
                auto* pActor = Cast<Actor>(TESForm::GetById(formId));
                if (!pActor)
                    return;
                using TDeathDrop = void(Actor*);
                POINTER_SKYRIMSE(TDeathDrop, deathDrop, 37320);
                deathDrop.Get()(pActor);
                spdlog::info("Test death_drop_now: {:X}", formId);
            });
            return Result(id, fmt::format("\"queued\":\"{:X}\"", formId));
        }
        if (command == "death_drop_chance")
        {
            // iDeathDropWeaponChance (GameSetting 374997, int value 374998): percent chance a dying NPC drops its weapon.
            POINTER_SKYRIMSE(int32_t, chance, 374998);
            const auto valueText = GetJsonString(acLine, "value");
            if (!valueText.empty())
                *chance.Get() = std::stoi(valueText);
            return Result(id, fmt::format("\"chance\":{}", *chance.Get()));
        }
        if (command == "give_weapon")
        {
            // Arm an actor (form_id) with a weapon (base, default Iron Sword 12EB7) and equip it, on the game thread.
            const auto formText = GetJsonString(acLine, "form_id");
            const auto baseText = GetJsonString(acLine, "base");
            const uint32_t formId = formText.empty() ? 0 : std::stoul(formText, nullptr, 16);
            const uint32_t baseId = baseText.empty() ? 0x12EB7 : std::stoul(baseText, nullptr, 16);
            QueueCreatorCall([formId, baseId]() {
                auto* pActor = Cast<Actor>(TESForm::GetById(formId));
                auto* pWeapon = TESForm::GetById(baseId);
                if (!pActor || !pWeapon)
                    return;
                using ObjectReference = TESObjectREFR;
                PAPYRUS_FUNCTION(void, ObjectReference, AddItem, TESForm*, int32_t, bool);
                s_pAddItem(pActor, pWeapon, 1, true);
                EquipManager::Get()->Equip(pActor, pWeapon, nullptr, 1, nullptr, false, true, false, true);
                spdlog::info("Test give_weapon: {:X} armed with {:X}", formId, baseId);
            });
            return Result(id, fmt::format("\"queued\":\"{:X}\"", formId));
        }
        if (command == "inventory")
        {
            // Every item a reference (form_id) holds with its count and worn flags: the corpse test compares a body's
            // contents across PCs (session 2026-09-29: a corpse's armor showed twice on the follower).
            const auto formText = GetJsonString(acLine, "form_id");
            auto* pReference = formText.empty() ? nullptr : Cast<TESObjectREFR>(TESForm::GetById(std::stoul(formText, nullptr, 16)));
            if (!pReference)
                return Result(id, "\"found\":false");
            auto& mods = World::Get().GetModSystem();
            std::string items;
            for (const auto& entry : pReference->GetInventory().Entries)
            {
                if (entry.Count == 0)
                    continue;
                items += fmt::format("{}{{\"base\":\"{:X}\",\"count\":{},\"worn\":{},\"wornLeft\":{}}}", items.empty() ? "" : ",",
                    mods.GetGameId(entry.BaseId), entry.Count, JsonBool(entry.ExtraWorn), JsonBool(entry.ExtraWornLeft));
            }
            return Result(id, fmt::format("\"found\":true,\"items\":[{}]", items));
        }
        if (command == "crosshair")
        {
            // What the crosshair is on (CrosshairPickData, 401585: target handle at +4), as the activate prompt and
            // the hold-to-grab see it.
            POINTER_SKYRIMSE(uint8_t*, pickData, 401585);
            auto* pData = *pickData.Get();
            const uint32_t handle = pData ? *reinterpret_cast<uint32_t*>(pData + 4) : 0;
            auto* pTarget = handle ? TESObjectREFR::GetByHandle(handle) : nullptr;
            auto* pActor = Cast<Actor>(pTarget);
            return Result(id, fmt::format("\"target\":\"{:X}\",\"base\":\"{:X}\",\"actor\":{},\"dead\":{},\"lifeState\":{}",
                pTarget ? pTarget->formID : 0, pTarget && pTarget->baseForm ? pTarget->baseForm->formID : 0,
                JsonBool(pActor != nullptr), JsonBool(pActor && pActor->IsDead()),
                pActor ? static_cast<int>((pActor->actorState.flags1 >> 21) & 0xF) : -1));
        }
        if (command == "ref_pose")
        {
            // A loose object's reference position, its drawn 3D position, and whether the host's physics stream drives
            // it here (followers apply the host's poses; the host has none).
            const auto form = GetJsonString(acLine, "form_id");
            auto* pRef = form.empty() ? nullptr : Cast<TESObjectREFR>(TESForm::GetById(std::stoul(form, nullptr, 16)));
            if (!pRef)
                return Error(id, "reference not found");
            NiAVObject* pRoot = pRef->GetNiNode();
            // A body: its pelvis bone (the ragdoll moves the bones, not the reference).
            static BSFixedString s_pelvis("NPC Pelvis [Pelv]");
            if (pRoot && Cast<Actor>(pRef))
                if (auto* pBone = static_cast<NiNode*>(pRoot)->GetByName(s_pelvis))
                    pRoot = pBone;
            ObjectService::RemotePhysicsDiagnostic authority{};
            const bool driven = m_world.ctx().at<ObjectService>().GetRemotePhysicsDiagnostic(pRef->formID, authority);
            return Result(id, fmt::format("\"position\":[{:.1f},{:.1f},{:.1f}],\"node\":[{:.1f},{:.1f},{:.1f}],"
                "\"hostDriven\":{},\"hostPosition\":[{:.1f},{:.1f},{:.1f}],\"hostAgeMs\":{},\"bodyDriven\":{},\"lease\":{}",
                pRef->position.x, pRef->position.y, pRef->position.z, pRoot ? pRoot->world.translate.x : 0.f,
                pRoot ? pRoot->world.translate.y : 0.f, pRoot ? pRoot->world.translate.z : 0.f, JsonBool(driven),
                authority.Position.x, authority.Position.y, authority.Position.z, authority.AgeMs,
                JsonBool(authority.BodyDriven), ObjectService::LeaseHolder(pRef->formID)));
        }
        if (command == "party_load_cell")
        {
            // Leader only: move the whole party into a cell (e.g. 32AE7 QASmoke, B1783 CTest) through the door barrier:
            // Go at a shared tick, each PC loads it (CenterOnCell on the main thread), the world gate holds until all
            // have. Never a per-player teleport into another cell.
            const auto cellText = GetJsonString(acLine, "cell");
            if (cellText.empty())
                return Error(id, "cell missing");
            // "name": the cell's editor id (QASmoke, CTest, HelgenExterior07); each PC loads it as the console's coc does.
            const auto nameText = GetJsonString(acLine, "name");
            const char* error = m_world.GetDoorVoteService().RequestTestCell(std::stoul(cellText, nullptr, 16), nameText.c_str());
            if (*error)
                return Error(id, error);
            return Result(id, fmt::format("\"requested\":\"{}\"", EscapeJson(cellText)));
        }
        if (command == "copy_native_tracking")
        {
            const auto enabled = GetJsonString(acLine, "enabled");
            if (!enabled.empty())
                g_copyNativeTracking.store(enabled != "false");
            return Result(id, fmt::format("\"enabled\":{}", JsonBool(g_copyNativeTracking.load())));
        }
        if (command == "actor_bones")
        {
            // Local rotations of named bones (default: head and beast tail) and the head-tracking inputs of an actor
            // (form_id). Session 2026-09-29: in Helgen Keep a player copy's tail stayed straight out and its head
            // stopped tracking; sampling twice shows whether the graph still moves those bones.
            const auto formText = GetJsonString(acLine, "form_id");
            auto* pActor = formText.empty() ? nullptr : Cast<Actor>(TESForm::GetById(std::stoul(formText, nullptr, 16)));
            if (!pActor)
                return Error(id, "actor not found");
            // The drawn third-person skeleton (19735); GetNiNode gives the first-person one in first person.
            using TGet3D = NiAVObject*(TESObjectREFR*);
            POINTER_SKYRIMSE(TGet3D, get3D, 19735);
            auto* pRoot = static_cast<NiNode*>(get3D.Get()(pActor));
            std::vector<std::string> names;
            if (const auto list = GetJsonString(acLine, "nodes"); !list.empty())
            {
                std::stringstream stream(list);
                for (std::string name; std::getline(stream, name, '|');)
                    names.push_back(name);
            }
            else
                names = {"NPC Head [Head]", "NPC Neck [Neck]", "TailBone0", "TailBone1", "TailBone2", "TailBone3", "TailBone4"};
            std::string bones;
            if (const auto match = GetJsonString(acLine, "match"); !match.empty() && pRoot)
            {
                // Every node whose name contains "match" (case-insensitive); NiObjectNET::name is the BSFixedString at +0x10.
                std::string needle = match;
                std::transform(needle.begin(), needle.end(), needle.begin(), [](unsigned char c) { return char(std::tolower(c)); });
                int visited = 0;
                auto walk = [&](auto& self, NiAVObject* apNode, int aDepth) -> void {
                    if (!apNode || aDepth > 64 || ++visited > 2048)
                        return;
                    const char* pName = *reinterpret_cast<const char* const*>(reinterpret_cast<const uint8_t*>(apNode) + 0x10);
                    std::string lower = pName ? pName : "";
                    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return char(std::tolower(c)); });
                    if (!lower.empty() && lower.find(needle) != std::string::npos)
                        names.push_back(pName);
                    if (auto* pNode = apNode->AsNode(); pNode && pNode->children.data)
                        for (uint16_t i = 0; i < pNode->children.length; ++i)
                            self(self, pNode->children.data[i], aDepth + 1);
                };
                names.clear();
                walk(walk, pRoot, 0);
            }
            for (const auto& name : names)
            {
                BSFixedString fixed(name.c_str());
                auto* pNode = pRoot ? pRoot->GetByName(fixed) : nullptr;
                if (!pNode)
                    continue;
                const auto& r = pNode->local.rotate.entry;
                // NiObjectNET::controller (+0x18) and the parent: a tail is not in the behavior graph's bones, so
                // whatever animates it hangs off the node or its parent. Vtable RVAs map through graph/vtables.tsv.
                const auto base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
                const auto rva = [base](const void* apObject) -> uintptr_t {
                    const auto vtable = apObject ? *static_cast<const uintptr_t*>(apObject) : 0;
                    return vtable > base ? vtable - base : 0;
                };
                const auto* pController = *reinterpret_cast<void* const*>(reinterpret_cast<const uint8_t*>(pNode) + 0x18);
                bones += fmt::format("{}{{\"name\":\"{}\",\"local\":[{:.3f},{:.3f},{:.3f},{:.3f},{:.3f},{:.3f},{:.3f},{:.3f},{:.3f}],"
                    "\"node\":\"{:X}\",\"controller\":\"{:X}\",\"parent\":\"{:X}\",\"flags\":\"{:X}\",\"address\":\"{:X}\"}}",
                    bones.empty() ? "" : ",", name, r[0][0], r[0][1], r[0][2], r[1][0], r[1][1], r[1][2], r[2][0], r[2][1], r[2][2],
                    rva(pNode), rva(pController), rva(pNode->parent), pNode->flags, reinterpret_cast<uintptr_t>(pNode));
            }
            bool isNpc{}, spine{}, headTracking{};
            BSFixedString npcName("IsNPC"), spineName("bHeadTrackSpine"), trackingName("bHeadTracking");
            const bool haveNpc = pActor->animationGraphHolder.GetVariableBool(&npcName, &isNpc);
            const bool haveSpine = pActor->animationGraphHolder.GetVariableBool(&spineName, &spine);
            const bool haveTracking = pActor->animationGraphHolder.GetVariableBool(&trackingName, &headTracking);
            using TTargetType = uint32_t(AIProcess*);
            POINTER_SKYRIMSE(TTargetType, targetType, 39486);
            const auto type = pActor->currentProcess ? targetType.Get()(pActor->currentProcess) : UINT32_MAX;
            const auto* pExtension = pActor->GetExtension();
            // Biped slot 10 (the beast tail addon) carries its own animation graph: 15674 / 0x14021E080 stores a
            // TailAnimationGraphManagerHolder at biped + 0x70 + slot * 0x78 when the addon's model has a behavior.
            std::string slotGraphs;
            if (const auto* pBiped = static_cast<const uint8_t*>(pActor->actorWeightData))
            {
                const auto moduleBase = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
                for (uint32_t slot = 0; slot < 32; ++slot)
                {
                    const void* pHolder{};
                    uintptr_t vtable{};
                    SIZE_T read{};
                    if (!ReadProcessMemory(GetCurrentProcess(), pBiped + 0x70 + slot * 0x78, &pHolder, sizeof(pHolder), &read) || !pHolder)
                        continue;
                    ReadProcessMemory(GetCurrentProcess(), pHolder, &vtable, sizeof(vtable), &read);
                    slotGraphs += fmt::format("{}{{\"slot\":{},\"holderVtable\":\"{:X}\"}}", slotGraphs.empty() ? "" : ",", slot,
                        vtable > moduleBase ? vtable - moduleBase : 0);
                }
            }
            // Native tracking gate (37361 / 37363): process + 0x137 clear, then *(middleHigh + 0x250) + 0x29c in
            // [0, threshold] (0x14209FF08, or 0x14209FF50 when +0x1F8's +0x128 is 3).
            std::string trackGate = "\"trackGate\":null";
            if (const auto* process = reinterpret_cast<const uint8_t*>(pActor->currentProcess))
            {
                const auto moduleBase = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
                const uint8_t* middle{};
                const uint8_t* data{};
                float value = -1.f;
                SIZE_T got{};
                ReadProcessMemory(GetCurrentProcess(), process + 8, &middle, sizeof(middle), &got);
                if (middle)
                    ReadProcessMemory(GetCurrentProcess(), middle + 0x250, &data, sizeof(data), &got);
                if (data)
                    ReadProcessMemory(GetCurrentProcess(), data + 0x29C, &value, sizeof(value), &got);
                const float threshold = *reinterpret_cast<const float*>(moduleBase + 0x209FF08);
                const float thresholdAlt = *reinterpret_cast<const float*>(moduleBase + 0x209FF50);
                trackGate = fmt::format("\"trackGate\":{{\"culled137\":{},\"middle\":{},\"data250\":{},\"value29C\":{:.1f},\"threshold\":{:.1f},\"thresholdAlt\":{:.1f}}}",
                    process[0x137], JsonBool(middle != nullptr), JsonBool(data != nullptr), value, threshold, thresholdAlt);
            }
            return Result(id, fmt::format("{},\"race\":\"{:X}\",\"remotePlayer\":{},\"slotGraphs\":[{}],\"bones\":[{}],\"isNPC\":{},\"headTrackSpine\":{},"
                "\"bHeadTracking\":{},\"graphRead\":[{},{},{}],\"flags2HeadTrack\":{},\"nativeTargetType\":{},\"flags1\":\"{:X}\",\"flags2\":\"{:X}\","
                "\"weaponDrawn\":{},{}",
                trackGate, pActor->race ? pActor->race->formID : 0, JsonBool(pExtension && pExtension->IsRemotePlayer()), slotGraphs, bones,
                JsonBool(isNpc), JsonBool(spine), JsonBool(headTracking), JsonBool(haveNpc), JsonBool(haveSpine),
                JsonBool(haveTracking), JsonBool((pActor->actorState.flags2 & (1u << 3)) != 0), type,
                pActor->actorState.flags1, pActor->actorState.flags2, JsonBool(pActor->actorState.IsWeaponDrawn()),
                PoseCopyAuthority::DescribeBoneSlots(pActor, GetJsonString(acLine, "slots").empty() ? "TailBone01" : GetJsonString(acLine, "slots"))));
        }
        if (command == "grab_object")
        {
            // The hold-Activate grab without a crosshair: StartGrabObject (40552) checks the crosshair reference's
            // mass and then calls 40555 (player, reference, 1, 1000.0, 0), which attaches the grab spring.
            // "release" ends it (PlayerCharacter::DestroyMouseSprings, 40557).
            const auto formText = GetJsonString(acLine, "form_id");
            const bool release = GetJsonString(acLine, "release") == "true";
            const uint32_t formId = formText.empty() ? 0 : std::stoul(formText, nullptr, 16);
            QueueCreatorCall([formId, release]() {
                auto* pPlayer = PlayerCharacter::Get();
                if (!pPlayer)
                    return;
                if (release)
                {
                    using TRelease = void(PlayerCharacter*);
                    POINTER_SKYRIMSE(TRelease, destroySprings, 40557);
                    destroySprings.Get()(pPlayer);
                    spdlog::info("Test grab_object: released");
                    return;
                }
                auto* pReference = Cast<TESObjectREFR>(TESForm::GetById(formId));
                if (!pReference)
                    return;
                using TGrab = void*(PlayerCharacter*, TESObjectREFR*, uint32_t, float, bool);
                POINTER_SKYRIMSE(TGrab, grab, 40555);
                const auto* pResult = grab.Get()(pPlayer, pReference, 1, 1000.f, false);
                spdlog::info("Test grab_object: {:X} grab spring {}", formId, fmt::ptr(pResult));
            });
            return Result(id, fmt::format("\"queued\":\"{:X}\",\"release\":{}", formId, JsonBool(release)));
        }
        if (command == "start_combat")
        {
            // attacker: a form id (hex); target: a form id or "remote_player" (the other player's copy here).
            const auto attackerText = GetJsonString(acLine, "attacker");
            const auto targetText = GetJsonString(acLine, "target");
            uint32_t target = 0;
            if (targetText == "remote_player")
            {
                auto view = m_world.view<FormIdComponent>();
                for (auto entity : view)
                {
                    auto* pCandidate = Cast<Actor>(TESForm::GetById(view.get<FormIdComponent>(entity).Id));
                    if (pCandidate && pCandidate->GetExtension() && pCandidate->GetExtension()->IsRemotePlayer())
                    {
                        target = pCandidate->formID;
                        break;
                    }
                }
            }
            else if (!targetText.empty())
                target = std::stoul(targetText, nullptr, 16);
            if (attackerText.empty() || !target)
                return Error(id, "attacker or target missing");
            std::lock_guard lock(s_mainFrameDropLock);
            s_mainFrameCombat = {static_cast<uint32_t>(std::stoul(attackerText, nullptr, 16)), target};
            return Result(id, fmt::format("\"queued\":true,\"target\":\"{:X}\"", target));
        }
        if (command == "damage_player")
        {
            const auto amountText = GetJsonString(acLine, "amount");
            std::lock_guard lock(s_mainFrameDropLock);
            s_mainFrameDamage = amountText.empty() ? 1000.f : std::stof(amountText);
            return Result(id, "\"queued\":true");
        }
        if (command == "drop_item")
        {
            const auto baseText = GetJsonString(acLine, "base");
            const auto countText = GetJsonString(acLine, "count");
            auto* pBase = baseText.empty() ? nullptr : Cast<TESBoundObject>(TESForm::GetById(std::stoul(baseText, nullptr, 16)));
            if (!pBase)
                return Error(id, "base form not found");
            std::lock_guard lock(s_mainFrameDropLock);
            s_mainFrameDrop = {pBase->formID, countText.empty() ? 1 : std::stoi(countText)};
            return Result(id, "\"queued\":true");
        }
        // References of a base within radius of a point (default: this PC's player), in the cells around the player.
        if (command == "nearby_refs")
        {
            const auto baseText = GetJsonString(acLine, "base");
            const auto radiusText = GetJsonString(acLine, "radius");
            auto* pPlayer = PlayerCharacter::Get();
            if (baseText.empty() || !pPlayer || !pPlayer->parentCell)
                return Error(id, "base or player missing");
            // "weapons": every weapon lying loose (a dead NPC's dropped weapon is a new temporary reference).
            const bool anyWeapon = baseText == "weapons";
            const uint32_t base = anyWeapon ? 0 : std::stoul(baseText, nullptr, 16);
            const float radius = radiusText.empty() ? 1000.f : std::stof(radiusText);
            NiPoint3 center = pPlayer->position;
            if (!GetJsonString(acLine, "x").empty())
            {
                center.x = std::stof(GetJsonString(acLine, "x"));
                center.y = std::stof(GetJsonString(acLine, "y"));
                center.z = std::stof(GetJsonString(acLine, "z"));
            }
            std::string found = "[";
            int count = 0;
            auto visit = [&](TESObjectCELL* pCell)
            {
                if (!pCell || !pCell->refData.refArray || pCell->refData.capacity > 50000)
                    return;
                for (uint32_t i = 0; i < pCell->refData.capacity; ++i)
                {
                    auto* pRef = pCell->refData.refArray[i].Get();
                    if (!pRef || !pRef->baseForm || pRef->IsDeleted() ||
                        (anyWeapon ? pRef->baseForm->formType != FormType::Weapon : pRef->baseForm->formID != base))
                        continue;
                    const auto d = pRef->position - center;
                    if (d.x * d.x + d.y * d.y + d.z * d.z > radius * radius)
                        continue;
                    found += fmt::format("{}{{\"form_id\":\"{:X}\",\"disabled\":{},\"position\":[{:.1f},{:.1f},{:.1f}]}}",
                        count++ ? "," : "", pRef->formID, JsonBool(pRef->IsDisabled()), pRef->position.x, pRef->position.y,
                        pRef->position.z);
                }
            };
            if (auto* pSpace = pPlayer->GetWorldSpace())
            {
                const int32_t cx = static_cast<int32_t>(std::floor(pPlayer->position.x / 4096.f));
                const int32_t cy = static_cast<int32_t>(std::floor(pPlayer->position.y / 4096.f));
                for (int32_t dx = -1; dx <= 1; ++dx)
                    for (int32_t dy = -1; dy <= 1; ++dy)
                        visit(ModManager::Get()->GetCellFromCoordinates(cx + dx, cy + dy, pSpace, false));
            }
            else
                visit(pPlayer->parentCell);
            return Result(id, fmt::format("\"count\":{},\"refs\":{}]", count, found));
        }
        // Knock an actor away from this PC's player (so it ragdolls), then kill it.
        if (command == "kill_actor")
        {
            const auto form = GetJsonString(acLine, "form_id");
            const auto pushText = GetJsonString(acLine, "push");
            auto* pActor = form.empty() ? nullptr : Cast<Actor>(TESForm::GetById(std::stoul(form, nullptr, 16)));
            auto* pPlayer = PlayerCharacter::Get();
            if (!pActor || !pPlayer || !pActor->currentProcess)
                return Error(id, "actor not found");
            const float push = pushText.empty() ? 20.f : std::stof(pushText);
            // push < 0: no knock first (a knock sheathes the weapon, and the death weapon drop needs it drawn).
            if (push >= 0.f)
                pActor->currentProcess->KnockExplosion(pActor, &pPlayer->position, push);
            pActor->Kill();
            return Result(id, fmt::format("\"push\":{}", push));
        }
        // An actor's state for comparing the PCs: life and knock state, position, ragdoll bodies, worn items.
        if (command == "actor_state")
        {
            const auto form = GetJsonString(acLine, "form_id");
            Actor* pActor = nullptr;
            if (form == "remote_player")
            {
                // The other player's copy on this PC (its form id differs per PC).
                auto view = m_world.view<FormIdComponent>();
                for (auto entity : view)
                {
                    auto* pCandidate = Cast<Actor>(TESForm::GetById(view.get<FormIdComponent>(entity).Id));
                    if (pCandidate && pCandidate->GetExtension() && pCandidate->GetExtension()->IsRemotePlayer())
                    {
                        pActor = pCandidate;
                        break;
                    }
                }
            }
            else if (!form.empty())
                pActor = Cast<Actor>(TESForm::GetById(std::stoul(form, nullptr, 16)));
            // Or the nearest non-player actor within 500 units of x,y,z (temporary actors have
            // different form ids on each PC).
            if (!pActor && !GetJsonString(acLine, "x").empty())
            {
                const float x = std::stof(GetJsonString(acLine, "x")), y = std::stof(GetJsonString(acLine, "y")),
                            z = std::stof(GetJsonString(acLine, "z"));
                float best = 500.f;
                auto view = m_world.view<FormIdComponent>();
                for (auto entity : view)
                {
                    auto* pCandidate = Cast<Actor>(TESForm::GetById(view.get<FormIdComponent>(entity).Id));
                    if (!pCandidate || !pCandidate->GetExtension() || pCandidate->GetExtension()->IsPlayer())
                        continue;
                    const float dx = pCandidate->position.x - x, dy = pCandidate->position.y - y, dz = pCandidate->position.z - z;
                    const float distance = std::sqrt(dx * dx + dy * dy + dz * dz);
                    if (distance < best)
                    {
                        best = distance;
                        pActor = pCandidate;
                    }
                }
            }
            if (!pActor)
                return Error(id, "actor not found");
            const uint32_t flags1 = pActor->actorState.flags1;
            std::string worn = "[";
            std::string items = "{";
            int wornCount = 0, itemCount = 0;
            for (const auto& entry : pActor->GetActorInventory().Entries)
            {
                // Every entry with its count (corpse loot comparisons between PCs).
                items += fmt::format("{}\"{:X}:{:X}\":{}", itemCount++ ? "," : "", entry.BaseId.ModId, entry.BaseId.BaseId,
                    entry.Count);
                if (!entry.IsWorn())
                    continue;
                worn += fmt::format("{}\"{:X}:{:X}\"", wornCount++ ? "," : "", entry.BaseId.ModId, entry.BaseId.BaseId);
            }
            worn += "]";
            items += "}";
            // Graph update calls for this actor's holder (a copy updated twice per frame animates at double speed).
            AnimationGraphUpdateTrace::WatchHolder(&pActor->animationGraphHolder, pActor->formID);
            const auto graph = AnimationGraphUpdateTrace::GetHolderSample(&pActor->animationGraphHolder);
            const auto* pRoot = pActor->GetNiNode();
            return Result(id, fmt::format("\"form_id\":\"{:X}\",\"dead\":{},\"lifeState\":{},\"knockState\":{},\"position\":[{:.1f},{:.1f},{:.1f}],"
                "\"has3D\":{},\"bodies\":{},\"worn\":{},\"visual\":{},\"health\":{:.1f},\"inCombat\":{},\"combatTarget\":\"{:X}\","
                "\"remote\":{},\"items\":{},\"graphCalls\":{},\"graphLastMs\":{},\"nowMs\":{},\"hidden\":{},\"magicka\":{:.1f},\"weaponState\":{},\"rightHand\":\"{:X}\",\"look\":{}", pActor->formID, JsonBool(pActor->IsDead()), (flags1 >> 21) & 0xF, (flags1 >> 25) & 0x7,
                pActor->position.x, pActor->position.y, pActor->position.z, JsonBool(pRoot != nullptr),
                CorpseRagdollService::DescribeRagdollBodies(pActor), worn, DescribeActorVisuals(pActor),
                pActor->GetActorValue(ActorValueInfo::kHealth), JsonBool(pActor->IsInCombat()),
                pActor->GetCombatTarget() ? pActor->GetCombatTarget()->formID : 0,
                JsonBool(pActor->GetExtension() && pActor->GetExtension()->IsRemote()), items, graph.Calls, graph.LastPostCallMs,
                GetTickCount64(), JsonBool(pRoot && (pRoot->flags & 1u) != 0),
                pActor->GetActorValue(ActorValueInfo::kMagicka), (pActor->actorState.flags2 >> 5) & 7,
                pActor->GetEquippedWeapon(1) ? pActor->GetEquippedWeapon(1)->formID : 0, HeadTrackService::DescribeLook(pActor)));
        }
        if (command == "world_snapshot")
        {
            // Every non-actor reference within "radius" (default 2500) of a point ("x","y","z"; default the player) in
            // the 3x3 loaded exterior cells, or the player's interior cell: [form, disabled, 3D loaded, collision bodies
            // in the physics world]. Diffed step by step on both PCs while a quest plays (Helgen inn, 2026-09-30).
            auto* pPlayer = PlayerCharacter::Get();
            if (!pPlayer || !pPlayer->parentCell)
                return Error(id, "player missing");
            const auto radiusText = GetJsonString(acLine, "radius");
            const float radius = radiusText.empty() ? 2500.f : std::stof(radiusText);
            NiPoint3 center = pPlayer->position;
            if (!GetJsonString(acLine, "x").empty())
            {
                center.x = std::stof(GetJsonString(acLine, "x"));
                center.y = std::stof(GetJsonString(acLine, "y"));
                center.z = std::stof(GetJsonString(acLine, "z"));
            }
            const auto bodiesInWorld = [](NiAVObject* apRoot) {
                int inWorld = 0, visited = 0;
                std::function<void(NiAVObject*, int)> walk = [&](NiAVObject* apNode, int aDepth) {
                    if (!apNode || aDepth > 8 || ++visited > 256)
                        return;
                    if (apNode->collisionObject)
                    {
                        void* pWrapper{};
                        void* pBody{};
                        void* pPhysicsWorld{};
                        if (ReadNative(reinterpret_cast<const uint8_t*>(apNode->collisionObject) + 0x20, pWrapper) && pWrapper &&
                            ReadNative(reinterpret_cast<const uint8_t*>(pWrapper) + 0x10, pBody) && pBody &&
                            ReadNative(static_cast<const uint8_t*>(pBody) + 0x10, pPhysicsWorld) && pPhysicsWorld)
                            ++inWorld;
                    }
                    if (auto* pAsNode = apNode->AsNode())
                        for (uint16_t i = 0; i < pAsNode->children.length; ++i)
                            walk(pAsNode->children.data[i], aDepth + 1);
                };
                walk(apRoot, 0);
                return inWorld;
            };
            std::string refs = "[";
            int count = 0;
            auto visit = [&](TESObjectCELL* pCell) {
                if (!pCell || !pCell->refData.refArray || pCell->refData.capacity > 50000)
                    return;
                for (uint32_t i = 0; i < pCell->refData.capacity; ++i)
                {
                    auto* pRef = pCell->refData.refArray[i].Get();
                    if (!pRef || !pRef->baseForm || pRef->IsDeleted() || pRef->formType == Actor::Type)
                        continue;
                    const auto d = pRef->position - center;
                    if (d.x * d.x + d.y * d.y + d.z * d.z > radius * radius)
                        continue;
                    auto* pRoot = pRef->GetNiNode();
                    refs += fmt::format("{}[\"{:X}\",{},{},{}]", count++ ? "," : "", pRef->formID, pRef->IsDisabled() ? 1 : 0,
                        pRoot ? 1 : 0, pRoot ? bodiesInWorld(pRoot) : 0);
                }
            };
            if (auto* pSpace = pPlayer->GetWorldSpace())
            {
                const int32_t cx = static_cast<int32_t>(std::floor(center.x / 4096.f));
                const int32_t cy = static_cast<int32_t>(std::floor(center.y / 4096.f));
                for (int32_t dx = -1; dx <= 1; ++dx)
                    for (int32_t dy = -1; dy <= 1; ++dy)
                        visit(ModManager::Get()->GetCellFromCoordinates(cx + dx, cy + dy, pSpace, false));
                // Persistent references (quest properties, enable-parent children such as the Helgen inn models)
                // live in the worldspace's persistent cell (TESWorldSpace +0x88), not in the grid cells.
                void* pPersistent{};
                if (ReadNative(reinterpret_cast<const uint8_t*>(pSpace) + 0x88, pPersistent))
                    visit(Cast<TESObjectCELL>(static_cast<TESForm*>(pPersistent)));
            }
            else
                visit(pPlayer->parentCell);
            return Result(id, fmt::format("\"count\":{},\"refs\":{}]", count, refs));
        }
        if (command == "ref_bodies")
        {
            const auto form = GetJsonString(acLine, "form_id");
            auto* pRef = form.empty() ? nullptr : Cast<TESObjectREFR>(TESForm::GetById(std::stoul(form, nullptr, 16)));
            if (!pRef || !pRef->GetNiNode())
                return Error(id, "reference or 3D not found");
            std::string bodies = "[";
            int emitted = 0;
            std::function<void(NiAVObject*, int)> walk = [&](NiAVObject* apNode, int aDepth)
            {
                if (!apNode || aDepth > 8 || emitted > 64)
                    return;
                const char* pName = *reinterpret_cast<const char* const*>(reinterpret_cast<const uint8_t*>(apNode) + 0x10);
                if (apNode->collisionObject)
                {
                    void* pWrapper{};
                    void* pBody{};
                    ActorPoseDiagnosticViews::RigidBody state{};
                    const bool readable = ReadNative(reinterpret_cast<const uint8_t*>(apNode->collisionObject) + 0x20, pWrapper) && pWrapper &&
                        ReadNative(reinterpret_cast<const uint8_t*>(pWrapper) + 0x10, pBody) && pBody && ReadNative(pBody, state);
                    // hkpWorldObject: world +0x10, collidable broadphase collisionFilterInfo +0x4C (layer = low 7 bits).
                    uint32_t filterInfo{};
                    void* pPhysicsWorld{};
                    if (readable)
                    {
                        ReadNative(static_cast<const uint8_t*>(pBody) + 0x4C, filterInfo);
                        ReadNative(static_cast<const uint8_t*>(pBody) + 0x10, pPhysicsWorld);
                    }
                    const auto& lr = apNode->local.rotate.entry;
                    const auto& wr = apNode->world.rotate.entry;
                    bodies += fmt::format("{}{{\"name\":\"{}\",\"layer\":{},\"filter\":\"{:X}\",\"inWorld\":{},\"depth\":{},\"readable\":{},\"motionType\":{},\"body\":[{},{},{}],\"node\":[{},{},{}],"
                        "\"localRot\":[{:.4f},{:.4f},{:.4f},{:.4f}],\"worldRot\":[{:.4f},{:.4f},{:.4f},{:.4f}],\"bodyRot\":[{:.4f},{:.4f},{:.4f},{:.4f}]}}",
                        emitted++ ? "," : "", EscapeJson(pName ? pName : ""), filterInfo & 0x7F, filterInfo, JsonBool(pPhysicsWorld != nullptr),
                        aDepth, JsonBool(readable), readable ? state.motionType : -1,
                        state.transform[12] * 70.f, state.transform[13] * 70.f, state.transform[14] * 70.f, apNode->world.translate.x,
                        apNode->world.translate.y, apNode->world.translate.z, lr[0][0], lr[0][1], lr[1][0], lr[2][2], wr[0][0], wr[0][1],
                        wr[1][0], wr[2][2], state.transform[0], state.transform[1], state.transform[4], state.transform[10]);
                }
                if (auto* pAsNode = apNode->AsNode())
                {
                    for (uint16_t i = 0; i < pAsNode->children.length; ++i)
                        walk(pAsNode->children.data[i], aDepth + 1);
                }
            };
            walk(pRef->GetNiNode(), 0);
            bodies += "]";
            return Result(id, fmt::format("\"bodies\":{}", bodies));
        }
        // An actor's 3D root and its parent chain (name, owning reference, world position).
        if (command == "actor_3d_parent")
        {
            const auto form = GetJsonString(acLine, "form_id");
            auto* pActor = form.empty() ? nullptr : Cast<Actor>(TESForm::GetById(std::stoul(form, nullptr, 16)));
            if (!pActor || !pActor->GetNiNode())
                return Error(id, "actor or 3D not found");
            std::string chain = "[";
            const NiAVObject* pNode = pActor->GetNiNode();
            for (int depth = 0; pNode && depth < 12; ++depth, pNode = pNode->parent)
            {
                const auto* pOwner = static_cast<const TESObjectREFR*>(pNode->userData);
                // NiObjectNET::name is the BSFixedString at +0x10.
                const char* pName = *reinterpret_cast<const char* const*>(reinterpret_cast<const uint8_t*>(pNode) + 0x10);
                chain += fmt::format("{}{{\"name\":\"{}\",\"owner\":{},\"world\":[{},{},{}]}}", depth ? "," : "",
                    EscapeJson(pName ? pName : ""), pOwner ? pOwner->formID : 0,
                    pNode->world.translate.x, pNode->world.translate.y, pNode->world.translate.z);
            }
            chain += "]";
            return Result(id, fmt::format("\"position\":[{},{},{}],\"chain\":{}", pActor->position.x, pActor->position.y,
                pActor->position.z, chain));
        }
        // Host-driven bone playback: stats, and "enabled":"false" to compare against local animation.
        if (command == "pose_authority")
        {
            ObjectService::ArmRenderDiagnostics();
            const auto enabled = GetJsonString(acLine, "enabled");
            if (!enabled.empty())
                PoseCopyAuthority::SetEnabled(enabled != "false");
            return Result(id, PoseCopyAuthority::StatsJson());
        }
        if (command == "cart_physics")
        {
            ObjectService::ArmRenderDiagnostics();
            const bool enabled = GetJsonString(acLine, "enabled") != "false";
            ObjectService::SetCartPhysicsEnabled(enabled);
            return Result(id, fmt::format("\"enabled\":{}", enabled));
        }
        if (command == "cart_node_refresh")
        {
            if (const auto enabled = GetJsonString(acLine, "enabled"); !enabled.empty())
                ObjectService::SetCartNodeRefresh(enabled != "false");
            return Result(id, fmt::format("\"enabled\":{}", ObjectService::IsCartNodeRefresh()));
        }
        if (command == "unseat_remote_players")
        {
            if (const auto enabled = GetJsonString(acLine, "enabled"); !enabled.empty())
                InterpolationSystem::SetUnseatRemotePlayers(enabled != "false");
            return Result(id, fmt::format("\"enabled\":{}", InterpolationSystem::IsUnseatRemotePlayers()));
        }
        if (command == "hazard_sync")
        {
            if (const auto enabled = GetJsonString(acLine, "enabled"); !enabled.empty())
                ObjectService::SetHazardSync(enabled != "false");
            return Result(id, fmt::format("\"enabled\":{}", ObjectService::IsHazardSync()));
        }
        if (command == "exact_body_drive")
        {
            if (const auto enabled = GetJsonString(acLine, "enabled"); !enabled.empty())
                ObjectService::SetExactBodyDrive(enabled != "false");
            return Result(id, fmt::format("\"enabled\":{}", ObjectService::IsExactBodyDrive()));
        }
        if (command == "cart_curve")
        {
            if (const auto enabled = GetJsonString(acLine, "enabled"); !enabled.empty())
                ObjectService::SetCartCurve(enabled != "false");
            return Result(id, fmt::format("\"enabled\":{}", ObjectService::IsCartCurve()));
        }
        if (command == "motion_trace")
            return Result(id, ObjectService::MotionTrace(GetJsonString(acLine, "ids"), GetJsonString(acLine, "bone"),
                GetJsonString(acLine, "dump")));
        if (command == "force_seen")
        {
            if (const auto enabled = GetJsonString(acLine, "enabled"); !enabled.empty())
                AnimationSystem::SetForceSeen(enabled != "false");
            return Result(id, AnimationSystem::ForceSeenJson());
        }
        if (command == "wide_cull")
        {
            if (const auto enabled = GetJsonString(acLine, "enabled"); !enabled.empty())
                AnimationSystem::SetWideCull(enabled != "false");
            return Result(id, AnimationSystem::WideCullJson());
        }
        if (command == "render_all")
        {
            if (const auto enabled = GetJsonString(acLine, "enabled"); !enabled.empty())
                ObjectService::SetRenderAll(enabled != "false");
            return Result(id, fmt::format("\"enabled\":{}", ObjectService::IsRenderAll()));
        }
        if (command == "horse_writeback")
        {
            if (const auto enabled = GetJsonString(acLine, "enabled"); !enabled.empty())
                ObjectService::SetHorseWriteback(enabled != "false");
            return Result(id, fmt::format("\"enabled\":{}", ObjectService::IsHorseWriteback()));
        }
        if (command == "set_fov")
        {
            // Diagnosis without the owner's eyes: widen the host view so almost everything is "in view".
            auto* camera = PlayerCamera::Get();
            if (!camera)
                return Error(id, "no camera");
            if (const auto fov = GetJsonString(acLine, "fov"); !fov.empty())
                camera->SetWorldFov(static_cast<float>(std::strtod(fov.c_str(), nullptr)));
            return Result(id, fmt::format("\"fov\":{}", camera->GetWorldFov()));
        }
        if (command == "scene_update_mode")
        {
            if (const auto mode = GetJsonString(acLine, "mode"); !mode.empty())
                ObjectService::SetSceneUpdateMode(static_cast<uint32_t>(std::strtoul(mode.c_str(), nullptr, 10)));
            return Result(id, fmt::format("\"mode\":{}", ObjectService::GetSceneUpdateMode()));
        }
        if (command == "sync_level")
        {
            // {"batch_ms":"0","delay_ms":"100"}: pose/movement batch interval (0 = every frame) and presentation
            // delay; always returns bytes/messages sent since the last sync_level read.
            extern std::atomic<uint32_t> g_syncBatchMs;
            extern std::atomic<uint64_t> g_transportBytesSent;
            extern std::atomic<uint64_t> g_transportMessagesSent;
            if (const auto batch = GetJsonString(acLine, "batch_ms"); !batch.empty())
                g_syncBatchMs.store(static_cast<uint32_t>(std::strtoul(batch.c_str(), nullptr, 10)));
            if (const auto delay = GetJsonString(acLine, "delay_ms"); !delay.empty())
                m_world.GetCharacterService().SetPresentationDelayMs(
                    static_cast<uint32_t>((std::clamp)(std::strtoul(delay.c_str(), nullptr, 10), 16ul, 500ul)));
            static uint64_t lastBytes{}, lastMessages{}, lastMs{};
            const auto nowMs = GetTickCount64();
            const auto bytes = g_transportBytesSent.load(), messages = g_transportMessagesSent.load();
            const double seconds = lastMs ? (nowMs - lastMs) / 1000.0 : 0.0;
            const auto out = fmt::format("\"batchMs\":{},\"delayMs\":{},\"bytesPerSec\":{:.0f},\"messagesPerSec\":{:.1f},\"windowS\":{:.1f}",
                g_syncBatchMs.load(), m_world.GetCharacterService().GetPresentationDelayMs(),
                seconds > 0 ? (bytes - lastBytes) / seconds : 0.0, seconds > 0 ? (messages - lastMessages) / seconds : 0.0, seconds);
            lastBytes = bytes; lastMessages = messages; lastMs = nowMs;
            return Result(id, out);
        }
        if (command == "turn_player")
        {
            // Scenario "host looks away": rotate the local player's heading by degrees (camera follows in
            // first person). Owner repro: looking away from NPCs broke their simulation.
            const auto degreesText = GetJsonString(acLine, "degrees");
            const double degrees = degreesText.empty() ? 180.0 : std::strtod(degreesText.c_str(), nullptr);
            auto* player = PlayerCharacter::Get();
            if (!player)
                return Error(id, "no player");
            const float z = player->rotation.z + static_cast<float>(degrees * 3.14159265358979 / 180.0);
            // Optional absolute look pitch in degrees, positive = down (head tracking up/down scenario).
            const auto pitchText = GetJsonString(acLine, "pitch");
            const float x = pitchText.empty() ? player->rotation.x :
                static_cast<float>(std::strtod(pitchText.c_str(), nullptr) * 3.14159265358979 / 180.0);
            player->SetRotation(x, player->rotation.y, z);
            return Result(id, fmt::format("\"yaw\":{},\"pitch\":{}", z, x));
        }
        if (command == "offscreen_simulate")
        {
            const auto enabled = GetJsonString(acLine, "enabled");
            if (!enabled.empty())
            {
                AnimationSystem::SetOffscreenSimulate(enabled != "false");
                spdlog::info("Offscreen simulate: {}", enabled != "false");
            }
            return Result(id, AnimationSystem::OffscreenSimulateJson());
        }
        if (command == "native_local_set_position")
        {
            extern std::atomic<bool> g_nativeLocalSetPosition;
            const auto enabled = GetJsonString(acLine, "enabled");
            if (!enabled.empty())
            {
                g_nativeLocalSetPosition.store(enabled != "false", std::memory_order_relaxed);
                spdlog::info("Native local SetPosition: {}", enabled != "false");
            }
            return Result(id, fmt::format("\"enabled\":{}", g_nativeLocalSetPosition.load()));
        }
        if (command == "cart_replay")
        {
            const auto enabled = GetJsonString(acLine, "enabled");
            if (!enabled.empty())
                ObjectService::SetCartReplayEnabled(enabled != "false");
            return Result(id, fmt::format("\"enabled\":{}", ObjectService::IsCartReplayEnabled()));
        }
        if (command == "visual_lag_frame")
        {
            ObjectService::ArmRenderDiagnostics();
            const bool enabled = GetJsonString(acLine, "enabled") != "false";
            ObjectService::SetVisualLagFrameEnabled(enabled);
            return Result(id, fmt::format("\"enabled\":{}", enabled));
        }
        if (command == "body_velocity")
        {
            ObjectService::ArmRenderDiagnostics();
            const bool enabled = GetJsonString(acLine, "enabled") != "false";
            ObjectService::SetBodyVelocityEnabled(enabled);
            return Result(id, fmt::format("\"enabled\":{}", enabled));
        }
        if (command == "cart_smoothing")
        {
            ObjectService::ArmRenderDiagnostics();
            const bool enabled = GetJsonString(acLine, "enabled") != "false";
            ObjectService::SetCartSmoothingEnabled(enabled);
            return Result(id, fmt::format("\"enabled\":{}", enabled));
        }
        if (command == "hermite_playback")
        {
            ObjectService::ArmRenderDiagnostics();
            const bool enabled = GetJsonString(acLine, "enabled") != "false";
            ObjectService::SetHermitePlaybackEnabled(enabled);
            return Result(id, fmt::format("\"enabled\":{}", enabled));
        }
        if (command == "physics_stamp")
        {
            ObjectService::ArmRenderDiagnostics();
            const bool enabled = GetJsonString(acLine, "enabled") != "false";
            ObjectService::SetPhysicsStampEnabled(enabled);
            return Result(id, fmt::format("\"enabled\":{}", enabled));
        }
        if (command == "main_frame_capture")
        {
            ObjectService::ArmRenderDiagnostics();
            const bool enabled = GetJsonString(acLine, "enabled") != "false";
            ObjectService::SetMainFrameCaptureEnabled(enabled);
            return Result(id, fmt::format("\"enabled\":{}", enabled));
        }
        if (command == "main_frame_playback")
        {
            ObjectService::ArmRenderDiagnostics();
            const bool enabled = GetJsonString(acLine, "enabled") != "false";
            ObjectService::SetMainFramePlaybackEnabled(enabled);
            return Result(id, fmt::format("\"enabled\":{}", enabled));
        }
        if (command == "host_driven_playback")
        {
            ObjectService::ArmRenderDiagnostics();
            const bool enabled = GetJsonString(acLine, "enabled") != "false";
            ObjectService::SetHostDrivenPlaybackEnabled(enabled);
            return Result(id, fmt::format("\"enabled\":{}", enabled));
        }
        if (command == "root_body_write")
        {
            ObjectService::ArmRenderDiagnostics();
            const bool enabled = GetJsonString(acLine, "enabled") != "false";
            ObjectService::SetRootBodyWriteEnabled(enabled);
            return Result(id, fmt::format("\"enabled\":{}", enabled));
        }
        if (command == "cell_handoff")
        {
            ObjectService::ArmRenderDiagnostics();
            const bool enabled = GetJsonString(acLine, "enabled") != "false";
            ObjectService::SetCellHandoffEnabled(enabled);
            return Result(id, fmt::format("\"enabled\":{}", enabled));
        }
        // Plays an idle (form ID, hex) on the local player, e.g. 10C00D IdleWalkingCameraEnd.
        if (command == "player_idle")
        {
            auto* pPlayer = PlayerCharacter::Get();
            const auto form = GetJsonString(acLine, "form_id");
            auto* pIdle = form.empty() ? nullptr : Cast<TESIdleForm>(TESForm::GetById(std::stoul(form, nullptr, 16)));
            if (!pPlayer || !pIdle)
                return Error(id, "player or idle form not found");
            return Result(id, fmt::format("\"played\":{}", pPlayer->PlayIdle(pIdle)));
        }
        if (command == "set_session_open")
        {
            if (!m_world.GetPartyService().IsLeader())
                return Error(id, "host party is not ready yet");
            const bool open = GetJsonString(acLine, "open") != "false";
            m_world.GetPartyService().SetSessionSettings(open, {});
            return Result(id, fmt::format("\"open\":{}", open));
        }
        if (command == "join_friend")
        {
            const auto steamId = GetJsonString(acLine, "steamId");
            if (steamId.empty())
                return Error(id, "steamId is required");
            m_world.GetSteamLobbyService().QueueJoinFriend(std::stoull(steamId));
            return Result(id, fmt::format("\"steamId\":\"{}\"", EscapeJson(steamId)));
        }
        // Steam session tests: the lobby as the UI sees it, direct invites, answering invites.
        if (command == "steam_state")
            return Result(id, fmt::format("\"steam\":{}", m_world.GetSteamLobbyService().TestStateJson()));
        if (command == "steam_host" || command == "steam_leave")
        {
            if (command == "steam_host") m_world.GetSteamLobbyService().QueueHostSession();
            else m_world.GetSteamLobbyService().QueueLeaveSession();
            return Result(id, "\"queued\":true");
        }
        if (command == "steam_invite")
        {
            const auto steamId = GetJsonString(acLine, "steamId");
            if (steamId.empty())
                return Error(id, "steamId is required");
            m_world.GetSteamLobbyService().QueueInviteFriendDirect(std::stoull(steamId));
            return Result(id, fmt::format("\"steamId\":\"{}\"", EscapeJson(steamId)));
        }
        if (command == "steam_answer_invite")
        {
            const auto lobby = GetJsonString(acLine, "lobby");
            if (lobby.empty())
                return Error(id, "lobby is required");
            const bool accept = GetJsonString(acLine, "accept") != "false";
            m_world.GetSteamLobbyService().QueueAnswerInvite(std::stoull(lobby), accept);
            return Result(id, fmt::format("\"lobby\":\"{}\",\"accept\":{}", EscapeJson(lobby), accept));
        }
        if (command == "set_ready")
        {
            const bool ready = GetJsonString(acLine, "ready") != "false";
            m_world.GetPartyService().SetReady(ready);
            return Result(id, fmt::format("\"ready\":{}", ready));
        }
        if (command == "start_new_campaign")
        {
            auto& party = m_world.GetPartyService();
            if (!party.IsLeader())
                return Error(id, "only the party leader can start a campaign");
            if (party.GetPartyMembers().size() < 2 ||
                party.GetReadyPlayerCount() != party.GetPartyMembers().size() ||
                party.GetSessionState() != 0)
                return Error(id, "party needs two or more members, all ready, and an idle session");
            party.SelectCampaign(PartyStartRequest::kNew);
            party.StartTogether(PartyStartRequest::kNew);
            return Result(id, "\"campaignMode\":1");
        }
        if (command == "start_continue_campaign")
        {
            auto& party = m_world.GetPartyService();
            if (!party.IsLeader())
                return Error(id, "only the party leader can start a campaign");
            if (party.GetPartyMembers().size() < 2 ||
                party.GetReadyPlayerCount() != party.GetPartyMembers().size() ||
                party.GetSessionState() != 0)
                return Error(id, "party needs two or more members, all ready, and an idle session");
            // Optional "checkpoint": a specific SSC_ checkpoint id present on every PC (e.g. the cart-exit save before
            // Lokir dies), instead of the newest. Empty keeps Continue's newest-checkpoint behavior.
            const String checkpoint = GetJsonString(acLine, "checkpoint").c_str();
            if (!checkpoint.empty() && !CheckpointSaves::Has(checkpoint))
                return Error(id, "checkpoint is not on this PC");
            party.SelectCampaign(PartyStartRequest::kContinue, checkpoint);
            party.StartTogether(PartyStartRequest::kContinue, checkpoint);
            return Result(id, fmt::format("\"campaignMode\":2,\"checkpoint\":\"{}\"", EscapeJson(checkpoint.c_str())));
        }
        if (command == "close_options")
        {
            m_world.GetOverlayService().SetActive(false);
            return Result(id, "\"action\":\"close_options\"");
        }
        if (command == "controller")
        {
            const auto button = GetJsonString(acLine, "button");
            if (!m_world.GetOverlayService().InjectTestControllerButton(button))
                return Error(id, "unknown controller button");
            return Result(id, fmt::format("\"button\":\"{}\"", EscapeJson(button)));
        }
        if (command == "race_menu_state")
        {
            auto* pUI = UI::Get();
            auto* pPlayer = PlayerCharacter::Get();
            auto* pNpc = pPlayer ? Cast<TESNPC>(pPlayer->baseForm) : nullptr;
            const std::string name = pNpc && pNpc->fullName.value.data ?
                pNpc->fullName.value.AsAscii() : "";
            const auto directInput = TiltedPhoques::DInputHook::GetDiagnostic();
            return Result(id, fmt::format(
                "\"raceMenuOpen\":{},\"messageBoxOpen\":{},\"playerName\":\"{}\","
                "\"overlayActive\":{},\"directInputSuppressed\":{},"
                "\"inputPoll\":{{\"calls\":{},\"forwarded\":{},\"unfocused\":{},"
                "\"overlay\":{},\"resumeDelay\":{}}},"
                "\"directInput\":{{\"keyboardStateCalls\":{},\"mouseStateCalls\":{},"
                "\"keyboardDataCalls\":{},\"mouseDataCalls\":{},"
                "\"keyboardEvents\":{},\"mouseEvents\":{}}}",
                JsonBool(pUI && pUI->GetMenuOpen(BSFixedString("RaceSex Menu"))),
                JsonBool(pUI && pUI->GetMenuOpen(BSFixedString("MessageBoxMenu"))),
                EscapeJson(name), JsonBool(m_world.GetOverlayService().GetActive()),
                JsonBool(TiltedPhoques::DInputHook::Get().IsEnabled()),
                g_inputPollDiagnostic.Calls.load(std::memory_order_relaxed),
                g_inputPollDiagnostic.Forwarded.load(std::memory_order_relaxed),
                g_inputPollDiagnostic.Unfocused.load(std::memory_order_relaxed),
                g_inputPollDiagnostic.OverlayActive.load(std::memory_order_relaxed),
                g_inputPollDiagnostic.ResumeDelay.load(std::memory_order_relaxed),
                directInput.KeyboardStateCalls, directInput.MouseStateCalls,
                directInput.KeyboardDataCalls, directInput.MouseDataCalls,
                directInput.KeyboardEvents, directInput.MouseEvents));
        }
        if (command == "confirm_character_native")
        {
            auto* pUI = UI::Get();
            if (!pUI || !pUI->GetMenuOpen(BSFixedString("RaceSex Menu")) ||
                !pUI->GetMenuOpen(BSFixedString("MessageBoxMenu")))
                return Error(id, "character confirmation is not open");
            if (m_nativeCreatorConfirmRequested.load(std::memory_order_acquire))
                return Error(id, "native confirmation already pending");
            {
                std::scoped_lock lock(m_snapshotMutex);
                m_nativeCreatorConfirmComplete = false;
                m_nativeCreatorConfirmSucceeded = false;
            }
            m_nativeCreatorConfirmRequested.store(true, std::memory_order_release);
            return Result(id, "\"queued\":true");
        }
        if (command == "confirm_character_native_status")
        {
            std::scoped_lock lock(m_snapshotMutex);
            return Result(id, fmt::format("\"complete\":{},\"nativeSelected\":{}",
                JsonBool(m_nativeCreatorConfirmComplete),
                JsonBool(m_nativeCreatorConfirmSucceeded)));
        }
        if (command == "race_menu_key" || command == "gameplay_key")
        {
            const bool gameplayKey = command == "gameplay_key";
            const auto key = GetJsonString(acLine, "key");
            if (gameplayKey ? (key != "quicksave" && key != "forward") :
                (key != "done" && key != "confirm" && key != "left" &&
                key != "right" && key != "click" && key != "type" &&
                key != "accept_name"))
                return Error(id, "unsupported key action");
            const auto name = GetJsonString(acLine, "name");
            if (key == "type" && name != "Host" && name != "Follower 1" &&
                name != "Follower 2" && name != "Follower 3" && name != "Follower 4")
                return Error(id, "type requires a party test character name");
            const auto x = GetJsonString(acLine, "x");
            const auto y = GetJsonString(acLine, "y");
            if (key == "click" && (x.empty() || y.empty() ||
                x.find_first_not_of("0123456789") != std::string::npos ||
                y.find_first_not_of("0123456789") != std::string::npos))
                return Error(id, "click requires decimal client x and y");

            auto* pUI = UI::Get();
            auto* pWindow = BSGraphics::GetMainWindow();
            if (!pUI || !pWindow || !pWindow->hWnd || !IsWindowVisible(pWindow->hWnd))
                return Error(id, "Skyrim window is not visible");
            if (gameplayKey)
            {
                auto* pPlayer = PlayerCharacter::Get();
                // The Loading Menu instance stays alive between loads, so GetMenuOpen is not a
                // loading test; a loaded player (cell and 3D) is.
                if (m_world.GetPartyService().GetSessionState() != 3 ||
                    !pPlayer || !pPlayer->parentCell || !pPlayer->GetNiNode() ||
                    pUI->GetMenuOpen(BSFixedString("RaceSex Menu")))
                    return Error(id, fmt::format("gameplay input requires shared gameplay, a loaded cell, and no creator menu "
                        "(session {}, cell {}, 3D {}, creator {})", m_world.GetPartyService().GetSessionState(),
                        pPlayer && pPlayer->parentCell, pPlayer && pPlayer->GetNiNode(),
                        pUI->GetMenuOpen(BSFixedString("RaceSex Menu"))));
            }
            else if (!pUI->GetMenuOpen(BSFixedString("RaceSex Menu")))
                return Error(id, "RaceSex Menu is not visible");
            const bool confirmationWasOpen = pUI->GetMenuOpen(BSFixedString("MessageBoxMenu"));
            if (!gameplayKey && (key == "done" || key == "type" || key == "accept_name") &&
                confirmationWasOpen)
                return Error(id, "character confirmation is open");
            if (!gameplayKey && (key == "confirm" || key == "left" || key == "right" ||
                key == "click") &&
                !confirmationWasOpen)
                return Error(id, "character confirmation is not open");

            // The pipe caller may be an SSH process in a different session.
            // Launch the actuator as Skyrim's child on the same interactive
            // desktop, and fail closed if the game cannot obtain focus.
            if (GetForegroundWindow() != pWindow->hWnd)
            {
                SetForegroundWindow(pWindow->hWnd);
                if (GetForegroundWindow() != pWindow->hWnd)
                    return Error(id, "Skyrim could not become foreground");
            }

            wchar_t modulePath[MAX_PATH]{};
            const auto pathLength = GetModuleFileNameW(nullptr, modulePath, MAX_PATH);
            if (!pathLength || pathLength >= MAX_PATH)
                return Error(id, "could not resolve Skyrim executable path");
            // The launcher spoofs GetModuleFileNameW(nullptr) to SkyrimSE.exe,
            // so modulePath resolves to the game root, not our binary folder.
            const auto module = std::filesystem::path(modulePath);
            std::filesystem::path helper;
            if (_wcsicmp(module.filename().c_str(), L"SkyrimSE.exe") == 0)
                helper = module.parent_path() / L"Data" / L"SkyrimTogetherReborn" / L"GameTestKeyHelper.exe";
            else if (_wcsicmp(module.filename().c_str(), L"SkyrimTogether.exe") == 0)
                helper = module.parent_path() / L"GameTestKeyHelper.exe";
            else
                return Error(id, "unexpected Skyrim process image path");
            if (!std::filesystem::is_regular_file(helper))
                return Error(id, fmt::format("GameTestKeyHelper.exe is not installed at {}",
                    helper.string()));
            const auto helperKey = key == "accept_name" ? "confirm" : key;
            std::wstring commandLine = L"\"" + helper.wstring() + L"\" " +
                std::to_wstring(reinterpret_cast<uintptr_t>(pWindow->hWnd)) + L" " +
                std::to_wstring(GetCurrentProcessId()) + L" " +
                std::wstring(helperKey.begin(), helperKey.end());
            if (key == "type")
                commandLine += L" \"" + std::wstring(name.begin(), name.end()) + L"\"";
            else if (key == "click")
                commandLine += L" " + std::wstring(x.begin(), x.end()) + L" " +
                    std::wstring(y.begin(), y.end());
            STARTUPINFOW startup{};
            startup.cb = sizeof(startup);
            PROCESS_INFORMATION child{};
            if (!CreateProcessW(helper.c_str(), commandLine.data(), nullptr, nullptr,
                FALSE, CREATE_NO_WINDOW, nullptr, helper.parent_path().c_str(), &startup, &child))
                return Error(id, fmt::format("could not start key helper (Win32 {})", GetLastError()));
            const auto wait = WaitForSingleObject(child.hProcess, 3000);
            if (wait == WAIT_TIMEOUT)
                TerminateProcess(child.hProcess, 8);
            DWORD childExit = 8;
            if (wait == WAIT_OBJECT_0)
                GetExitCodeProcess(child.hProcess, &childExit);
            CloseHandle(child.hThread);
            CloseHandle(child.hProcess);
            if (wait != WAIT_OBJECT_0 || childExit != 0)
                return Error(id, fmt::format("key helper failed (wait {}, exit {})", wait, childExit));
            return Result(id, fmt::format("\"key\":\"{}\",\"helperExit\":{}",
                EscapeJson(key), childExit));
        }
        if (command == "toggle_window")
        {
            m_world.GetGameSettingsService().ToggleWindowMode();
            return Result(id, "\"action\":\"toggle_window\"");
        }
        if (command == "confirm_display")
        {
            m_world.GetGameSettingsService().ConfirmDisplaySettings();
            return Result(id, "\"action\":\"confirm_display\"");
        }
        if (command == "setting")
        {
            const auto name = GetJsonString(acLine, "name");
            const auto value = GetJsonString(acLine, "value");
            if (name.empty())
                return Error(id, "setting name is required");
            m_world.GetGameSettingsService().PreviewSetting(name.c_str(), value.c_str());
            return Result(id, fmt::format("\"name\":\"{}\",\"value\":\"{}\"",
                EscapeJson(name), EscapeJson(value)));
        }
        if (command == "screenshot")
        {
            const auto path = m_world.GetGameSettingsService().CaptureTestScreenshot();
            if (path.empty())
                return Error(id, "screenshot failed");
            return Result(id, fmt::format("\"path\":\"{}\"", EscapeJson(path.string())));
        }
        if (command == "snapshot")
        {
            auto* pWindow = BSGraphics::GetMainWindow();
            auto* pRenderer = BSGraphics::GetRendererData();
            if (!pWindow || !pWindow->hWnd || !pRenderer)
                return Error(id, "renderer is not initialized");

            RECT client{}, outer{};
            GetClientRect(pWindow->hWnd, &client);
            GetWindowRect(pWindow->hWnd, &outer);
            CURSORINFO cursor{sizeof(cursor)};
            GetCursorInfo(&cursor);
            DXGI_SWAP_CHAIN_DESC swap{};
            const bool haveSwap = pWindow->pSwapChain && SUCCEEDED(pWindow->pSwapChain->GetDesc(&swap));

            uint32_t overlayWidth = 0, overlayHeight = 0;
            uint16_t cursorX = 0, cursorY = 0;
            bool cefCursor = false;
            if (auto* pApp = m_world.GetOverlayService().GetOverlayApp(); pApp && pApp->GetClient())
                if (auto handler = pApp->GetClient()->GetOverlayRenderHandler())
                {
                    std::tie(overlayWidth, overlayHeight) = handler->GetRenderSize();
                    std::tie(cursorX, cursorY) = handler->GetCursorLocation();
                    cefCursor = handler->IsCursorVisible();
                }

            bool mainMenu = false;
            if (auto* pUI = UI::Get())
                mainMenu = pUI->GetMenuOpen(BSFixedString("Main Menu"));

            const auto style = static_cast<uint64_t>(GetWindowLongPtrW(pWindow->hWnd, GWL_STYLE));
            return Result(id, fmt::format(
                "\"window\":{{\"hwnd\":{},\"foreground\":{},\"style\":{},\"clientWidth\":{},\"clientHeight\":{},"
                "\"outerWidth\":{},\"outerHeight\":{},\"x\":{},\"y\":{}}},"
                "\"renderer\":{{\"width\":{},\"height\":{},\"fullscreen\":{},\"borderless\":{},"
                "\"swapWidth\":{},\"swapHeight\":{},\"swapWindowed\":{}}},"
                "\"overlay\":{{\"active\":{},\"titleScreen\":{},\"width\":{},\"height\":{},"
                "\"cursorVisible\":{},\"cursorX\":{},\"cursorY\":{}}},"
                "\"systemCursor\":{{\"visible\":{}}},\"mainMenuOpen\":{}",
                reinterpret_cast<uintptr_t>(pWindow->hWnd), GetForegroundWindow() == pWindow->hWnd,
                style, client.right, client.bottom,
                outer.right - outer.left, outer.bottom - outer.top, outer.left, outer.top,
                pRenderer->RenderWindowA[0].uiWindowWidth, pRenderer->RenderWindowA[0].uiWindowHeight,
                pRenderer->bAppFullScreen, pRenderer->bBorderlessWindow,
                haveSwap ? swap.BufferDesc.Width : 0, haveSwap ? swap.BufferDesc.Height : 0,
                haveSwap ? swap.Windowed != FALSE : false,
                m_world.GetOverlayService().GetActive(), m_world.GetOverlayService().GetTitleScreen(),
                overlayWidth, overlayHeight, cefCursor, cursorX, cursorY,
                (cursor.flags & CURSOR_SHOWING) != 0, mainMenu));
        }
        return Error(id, "unknown command");
    }
    catch (const std::exception& exception)
    {
        return Error(id, exception.what());
    }
    catch (...)
    {
        return Error(id, "native exception");
    }
}
