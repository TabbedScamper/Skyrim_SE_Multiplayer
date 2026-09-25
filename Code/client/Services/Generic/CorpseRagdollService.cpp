#include <Services/CorpseRagdollService.h>

#include <World.h>
#include <Events/UpdateEvent.h>
#include <Events/DisconnectedEvent.h>
#include <Messages/NotifyCorpseRagdoll.h>
#include <Services/TransportService.h>
#include <Components.h>

#include <Games/Skyrim/Actor.h>
#include <Games/Skyrim/BSAnimationGraphManager.h>
#include <Games/Skyrim/Havok/ActorPoseDiagnosticViews.h>
#include <Utils.h>

#include <chrono>
#include <cmath>

namespace
{
using namespace ActorPoseDiagnosticViews;

// Havok world units to game units (matches ObjectService).
constexpr float kHavokToGameUnits = 70.f;
// A body counts as settled below this speed (Havok units/s, about 2 game units/s).
constexpr float kSettledLinear = 0.03f;
constexpr float kSettledAngular = 0.05f;
constexpr uint64_t kSettleMs = 1500;
constexpr uint64_t kResendMs = 5000;
// Follower bodies further than this from the owner's pose are put back (game units).
constexpr float kDriftGameUnits = 0.25f;

uint64_t NowMs() noexcept
{
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

template <class T> bool ReadNative(const void* apSource, T& aTarget) noexcept
{
    SIZE_T copied{};
    return apSource && ReadProcessMemory(GetCurrentProcess(), apSource, &aTarget, sizeof(T), &copied) && copied == sizeof(T);
}

struct GraphRef
{
    BSAnimationGraphManager* pManager{};
    ~GraphRef()
    {
        if (pManager)
            pManager->Release();
    }
};

// The active behavior graph and its ragdoll rigid bodies (hkpRigidBody*), in ragdoll order.
bool GetRagdollBodies(Actor* apActor, void*& aGraph, Vector<RigidBody*>& aBodies) noexcept
{
    aBodies.clear();
    GraphRef ref;
    if (!apActor || !apActor->animationGraphHolder.GetBSAnimationGraph(&ref.pManager) || !ref.pManager)
        return false;
    BSScopedLock<BSRecursiveLock> lock(ref.pManager->lock);
    const auto count = ref.pManager->animationGraphs.size;
    const auto index = ref.pManager->animationGraphIndex;
    if (!count || count > 32 || index >= count)
        return false;
    void* pGraph = ref.pManager->animationGraphs.Get(index);
    AnimationGraph graph{};
    RagdollDriver driver{};
    RagdollInstance ragdoll{};
    if (!ReadNative(pGraph, graph) || !ReadNative(graph.characterInstance.ragdollDriver, driver) ||
        !ReadNative(driver.ragdoll, ragdoll) || ragdoll.rigidBodies.size <= 0 ||
        ragdoll.rigidBodies.size > static_cast<int32_t>(CorpseRagdollRequest::kMaxBodies))
        return false;
    for (int32_t i = 0; i < ragdoll.rigidBodies.size; ++i)
    {
        void* pBody{};
        RigidBody probe{};
        if (!ReadNative(ragdoll.rigidBodies.data + i, pBody) || !ReadNative(pBody, probe) || !probe.world)
            return false;
        aBodies.push_back(static_cast<RigidBody*>(pBody));
    }
    aGraph = pGraph;
    return true;
}

// hkTransform rotation columns are transform[0..2], [4..6], [8..10].
void MatrixToQuaternion(const float* t, float* q) noexcept
{
    const float m00 = t[0], m10 = t[1], m20 = t[2];
    const float m01 = t[4], m11 = t[5], m21 = t[6];
    const float m02 = t[8], m12 = t[9], m22 = t[10];
    const float trace = m00 + m11 + m22;
    if (trace > 0.f)
    {
        const float s = std::sqrt(trace + 1.f) * 2.f;
        q[3] = 0.25f * s;
        q[0] = (m21 - m12) / s;
        q[1] = (m02 - m20) / s;
        q[2] = (m10 - m01) / s;
    }
    else if (m00 > m11 && m00 > m22)
    {
        const float s = std::sqrt(1.f + m00 - m11 - m22) * 2.f;
        q[3] = (m21 - m12) / s;
        q[0] = 0.25f * s;
        q[1] = (m01 + m10) / s;
        q[2] = (m02 + m20) / s;
    }
    else if (m11 > m22)
    {
        const float s = std::sqrt(1.f + m11 - m00 - m22) * 2.f;
        q[3] = (m02 - m20) / s;
        q[0] = (m01 + m10) / s;
        q[1] = 0.25f * s;
        q[2] = (m12 + m21) / s;
    }
    else
    {
        const float s = std::sqrt(1.f + m22 - m00 - m11) * 2.f;
        q[3] = (m10 - m01) / s;
        q[0] = (m02 + m20) / s;
        q[1] = (m12 + m21) / s;
        q[2] = 0.25f * s;
    }
    const float norm = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    for (int i = 0; i < 4; ++i)
        q[i] /= norm;
}

// hkpEntity::requestDeactivation (ID 60850): puts the body's simulation island to sleep (its
// deactivation counters go to max), so a placed ragdoll stays exactly put until disturbed.
void SleepBody(RigidBody* apBody) noexcept
{
    using TRequestDeactivation = void(void*);
    POINTER_SKYRIMSE(TRequestDeactivation, s_requestDeactivation, 60850);
    s_requestDeactivation.Get()(apBody);
}

// hkpRigidBody::setPositionAndRotation(const hkVector4&, const hkQuaternion&) (ID 60898).
void SetBodyPose(RigidBody* apBody, const float* apPosition, const float* apRotation) noexcept
{
    alignas(16) float position[4]{apPosition[0], apPosition[1], apPosition[2], 0.f};
    alignas(16) float rotation[4]{apRotation[0], apRotation[1], apRotation[2], apRotation[3]};
    using TSetPositionAndRotation = void(void*, const float*, const float*);
    POINTER_SKYRIMSE(TSetPositionAndRotation, s_setPose, 60898);
    s_setPose.Get()(apBody, position, rotation);
    std::fill(std::begin(apBody->linearVelocity), std::end(apBody->linearVelocity), 0.f);
    std::fill(std::begin(apBody->angularVelocity), std::end(apBody->angularVelocity), 0.f);
}
} // namespace

CorpseRagdollService::CorpseRagdollService(World& aWorld, entt::dispatcher& aDispatcher, TransportService& aTransport) noexcept
    : m_world(aWorld)
    , m_transport(aTransport)
{
    m_updateConnection = aDispatcher.sink<UpdateEvent>().connect<&CorpseRagdollService::OnUpdate>(this);
    m_disconnectConnection = aDispatcher.sink<DisconnectedEvent>().connect<&CorpseRagdollService::OnDisconnected>(this);
    m_ragdollConnection = aDispatcher.sink<NotifyCorpseRagdoll>().connect<&CorpseRagdollService::OnCorpseRagdoll>(this);
}

void CorpseRagdollService::OnDisconnected(const DisconnectedEvent&) noexcept
{
    m_owned.clear();
    m_remote.clear();
}

void CorpseRagdollService::OnCorpseRagdoll(const NotifyCorpseRagdoll& acMessage) noexcept
{
    auto& corpse = m_remote[acMessage.ServerId];
    if (corpse.Bodies != acMessage.Bodies)
    {
        corpse.Bodies = acMessage.Bodies;
        corpse.NextCheckMs = 0;
    }
}

void CorpseRagdollService::OnUpdate(const UpdateEvent&) noexcept
{
    if (!m_transport.IsConnected())
        return;
    const auto now = NowMs();
    if (now < m_nextTickMs)
        return;
    m_nextTickMs = now + 250;
    CaptureOwned(now);
    ApplyRemote(now);
}

void CorpseRagdollService::CaptureOwned(const uint64_t aNowMs) noexcept
{
    Set<uint32_t> seen;
    auto view = m_world.view<FormIdComponent, LocalComponent>();
    for (auto entity : view)
    {
        auto* pActor = Cast<Actor>(TESForm::GetById(view.get<FormIdComponent>(entity).Id));
        if (!pActor || !pActor->IsDead() || !pActor->GetNiNode())
            continue;
        const uint32_t serverId = view.get<LocalComponent>(entity).Id;
        void* pGraph{};
        Vector<RigidBody*> bodies;
        if (!GetRagdollBodies(pActor, pGraph, bodies))
            continue;
        seen.insert(serverId);

        bool settled = true;
        for (auto* pBody : bodies)
        {
            const float* v = pBody->linearVelocity;
            const float* w = pBody->angularVelocity;
            if (std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]) > kSettledLinear ||
                std::sqrt(w[0] * w[0] + w[1] * w[1] + w[2] * w[2]) > kSettledAngular)
                settled = false;
        }
        auto& owned = m_owned[serverId];
        if (!settled)
        {
            owned.SettledSinceMs = 0;
            owned.LastSentMs = 0;
            continue;
        }
        if (!owned.SettledSinceMs)
            owned.SettledSinceMs = aNowMs;
        if (aNowMs - owned.SettledSinceMs < kSettleMs || (owned.LastSentMs && aNowMs - owned.LastSentMs < kResendMs))
            continue;

        CorpseRagdollRequest request{};
        request.ServerId = serverId;
        for (auto* pBody : bodies)
        {
            CorpseRagdollBody body{};
            body.Position[0] = pBody->transform[12] * kHavokToGameUnits - pActor->position.x;
            body.Position[1] = pBody->transform[13] * kHavokToGameUnits - pActor->position.y;
            body.Position[2] = pBody->transform[14] * kHavokToGameUnits - pActor->position.z;
            MatrixToQuaternion(pBody->transform, body.Rotation);
            request.Bodies.push_back(body);
        }
        m_transport.Send(request);
        if (!owned.LastSentMs)
            spdlog::info("Corpse {:X} (server {:X}) settled: sent {} ragdoll bodies", pActor->formID, serverId, bodies.size());
        owned.LastSentMs = aNowMs;
    }
    for (auto it = m_owned.begin(); it != m_owned.end();)
        it = seen.contains(it->first) ? std::next(it) : m_owned.erase(it);
}

void CorpseRagdollService::ApplyRemote(const uint64_t aNowMs) noexcept
{
    for (auto it = m_remote.begin(); it != m_remote.end(); ++it)
    {
        auto& corpse = it.value();
        if (aNowMs < corpse.NextCheckMs)
            continue;
        corpse.NextCheckMs = aNowMs + 1000;
        auto* pActor = Utils::GetByServerId<Actor>(it->first);
        if (!pActor || !pActor->IsDead() || !pActor->GetNiNode())
        {
            corpse.Keyframed = false;
            continue;
        }
        void* pGraph{};
        Vector<RigidBody*> bodies;
        if (!GetRagdollBodies(pActor, pGraph, bodies))
            continue;
        if (bodies.size() != corpse.Bodies.size())
        {
            if (!corpse.CountMismatchLogged)
                spdlog::warn("Corpse {:X}: {} local ragdoll bodies, owner sent {}", pActor->formID, bodies.size(), corpse.Bodies.size());
            corpse.CountMismatchLogged = true;
            continue;
        }

        // The bodies stay simulated, placed at the owner's pose with zero velocity and put to sleep.
        // (Motion types are not changed: BShkbAnimationGraph slot 8 keyframed nothing lasting, and a
        // direct hkpRigidBody motion-type call (ID 60908) with guessed arguments crashed the game.)
        float worstDrift = 0.f;
        for (size_t i = 0; i < bodies.size(); ++i)
        {
            const auto& target = corpse.Bodies[i];
            const float wanted[3]{(pActor->position.x + target.Position[0]) / kHavokToGameUnits,
                (pActor->position.y + target.Position[1]) / kHavokToGameUnits,
                (pActor->position.z + target.Position[2]) / kHavokToGameUnits};
            const float dx = (bodies[i]->transform[12] - wanted[0]) * kHavokToGameUnits;
            const float dy = (bodies[i]->transform[13] - wanted[1]) * kHavokToGameUnits;
            const float dz = (bodies[i]->transform[14] - wanted[2]) * kHavokToGameUnits;
            worstDrift = (std::max)(worstDrift, std::sqrt(dx * dx + dy * dy + dz * dz));
        }
        if (corpse.Keyframed && worstDrift <= kDriftGameUnits)
            continue;

        for (size_t i = 0; i < bodies.size(); ++i)
        {
            const auto& target = corpse.Bodies[i];
            const float wanted[3]{(pActor->position.x + target.Position[0]) / kHavokToGameUnits,
                (pActor->position.y + target.Position[1]) / kHavokToGameUnits,
                (pActor->position.z + target.Position[2]) / kHavokToGameUnits};
            SetBodyPose(bodies[i], wanted, target.Rotation);
        }
        for (auto* pBody : bodies)
            SleepBody(pBody);
        corpse.Keyframed = true;
        if (corpse.Applications++ < 3)
            spdlog::info("Corpse {:X}: placed {} ragdoll bodies at the owner's pose (drift was {:.2f})", pActor->formID,
                bodies.size(), worstDrift);
    }
}
