#include "PlayerStateTrace.h"

#include <World.h>
#include <PlayerCharacter.h>
#include <Forms/TESIdleForm.h>
#include <Services/ObjectService.h>
#include <Events/UpdateEvent.h>
#include <Interface/UI.h>
#include <BSAnimationGraphManager.h>
#include <FunctionHook.hpp>
#include <Games/Animation/TESActionData.h>
#include <Structs/ActionEvent.h>
#include <intrin.h>
#include <mutex>

namespace
{
// No engine pointers survive as dereferenceable objects in this queue. Hooks
// do not log, allocate, resolve VM handles, or acquire another engine lock.
struct Record
{
    enum Kind { Idle, Restraint, Event, Action, Published } Type{};
    uint64_t Tick{};
    uintptr_t Graph{}, Caller{};
    uint32_t Actor{}, Form{}, Stack{}, Before{}, After{}, Thread{};
    uint32_t Input{}, SomeFlag{}, SomeFlagAfter{};
    bool Result{}, Flag{};
    char Name[96]{};
};
std::mutex s_mutex;
std::array<Record, 256> s_records{};
size_t s_count{};
std::atomic<uint32_t> s_dropped{};
std::atomic<bool> s_enabled{};

void Enqueue(Record aRecord)
{
    aRecord.Thread = GetCurrentThreadId();
    std::unique_lock lock(s_mutex, std::try_to_lock);
    if (!lock || s_count == s_records.size())
    {
        s_dropped.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    s_records[s_count++] = aRecord;
}

using PlayIdle = bool (*)(void*, uint32_t, Actor*, TESIdleForm*);
using SetRestrained = void (*)(Actor*, bool);
using SendEvent = bool (*)(BShkbAnimationGraph*, BSFixedString*);
using SendAction = uint8_t (*)(void*, TESActionData*);
PlayIdle s_playIdle{};
SetRestrained s_setRestrained{};
SendEvent s_sendEvent{};
SendAction s_sendAction{};

uint8_t HookSendAction(void* aDispatcher, TESActionData* aAction)
{
    // 43567 is the shared *actual dispatch* used by 38949 and 38953. A dry
    // run (38949's someFlag & 1 path) returns before dispatch. Observe here
    // without double-hooking the existing 38949 transport detour.
    const char* name = s_enabled.load(std::memory_order_relaxed) && aAction ? aAction->eventName.AsAscii() : nullptr;
    const bool trace = name && std::strncmp(name, "Offset", 6) == 0;
    Record record{};
    if (trace)
    {
        record.Type = Record::Action;
        record.Tick = GetTickCount64();
        record.Caller = reinterpret_cast<uintptr_t>(_ReturnAddress());
        record.Actor = aAction->actor ? aAction->actor->formID : 0;
        record.Form = aAction->idleForm ? aAction->idleForm->formID : 0;
        record.Input = aAction->unkInput;
        record.SomeFlag = aAction->someFlag;
        for (size_t i = 0; i + 1 < sizeof(record.Name) && name[i]; ++i)
            record.Name[i] = name[i];
    }
    const auto result = s_sendAction(aDispatcher, aAction);
    if (trace)
    {
        record.Result = result != 0;
        record.SomeFlagAfter = aAction->someFlag;
        Enqueue(record);
    }
    return result;
}

bool HookPlayIdle(void* aVm, uint32_t aStack, Actor* aActor, TESIdleForm* aIdle)
{
    const bool trace = s_enabled.load(std::memory_order_relaxed) && aActor && aActor == PlayerCharacter::Get();
    Record record{};
    if (trace)
    {
        record.Type = Record::Idle;
        record.Tick = GetTickCount64();
        record.Actor = aActor->formID;
        record.Form = aIdle ? aIdle->formID : 0;
        record.Stack = aStack;
        record.Before = aActor->actorState.flags1;
    }
    const bool result = s_playIdle(aVm, aStack, aActor, aIdle);
    if (trace)
    {
        record.Result = result;
        record.After = aActor->actorState.flags1;
        Enqueue(record);
    }
    return result;
}

void HookSetRestrained(Actor* aActor, bool aRestrained)
{
    const bool trace = s_enabled.load(std::memory_order_relaxed) && aActor && aActor == PlayerCharacter::Get();
    Record record{};
    if (trace)
    {
        record.Type = Record::Restraint;
        record.Tick = GetTickCount64();
        record.Actor = aActor->formID;
        record.Flag = aRestrained;
        record.Before = aActor->actorState.flags1;
        record.Caller = reinterpret_cast<uintptr_t>(_ReturnAddress());
    }
    s_setRestrained(aActor, aRestrained);
    if (trace)
    {
        record.After = aActor->actorState.flags1;
        Enqueue(record);
    }
}

bool HookSendEvent(BShkbAnimationGraph* aGraph, BSFixedString* aEvent)
{
    // Prefix filtering is diagnostic only: no event is replayed or suppressed.
    // ID 63591 is called with a live graph under its manager's native lock.
    const bool enabled = s_enabled.load(std::memory_order_relaxed);
    const char* name = enabled && aEvent ? aEvent->AsAscii() : nullptr;
    const bool trace = name && std::strncmp(name, "Offset", 6) == 0;
    Record record{};
    if (trace)
    {
        record.Type = Record::Event;
        record.Tick = GetTickCount64();
        record.Graph = reinterpret_cast<uintptr_t>(aGraph);
        record.Caller = reinterpret_cast<uintptr_t>(_ReturnAddress());
        for (size_t i = 0; i + 1 < sizeof(record.Name) && name[i]; ++i)
            record.Name[i] = name[i];
    }
    const bool result = s_sendEvent(aGraph, aEvent);
    if (trace)
    {
        record.Result = result;
        Enqueue(record);
    }
    return result;
}

static TiltedPhoques::Initializer s_hooks([]()
{
    // 1.7.104 corpus plus callers/binder: 54732/140A01930 forwards four
    // arguments to 140A12E60 and returns its bool in AL. 37488/140687B40
    // takes (Actor*, bool). 63591/140BCF690 takes (graph, event&) -> bool.
    POINTER_SKYRIMSE(std::remove_pointer_t<PlayIdle>, playIdle, 54732);
    POINTER_SKYRIMSE(std::remove_pointer_t<SetRestrained>, restrained, 37488);
    POINTER_SKYRIMSE(std::remove_pointer_t<SendEvent>, sendEvent, 63591);
    POINTER_SKYRIMSE(std::remove_pointer_t<SendAction>, sendAction, 43567);
    s_playIdle = playIdle.Get();
    s_setRestrained = restrained.Get();
    s_sendEvent = sendEvent.Get();
    s_sendAction = sendAction.Get();
    TP_HOOK(&s_playIdle, HookPlayIdle);
    TP_HOOK(&s_setRestrained, HookSetRestrained);
    TP_HOOK(&s_sendEvent, HookSendEvent);
    TP_HOOK(&s_sendAction, HookSendAction);
});
}

PlayerStateTrace::PlayerStateTrace(World& aWorld, entt::dispatcher& aDispatcher) noexcept
    : m_world(aWorld)
    , m_updateConnection(aDispatcher.sink<UpdateEvent>().connect<&PlayerStateTrace::OnUpdate>(this))
    , m_actionConnection(aDispatcher.sink<ActionEvent>().connect<&PlayerStateTrace::OnAction>(this))
{
}

PlayerStateTrace::~PlayerStateTrace()
{
    s_enabled.store(false, std::memory_order_relaxed);
}

void PlayerStateTrace::OnAction(const ActionEvent& aAction) noexcept
{
    if (!s_enabled.load(std::memory_order_relaxed))
        return;
    const auto* name = aAction.EventName.c_str();
    if (!name || std::strncmp(name, "Offset", 6) != 0)
        return;
    Record record{};
    record.Type = Record::Published;
    record.Tick = GetTickCount64();
    record.Graph = aAction.Tick; // Wire tick, not an engine pointer for this record kind.
    record.Actor = aAction.ActorId;
    record.Form = aAction.IdleId;
    record.Input = aAction.Type;
    record.Before = aAction.State1;
    record.After = aAction.State2;
    record.Stack = static_cast<uint32_t>(aAction.Variables.Booleans.size());
    record.SomeFlag = static_cast<uint32_t>(aAction.Variables.Integers.size());
    record.SomeFlagAfter = static_cast<uint32_t>(aAction.Variables.Floats.size());
    for (size_t i = 0; i + 1 < sizeof(record.Name) && name[i]; ++i)
        record.Name[i] = name[i];
    Enqueue(record);
}

PlayerStateTrace::Snapshot PlayerStateTrace::ReadSnapshot(Actor* aActor) const noexcept
{
    Snapshot result{};
    result.Actor = aActor->formID;
    result.Life = (aActor->actorState.flags1 >> 21) & 0xF;
    result.Node = reinterpret_cast<uintptr_t>(aActor->GetNiNode());
    BSAnimationGraphManager* manager{};
    aActor->animationGraphHolder.GetBSAnimationGraph(&manager);
    if (manager)
    {
        {
            // Native 140554220 releases the reference returned by holder slot
            // 02. Copy identities while locked; log only after releasing it.
            BSScopedLock<BSRecursiveLock> lock(manager->lock);
            result.Manager = reinterpret_cast<uintptr_t>(manager);
            result.Index = manager->animationGraphIndex;
            result.Count = manager->animationGraphs.size;
            for (uint32_t i = 0; i < result.Count && i < result.Graphs.size(); ++i)
            {
                auto* graph = manager->animationGraphs.Get(i);
                result.Graphs[i] = reinterpret_cast<uintptr_t>(graph);
                result.Behaviors[i] = graph ? reinterpret_cast<uintptr_t>(graph->behaviorGraph) : 0;
            }
        }
        manager->Release();
    }
    return result;
}

void PlayerStateTrace::OnUpdate(const UpdateEvent&) noexcept
{
    const bool armed = ObjectService::IsRenderDiagnosticsArmed();
    if (armed != m_armed)
    {
        s_enabled.store(false, std::memory_order_relaxed);
        {
            std::lock_guard lock(s_mutex);
            s_count = 0;
        }
        s_dropped.store(0, std::memory_order_relaxed);
        m_records = m_snapshots = 0;
        m_nextSample = 0;
        m_previous = {};
        m_armed = armed;
        spdlog::info("Player state trace: armed={} tick={} replay=disabled limits=32-actors/4-graphs/2048-native/2048-snapshots", armed, GetTickCount64());
        s_enabled.store(armed, std::memory_order_relaxed);
    }
    if (!armed)
        return;

    const auto now = GetTickCount64();
    if (now < m_nextSample)
        return;
    m_nextSample = now + 250;

    std::array<Record, 256> records{};
    size_t count{};
    {
        std::lock_guard lock(s_mutex);
        count = s_count;
        std::copy_n(s_records.begin(), count, records.begin());
        s_count = 0;
    }
    if (const auto dropped = s_dropped.exchange(0, std::memory_order_relaxed))
        spdlog::warn("Player state trace: dropped={} evidence-incomplete", dropped);
    for (size_t i = 0; i < count && m_records < 2048; ++i, ++m_records)
    {
        const auto& r = records[i];
        if (r.Type == Record::Event)
            spdlog::info("Player state trace: offset tick={} thread={} graph={:X} event={} accepted={} caller={:X}",
                r.Tick, r.Thread, r.Graph, r.Name, r.Result, r.Caller);
        else if (r.Type == Record::Action)
            spdlog::info("Player state trace: offset-action tick={} thread={} actor={:X} event={} idle={:X} input={} someFlag={}->{} dispatchType={} accepted={} caller={:X}",
                r.Tick, r.Thread, r.Actor, r.Name, r.Form, r.Input, r.SomeFlag, r.SomeFlagAfter,
                r.Input | (r.SomeFlag ? 4u : 0u), r.Result, r.Caller);
        else if (r.Type == Record::Published)
            spdlog::info("Player state trace: offset-published tick={} wireTick={} actor={:X} event={} idle={:X} wireType={} flags={:X},{:X} variables={},{},{}",
                r.Tick, r.Graph, r.Actor, r.Name, r.Form, r.Input, r.Before, r.After,
                r.Stack, r.SomeFlag, r.SomeFlagAfter);
        else if (r.Type == Record::Idle)
            spdlog::info("Player state trace: PlayIdle tick={} thread={} actor={:X} idle={:X} stack={} accepted={} state={:X}->{:X}",
                r.Tick, r.Thread, r.Actor, r.Form, r.Stack, r.Result, r.Before, r.After);
        else
            spdlog::info("Player state trace: SetRestrained tick={} thread={} actor={:X} value={} caller={:X} state={:X}->{:X}",
                r.Tick, r.Thread, r.Actor, r.Flag, r.Caller, r.Before, r.After);
    }
    if (m_records == 2048 && s_enabled.exchange(false, std::memory_order_relaxed))
        spdlog::warn("Player state trace: native budget exhausted; evidence-incomplete until rearmed");

    if (m_snapshots >= 2048)
        return;
    const auto* ui = UI::Get();
    const bool creator = ui && ui->GetMenuOpen(BSFixedString("RaceSex Menu"));
    if (creator != m_creator)
    {
        m_creator = creator;
        spdlog::info("Player state trace: creator tick={} open={}", now, creator);
    }
    size_t slot{};
    const auto sample = [&](Actor* actor)
    {
        if (!actor || slot == m_previous.size())
            return;
        const auto state = ReadSnapshot(actor);
        auto& previous = m_previous[slot++];
        if (state == previous)
            return;
        previous = state;
        ++m_snapshots;
        spdlog::info("Player state trace: graph tick={} actor={:X} local={} leader={} life={} node={:X} manager={:X} index={} count={} graphs={:X},{:X},{:X},{:X} behaviors={:X},{:X},{:X},{:X}",
            now, state.Actor, actor == PlayerCharacter::Get(), m_world.GetPartyService().IsLeader(), state.Life,
            state.Node, state.Manager, state.Index, state.Count,
            state.Graphs[0], state.Graphs[1], state.Graphs[2], state.Graphs[3],
            state.Behaviors[0], state.Behaviors[1], state.Behaviors[2], state.Behaviors[3]);
    };
    sample(PlayerCharacter::Get());
    auto view = m_world.view<FormIdComponent, PlayerComponent>();
    for (auto entity : view)
    {
        if (slot == m_previous.size())
            break;
        const auto id = view.get<FormIdComponent>(entity).Id;
        if (id != 0x14)
            sample(Cast<Actor>(TESForm::GetById(id)));
    }
    if (m_snapshots >= 2048)
        spdlog::warn("Player state trace: snapshot budget exhausted; evidence-incomplete until rearmed");
}
