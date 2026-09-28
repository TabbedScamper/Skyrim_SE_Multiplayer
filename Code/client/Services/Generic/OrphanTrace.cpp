#include "OrphanTrace.h"

#include <World.h>
#include <Components.h>
#include <Services/PartyService.h>
#include <Events/ActorAddedEvent.h>
#include <Events/ActorRemovedEvent.h>
#include <Events/UpdateEvent.h>
#include <Actor.h>
#include <PlayerCharacter.h>
#include <Forms/TESObjectCELL.h>
#include <atomic>
#include <mutex>

namespace
{
struct NativeSample
{
    uint32_t Form{};
    uint64_t RemovedAt{}, RequestedAt{}, SessionEpoch{};
    const char* Phase{}; // Only static phase literals enter the queue.
};
std::mutex s_samplesMutex;
std::array<NativeSample, 64> s_samples;
size_t s_sampleCount{};
std::atomic<bool> s_pending{};

// 107306 / 14154AF70 is void(void*), called only by Main::Update at
// 1406594B9 after its frame cleanup. Avoid a second hook on
// Main::Update itself: our MinHook manager cannot chain duplicate targets.
using FrameEnd = void(void*);
FrameEnd* s_frameEnd{};
void ObserveFrameEnd(void* apFrameState)
{
    s_frameEnd(apFrameState);
    if (!s_pending.load(std::memory_order_acquire))
        return;
    POINTER_SKYRIMSE(void, mainLoop, 36564);
    if (reinterpret_cast<uintptr_t>(_ReturnAddress()) != reinterpret_cast<uintptr_t>(mainLoop.Get()) + 0xC4E)
        return;
    std::array<NativeSample, 64> samples;
    size_t count;
    {
        std::lock_guard lock(s_samplesMutex);
        count = s_sampleCount;
        std::copy_n(s_samples.begin(), count, samples.begin());
        s_sampleCount = 0;
        s_pending.store(false, std::memory_order_release);
    }
    for (size_t i = 0; i < count; ++i)
    {
        const auto& sample = samples[i];
        // 14617 releases the map read lock before returning. Do not carry the
        // returned native pointer to another thread or frame. Get3D (19735)
        // reads loadedData without locking; it is confined to this frame phase.
        auto* actor = Cast<Actor>(TESForm::GetById(sample.Form));
        auto* player = PlayerCharacter::Get();
        const auto now = GetTickCount64();
        spdlog::info("Orphan native: phase={} ref={:X} ageMs={} queueMs={} exists={} cell={:X} playerCell={:X} has3D={} thread={} requestEpoch={} removalTick={}",
            sample.Phase, sample.Form, now - sample.RemovedAt, now - sample.RequestedAt, actor != nullptr,
            actor && actor->parentCell ? actor->parentCell->formID : 0,
            player && player->parentCell ? player->parentCell->formID : 0,
            actor && actor->GetNiNode(), GetCurrentThreadId(), sample.SessionEpoch, sample.RemovedAt);
    }
}
static TiltedPhoques::Initializer s_hook([] {
    POINTER_SKYRIMSE(FrameEnd, frameEnd, 107306);
    s_frameEnd = frameEnd.Get();
    TP_HOOK(&s_frameEnd, ObserveFrameEnd);
});
}

OrphanTrace::OrphanTrace(World& aWorld, entt::dispatcher& aDispatcher) noexcept
    : m_world(aWorld)
    , m_removed(aDispatcher.sink<ActorRemovedEvent>().connect<&OrphanTrace::OnRemoved>(this))
    , m_added(aDispatcher.sink<ActorAddedEvent>().connect<&OrphanTrace::OnAdded>(this))
    , m_update(aDispatcher.sink<UpdateEvent>().connect<&OrphanTrace::OnUpdate>(this))
{
}

void OrphanTrace::Log(const Entry& aEntry, const char* aPhase) const noexcept
{
    // ECS belongs to the UpdateEvent worker. Only value requests cross to the
    // main frame; its native samples never access the registry or this service.
    uint32_t localId = 0, remoteId = 0, waiting = 0;
    uint32_t ownershipEpoch = 0;
    bool registered = false;
    for (auto entity : m_world.view<FormIdComponent>())
    {
        if (m_world.get<FormIdComponent>(entity).Id != aEntry.Form)
            continue;
        registered = true;
        if (const auto* local = m_world.try_get<LocalComponent>(entity))
        {
            localId = local->Id;
            ownershipEpoch = local->OwnershipEpoch;
        }
        if (const auto* remote = m_world.try_get<RemoteComponent>(entity))
        {
            remoteId = remote->Id;
            ownershipEpoch = remote->OwnershipEpoch;
        }
        waiting = m_world.all_of<WaitingForAssignmentComponent>(entity);
        break;
    }
    const auto sessionEpoch = m_world.GetPartyService().GetStartEpoch();
    spdlog::info("Orphan discovery: phase={} ref={:X} ageMs={} local={:X} remote={:X} waiting={} rediscovered={} online={} leader={} registered={} ownershipEpoch={} sessionEpoch={} removalTick={}",
        aPhase, aEntry.Form, GetTickCount64() - aEntry.RemovedAt, localId, remoteId, waiting, aEntry.Rediscovered,
        m_world.GetTransport().IsOnline(), m_world.GetPartyService().IsLeader(), registered, ownershipEpoch, sessionEpoch, aEntry.RemovedAt);
    std::lock_guard lock(s_samplesMutex);
    if (s_sampleCount == s_samples.size())
    {
        spdlog::info("Orphan native: queue full ref={:X} phase={} (64-entry evidence bound)", aEntry.Form, aPhase);
        return;
    }
    s_samples[s_sampleCount++] = {aEntry.Form, aEntry.RemovedAt, GetTickCount64(), sessionEpoch, aPhase};
    s_pending.store(true, std::memory_order_release);
}

void OrphanTrace::OnRemoved(const ActorRemovedEvent& aEvent) noexcept
{
    if (!m_world.GetPartyService().IsInParty() || !m_world.GetPartyService().IsLeader())
        return;
    auto slot = std::find_if(m_entries.begin(), m_entries.end(), [&](const Entry& e) { return e.Form == aEvent.FormId; });
    if (slot == m_entries.end())
        slot = std::find_if(m_entries.begin(), m_entries.end(), [](const Entry& e) { return !e.Form; });
    if (slot == m_entries.end())
    {
        slot = std::min_element(m_entries.begin(), m_entries.end(), [](const Entry& a, const Entry& b) { return a.RemovedAt < b.RemovedAt; });
        spdlog::info("Orphan native: evicted ref={:X} (64-entry evidence bound)", slot->Form);
    }
    *slot = {aEvent.FormId, GetTickCount64(), 0, false};
    Log(*slot, "removed");
}

void OrphanTrace::OnAdded(const ActorAddedEvent& aEvent) noexcept
{
    for (auto& entry : m_entries)
        if (entry.Form == aEvent.FormId)
        {
            entry.Rediscovered = true;
            Log(entry, "added");
            return;
        }
}

void OrphanTrace::OnUpdate(const UpdateEvent&) noexcept
{
    // Finish already scheduled evidence even after disconnect or leader change.
    const auto now = GetTickCount64();
    if (now < m_nextSample)
        return;
    m_nextSample = now + 1000;
    constexpr uint64_t delays[] = {1000, 5000, 20000};
    for (auto& entry : m_entries)
        if (entry.Form && now - entry.RemovedAt >= delays[entry.Sample])
        {
            Log(entry, "sample");
            if (++entry.Sample == std::size(delays))
                entry = {};
        }
}
