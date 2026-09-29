#include <TiltedOnlinePCH.h>
#include <Services/WorldStateService.h>
#include <World.h>
#include <Events/UpdateEvent.h>
#include <Events/DisconnectedEvent.h>
#include <Messages/RequestWorldState.h>
#include <Messages/RequestWorldStateCell.h>
#include <Messages/NotifyWorldState.h>
#include <Games/References.h>
#include <Games/TES.h>
#include <Forms/TESObjectCELL.h>
#include <Forms/TESWorldSpace.h>
#include <Misc/GameVM.h>
#include <atomic>
#include <WorldAnimation.h>

namespace
{
std::atomic<WorldStateService*> s_service{};
std::atomic<DWORD> s_mainThread{};
std::atomic<uint64_t> s_lastPump{};
std::atomic<uint64_t> s_captureEpoch{};
std::atomic<uint64_t> s_captureSerial{};
std::atomic<bool> s_active{}, s_applying{};
std::mutex s_observationMutex;
std::map<WorldStateTable::Key, WorldState> s_observations;
std::deque<WorldState> s_animationEvents;
std::map<uint32_t, size_t> s_animationPending;
std::set<uint32_t> s_animationDirty;
std::set<uint32_t> s_animationKnown;
std::set<uint32_t> s_attached;
std::set<uint32_t> s_watched;
std::set<uint32_t> s_watchRequests;
std::atomic<bool> s_hasWatches{};
uint32_t s_diagnosticRequest{}, s_diagnosticId{};
std::string s_diagnostic;
using PhaseFn = void();
PhaseFn* s_phase{};
void HookPhase()
{
    s_phase();
    // 21873 clears per-thread frame caches. Its main-loop caller is unconditional,
    // after the worker joins. Other callers must not pump the world service.
    POINTER_SKYRIMSE(void, mainLoop, 36564);
    const auto caller = reinterpret_cast<uintptr_t>(_ReturnAddress());
    const auto begin = reinterpret_cast<uintptr_t>(mainLoop.Get());
    // 1.7.104 CALL 1406594A8 returns at 1406594AD (36564 + C3D).
    if (caller != begin + 0xC3D) return;
    auto* vm = SkyrimVM::Get();
    if (!vm || vm->inactive) return;
    s_mainThread.store(GetCurrentThreadId(), std::memory_order_release);
    WorldStateService::MainThreadUpdate();
}
static TiltedPhoques::Initializer s_hook([] {
    POINTER_SKYRIMSE(PhaseFn, phase, 21873);
    s_phase = phase.Get();
    TP_HOOK(&s_phase, HookPhase);
});
}

WorldStateService::WorldStateService(World& world, entt::dispatcher& dispatcher, TransportService& transport) noexcept
    : m_world(world), m_transport(transport)
    , m_updateConnection(dispatcher.sink<UpdateEvent>().connect<&WorldStateService::OnUpdate>(this))
    , m_disconnectConnection(dispatcher.sink<DisconnectedEvent>().connect<&WorldStateService::OnDisconnected>(this))
    , m_stateConnection(dispatcher.sink<NotifyWorldState>().connect<&WorldStateService::OnState>(this))
{ s_service.store(this, std::memory_order_release); }

bool WorldStateService::IsMainThread() noexcept { return s_mainThread.load(std::memory_order_acquire) == GetCurrentThreadId(); }
void WorldStateService::MainThreadUpdate() noexcept
{
    s_lastPump.store(GetTickCount64(), std::memory_order_release);
    if (auto* service = s_service.load(std::memory_order_acquire)) service->Pump();
}

// Set while the cell baseline sweep samples (main thread): its publishes are not live script changes.
thread_local bool s_baselineSampling{};

bool WorldStateService::Eligible(TESObjectREFR* ref) noexcept
{
    if (!ref || static_cast<uint8_t>(ref->formType) != 61 || !ref->baseForm ||
        !ref->formID || ref->IsTemporary() || ref->IsDeleted()) return false;
    for (unsigned depth = 0; depth < 32; ++depth)
    {
        const auto id = EnableParent(ref);
        if (!id) return true;
        if (id == 0x14 || id >= 0xFF000000) return false;
        ref = Cast<TESObjectREFR>(TESForm::GetById(id));
        if (!ref) return false;
    }
    return false;
}

void WorldStateService::Observe(TESObjectREFR* ref, WorldStateKind kind, uint32_t value, float scalar, const char* animation) noexcept
{
    const auto serial = s_captureSerial.load(std::memory_order_acquire);
    const auto epoch = s_captureEpoch.load(std::memory_order_acquire);
    // Main-thread sampling only. Hooks use ObserveId with the ID copied on entry.
    if (!IsMainThread() || !epoch || s_applying.load(std::memory_order_acquire) || !ref || !ref->formID || ref->formID >= 0xFF000000) return;
    WorldState state;
    state.Epoch = epoch; state.Reference = GameId{0, ref->formID};
    state.Kind = kind; state.Value = value; state.Scalar = scalar;
    state.Animation = animation ? animation : "";
    if (state.Animation.size() > 128) return;
    std::lock_guard lock(s_observationMutex);
    if (epoch == s_captureEpoch.load(std::memory_order_relaxed) &&
        serial == s_captureSerial.load(std::memory_order_relaxed))
    {
        const auto key = WorldStateTable::MakeKey(state);
        const auto previous = s_observations.find(key);
        if (kind == WorldStateKind::Open && (scalar == 1 || scalar == 3) && previous != s_observations.end() && previous->second.Scalar == 0)
            return; // A baseline cannot erase a live script command in this batch.
        s_observations.insert_or_assign(key, std::move(state));
    }
}
void WorldStateService::ObserveId(uint32_t id, WorldStateKind kind, uint32_t value, float scalar) noexcept
{
    const auto serial = s_captureSerial.load(std::memory_order_acquire);
    const auto epoch = s_captureEpoch.load(std::memory_order_acquire);
    if (!epoch || s_applying.load(std::memory_order_acquire) || !id || id >= 0xFF000000) return;
    WorldState state;
    state.Epoch = epoch; state.Reference = GameId{0, id};
    state.Kind = kind; state.Value = value; state.Scalar = scalar;
    std::lock_guard lock(s_observationMutex);
    if (epoch != s_captureEpoch.load(std::memory_order_relaxed) ||
        serial != s_captureSerial.load(std::memory_order_relaxed)) return;
    const auto key = WorldStateTable::MakeKey(state);
    const auto previous = s_observations.find(key);
    if (kind == WorldStateKind::Open && scalar == 1 && previous != s_observations.end() && previous->second.Scalar == 0) return;
    s_observations.insert_or_assign(key, std::move(state));
}
void WorldStateService::Attached(uint32_t id) noexcept
{
    if (!s_active.load(std::memory_order_acquire) || !id || id >= 0xFF000000) return;
    std::lock_guard lock(s_observationMutex);
    s_attached.insert(id);
    if (s_captureEpoch.load(std::memory_order_acquire) && s_animationKnown.count(id)) s_animationDirty.insert(id);
}

void WorldStateService::AnimationEvent(uint32_t id, const char* name) noexcept
{
    const auto serial = s_captureSerial.load(std::memory_order_acquire);
    const auto epoch = s_captureEpoch.load(std::memory_order_acquire);
    if (!epoch || s_applying.load(std::memory_order_acquire) || !id || id >= 0xFF000000 || !name) return;
    const auto size = strnlen(name, 129);
    if (!size || size > 128) return;
    WorldState state;
    state.Epoch = epoch; state.Reference = GameId{0, id};
    state.Kind = WorldStateKind::AnimationEvent; state.Animation = name;
    std::lock_guard lock(s_observationMutex);
    if (epoch != s_captureEpoch.load(std::memory_order_relaxed) ||
        serial != s_captureSerial.load(std::memory_order_relaxed)) return;
    s_animationEvents.push_back(std::move(state));
    ++s_animationPending[id]; s_animationDirty.insert(id);
}
void WorldStateService::AnimationDirty(uint32_t id) noexcept
{
    if (!s_captureEpoch.load(std::memory_order_acquire) || s_applying.load(std::memory_order_acquire) || !id || id >= 0xFF000000) return;
    std::lock_guard lock(s_observationMutex);
    s_animationDirty.insert(id);
}
void WorldStateService::AnimationKnown(uint32_t id) noexcept
{
    if (!id || id >= 0xFF000000) return;
    std::lock_guard lock(s_observationMutex);
    s_animationKnown.insert(id);
    if (s_captureEpoch.load(std::memory_order_acquire)) s_animationDirty.insert(id);
}
void WorldStateService::PumpAnimations() noexcept
{
    std::deque<WorldState> events;
    std::vector<uint32_t> dirty;
    {
        std::lock_guard lock(s_observationMutex);
        // Session/authority changes seed only references registered by native
        // graph construction, including graphs restored from a local save.
        // No cell/reference survey and no per-frame history traversal.
        for (unsigned budget = 0; m_animationSeeding && budget < 8; ++budget)
        {
            const auto it = s_animationKnown.upper_bound(m_animationSeedCursor);
            if (it == s_animationKnown.end()) { m_animationSeeding = false; break; }
            m_animationSeedCursor = *it; s_animationDirty.insert(*it);
        }
        for (unsigned budget = 0; budget < 32 && !s_animationEvents.empty(); ++budget)
        {
            auto& event = s_animationEvents.front();
            if (--s_animationPending[event.Reference.BaseId] == 0) s_animationPending.erase(event.Reference.BaseId);
            events.push_back(std::move(event)); s_animationEvents.pop_front();
        }
        // Inspect at most eight dirty IDs, with no all-reference or history scan.
        for (unsigned budget = 0; budget < 8 && !s_animationDirty.empty(); ++budget)
        {
            auto it = s_animationDirty.upper_bound(m_animationCursor);
            if (it == s_animationDirty.end()) it = s_animationDirty.begin();
            m_animationCursor = *it;
            dirty.push_back(*it); s_animationDirty.erase(it);
        }
    }
    for (auto& event : events) if (event.Epoch == m_authority.Epoch) Publish(std::move(event));
    for (const auto id : dirty)
    {
        auto* ref = Cast<TESObjectREFR>(TESForm::GetById(id));
        if (!WorldAnimation::Eligible(ref)) continue;
        WorldState snapshot;
        snapshot.Epoch = m_authority.Epoch; snapshot.Reference = GameId{0, id};
        snapshot.Kind = WorldStateKind::AnimationSnapshot; snapshot.Value = 1;
        if (!WorldAnimation::Capture(ref, snapshot.AnimationData)) continue; // attachment/state event wakes it
        {
            std::lock_guard lock(s_observationMutex);
            // A checkpoint cannot overtake an input still waiting to publish.
            if (s_animationPending.count(id)) { s_animationDirty.insert(id); continue; }
        }
        Publish(std::move(snapshot));
    }
}

void WorldStateService::OnState(const NotifyWorldState& message) noexcept
{
    const auto& party = m_world.GetPartyService();
    if (!m_transport.IsConnected() || !party.IsInParty() || party.IsLeader() ||
        message.LeaderId != party.GetLeaderPlayerId() || message.State.Epoch != party.GetStartEpoch() ||
        !message.State.Valid() || (!WorldStateTable::ShouldDeliverLive(message.State) && message.State.Scalar != 2)) return;
    auto state = message.State;
    state.Reference = GameId{0, m_world.GetModSystem().GetGameId(state.Reference)};
    state.Cell = GameId{0, m_world.GetModSystem().GetGameId(state.Cell)};
    if (!state.Reference.BaseId || !state.Cell.BaseId) return;
    std::lock_guard lock(m_mailboxMutex);
    m_incoming.push_back(std::move(state));
}
void WorldStateService::OnUpdate(const UpdateEvent&) noexcept
{
    const auto& party = m_world.GetPartyService();
    Authority authority;
    if (m_transport.IsConnected() && party.IsInParty() && party.GetSessionState() >= 2)
        authority = {party.GetStartEpoch(), party.GetLeaderPlayerId(), party.IsLeader()};
    const auto now = GetTickCount64();
    if (!authority.Epoch) { m_watchdogStart = now; m_watchdogReported = false; }
    else if (!m_watchdogStart) m_watchdogStart = now;
    else if (!m_watchdogReported && now - (std::max)(m_watchdogStart, s_lastPump.load(std::memory_order_acquire)) > 5000)
    {
        spdlog::error("World state pump stalled: epoch={} mainThread={} (no native main-loop tail in 5s)", authority.Epoch, s_mainThread.load());
        m_watchdogReported = true;
    }
    std::deque<WorldState> outgoing;
    std::set<uint32_t> requests;
    {
        std::lock_guard lock(m_mailboxMutex);
        authority.Generation = m_connectionGeneration;
        if (!(authority == m_requestedAuthority))
        {
            // Serialize the capture boundary with worker hook enqueueing. Known
            // graph IDs are a registry, retained for the new leader's seed pass.
            std::lock_guard observations(s_observationMutex);
            s_captureEpoch.store(0, std::memory_order_release);
            ++s_captureSerial;
            s_animationEvents.clear(); s_animationPending.clear(); s_animationDirty.clear();
            s_observations.clear(); s_attached.clear();
            m_outgoing.clear(); m_incoming.clear();
        }
        m_requestedAuthority = authority;
        WorldStateReplay::Drain(m_outgoing, outgoing);
        for (unsigned budget = 0; budget < 4 && !m_cellRequests.empty(); ++budget)
        {
            requests.insert(*m_cellRequests.begin()); m_cellRequests.erase(m_cellRequests.begin());
        }
    }
    s_captureEpoch.store(authority.IsLeader ? authority.Epoch : 0, std::memory_order_release);
    s_active.store(authority.Epoch != 0, std::memory_order_release);
    bool sendFailed = false;
    std::deque<WorldState> retry;
    for (auto& state : outgoing)
    {
        if (!authority.IsLeader || state.Epoch != authority.Epoch) continue;
        const auto local = state;
        if (!m_world.GetModSystem().GetServerModId(local.Reference.BaseId, state.Reference) ||
            !m_world.GetModSystem().GetServerModId(local.Cell.BaseId, state.Cell) || !state.Valid()) continue;
        RequestWorldState request; request.State = state;
        if (sendFailed || !m_transport.Send(request))
        {
            sendFailed = true; // reliable event order must survive a failed send
            retry.push_back(local);
        }
    }
    {
        std::lock_guard lock(m_mailboxMutex);
        // The native phase may have queued newer revisions during transmission.
        // Failed older messages must go in front of them, in original order.
        // A disconnect during Send must not resurrect the old connection's work.
        if (authority == m_requestedAuthority)
            WorldStateReplay::PrependFailed(m_outgoing, retry);
    }
    if (!authority.Epoch || authority.IsLeader) return;
    for (const auto cell : requests)
    {
        RequestWorldStateCell request; request.Epoch = authority.Epoch;
        if (m_world.GetModSystem().GetServerModId(cell, request.Cell) && !m_transport.Send(request))
        {
            std::lock_guard lock(m_mailboxMutex);
            m_cellRequests.insert(cell);
        }
    }
}
void WorldStateService::VisitCells() noexcept
{
    const auto now = GetTickCount64();
    if (now < m_nextCells) return;
    m_nextCells = now + 1000;
    std::set<uint32_t> present, scopes;
    auto visit = [&](TESObjectCELL* cell) {
        if (!cell || !cell->IsAttached() || !present.insert(cell->formID).second) return;
        const auto scope = cell->worldspace ? cell->worldspace->formID : cell->formID;
        scopes.insert(scope);
        if (m_cells.count(cell->formID)) return;
        if (!m_authority.IsLeader)
        {
            std::lock_guard lock(m_mailboxMutex);
            m_cellRequests.insert(scope);
            return;
        }
        m_baselines.push_back({cell->formID});
    };
    const auto* tes = TES::Get();
    if (!tes) return;
    visit(tes->interiorCell);
    if (tes->cells && tes->cells->arr)
        for (uint32_t i = 0; i < tes->cells->dimension * tes->cells->dimension; ++i) visit(tes->cells->arr[i]);
    // Only cell headers are reconciled, once per second. Baselines are budgeted.
    m_cells = std::move(present);
    const bool changedScope = m_scopes != scopes;
    m_scopes = std::move(scopes);
    if (changedScope && !m_authority.IsLeader) m_follower.EvictOutside(m_scopes);
}
void WorldStateService::Baseline() noexcept
{
    if (m_baselines.empty()) return;
    auto& work = m_baselines.front();
    auto* cell = Cast<TESObjectCELL>(TESForm::GetById(work.Cell));
    if (!cell || !cell->IsAttached()) { m_baselines.pop_front(); return; }
    bool done{};
    {
        BSScopedLock<BSRecursiveLock> lock(cell->lock);
        if (work.Pending.empty())
            for (unsigned budget = 0; cell->refData.refArray && work.Cursor < cell->refData.capacity && budget < 32; ++budget, ++work.Cursor)
                if (auto* ref = cell->refData.refArray[work.Cursor].Get()) work.Pending.push_back(ref->formID);
        done = !cell->refData.refArray || work.Cursor >= cell->refData.capacity;
    }
    for (unsigned budget = 0; budget < 32 && !work.Pending.empty(); ++budget)
    {
        const auto id = work.Pending.front();
        work.Pending.pop_front();
        if (!work.Seen.insert(id).second) continue;
        auto* ref = Cast<TESObjectREFR>(TESForm::GetById(id));
        if (!ref) continue;
        s_baselineSampling = true;
        Sample(ref);
        s_baselineSampling = false;
        const auto parent = EnableParent(ref);
        if (parent && parent != UINT32_MAX && !work.Seen.count(parent)) work.Pending.push_back(parent);
    }
    if (done && work.Pending.empty())
    {
        spdlog::info("World state baseline: cell {:X} sampled {} references", m_baselines.front().Cell, work.Seen.size());
        m_baselines.pop_front();
    }
}
void WorldStateService::Publish(WorldState state) noexcept
{
    auto* ref = Cast<TESObjectREFR>(TESForm::GetById(state.Reference.BaseId));
    const bool animation = WorldAnimationReplay::Handles(state.Kind);
    if (!(animation ? WorldAnimation::Eligible(ref) : Eligible(ref)))
        return;
    if (state.Kind == WorldStateKind::Disabled && EnableParent(ref))
    {
        // Children follow their enable parent's replicated state natively; logged so a child whose parent never
        // replicates is visible (bounded).
        static std::atomic<uint32_t> s_childLogs{};
        if (s_childLogs.fetch_add(1, std::memory_order_relaxed) < 2000)
            spdlog::info("World state publish skipped: ref={:X} disabled={} follows enable parent {:X}", ref->formID,
                state.Value, EnableParent(ref));
        return;
    }
    if (state.Kind == WorldStateKind::Open && ref->baseForm->formType != FormType::Door) return;
    auto* cell = ref->GetParentCellEx();
    if (!cell) return;
    state.Cell = GameId{0, cell->worldspace ? cell->worldspace->formID : cell->formID};
    const auto key = WorldStateTable::MakeKey(state);
    const auto old = m_latest.find(key);
    // A baseline is an end-state observation, not another live script command.
    if (state.Kind == WorldStateKind::Open && state.Scalar == 3 && old != m_latest.end() &&
        old->second.Cell == state.Cell && old->second.Value == state.Value) return;
    if (state.Kind != WorldStateKind::AnimationEvent && old != m_latest.end() && old->second.Cell == state.Cell && old->second.Value == state.Value &&
        old->second.Scalar == state.Scalar && old->second.Animation == state.Animation && old->second.AnimationData == state.AnimationData) return;
    state.Sequence = ++m_sequences[ref->formID];
    // Durable states are always logged (bounded): the 2026-09-28 playthrough could not tell whether the Helgen inn
    // roof's collision change was ever sent, because only watched references were.
    // Live changes only; the startup baseline sweep publishes every reference and used to exhaust the cap in seconds.
    static std::atomic<uint32_t> s_publishLogs{};
    const bool durable = state.Kind == WorldStateKind::Disabled || state.Kind == WorldStateKind::Destroyed ||
        state.Kind == WorldStateKind::DestructionHealth;
    if (m_watches.count(ref->formID) ||
        (durable && !s_baselineSampling && s_publishLogs.fetch_add(1, std::memory_order_relaxed) < 4000))
        spdlog::info("World state publish: epoch={} ref={:X} base={:X} kind={} seq={} value={} scalar={}", state.Epoch,
            ref->formID, ref->baseForm ? ref->baseForm->formID : 0, static_cast<unsigned>(state.Kind), state.Sequence,
            state.Value, state.Scalar);
    m_latest.insert_or_assign(key, state);
    std::lock_guard lock(m_mailboxMutex);
    if (!(m_authority == m_requestedAuthority)) return;
    m_outgoing.push_back(std::move(state));
}
std::string WorldStateService::Diagnostic(uint32_t id) noexcept
{
    std::lock_guard lock(s_observationMutex);
    auto reserved = s_watched;
    reserved.insert(s_watchRequests.begin(), s_watchRequests.end());
    if (!reserved.count(id) && reserved.size() >= 16) return "\"error\":\"world-state watch limit (16); watches expire after 120s\"";
    // A batch of bridge requests before the next pump must arm every watch,
    // even though the response mailbox holds only the last requested sample.
    s_watchRequests.insert(id);
    s_diagnosticRequest = id;
    return s_diagnosticId == id ? s_diagnostic : "\"pending\":true";
}
void WorldStateService::TraceAnimation(uint32_t id, const char* event) noexcept
{
    if (!s_hasWatches.load(std::memory_order_relaxed)) return;
    bool watched{};
    { std::lock_guard lock(s_observationMutex); watched = s_watched.count(id) != 0; }
    if (watched) spdlog::info("World state animation probe: ref={:X} event={} tick={} thread={}", id, event ? event : "", GetTickCount64(), GetCurrentThreadId());
}
void WorldStateService::Pump() noexcept
{
    {
        // Queue depths every 5 s while non-empty: a live script change waits behind every queued state (the
        // 2026-09-28 Helgen inn collision markers reached the follower minutes late).
        static uint64_t s_nextDepthLog{};
        if (const auto nowMs = GetTickCount64(); nowMs >= s_nextDepthLog)
        {
            s_nextDepthLog = nowMs + 5000;
            size_t outgoing{};
            {
                std::lock_guard lock(m_mailboxMutex);
                outgoing = m_outgoing.size();
            }
            if (outgoing || m_follower.Pending() || !m_baselines.empty())
                spdlog::info("World state queues: outgoing={} follower-pending={} baseline-cells={}", outgoing,
                    m_follower.Pending(), m_baselines.size());
        }
    }
    std::set<uint32_t> watchRequests;
    uint32_t diagnostic{};
    {
        std::lock_guard lock(s_observationMutex);
        diagnostic = std::exchange(s_diagnosticRequest, 0u);
        watchRequests.swap(s_watchRequests);
    }
    const auto probeNow = GetTickCount64();
    for (const auto id : watchRequests)
        if (m_watches.count(id) || m_watches.size() < 16) m_watches[id].Until = probeNow + 120000;
    bool sampled{};
    auto watchIt = m_watches.upper_bound(m_watchCursor);
    for (size_t remaining = m_watches.size(); remaining && !m_watches.empty(); --remaining)
    {
        if (watchIt == m_watches.end()) watchIt = m_watches.begin();
        const auto current = watchIt++;
        if (current->second.Until < probeNow) { m_watches.erase(current); continue; }
        const auto id = current->first;
        auto& watch = current->second;
        if (id != diagnostic && probeNow < watch.Next) continue;
        if (sampled || (diagnostic && id != diagnostic)) continue;
        sampled = true; // At most one bounded collision/graph watch per phase.
        m_watchCursor = id; // Low IDs cannot monopolize slow frames.
        const auto probeStart = std::chrono::steady_clock::now();
        watch.Next = probeNow + 250;
        auto* ref = Cast<TESObjectREFR>(TESForm::GetById(id));
        std::string result = "\"available\":false";
        if (ref)
        {
            const auto parent = EnableParent(ref);
            const auto* extra = ref->extraData.GetByType(static_cast<ExtraDataType>(0x36));
            const auto opposite = extra && (reinterpret_cast<const uint8_t*>(extra)[0x10] & 1);
            result = fmt::format("\"formId\":{},\"baseId\":{},\"disabled\":{},\"destroyed\":{},"
                "\"loaded\":{},\"cellId\":{},\"enableParent\":{}",
                ref->formID, ref->baseForm ? ref->baseForm->formID : 0, ref->IsDisabled(),
                (ref->flags & 0x800000) != 0, ref->GetNiNode() != nullptr, ref->GetCellId(), parent);
            result += fmt::format(",\"enableParentOpposite\":{},\"position\":[{},{},{}],{}", opposite,
                ref->position.x, ref->position.y, ref->position.z, AnimationDiagnostic(ref));
            auto* parentRef = parent && parent != UINT32_MAX ? Cast<TESObjectREFR>(TESForm::GetById(parent)) : nullptr;
            result += fmt::format(",\"parentAvailable\":{},\"parentDisabled\":{}", parentRef != nullptr, parentRef && parentRef->IsDisabled());
        }
        if (result != watch.Previous)
        {
            const auto probeUs = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - probeStart).count();
            spdlog::info("World state probe: epoch={} leader={} tick={} ref={:X} probeUs={} {{{}}}", m_authority.Epoch, m_authority.IsLeader, probeNow, id, probeUs, result);
            watch.Previous = result;
        }
        if (id == diagnostic)
        {
            std::lock_guard lock(s_observationMutex);
            s_diagnosticId = id; s_diagnostic = result + fmt::format(",\"sampleTick\":{},\"watchMs\":120000", probeNow);
        }
    }
    {
        std::lock_guard lock(s_observationMutex);
        s_watched.clear();
        for (const auto& [id, watch] : m_watches) s_watched.insert(id);
        s_hasWatches.store(!s_watched.empty(), std::memory_order_relaxed);
    }
    Authority authority;
    std::deque<WorldState> incoming;
    {
        std::lock_guard lock(m_mailboxMutex);
        authority = m_requestedAuthority; WorldStateReplay::Drain(m_incoming, incoming);
    }
    if (!(authority == m_authority))
    {
        m_cells.clear(); m_scopes.clear(); m_sequences.clear(); m_latest.clear(); m_follower.Clear();
        m_authority = authority;
        m_animationSeeding = authority.IsLeader;
        m_animationSeedCursor = 0;
        m_baselines.clear(); m_nextCells = 0;
        spdlog::info("World state pump active: epoch={} leader={} thread={}", authority.Epoch, authority.IsLeader, GetCurrentThreadId());
    }
    if (!authority.Epoch) return;
    VisitCells();
    if (authority.IsLeader) Baseline();
    std::set<uint32_t> attached;
    std::map<WorldStateTable::Key, WorldState> observations;
    {
        std::lock_guard lock(s_observationMutex);
        for (unsigned budget = 0; budget < 64 && !s_attached.empty(); ++budget)
        {
            auto it = s_attached.upper_bound(m_attachmentCursor);
            if (it == s_attached.end()) it = s_attached.begin();
            m_attachmentCursor = *it; attached.insert(*it); s_attached.erase(it);
        }
        for (unsigned budget = 0; budget < 64 && !s_observations.empty(); ++budget)
        {
            auto it = s_observations.upper_bound(m_observationCursor);
            if (it == s_observations.end()) it = s_observations.begin();
            m_observationCursor = it->first;
            observations.emplace(it->first, std::move(it->second)); s_observations.erase(it);
        }
    }
    if (authority.IsLeader)
    {
        PumpAnimations();
        for (const auto id : attached) Sample(Cast<TESObjectREFR>(TESForm::GetById(id)));
        for (auto& [key, state] : observations)
        {
            if (state.Epoch != authority.Epoch) continue;
            if (state.Kind == WorldStateKind::Count) Sample(Cast<TESObjectREFR>(TESForm::GetById(state.Reference.BaseId)));
            else Publish(std::move(state));
        }
        return;
    }
    for (const auto& state : incoming)
        if (state.Epoch == authority.Epoch)
        {
            const auto id = state.Reference.LogFormat();
            const bool hadGap = m_follower.AnimationNeedsCheckpoint(id);
            const bool accepted = m_follower.Receive(state);
            if (!hadGap && m_follower.AnimationNeedsCheckpoint(id))
                spdlog::error("World anim: {:X} history overflow; synchronization incomplete until a covering checkpoint", state.Reference.BaseId);
            static std::atomic<uint32_t> s_receiveLogs{};
            const bool durable = state.Kind == WorldStateKind::Disabled || state.Kind == WorldStateKind::Destroyed ||
                state.Kind == WorldStateKind::DestructionHealth;
            if (m_watches.count(state.Reference.BaseId) || (durable && s_receiveLogs.fetch_add(1, std::memory_order_relaxed) < 4000))
                spdlog::info("World state receive: epoch={} ref={:X} kind={} seq={} accepted={} value={} scalar={} animation={}",
                    state.Epoch, state.Reference.BaseId, static_cast<unsigned>(state.Kind), state.Sequence,
                    accepted, state.Value, state.Scalar, state.Animation.c_str());
        }
    m_follower.EvictAnimationsOutside(m_scopes);
    for (const auto id : attached) m_follower.Attach(id);
    // Transient failures have bounded timed wakes as well as Set3D notifications.
    struct ReplayScope
    {
        ReplayScope() { s_applying.store(true, std::memory_order_release); }
        ~ReplayScope() { s_applying.store(false, std::memory_order_release); }
    } replay;
    const auto now = GetTickCount64();
    for (const auto& state : m_follower.Take(64, now))
    {
        auto* ref = Cast<TESObjectREFR>(TESForm::GetById(state.Reference.BaseId));
        const bool animation = WorldAnimationReplay::Handles(state.Kind);
        if ((animation ? WorldAnimation::Eligible(ref) : Eligible(ref)) && (animation ? WorldAnimation::Apply(ref, state) : Apply(ref, state)))
        {
            if (animation) spdlog::info("World anim: {:X} {} -> follower ({})", ref->formID,
                state.Kind == WorldStateKind::AnimationEvent ? state.Animation.c_str() : "graph snapshot",
                state.Sequence);
            spdlog::info("World state: {:X} {} -> {} (from host)", ref->formID, static_cast<unsigned>(state.Kind),
                state.Animation.empty() ? std::to_string(state.Value) : std::string(state.Animation.c_str()));
            m_follower.Applied(state);
        }
        else
        {
            const bool unsupported = state.Kind == WorldStateKind::FinishedSequence || state.Kind == WorldStateKind::DestructionHealth;
            if (m_follower.Failed(state, now, unsupported))
                spdlog::warn("World state pending: {:X} kind={} seq={} reason={}", state.Reference.BaseId,
                    static_cast<unsigned>(state.Kind), state.Sequence, unsupported ? "unsupported end state; replay disabled" : "transient; retry in 1s");
        }
    }
}
void WorldStateService::OnDisconnected(const DisconnectedEvent&) noexcept
{
    s_captureEpoch.store(0, std::memory_order_release); s_active.store(false, std::memory_order_release);
    std::lock_guard lock(m_mailboxMutex);
    ++m_connectionGeneration;
    {
        std::lock_guard observations(s_observationMutex);
        ++s_captureSerial;
        s_animationEvents.clear(); s_animationPending.clear(); s_animationDirty.clear();
        s_attached.clear(); s_observations.clear();
    }
    m_requestedAuthority = {}; m_incoming.clear(); m_outgoing.clear(); m_cellRequests.clear();
    m_requestedAuthority.Generation = m_connectionGeneration;
}
