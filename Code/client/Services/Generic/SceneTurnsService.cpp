#include <TiltedOnlinePCH.h>
#include "SceneTurnsService.h"

#include <World.h>
#include <Components.h>
#include <Events/UpdateEvent.h>
#include <Events/DisconnectedEvent.h>
#include <Actor.h>
#include <PlayerCharacter.h>
#include <Games/ActorExtension.h>
#include <Games/Skyrim/Interface/UI.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace
{
constexpr uint64_t kSceneTimeoutMs = 90000;
constexpr uint64_t kApproachTimeoutMs = 8000;
constexpr uint64_t kIdleTimeoutMs = 10000;
constexpr float kMaximumDistance = 2000.f;
constexpr float kArrivalDistance = 120.f;
constexpr size_t kMaximumPendingActions = 64;

uint64_t Key(const SceneTurnsNative::IdleStep& aStep) noexcept
{
    return (uint64_t{aStep.SceneId} << 32) | aStep.ActionIndex;
}

struct Gate
{
    SceneTurnsNative::IdleStep Step;
    bool Held{true};
};

// Hooks can run outside World::Update. No ECS access or native mutations occur
// under this mutex. A gate expires on the engine thread even if updates stop.
std::mutex s_gateMutex;
std::unordered_map<uint64_t, Gate> s_gates;
std::vector<SceneTurnsNative::IdleStep> s_inbox;
std::atomic<bool> s_authoritative{};

void ReleaseGate(const SceneTurnsNative::IdleStep& aStep) noexcept
{
    std::lock_guard lock(s_gateMutex);
    const auto it = s_gates.find(Key(aStep));
    if (it != s_gates.end() && it->second.Step.CapturedMs == aStep.CapturedMs)
        it->second.Held = false;
}

float DistanceSquared(const Actor* apActor, const Actor* apTarget) noexcept
{
    const auto x = apActor->position.x - apTarget->position.x;
    const auto y = apActor->position.y - apTarget->position.y;
    const auto z = apActor->position.z - apTarget->position.z;
    return x * x + y * y + z * z;
}

Actor* Resolve(uint32_t aHandle, uint32_t aFormId) noexcept
{
    auto* pActor = Cast<Actor>(TESObjectREFR::GetByHandle(aHandle));
    return pActor && pActor->formID == aFormId ? pActor : nullptr;
}

bool Available(Actor* apActor) noexcept
{
    return apActor && apActor->parentCell && apActor->GetNiNode() &&
        !apActor->IsDead() && !apActor->IsDisabled() && !apActor->IsDeleted() &&
        !apActor->IsInCombat();
}
}

struct SceneTurnsService::State
{
    enum class Phase { Host, Approach, Idle };
    struct Target
    {
        uint32_t PlayerId;
        uint32_t FormId;
        uint32_t Handle;
        float Distance;
    };
    struct Turn
    {
        SceneTurnsNative::IdleStep Step;
        std::vector<Target> Targets;
        size_t Next{};
        Phase Current{Phase::Host};
        uint64_t PhaseStartedMs{};
        bool ApproachOwned{};
        bool SawBusy{};
    };
    std::vector<Turn> Turns;
    uint64_t Epoch{};
};

SceneTurnsService::SceneTurnsService(World& aWorld, entt::dispatcher& aDispatcher,
    TransportService& aTransport) noexcept
    : m_state(std::make_unique<State>())
    , m_world(aWorld)
    , m_transport(aTransport)
    , m_updateConnection(aDispatcher.sink<UpdateEvent>().connect<&SceneTurnsService::OnUpdate>(this))
    , m_disconnectConnection(aDispatcher.sink<DisconnectedEvent>().connect<&SceneTurnsService::OnDisconnected>(this))
{
    spdlog::info("Scene turns: prototype {} (SKYRIM_COOP_SCENE_TURNS=1; paired validation required)",
        SceneTurnsNative::IsEnabled() ? "enabled" : "disabled");
}

SceneTurnsService::~SceneTurnsService()
{
    // World services may already be destructing; never call Papyrus here.
    s_authoritative.store(false, std::memory_order_release);
    std::lock_guard lock(s_gateMutex);
    s_gates.clear();
    s_inbox.clear();
}

void SceneTurnsService::Capture(const SceneTurnsNative::IdleStep& aStep) noexcept
{
    if (!s_authoritative.load(std::memory_order_acquire))
        return;
    std::lock_guard lock(s_gateMutex);
    if (!s_authoritative.load(std::memory_order_acquire))
        return;
    const auto key = Key(aStep);
    const auto it = s_gates.find(key);
    if (it != s_gates.end() && it->second.Step.Phase == aStep.Phase)
        return; // Completed gates remain tombstones until the scene leaves the phase.
    if (s_gates.size() >= kMaximumPendingActions || s_inbox.size() >= kMaximumPendingActions)
    {
        spdlog::warn("Scene turns: skipped scene={:X} action={} reason=capacity", aStep.SceneId, aStep.ActionIndex);
        return;
    }
    s_gates.insert_or_assign(key, Gate{aStep});
    s_inbox.push_back(aStep);
}

bool SceneTurnsService::IsAuthoritative() noexcept
{
    return s_authoritative.load(std::memory_order_acquire);
}

bool SceneTurnsService::HoldPhase(uint32_t aSceneId, uint32_t aPhase) noexcept
{
    if (!s_authoritative.load(std::memory_order_acquire))
        return false;
    const auto now = GetTickCount64();
    std::lock_guard lock(s_gateMutex);
    for (const auto& [key, gate] : s_gates)
    {
        if (gate.Held && gate.Step.SceneId == aSceneId && gate.Step.Phase == aPhase &&
            now >= gate.Step.CapturedMs && now - gate.Step.CapturedMs < kSceneTimeoutMs)
            return true;
    }
    return false;
}

void SceneTurnsService::Clear() noexcept
{
    s_authoritative.store(false, std::memory_order_release);
    for (const auto& turn : m_state->Turns)
        if (turn.ApproachOwned)
            SceneTurnsNative::ReleaseApproach(Resolve(turn.Step.ActorHandle, turn.Step.ActorId));
    m_state->Turns.clear();
    m_state->Epoch = 0;
    std::lock_guard lock(s_gateMutex);
    s_gates.clear();
    s_inbox.clear();
}

void SceneTurnsService::OnDisconnected(const DisconnectedEvent&) noexcept
{
    Clear();
}

void SceneTurnsService::OnUpdate(const UpdateEvent&) noexcept
{
    if (!SceneTurnsNative::IsEnabled())
        return;
    const auto& party = m_world.GetPartyService();
    auto* pPlayer = PlayerCharacter::Get();
    auto* pUI = UI::Get();
    if (!m_transport.IsConnected() || !party.IsInParty() || !party.IsLeader() ||
        !party.GetStartEpoch() || !pPlayer || !pPlayer->GetNiNode() || !pUI ||
        pUI->GetMenuOpen(BSFixedString("Loading Menu")) ||
        pUI->GetMenuOpen(BSFixedString("Main Menu")))
    {
        Clear();
        return;
    }
    if (m_state->Epoch != party.GetStartEpoch())
    {
        Clear();
        m_state->Epoch = party.GetStartEpoch();
    }
    s_authoritative.store(true, std::memory_order_release);
    const auto now = GetTickCount64();
    std::vector<SceneTurnsNative::IdleStep> inbox;
    std::vector<SceneTurnsNative::IdleStep> gates;
    {
        std::lock_guard lock(s_gateMutex);
        inbox.swap(s_inbox);
        for (const auto& [key, gate] : s_gates)
            gates.push_back(gate.Step);
    }
    // Native form lookup happens outside the hook mutex. Revalidate identity
    // before removing a gate that an engine callback may have replaced.
    for (const auto& step : gates)
    {
        bool complete{};
        if (SceneTurnsNative::IsCurrent(step, complete))
            continue;
        std::lock_guard lock(s_gateMutex);
        const auto it = s_gates.find(Key(step));
        if (it != s_gates.end() && it->second.Step.CapturedMs == step.CapturedMs)
            s_gates.erase(it);
    }

    const auto& members = party.GetPartyMembers();
    auto view = m_world.view<FormIdComponent, PlayerComponent, RemoteComponent>();
    for (const auto& step : inbox)
    {
        auto* pActor = Resolve(step.ActorHandle, step.ActorId);
        bool complete{};
        if (!Available(pActor) || !SceneTurnsNative::IsCurrent(step, complete))
        {
            ReleaseGate(step);
            continue;
        }
        // A single NPC cannot perform independent turns for overlapping actions.
        const bool owned = std::any_of(m_state->Turns.begin(), m_state->Turns.end(),
            [&](const State::Turn& aTurn) { return aTurn.Step.ActorHandle == step.ActorHandle; });
        if (owned)
        {
            spdlog::warn("Scene turns: skipped scene={:X} action={} reason=actor-already-busy", step.SceneId, step.ActionIndex);
            ReleaseGate(step);
            continue;
        }
        State::Turn turn{};
        turn.Step = step;
        spdlog::info("Scene turns: {:X} action {} for {} (original accepted)",
            step.SceneId, step.ActionIndex, m_transport.GetLocalPlayerId());
        for (auto entity : view)
        {
            const auto playerId = view.get<PlayerComponent>(entity).Id;
            if (std::find(members.begin(), members.end(), playerId) == members.end())
                continue;
            const auto formId = view.get<FormIdComponent>(entity).Id;
            auto* pTarget = Cast<Actor>(TESForm::GetById(formId));
            if (!Available(pTarget) || pTarget == pPlayer || pTarget->parentCell != pActor->parentCell)
            {
                spdlog::info("Scene turns: skipped scene={:X} action={} player={} reason=unavailable-or-other-cell",
                    step.SceneId, step.ActionIndex, playerId);
                continue;
            }
            const auto distance = DistanceSquared(pActor, pTarget);
            if (!std::isfinite(distance) || distance > kMaximumDistance * kMaximumDistance)
            {
                spdlog::info("Scene turns: skipped scene={:X} action={} player={} reason=far-away", step.SceneId, step.ActionIndex, playerId);
                continue;
            }
            turn.Targets.push_back({playerId, formId, pTarget->GetHandle().handle.iBits, distance});
        }
        std::sort(turn.Targets.begin(), turn.Targets.end(), [](const auto& a, const auto& b)
            { return a.Distance != b.Distance ? a.Distance < b.Distance : a.PlayerId < b.PlayerId; });
        if (turn.Targets.empty())
            ReleaseGate(step);
        else
        {
            spdlog::info("Scene turns: captured scene={:X} action={} phase={} actor={:X} idle={:X} recipients={}",
                step.SceneId, step.ActionIndex, step.Phase, step.ActorId, step.IdleId, turn.Targets.size());
            m_state->Turns.push_back(std::move(turn));
        }
    }

    for (auto it = m_state->Turns.begin(); it != m_state->Turns.end();)
    {
        auto& turn = *it;
        auto* pActor = Resolve(turn.Step.ActorHandle, turn.Step.ActorId);
        bool hostComplete{};
        const bool current = SceneTurnsNative::IsCurrent(turn.Step, hostComplete);
        if (!Available(pActor) || !current || now - turn.Step.CapturedMs >= kSceneTimeoutMs ||
            turn.Next >= turn.Targets.size())
        {
            if (turn.ApproachOwned)
                SceneTurnsNative::ReleaseApproach(pActor);
            spdlog::info("Scene turns: released scene={:X} action={} reason={}",
                turn.Step.SceneId, turn.Step.ActionIndex,
                turn.Next >= turn.Targets.size() ? "queue-drained" : "scene-invalid-or-timeout");
            ReleaseGate(turn.Step);
            it = m_state->Turns.erase(it);
            continue;
        }
        const auto& target = turn.Targets[turn.Next];
        auto* pTarget = Resolve(target.Handle, target.FormId);
        bool connected = false;
        for (auto entity : view)
            if (view.get<PlayerComponent>(entity).Id == target.PlayerId &&
                view.get<FormIdComponent>(entity).Id == target.FormId)
                connected = true;
        connected = connected && std::find(members.begin(), members.end(), target.PlayerId) != members.end();
        const auto advance = [&](const char* apReason)
        {
            if (turn.ApproachOwned)
                SceneTurnsNative::ReleaseApproach(pActor);
            turn.ApproachOwned = false;
            spdlog::info("Scene turns: turn ended scene={:X} action={} player={} reason={}",
                turn.Step.SceneId, turn.Step.ActionIndex, target.PlayerId, apReason);
            ++turn.Next;
            turn.Current = State::Phase::Host;
            turn.SawBusy = false;
        };
        if (!connected || !Available(pTarget) || pTarget->parentCell != pActor->parentCell ||
            !std::isfinite(DistanceSquared(pActor, pTarget)) ||
            DistanceSquared(pActor, pTarget) > kMaximumDistance * kMaximumDistance)
        {
            advance("skipped-disconnected-unloaded-or-far");
            ++it;
            continue;
        }
        if (turn.Current == State::Phase::Host && hostComplete)
        {
            turn.ApproachOwned = SceneTurnsNative::Approach(pActor, pTarget);
            if (!turn.ApproachOwned)
                advance("skipped-movement-unavailable");
            else
            {
                turn.PhaseStartedMs = now;
                turn.Current = State::Phase::Approach;
            }
        }
        else if (turn.Current == State::Phase::Approach)
        {
            if (now - turn.PhaseStartedMs >= kApproachTimeoutMs)
                advance("skipped-approach-timeout");
            else if (DistanceSquared(pActor, pTarget) <= kArrivalDistance * kArrivalDistance)
            {
                bool busy{};
                if (!SceneTurnsNative::AnimationBusy(pActor, busy) || busy)
                {
                    ++it;
                    continue;
                }
                SceneTurnsNative::ReleaseApproach(pActor);
                turn.ApproachOwned = false;
                const float facing = std::atan2(pTarget->position.x - pActor->position.x,
                    pTarget->position.y - pActor->position.y);
                pActor->SetRotation(pActor->rotation.x, pActor->rotation.y, facing);
                if (!SceneTurnsNative::Replay(turn.Step, pActor, pTarget))
                    advance("skipped-idle-rejected");
                else
                {
                    spdlog::info("Scene turns: {:X} action {} for {}", turn.Step.SceneId, turn.Step.ActionIndex, target.PlayerId);
                    turn.Current = State::Phase::Idle;
                    turn.PhaseStartedMs = now;
                    turn.SawBusy = false;
                }
            }
        }
        else if (turn.Current == State::Phase::Idle)
        {
            bool busy{};
            const bool observed = SceneTurnsNative::AnimationBusy(pActor, busy);
            if (observed && busy)
                turn.SawBusy = true;
            if (observed && turn.SawBusy && !busy && now - turn.PhaseStartedMs >= 250)
                advance("observed-animation-release");
            else if (now - turn.PhaseStartedMs >= kIdleTimeoutMs)
                advance("skipped-no-confirmed-animation-end");
        }
        ++it;
    }
}
