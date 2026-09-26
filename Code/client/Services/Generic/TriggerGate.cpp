#include <Services/TriggerGate.h>

#include <World.h>
#include <Components.h>
#include <Events/UpdateEvent.h>
#include <Messages/RequestPartyUnstuck.h>
#include <Messages/NotifyPartyUnstuck.h>
#include <PlayerCharacter.h>
#include <Forms/TESObjectCELL.h>
#include <Forms/TESWorldSpace.h>
#include <Misc/GameVM.h>
#include <Interface/UI.h>
#include <BSGraphics/BSGraphicsRenderer.h>
#include <Structs/GridCellCoords.h>
#include <FunctionHook.hpp>

#include <array>
#include <atomic>
#include <cmath>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <unordered_set>

namespace
{
// Research (1.7.104): the phantom update (26036, 1404066c0) walks overlapping
// bodies, resolves the trigger and entrant handles, and dispatches enter/leave.
// This preserves rotated/large primitive occupancy without guessed Havok bounds.
// SkyrimVM trigger/enter/leave sinks are IDs 54000/54001/54002,
// VAs 1409e0000/1409e01a0/1409e0340. Each sends to scripts, then RelayEvent
// (54033, 1409e3580) sends to aliases. Hook the VM sinks, not the dispatcher.
// CommonLibSSE-NG Object{,TypeInfo}.h and IVirtualMachine.h supply the layouts:
// https://github.com/alandtse/CommonLibSSE-NG/tree/main/include/RE
// SkyMP EventHandler.cpp observes these events but cannot defer Papyrus delivery.
// ForEachBoundObject (104791, 1414b2600) holds the attached-script lock while
// visiting Object* at slot 1, with a second bool argument; 1 continues iteration.
// ForEachAlias (19656, 1402e6a70) uses ExtraAliasInstanceArray's read lock
// (12720, 140187920); EventRelayFunctor (1409e33e0) uses VM type 0x8c for aliases.
// Read script declarations rather than live Papyrus variables, which workers mutate.
// This is capability classification, not whole-program Papyrus analysis: dynamic
// quest lookups can escape it, and an ambient script holding a Quest can match.
// Helgen source: MQ101DASetStage25 (10908A), defaultsetStageTrigSCRIPT, advances
// MQ101DragonAttack to 25/Fragment_310, closing TowerDoor and enabling TowerCollision;
// stage 50/Fragment_228 repeats the closure. run4/host.log:21254 ends at MQ101 160,
// so the reported closure is not confirmed by that log. No quest IDs are gated here.
// Separately, MQ101 250/Fragment_44 blocks both keep interior doors; the
// MQ101KeepDoorScript alias displays the blocked-exit message in OnActivate.
// Reviewer A reviewed the ABI and implementation. Its suggested demotion flush was
// rejected: a former leader must not execute world scripts after authority changes.
// Its repeat-only hold, smart-pointer move and cooldown findings were addressed.
// Reviewer B could not start (auth.json access denied). docs/REFERENCE_RESEARCH.md is
// outside this task's edit list; these references are retained here for integration.
struct ScriptType
{
    uint64_t RefCount;
    const char* Name;
    ScriptType* Parent;
    const char* Documentation;
    uint32_t Counts;
    uint32_t PropertyCounts;
    uint32_t FunctionCounts;
    uint32_t Padding;
    const uint8_t* Data;
};
static_assert(sizeof(ScriptType) == 0x38);

struct ScriptVariable
{
    const char* Name;
    uintptr_t Type;
};
static_assert(sizeof(ScriptVariable) == 0x10);

bool IsQuestType(const ScriptType* apType) noexcept
{
    for (unsigned depth = 0; apType && depth < 64; ++depth, apType = apType->Parent)
    {
        if (apType->Name && (!_stricmp(apType->Name, "Quest") ||
            !_stricmp(apType->Name, "ReferenceAlias")))
            return true;
    }
    return false;
}

struct ScriptVisitor
{
    virtual ~ScriptVisitor() = default;
    virtual uint32_t Visit(const void* apObject, bool)
    {
        if (!apObject)
            return 1;
        auto* pType = *reinterpret_cast<ScriptType* const*>(
            static_cast<const uint8_t*>(apObject) + 8);
        for (unsigned depth = 0; pType && depth < 64; ++depth, pType = pType->Parent)
        {
            if ((pType->Counts & 3) != 3)
                break;
            if (pType->Name && (!_strnicmp(pType->Name, "defaultSetStageTRIG", 19) ||
                !_stricmp(pType->Name, "ReferenceAlias")))
                Important = true;
            if (!pType->Data)
                continue;
            const auto count = (pType->Counts >> 8) & 0x3ff;
            const auto flags = (pType->Counts >> 2) & 0x3f;
            auto* pVariables = reinterpret_cast<const ScriptVariable*>(pType->Data + flags * 8);
            for (uint32_t i = 0; i < count && !Important; ++i)
            {
                const auto type = pVariables[i].Type;
                if (type >= 16 && IsQuestType(reinterpret_cast<const ScriptType*>(type & ~uintptr_t{1})))
                    Important = true;
            }
        }
        return Important ? 0 : 1;
    }
    bool Important{};
};

void VisitScripts(void* apObject, uint32_t aType, ScriptVisitor& aVisitor)
{
    auto* pVm = GameVM::Get()->virtualMachine;
    auto* pPolicy = pVm->GetObjectHandlePolicy();
    using HandleFn = uint64_t (*)(void*, uint32_t, const void*);
    const auto getHandle = reinterpret_cast<HandleFn>((*reinterpret_cast<void***>(pPolicy))[4]);
    const auto handle = getHandle(pPolicy, aType, apObject);
    using VisitFn = void(void*, uint64_t, ScriptVisitor*);
    POINTER_SKYRIMSE(VisitFn, visit, 104791);
    visit.Get()(pVm, handle, &aVisitor);
}

struct AliasVisitor
{
    virtual ~AliasVisitor() = default;
    virtual uint32_t Visit(void* apAlias)
    {
        VisitScripts(apAlias, 0x8c, Scripts);
        return Scripts.Important ? 0 : 1;
    }
    ScriptVisitor Scripts;
};

bool IsStoryTrigger(TESObjectREFR* apTrigger)
{
    if (!apTrigger->baseForm || apTrigger->baseForm->formType != FormType::Activator)
        return false;
    ScriptVisitor scripts;
    VisitScripts(apTrigger, static_cast<uint32_t>(apTrigger->formType), scripts);
    if (scripts.Important)
        return true;
    AliasVisitor aliases;
    using VisitFn = void(TESObjectREFR*, AliasVisitor*);
    POINTER_SKYRIMSE(VisitFn, visit, 19656);
    visit.Get()(apTrigger, &aliases);
    return aliases.Scripts.Important;
}

enum : uint8_t { kTrigger, kEnter, kLeave };
constexpr std::array<size_t, 3> kSinkOffsets{0x158, 0x160, 0x168};
using SinkFn = BSTEventResult (*)(void*, const TESTriggerEnterEvent*, const void*);
std::array<SinkFn, 3> s_original{};
std::atomic<TriggerGate*> s_gate{};

template <uint8_t Kind>
BSTEventResult HookTrigger(void* apSink, const TESTriggerEnterEvent* apEvent, const void* apSource)
{
    auto* pGate = s_gate.load(std::memory_order_acquire);
    if (pGate && apEvent && pGate->Hold(Kind, apEvent->pTrigger, apEvent->pActionRef))
        return BSTEventResult::kOk;
    return s_original[Kind](apSink, apEvent, apSource);
}

void InstallHooks()
{
    static bool attempted = false;
    if (attempted || !GameVM::Get() || !GameVM::Get()->virtualMachine)
        return;
    attempted = true;
    using Sink = std::remove_pointer_t<SinkFn>;
    POINTER_SKYRIMSE(Sink, trigger, 54000);
    POINTER_SKYRIMSE(Sink, enter, 54001);
    POINTER_SKYRIMSE(Sink, leave, 54002);
    const std::array<SinkFn, 3> expected{trigger.Get(), enter.Get(), leave.Get()};
    const std::array<SinkFn, 3> hooks{HookTrigger<kTrigger>, HookTrigger<kEnter>, HookTrigger<kLeave>};
    auto* pVm = reinterpret_cast<uint8_t*>(GameVM::Get());
    for (size_t i = 0; i < 3; ++i)
    {
        const auto actual = reinterpret_cast<SinkFn>((*reinterpret_cast<void***>(pVm + kSinkOffsets[i]))[1]);
        if (actual != expected[i])
        {
            spdlog::error("Trigger gate: unexpected VM sink {}; hooks not installed", i);
            return;
        }
    }
    s_original = expected;
    for (size_t i = 0; i < 3; ++i)
        TiltedPhoques::HookVTable(pVm + kSinkOffsets[i], 1, hooks[i]);
}

struct Location
{
    glm::vec3 Position{};
    uint32_t Cell{};
    uint32_t Worldspace{};
    uint32_t Handle{};
};

Location GetLocation(TESObjectREFR* apRef)
{
    if (!apRef || !apRef->parentCell)
        return {};
    const auto* pWorld = apRef->GetWorldSpace();
    return {apRef->position, apRef->parentCell->formID, pWorld ? pWorld->formID : 0,
        apRef->GetHandle().handle.iBits};
}

bool SameSpace(const Location& a, const Location& b)
{
    return a.Cell && b.Cell && (a.Worldspace ? a.Worldspace == b.Worldspace :
        !b.Worldspace && a.Cell == b.Cell);
}

struct Reference
{
    explicit Reference(TESObjectREFR* apRef) : Ref(apRef) {}
    ~Reference() { if (Ref) Ref->handleRefObject.DecRefHandle(); }
    Reference(const Reference&) = delete;
    Reference& operator=(const Reference&) = delete;
    operator TESObjectREFR*() const { return Ref; }
    TESObjectREFR* operator->() const { return Ref; }
    TESObjectREFR* Ref;
};

Reference Resolve(uint32_t aHandle)
{
    // ID 17201 returns an owning smart pointer. Keep it alive through replay.
    TESObjectREFR* pRef{};
    using ResolveFn = void(uint32_t&, TESObjectREFR*&);
    POINTER_SKYRIMSE(ResolveFn, resolve, 17201);
    resolve.Get()(aHandle, pRef);
    return Reference(pRef);
}

bool InWorld()
{
    auto* pUI = UI::Get();
    return pUI && !pUI->GetMenuOpen(BSFixedString("Loading Menu")) &&
        !pUI->GetMenuOpen(BSFixedString("Main Menu")) &&
        !pUI->GetMenuOpen(BSFixedString("RaceSex Menu"));
}
}

struct TriggerGate::State
{
    struct Held
    {
        uint32_t Handle{};
        uint32_t FormId{};
        uint64_t Order{};
        bool Enter{};
        bool Trigger{};
        bool Left{};
        std::unordered_set<uint32_t> Members;
        uint64_t Since{};
    };
    std::mutex Mutex;
    std::mutex MoveMutex;
    bool Leader{};
    bool InParty{};
    uint64_t Epoch{};
    uint64_t Generation{};
    uint64_t NextHold{};
    uint32_t LeaderId{};
    uint32_t PlayerHandle{};
    uint64_t SampleTime{};
    std::unordered_map<uint32_t, Location> Players;
    std::unordered_map<uint32_t, Held> Pending;
    std::unordered_map<uint32_t, std::unordered_set<uint32_t>> Inside;
    uint64_t NextNotice{};
    uint64_t NextUnstuck{};
    uint64_t Sequence{};
    uint64_t ReceivedSequence{};
    uint64_t ReceivedEpoch{};
    uint32_t ReceivedLeader{};
    bool KeyDown{true};
    bool Focused{};
    std::optional<NotifyPartyUnstuck> Move;
    uint64_t MoveExpires{};

    bool AllPresent(const Location& aTrigger) const
    {
        if (GetTickCount64() - SampleTime > 500)
            return false;
        const auto inside = Inside.find(aTrigger.Handle);
        const auto leader = Players.find(LeaderId);
        for (const auto& [id, player] : Players)
        {
            if (!SameSpace(player, aTrigger) || !player.Handle)
                return false;
            const auto delta = player.Position - aTrigger.Position;
            if (glm::dot(delta, delta) <= 400.f * 400.f)
                continue;
            // With the leader: a remote player's copy does not raise enter events here, so a big trigger
            // (the intro dragon's landing box) was held for minutes with everyone standing together.
            if (leader != Players.end() && SameSpace(player, leader->second))
            {
                const auto toLeader = player.Position - leader->second.Position;
                if (glm::dot(toLeader, toLeader) <= 600.f * 600.f)
                    continue;
            }
            if (inside == Inside.end() || !inside->second.contains(player.Handle))
                return false;
        }
        return true;
    }
};

TriggerGate::TriggerGate(World& aWorld, entt::dispatcher& aDispatcher, TransportService& aTransport) noexcept
    : m_state(std::make_unique<State>())
    , m_world(aWorld)
    , m_transport(aTransport)
    , m_updateConnection(aDispatcher.sink<UpdateEvent>().connect<&TriggerGate::OnUpdate>(this))
    , m_unstuckConnection(aDispatcher.sink<NotifyPartyUnstuck>().connect<&TriggerGate::OnUnstuck>(this))
{
    s_gate.store(this, std::memory_order_release);
    EventDispatcherManager::Get()->loadGameEvent.RegisterSink(this);
}

TriggerGate::~TriggerGate() noexcept
{
    s_gate.store(nullptr, std::memory_order_release);
    EventDispatcherManager::Get()->loadGameEvent.UnRegisterSink(this);
}

BSTEventResult TriggerGate::OnEvent(const TESLoadGameEvent*, const EventDispatcher<TESLoadGameEvent>*)
{
    std::lock_guard lock(m_state->Mutex);
    ++m_state->Generation;
    if (!m_state->Pending.empty())
        spdlog::warn("Trigger gate: canceled {} held triggers after loading a save", m_state->Pending.size());
    m_state->Pending.clear();
    m_state->Inside.clear();
    m_state->Players.clear();
    m_state->Leader = false;
    std::lock_guard moveLock(m_state->MoveMutex);
    m_state->Move.reset();
    return BSTEventResult::kOk;
}

bool TriggerGate::Hold(uint8_t aKind, TESObjectREFR* apTrigger, TESObjectREFR* apActor) noexcept
{
    if (!apTrigger || !apActor)
        return false;
    std::unique_lock lock(m_state->Mutex);
    auto& state = *m_state;
    if (!state.Leader || !state.InParty || state.Players.size() < 2)
        return false;
    const auto actorHandle = apActor->GetHandle().handle.iBits;
    const bool partyActor = std::any_of(state.Players.begin(), state.Players.end(),
        [actorHandle](const auto& entry) { return actorHandle && entry.second.Handle == actorHandle; });
    if (!partyActor)
        return false;
    const auto trigger = GetLocation(apTrigger);
    if (!trigger.Handle)
        return false;
    if (aKind == kLeave)
    {
        if (auto inside = state.Inside.find(trigger.Handle); inside != state.Inside.end())
        {
            inside->second.erase(actorHandle);
            if (inside->second.empty())
                state.Inside.erase(inside);
        }
    }
    else
        state.Inside[trigger.Handle].insert(actorHandle);
    // Remote actors and NPCs always retain their native event delivery.
    if (apActor != PlayerCharacter::Get())
        return false;
    auto held = state.Pending.find(trigger.Handle);
    if (held == state.Pending.end())
    {
        // A contact whose enter already ran must never be gated later merely
        // because a follower walks away. Native 26041 queues enter for new contacts;
        // 26038 emits repeating OnTrigger only for contacts already recorded.
        if (aKind != kEnter || state.AllPresent(trigger))
            return false;
        // Never acquire VM/alias locks while holding our snapshot mutex.
        const auto epoch = state.Epoch;
        const auto leaderId = state.LeaderId;
        const auto generation = state.Generation;
        lock.unlock();
        const bool important = IsStoryTrigger(apTrigger);
        lock.lock();
        if (!important || !state.Leader || !state.InParty || epoch != state.Epoch ||
            leaderId != state.LeaderId || generation != state.Generation)
            return false;
        State::Held item{trigger.Handle, apTrigger->formID, ++state.NextHold};
        item.Since = GetTickCount64();
        for (const auto& [id, location] : state.Players)
            item.Members.insert(id);
        held = state.Pending.emplace(trigger.Handle, std::move(item)).first;
        spdlog::info("Trigger gate: holding {:X} for {} players", apTrigger->formID, state.Players.size());
    }
    if (aKind == kEnter)
    {
        held->second.Enter = true;
        held->second.Left = false;
    }
    else if (aKind == kTrigger)
        held->second.Trigger = true;
    else
        held->second.Left = true;
    return true;
}

void TriggerGate::OnUpdate(const UpdateEvent&) noexcept
{
    InstallHooks();
    auto& state = *m_state;
    const auto& party = m_world.GetPartyService();
    const auto now = GetTickCount64();
    const bool active = InWorld();
    const bool inParty = m_transport.IsConnected() && party.IsInParty();
    const bool leader = inParty && party.IsLeader();
    const bool keyDown = (GetAsyncKeyState(VK_F8) & 0x8000) != 0;
    const auto* pWindow = BSGraphics::GetMainWindow();
    const bool focused = pWindow && GetForegroundWindow() == pWindow->hWnd;
    const bool pressed = focused && state.Focused && keyDown && !state.KeyDown;
    state.KeyDown = keyDown;
    state.Focused = focused;

    std::unordered_map<uint32_t, Location> players;
    if (leader && active)
    {
        for (const auto id : party.GetPartyMembers())
            players.emplace(id, Location{});
        players[m_transport.GetLocalPlayerId()] = GetLocation(PlayerCharacter::Get());
        auto view = m_world.view<PlayerComponent, RemoteComponent, FormIdComponent>();
        for (const auto entity : view)
        {
            const auto id = view.get<PlayerComponent>(entity).Id;
            if (players.contains(id))
                players[id] = GetLocation(Cast<Actor>(TESForm::GetById(view.get<FormIdComponent>(entity).Id)));
        }
    }
    std::vector<State::Held> release;
    bool notice = false;
    {
        std::lock_guard lock(state.Mutex);
        const bool changedAuthority = inParty && state.InParty &&
            (state.Epoch != party.GetStartEpoch() || state.LeaderId != party.GetLeaderPlayerId());
        if (changedAuthority || (inParty && !leader))
        {
            ++state.Generation;
            if (!state.Pending.empty())
                spdlog::warn("Trigger gate: canceled {} held triggers after authority changed", state.Pending.size());
            state.Pending.clear();
            state.Inside.clear();
        }
        if (!inParty || changedAuthority)
        {
            std::lock_guard moveLock(state.MoveMutex);
            state.Move.reset();
            state.ReceivedSequence = 0;
            state.ReceivedLeader = 0;
        }
        state.Leader = leader && active;
        state.InParty = inParty;
        state.Epoch = party.GetStartEpoch();
        state.LeaderId = party.GetLeaderPlayerId();
        state.PlayerHandle = PlayerCharacter::Get() ? PlayerCharacter::Get()->GetHandle().handle.iBits : 0;
        state.Players = std::move(players);
        state.SampleTime = now;
        // Loading suspends the wait; it does not release story events early.
        for (auto it = state.Pending.begin(); active && it != state.Pending.end();)
        {
            auto pTrigger = Resolve(it->first);
            if (!pTrigger || pTrigger->IsDeleted() || pTrigger->IsDisabled())
            {
                spdlog::warn("Trigger gate: canceled {:X}, trigger unavailable or disabled", it->second.FormId);
                it = state.Pending.erase(it);
                continue;
            }
            const bool left = !inParty || std::any_of(it->second.Members.begin(), it->second.Members.end(),
                [&state](uint32_t id) { return !state.Players.contains(id); });
            if (!inParty || state.AllPresent(GetLocation(pTrigger)))
            {
                spdlog::info("Trigger gate: {:X} released ({})", it->second.FormId,
                    left ? "player left the party" : "all players in");
                release.push_back(it->second);
                it = state.Pending.erase(it);
            }
            else
                ++it;
        }
        // Only when it is needed: a trigger held for a while with a player actually left behind.
        const bool someoneBehind = std::any_of(state.Pending.begin(), state.Pending.end(),
            [now](const auto& aHeld) { return aHeld.second.Since && now - aHeld.second.Since > 8000; });
        notice = active && leader && someoneBehind && now >= state.NextNotice;
        if (notice)
            state.NextNotice = now + 10000;
    }
    // Native sends can acquire VM locks and must run outside the snapshot lock.
    std::sort(release.begin(), release.end(), [](const auto& a, const auto& b) { return a.Order < b.Order; });
    for (const auto& held : release)
    {
        auto pTrigger = Resolve(held.Handle);
        auto pPlayer = Resolve(state.PlayerHandle);
        if (!pTrigger || !pPlayer || !GameVM::Get())
            continue;
        const TESTriggerEnterEvent event{pTrigger, pPlayer};
        const std::array<bool, 3> send{held.Trigger, held.Enter, held.Left};
        for (const uint8_t kind : {kEnter, kTrigger, kLeave})
        {
            if (send[kind])
                s_original[kind](reinterpret_cast<uint8_t*>(GameVM::Get()) + kSinkOffsets[kind], &event, nullptr);
        }
    }
    if (notice)
    {
        using ShowFn = void(const char*, const char*, bool);
        POINTER_SKYRIMSE(ShowFn, show, 52933);
        show.Get()("Waiting for the other players. F8: bring them here", nullptr, true);
    }
    if (pressed && leader && active && now >= state.NextUnstuck)
        RequestUnstuck();

    std::optional<NotifyPartyUnstuck> pendingMove;
    {
        std::lock_guard moveLock(state.MoveMutex);
        if (state.Move && (now > state.MoveExpires || !inParty || leader ||
            state.Move->LeaderId != party.GetLeaderPlayerId() || state.Move->Move.Epoch != party.GetStartEpoch()))
        {
            state.Move.reset();
        }
        if (state.Move && active && PlayerCharacter::Get() && PlayerCharacter::Get()->parentCell)
            pendingMove = std::exchange(state.Move, std::nullopt);
    }
    if (pendingMove)
    {
        const auto& message = *pendingMove;
        auto& mods = m_world.GetModSystem();
        auto* pCell = Cast<TESObjectCELL>(TESForm::GetById(mods.GetGameId(message.Move.CellId)));
        if (message.Move.WorldSpaceId)
        {
            auto* pWorld = Cast<TESWorldSpace>(TESForm::GetById(mods.GetGameId(message.Move.WorldSpaceId)));
            if (!pWorld)
                return;
            const auto grid = GridCellCoords::CalculateGridCellCoords(message.Move.Position);
            pCell = pWorld->LoadCell(grid.X, grid.Y);
        }
        if (!pCell)
            return;
        // Compact rows keep the first five players close even in small interiors.
        const float back = 64.f + 48.f * static_cast<float>(message.Slot / 3);
        const float side = message.Slot % 3 == 0 ? 0.f : (message.Slot % 3 == 1 ? -40.f : 40.f);
        const float heading = message.Move.Heading;
        NiPoint3 position(message.Move.Position);
        position.x += -std::sin(heading) * back + std::cos(heading) * side;
        position.y += -std::cos(heading) * back - std::sin(heading) * side;
        // Existing native player teleport: ID 56626, VA 140a5d370 queues the
        // player's cell transition. Arrival is confirmed by normal movement replication.
        PlayerCharacter::Get()->MoveTo(pCell, position);
        spdlog::info("Unstuck: applying host move sequence={} cell={:X}", message.Move.Sequence, pCell->formID);
    }
}

void TriggerGate::RequestUnstuck() noexcept
{
    auto* pPlayer = PlayerCharacter::Get();
    if (!pPlayer || !pPlayer->parentCell || m_world.GetPartyService().GetPartyMembers().size() < 2)
        return;
    RequestPartyUnstuck request;
    auto& move = request.Move;
    move.Epoch = m_world.GetPartyService().GetStartEpoch();
    move.Sequence = ++m_state->Sequence;
    move.Position = pPlayer->position;
    move.Heading = std::remainder(pPlayer->rotation.z, 6.283185307f);
    auto& mods = m_world.GetModSystem();
    if (!mods.GetServerModId(pPlayer->parentCell->formID, move.CellId))
        return;
    if (const auto* pWorld = pPlayer->GetWorldSpace(); pWorld &&
        !mods.GetServerModId(pWorld->formID, move.WorldSpaceId))
        return;
    if (move.IsValid() && m_transport.Send(request))
    {
        m_state->NextUnstuck = GetTickCount64() + 2000;
        spdlog::info("Unstuck: host requested followers sequence={}", move.Sequence);
    }
}

void TriggerGate::OnUnstuck(const NotifyPartyUnstuck& acMessage) noexcept
{
    const auto& party = m_world.GetPartyService();
    std::lock_guard moveLock(m_state->MoveMutex);
    if (m_state->ReceivedLeader != party.GetLeaderPlayerId() || m_state->ReceivedEpoch != party.GetStartEpoch())
    {
        m_state->ReceivedSequence = 0;
        m_state->ReceivedLeader = party.GetLeaderPlayerId();
        m_state->ReceivedEpoch = party.GetStartEpoch();
    }
    if (!m_transport.IsConnected() || !party.IsInParty() || party.IsLeader() ||
        acMessage.LeaderId != party.GetLeaderPlayerId() || acMessage.Move.Epoch != party.GetStartEpoch() ||
        !acMessage.Move.IsValid() || acMessage.Move.Sequence <= m_state->ReceivedSequence ||
        acMessage.Slot >= party.GetPartyMembers().size())
        return;
    m_state->ReceivedSequence = acMessage.Move.Sequence;
    m_state->Move = acMessage;
    m_state->MoveExpires = GetTickCount64() + 10000;
}
