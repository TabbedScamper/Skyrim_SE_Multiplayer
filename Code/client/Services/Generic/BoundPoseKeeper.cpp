#include "BoundPoseKeeper.h"

#include <PlayerCharacter.h>
#include <AI/AIProcess.h>
#include <Games/ActorExtension.h>
#include <Games/Animation/ActorMediator.h>
#include <Games/Animation/TESActionData.h>
#include <Forms/BGSAction.h>
#include <Forms/TESObjectCELL.h>
#include <BSAnimationGraphManager.h>
#include <Havok/hkbBehaviorGraph.h>
#include <Services/ObjectService.h>
#include <FunctionHook.hpp>
#include <intrin.h>
#include <array>
#include <mutex>

namespace
{
// These records only observe the native creator and Reset3D paths. Repair
// below is explicit, independent of diagnostic arming, and retains no pose.
struct Record
{
    enum Kind { Creator, Reset } Type{};
    uint64_t Session{}, Tick{}, EndTick{};
    uintptr_t Caller{}, Menu{};
    uint32_t ActorId{}, Thread{}, StateBefore{}, StateAfter{};
    uint16_t GatesBefore{}, GatesAfter{};
    uint8_t ModelBefore{}, ModelAfter{};
    bool Local{}, Reload{};
};
std::mutex s_lock;
std::array<Record, 128> s_records{};
size_t s_count{};
std::atomic<uint64_t> s_session{};
std::atomic<uint64_t> s_nextHeartbeat{};
std::atomic<uint32_t> s_dropped{};
std::atomic<bool> s_traceEnabled{};
uint64_t s_nextDrain{}; // Main thread only.
uint32_t s_logged{};
bool s_armed{};
// Main-thread creator lifecycle only; never a remembered gameplay bound state.
bool s_creatorWasOpen{};
bool s_sharedCreation{};
uint64_t s_creatorCloseDeadline{};
uint32_t s_creatorCell{};

using AdvanceMovie = void (*)(void*, float, uint32_t);
using Reset3D = void (*)(Actor*, bool);
AdvanceMovie s_advanceMovie{};
using CreatorRebuild = void (*)(void*);
CreatorRebuild s_creatorRebuild{};
Reset3D s_reset3D{};
using GetFlags = uint8_t (*)(AIProcess*);
GetFlags s_getFlags{};
using UsesTaskQueue = uint8_t (*)();
UsesTaskQueue s_usesTaskQueue{};
std::atomic<DWORD> s_mainThread{};
// Another player's copy whose bound event was rejected right after its appearance rebuild (graph not ready yet):
// retried every main frame until accepted or the deadline (owner report: the other player's copy had unbound hands
// after the character creator; the rebuild event was rejected once and never retried). Main thread only.
struct PendingReplica
{
    uint64_t DeadlineMs{};
    const char* Reason{};
};
std::unordered_map<uint32_t, PendingReplica> s_pendingReplicas;

uint8_t ModelFlags(Actor* aActor)
{
    // 39911 / 140724680 reads process+8 -> middleHigh+311, null-safe.
    return aActor && aActor->currentProcess ? s_getFlags(aActor->currentProcess) : 0;
}

void Enqueue(const Record& aRecord)
{
    std::unique_lock lock(s_lock, std::try_to_lock);
    if (!lock)
    {
        s_dropped.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (!s_traceEnabled.load(std::memory_order_relaxed) || aRecord.Session != s_session.load())
    {
        s_dropped.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (s_count == s_records.size())
        s_dropped.fetch_add(1, std::memory_order_relaxed);
    else
        s_records[s_count++] = aRecord;
}

uint16_t CreatorGates(void* aMenu)
{
    // 52346 / 140968830 tests these two bytes and clears both only after
    // rebuilding and sending OffsetBoundStandingPlayerInstant. Raw values,
    // not inferred names for the flags. The pointer is the native live this.
    const auto* bytes = static_cast<const uint8_t*>(aMenu);
    return bytes ? static_cast<uint16_t>(bytes[0x1A3] | (bytes[0x1A4] << 8)) : 0;
}

void HookCreatorRebuild(void* aMenu)
{
    // 52391 / 14096BCF0 is shared by initial preview, presets and race/sex.
    // 52346 restores race/sex itself; its completion hook publishes that event.
    // Preview/presets lack that send. Match vanilla's post-rebuild operation
    // without requiring the rebuilt graph to already contain the lost pose.
    const bool nativeWillRestore = CreatorGates(aMenu) != 0;
    auto* player = PlayerCharacter::Get();
    // A normal later showracemenu must not bind a previously free player.
    // The shared New Game creator establishes intent for both participants;
    // other previews only preserve their actual pre-rebuild pose.
    const auto pose = !nativeWillRestore && !s_sharedCreation ? BoundPoseKeeper::ReadPose(player) : BoundPoseKeeper::Pose{};
    s_creatorRebuild(aMenu);
    if (!nativeWillRestore)
        BoundPoseKeeper::AfterRebuild(player, pose, "native creator rebuild", s_sharedCreation);
}

void HookAdvanceMovie(void* aMenu, float aInterval, uint32_t aCurrentTime)
{
    // 52346 has already sent PlayerInstant when these gates clear. Route the
    // completed native event through the ordinary action entry for peer delivery.
    // Its arm states use SELF_TRANSITION_MODE_NO_TRANSITION, so the second send
    // does not restart an already active bound clip (vanilla mt_behavior.hkx).
    const auto gates = CreatorGates(aMenu);
    const auto complete = [&]() {
        if (gates && !CreatorGates(aMenu))
        {
            auto* player = PlayerCharacter::Get();
            BoundPoseKeeper::AfterRebuild(player, {}, "native race/sex completion", true);
        }
    };
    if (!s_traceEnabled.load(std::memory_order_relaxed))
    {
        s_advanceMovie(aMenu, aInterval, aCurrentTime);
        complete();
        return;
    }
    Record record{};
    record.Type = Record::Creator;
    record.Session = s_session.load();
    record.Tick = GetTickCount64();
    record.Thread = GetCurrentThreadId();
    record.Menu = reinterpret_cast<uintptr_t>(aMenu);
    record.GatesBefore = CreatorGates(aMenu);
    auto* player = PlayerCharacter::Get();
    record.ActorId = player ? player->formID : 0;
    record.StateBefore = player ? player->actorState.flags1 : 0;
    record.ModelBefore = ModelFlags(player);
    s_advanceMovie(aMenu, aInterval, aCurrentTime);
    record.GatesAfter = CreatorGates(aMenu);
    record.EndTick = GetTickCount64();
    player = PlayerCharacter::Get();
    record.StateAfter = player ? player->actorState.flags1 : 0;
    record.ModelAfter = ModelFlags(player);
    // Log completions/changes immediately, plus one callback heartbeat per
    // second so zero gates can be distinguished from a callback not running.
    const bool heartbeat = record.Tick >= s_nextHeartbeat.load(std::memory_order_relaxed);
    if (heartbeat)
        s_nextHeartbeat.store(record.Tick + 1000, std::memory_order_relaxed);
    if (heartbeat || record.GatesBefore != record.GatesAfter)
        Enqueue(record);
    complete();
}

void HookReset3D(Actor* aActor, bool aReload)
{
    // Reset3D also runs on task threads. Inspect no extension map there.
    const bool enabled = s_traceEnabled.load(std::memory_order_relaxed);
    auto* player = enabled ? PlayerCharacter::Get() : nullptr;
    const bool local = aActor && aActor == player;
    const bool trace = enabled && aActor && (local ||
        (GetCurrentThreadId() == s_mainThread.load(std::memory_order_relaxed) &&
            aActor->GetExtension() && aActor->GetExtension()->IsRemotePlayer()));
    Record record{};
    if (trace)
    {
        record.Type = Record::Reset;
        record.Session = s_session.load();
        record.Tick = GetTickCount64();
        record.Thread = GetCurrentThreadId();
        record.ActorId = aActor->formID;
        record.Local = local;
        record.Reload = aReload;
        record.Caller = reinterpret_cast<uintptr_t>(_ReturnAddress());
        record.StateBefore = aActor->actorState.flags1;
        record.ModelBefore = ModelFlags(aActor);
    }
    s_reset3D(aActor, aReload);
    if (trace)
    {
        record.EndTick = GetTickCount64();
        record.StateAfter = aActor->actorState.flags1;
        record.ModelAfter = ModelFlags(aActor);
        Enqueue(record);
    }
}

static TiltedPhoques::Initializer s_hooks([]()
{
    // 52346 / 140968830 is RaceSexMenu vtable slot 5: (this, XMM1 float,
    // R8D time) -> void. 40255 / 140739A40 takes (Actor*, DL bool) -> void.
    // Bodies, vtable/call sites and assembly are recorded in the task report.
    POINTER_SKYRIMSE(std::remove_pointer_t<AdvanceMovie>, advanceMovie, 52346);
    POINTER_SKYRIMSE(std::remove_pointer_t<CreatorRebuild>, creatorRebuild, 52391);
    POINTER_SKYRIMSE(std::remove_pointer_t<Reset3D>, reset3D, 40255);
    POINTER_SKYRIMSE(std::remove_pointer_t<GetFlags>, getFlags, 39911);
    POINTER_SKYRIMSE(std::remove_pointer_t<UsesTaskQueue>, usesTaskQueue, 39033);
    s_advanceMovie = advanceMovie.Get();
    s_creatorRebuild = creatorRebuild.Get();
    s_reset3D = reset3D.Get();
    s_getFlags = getFlags.Get();
    s_usesTaskQueue = usesTaskQueue.Get();
    TP_HOOK(&s_advanceMovie, HookAdvanceMovie);
    TP_HOOK(&s_creatorRebuild, HookCreatorRebuild);
    TP_HOOK(&s_reset3D, HookReset3D);
});
}

BoundPoseKeeper::BoundPoseKeeper(entt::dispatcher&) noexcept
{
}

BoundPoseKeeper::~BoundPoseKeeper() noexcept
{
    s_traceEnabled.store(false, std::memory_order_relaxed);
    s_creatorWasOpen = false;
    s_sharedCreation = false;
    s_creatorCloseDeadline = 0;
}

BoundPoseKeeper::Pose BoundPoseKeeper::ReadPose(Actor* aActor) noexcept
{
    Pose result{};
    if (!aActor || GetCurrentThreadId() != s_mainThread.load(std::memory_order_relaxed))
        return result;
    BSAnimationGraphManager* manager{};
    aActor->animationGraphHolder.GetBSAnimationGraph(&manager);
    if (!manager)
        return result;
    {
        BSScopedLock<BSRecursiveLock> lock(manager->lock);
        // Graph 0 is the third-person graph, also while the player views graph 1.
        auto* graph = manager->animationGraphs.size ? manager->animationGraphs.Get(0) : nullptr;
        auto* behavior = graph ? graph->behaviorGraph : nullptr;
        auto* nodes = behavior ? behavior->struct98 : nullptr;
        // 58377 / 140ACFC70 traverses these live clones at stride 90, skipping
        // byte84. This is a rebuild-boundary query, never a per-frame graph walk.
        if (behavior && behavior->byte12C && behavior->byte12D && nodes && nodes->data &&
            nodes->count > 0 && nodes->count <= 512)
        {
            POINTER_SKYRIMSE(void*, stateVtable, 226812);
            POINTER_SKYRIMSE(void*, clipVtable, 226785);
            using StateIndex = int32_t(void*, int32_t);
            POINTER_SKYRIMSE(StateIndex, stateIndex, 59372);
            bool leftBound{}, rightBound{};
            for (int i = 0; i < nodes->count; ++i)
            {
                const auto& node = nodes->data[i];
                auto* state = reinterpret_cast<const uint8_t*>(node.generator);
                if (node.byte84 || !state || *reinterpret_cast<void* const*>(state) != stateVtable.Get() || !state[0x88])
                    continue;
                // hkStringPtr's low bit is ownership, not part of the address.
                const auto* name = reinterpret_cast<const char*>(*reinterpret_cast<const uintptr_t*>(state + 0x38) & ~uintptr_t{1});
                const bool left = name && std::strcmp(name, "CarryLeftArmBehavior") == 0;
                const bool right = name && std::strcmp(name, "CarryRightArmBehavior") == 0;
                if (!left && !right)
                    continue;
                // 59372 / 140AF4A30 resolves a state ID to the state's index;
                // 59343 / 140AF3440 reads StateInfo::generator at +58.
                const auto id = *reinterpret_cast<const int32_t*>(state + 0x80);
                const auto index = stateIndex.Get()(const_cast<uint8_t*>(state), id);
                const auto count = *reinterpret_cast<const int32_t*>(state + 0x98);
                const auto* states = *reinterpret_cast<const uint8_t* const* const*>(state + 0x90);
                if (!states || index < 0 || index >= count)
                    continue;
                auto* info = states[index];
                auto* clip = info ? *reinterpret_cast<const uint8_t* const*>(info + 0x58) : nullptr;
                const char* animation = clip && *reinterpret_cast<void* const*>(clip) == clipVtable.Get() ?
                    reinterpret_cast<const char*>(*reinterpret_cast<const uintptr_t*>(clip + 0x48) & ~uintptr_t{1}) : nullptr;
                const bool bound = animation && _stricmp(animation, "Animations\\OffsetBoundStanding.hkx") == 0;
                if (left)
                {
                    result.Left = id;
                    leftBound = bound;
                }
                else
                {
                    result.Right = id;
                    rightBound = bound;
                }
                if (result.Left >= 0 && result.Right >= 0)
                    break;
            }
            result.Known = result.Left >= 0 && result.Right >= 0;
            result.Bound = result.Known && leftBound && rightBound;
        }
    }
    manager->Release();
    return result;
}

bool BoundPoseKeeper::CanRebuildNow(Actor* aActor) noexcept
{
    return aActor && GetCurrentThreadId() == s_mainThread.load(std::memory_order_relaxed) &&
        aActor->currentProcess && aActor->currentProcess->unk8 && aActor->currentProcess->middleProcess &&
        aActor->GetNiNode() && !aActor->IsDeleted() && !aActor->IsDisabled() &&
        !s_usesTaskQueue() && !ModelFlags(aActor) && aActor->animationGraphHolder.IsReady();
}

bool BoundPoseKeeper::AfterRebuild(Actor* aActor, Pose aBefore, const char* aReason, bool aCreatorRebuild) noexcept
{
    if (!aActor || GetCurrentThreadId() != s_mainThread.load(std::memory_order_relaxed))
        return false;
    const auto model = ModelFlags(aActor);
    if (s_traceEnabled.load(std::memory_order_relaxed))
        spdlog::info("Bound pose trace: rebuild-boundary tick={} actor={:X} reason={} creator={} known={} bound={} arms={},{} model={:02X}",
            GetTickCount64(), aActor->formID, aReason, aCreatorRebuild, aBefore.Known, aBefore.Bound, aBefore.Left, aBefore.Right, model);
    if (!aCreatorRebuild && !aBefore.Bound)
        return false;
    // 40255's queued branch does not set model flags until its task runs.
    // Zero flags alone could still describe the old, about-to-be-reset graph.
    // This is a repair guard, using the same native queue decision as 40255.
    const bool mayQueue = s_usesTaskQueue() != 0;
    if (mayQueue || model || !aActor->animationGraphHolder.IsReady())
    {
        spdlog::warn("Bound pose: rebuild not complete ({}) actor={:X} model={:02X} mayQueue={}; no replay queued", aReason, aActor->formID, model, mayQueue);
        return false;
    }
    BSFixedString event("OffsetBoundStandingPlayerInstant");
    bool accepted{};
    if (aActor == PlayerCharacter::Get())
    {
        // Native 38949's precomputed-action path (someFlag=2) calls 43567,
        // which sends exactly this holder event. Enter the installed normal
        // action hook: it saves flags/variables and publishes with its existing
        // exclusions. ActorMediator::PerformAction would bypass that hook.
        using GetDefaultAction = BGSAction*(uint32_t);
        using Perform = uint8_t(ActorMediator*, TESActionData*);
        POINTER_SKYRIMSE(GetDefaultAction, getDefaultAction, 11436);
        POINTER_SKYRIMSE(Perform, perform, 38949);
        auto* idle = getDefaultAction.Get()(0x40); // DEFAULT_OBJECT::kActionIdle, not a form ID.
        auto* mediator = ActorMediator::Get();
        if (!idle || !mediator)
            return false;
        TESActionData action(2, aActor, idle, nullptr);
        action.eventName = event;
        action.someFlag = BGSActionData::kSkip;
        accepted = perform.Get()(mediator, &action) != 0;
    }
    else
    {
        // This replica has independently rebuilt after receiving its owner's
        // action. Match 52346's local model repair; do not inject ForceAction or
        // another network action outside AnimationSystem's one-action budget.
        accepted = aActor->animationGraphHolder.SendAnimationEvent(&event);
    }
    if (accepted)
        spdlog::info("Bound pose: re-applied after rebuild ({}) actor={:X} local={}", aReason, aActor->formID, aActor == PlayerCharacter::Get());
    else
    {
        spdlog::warn("Bound pose: rebuild event rejected ({}) actor={:X}", aReason, aActor->formID);
        if (aActor != PlayerCharacter::Get())
            s_pendingReplicas[aActor->formID] = {GetTickCount64() + 3000, aReason};
    }
    return accepted;
}

void BoundPoseKeeper::OnMainFrame(bool aCreatorOpen, bool aSharedCreation) noexcept
{
    s_mainThread.store(GetCurrentThreadId(), std::memory_order_relaxed);
    const auto now = GetTickCount64();
    // Run before the diagnostics early return. The creator's destructor restores
    // graph variables (52343 / 140967B40); finish on the next main frame after
    // teardown, including a Done with no race/sex change (no 1A3/1A4 edge).
    auto* player = PlayerCharacter::Get();
    if (aCreatorOpen)
    {
        if (!s_creatorWasOpen)
            s_sharedCreation = false;
        s_sharedCreation = s_sharedCreation || aSharedCreation;
        s_creatorCloseDeadline = 0;
        s_creatorCell = player && player->parentCell ? player->parentCell->formID : 0;
    }
    else if (s_creatorWasOpen)
    {
        if (s_sharedCreation)
            s_creatorCloseDeadline = now + 2000;
        s_sharedCreation = false;
    }
    s_creatorWasOpen = aCreatorOpen;
    for (auto it = s_pendingReplicas.begin(); it != s_pendingReplicas.end();)
    {
        auto* pCopy = Cast<Actor>(TESForm::GetById(it->first));
        bool done = !pCopy || now >= it->second.DeadlineMs;
        if (pCopy && !done && pCopy->GetNiNode() && !ModelFlags(pCopy) && pCopy->animationGraphHolder.IsReady())
        {
            BSFixedString event("OffsetBoundStandingPlayerInstant");
            if (pCopy->animationGraphHolder.SendAnimationEvent(&event))
            {
                spdlog::info("Bound pose: re-applied to {:X} on retry ({})", it->first, it->second.Reason);
                done = true;
            }
        }
        if (done && pCopy && now >= it->second.DeadlineMs)
            spdlog::warn("Bound pose: retry for {:X} timed out ({})", it->first, it->second.Reason);
        it = done ? s_pendingReplicas.erase(it) : std::next(it);
    }
    if (s_creatorCloseDeadline)
    {
        if (!player || !player->parentCell || player->parentCell->formID != s_creatorCell ||
            ((player->actorState.flags1 >> 21) & 0x7F) != 0 || now >= s_creatorCloseDeadline)
        {
            spdlog::warn("Bound pose: creator completion cancelled or timed out before graph readiness");
            s_creatorCloseDeadline = 0;
        }
        else if (CanRebuildNow(player))
        {
            // One dispatch, no periodic enforcement after control returns.
            s_creatorCloseDeadline = 0;
            AfterRebuild(player, {}, "creator closed", true);
        }
    }
    const bool armed = ObjectService::IsRenderDiagnosticsArmed();
    if (armed != s_armed)
    {
        s_traceEnabled.store(false, std::memory_order_relaxed);
        {
            std::lock_guard lock(s_lock);
            ++s_session;
            s_count = 0;
        }
        s_dropped.store(0, std::memory_order_relaxed);
        s_nextHeartbeat.store(0, std::memory_order_relaxed);
        s_nextDrain = 0;
        s_logged = 0;
        s_armed = armed;
        spdlog::info("Bound pose trace: armed={} session={} readOnly=true limit=1024", armed, s_session.load());
        s_traceEnabled.store(armed, std::memory_order_relaxed);
    }
    if (!armed || now < s_nextDrain)
        return;
    s_nextDrain = now + 250;
    std::array<Record, 128> records{};
    size_t count{};
    {
        std::lock_guard lock(s_lock);
        count = s_count;
        std::copy_n(s_records.begin(), count, records.begin());
        s_count = 0;
    }
    if (const auto dropped = s_dropped.exchange(0, std::memory_order_relaxed))
        spdlog::warn("Bound pose trace: dropped={} evidence-incomplete", dropped);
    for (size_t i = 0; i < count && s_logged < 1024; ++i, ++s_logged)
    {
        const auto& r = records[i];
        if (r.Type == Record::Creator)
            spdlog::info("Bound pose trace: creator-native tick={}->{} thread={} menu={:X} actor={:X} gates1A3_1A4={:04X}->{:04X} model={:02X}->{:02X} state={:X}->{:X}",
                r.Tick, r.EndTick, r.Thread, r.Menu, r.ActorId, r.GatesBefore, r.GatesAfter,
                r.ModelBefore, r.ModelAfter, r.StateBefore, r.StateAfter);
        else
            spdlog::info("Bound pose trace: Reset3D tick={}->{} thread={} actor={:X} local={} reload={} caller={:X} model={:02X}->{:02X} state={:X}->{:X}",
                r.Tick, r.EndTick, r.Thread, r.ActorId, r.Local, r.Reload, r.Caller,
                r.ModelBefore, r.ModelAfter, r.StateBefore, r.StateAfter);
    }
    if (s_logged == 1024 && s_traceEnabled.exchange(false, std::memory_order_relaxed))
        spdlog::warn("Bound pose trace: budget exhausted; evidence-incomplete until rearmed");
}
