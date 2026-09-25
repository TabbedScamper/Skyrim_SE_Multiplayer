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
#include <Games/Skyrim/Havok/PoseCopyAuthority.h>
#include <Utils.h>
#include <AI/AIProcess.h>

#include <chrono>
#include <cmath>

namespace
{
using namespace ActorPoseDiagnosticViews;

// Havok world units to game units (matches ObjectService).
constexpr float kHavokToGameUnits = 70.f;
std::atomic<CorpseRagdollService*> s_ragdollService{};
std::mutex s_followingLock;
std::unordered_map<uint32_t, uint64_t> s_followingSinceMs; // form id -> last sample received
// A body counts as settled below this speed (Havok units/s, about 2 game units/s).
constexpr float kSettledLinear = 0.03f;
constexpr float kSettledAngular = 0.05f;
constexpr uint64_t kSettleMs = 1500;
constexpr uint64_t kStreamMs = 50;
constexpr uint64_t kResendMs = 5000;
// Presentation past the newest sample by more than this: the owner stopped streaming (settled).
constexpr uint64_t kHoldMs = 300;
// A settled follower ragdoll further than this from the owner's pose is put back (game units).
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

// Dying, dead, knocked down or ragdolling (ActorState1 lifeState bits 21-24, knockState 25-27).
bool PhysicsOwnsSkeleton(const Actor* apActor) noexcept
{
    const uint32_t flags1 = apActor->actorState.flags1;
    return ((flags1 >> 21) & 0xF) != 0 || ((flags1 >> 25) & 0x7) != 0;
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

// The active behavior graph's ragdoll rigid bodies (hkpRigidBody*), in ragdoll order.
bool GetRagdollBodies(Actor* apActor, Vector<RigidBody*>& aBodies) noexcept
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
    return true;
}

// hkTransform rotation columns are transform[0..2], [4..6], [8..10].
void MatrixToQuaternion(const float* t, float* q) noexcept
{
    const float m00 = t[0], m10 = t[1], m20 = t[2], m01 = t[4], m11 = t[5], m21 = t[6], m02 = t[8], m12 = t[9], m22 = t[10];
    const float trace = m00 + m11 + m22;
    if (trace > 0.f)
    {
        const float s = std::sqrt(trace + 1.f) * 2.f;
        q[3] = 0.25f * s; q[0] = (m21 - m12) / s; q[1] = (m02 - m20) / s; q[2] = (m10 - m01) / s;
    }
    else if (m00 > m11 && m00 > m22)
    {
        const float s = std::sqrt(1.f + m00 - m11 - m22) * 2.f;
        q[3] = (m21 - m12) / s; q[0] = 0.25f * s; q[1] = (m01 + m10) / s; q[2] = (m02 + m20) / s;
    }
    else if (m11 > m22)
    {
        const float s = std::sqrt(1.f + m11 - m00 - m22) * 2.f;
        q[3] = (m02 - m20) / s; q[0] = (m01 + m10) / s; q[1] = 0.25f * s; q[2] = (m12 + m21) / s;
    }
    else
    {
        const float s = std::sqrt(1.f + m22 - m00 - m11) * 2.f;
        q[3] = (m10 - m01) / s; q[0] = (m02 + m20) / s; q[1] = (m12 + m21) / s; q[2] = 0.25f * s;
    }
    const float norm = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    for (int i = 0; i < 4; ++i)
        q[i] /= norm;
}

// hkpEntity::requestDeactivation (ID 60850): sleep the body's island so it stays exactly put.
void SleepBody(RigidBody* apBody) noexcept
{
    using TRequestDeactivation = void(void*);
    POINTER_SKYRIMSE(TRequestDeactivation, s_requestDeactivation, 60850);
    s_requestDeactivation.Get()(apBody);
}

// Places a ragdoll body through its bhkRigidBody wrapper (hkpWorldObject user data, +0x18):
// virtual slot 0x37 SetPositionAndRotation takes the bhkWorld write lock around
// hkpRigidBody::setPositionAndRotation (ID 60898). The raw Havok call from our update crashed
// (0x140B4CF03) when a physics step was running. Velocities are cleared so the solver does not
// carry the body off before the next placement.
bool SetBodyPose(RigidBody* apBody, const float* apPosition, const float* apRotation, const float* apVelocity) noexcept
{
    void* pWrapper = nullptr;
    void* pWrapped = nullptr;
    void** pVtable = nullptr;
    void* pMethod = nullptr;
    if (!ReadNative(reinterpret_cast<const uint8_t*>(apBody) + 0x18, pWrapper) || !pWrapper ||
        !ReadNative(reinterpret_cast<const uint8_t*>(pWrapper) + 0x10, pWrapped) || pWrapped != apBody ||
        !ReadNative(pWrapper, pVtable) || !pVtable || !ReadNative(pVtable + 0x37, pMethod) || !pMethod)
        return false;
    MEMORY_BASIC_INFORMATION info{};
    if (!VirtualQuery(pMethod, &info, sizeof(info)) || info.State != MEM_COMMIT ||
        ((info.Protect & 0xFF) != PAGE_EXECUTE && (info.Protect & 0xFF) != PAGE_EXECUTE_READ &&
         (info.Protect & 0xFF) != PAGE_EXECUTE_READWRITE && (info.Protect & 0xFF) != PAGE_EXECUTE_WRITECOPY))
        return false;
    alignas(16) float position[4]{apPosition[0], apPosition[1], apPosition[2], 0.f};
    alignas(16) float rotation[4]{apRotation[0], apRotation[1], apRotation[2], apRotation[3]};
    using TSetPositionAndRotation = void(__fastcall*)(void*, const float*, const float*);
    reinterpret_cast<TSetPositionAndRotation>(pMethod)(pWrapper, position, rotation);
    // The owner's motion between its samples (Havok units per second): this PC's step then carries
    // the body the way the owner's did until the next placement, instead of dropping it.
    for (int axis = 0; axis < 3; ++axis)
        apBody->linearVelocity[axis] = apVelocity ? apVelocity[axis] : 0.f;
    apBody->linearVelocity[3] = 0.f;
    std::fill(std::begin(apBody->angularVelocity), std::end(apBody->angularVelocity), 0.f);
    return true;
}

bool Moving(const Vector<RigidBody*>& acBodies) noexcept
{
    for (auto* pBody : acBodies)
    {
        const float* v = pBody->linearVelocity;
        const float* w = pBody->angularVelocity;
        if (std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]) > kSettledLinear ||
            std::sqrt(w[0] * w[0] + w[1] * w[1] + w[2] * w[2]) > kSettledAngular)
            return true;
    }
    return false;
}
} // namespace

CorpseRagdollService::CorpseRagdollService(World& aWorld, entt::dispatcher& aDispatcher, TransportService& aTransport) noexcept
    : m_world(aWorld)
    , m_transport(aTransport)
{
    s_ragdollService.store(this, std::memory_order_release);
    m_updateConnection = aDispatcher.sink<UpdateEvent>().connect<&CorpseRagdollService::OnUpdate>(this);
    m_disconnectConnection = aDispatcher.sink<DisconnectedEvent>().connect<&CorpseRagdollService::OnDisconnected>(this);
    m_ragdollConnection = aDispatcher.sink<NotifyCorpseRagdoll>().connect<&CorpseRagdollService::OnCorpseRagdoll>(this);
}

void CorpseRagdollService::OnMainFrame() noexcept
{
    auto* pService = s_ragdollService.load(std::memory_order_acquire);
    if (!pService || !pService->m_applyOnMainFrame.load(std::memory_order_relaxed))
        return;
    std::lock_guard lock(pService->m_remoteLock);
    pService->ApplyRemote(NowMs());
}

void CorpseRagdollService::OnDisconnected(const DisconnectedEvent&) noexcept
{
    m_applyOnMainFrame.store(false, std::memory_order_relaxed);
    std::lock_guard lock(m_remoteLock);
    m_owned.clear();
    m_remote.clear();
}

bool CorpseRagdollService::IsFollowingOwner(const uint32_t aFormId) noexcept
{
    std::lock_guard lock(s_followingLock);
    const auto it = s_followingSinceMs.find(aFormId);
    return it != s_followingSinceMs.end() && NowMs() - it->second < 10000;
}

void CorpseRagdollService::OnCorpseRagdoll(const NotifyCorpseRagdoll& acMessage) noexcept
{
    std::lock_guard lock(m_remoteLock);
    if (auto* pActor = Utils::GetByServerId<Actor>(acMessage.ServerId))
    {
        std::lock_guard followingLock(s_followingLock);
        if (!s_followingSinceMs.contains(pActor->formID) || NowMs() - s_followingSinceMs[pActor->formID] > 10000)
            spdlog::info("Ragdoll {:X} (server {:X}): owner's ragdoll stream received (tick {}, {} bodies)", pActor->formID,
                acMessage.ServerId, acMessage.Tick, acMessage.Bodies.size());
        s_followingSinceMs[pActor->formID] = NowMs();
    }
    else
        spdlog::info("Ragdoll stream for server id {:X}: no actor here", acMessage.ServerId);
    auto& ragdoll = m_remote[acMessage.ServerId];
    const auto size = static_cast<uint32_t>(ragdoll.Ring.size());
    if (ragdoll.RingCount)
    {
        const auto& newest = ragdoll.Ring[(ragdoll.RingNext + size - 1) % size];
        if (acMessage.Tick <= newest.Tick)
            return;
        // A different body count is a different ragdoll (3D reloaded, another skeleton): restart.
        if (newest.Bodies.size() != acMessage.Bodies.size())
            ragdoll.RingCount = 0;
        // A gap means a new ragdoll event (a later knockdown): knock this copy again if needed.
        if (acMessage.Tick > newest.Tick + 2000)
        {
            ragdoll.Knocked = false;
            ragdoll.LiveLogged = false;
        }
    }
    // A new ragdoll event (first sample, or after a gap): knock this copy into ragdoll now, on
    // receipt, so its bodies follow from the first frame. Waiting for the death or knock sync left
    // the intro prisoner animated on the follower for the whole fall (24 s behind the owner).
    // The knock itself waits for the presentation time to reach the first sample (ApplyRemote).
    auto& sample = ragdoll.Ring[ragdoll.RingNext];
    sample.Tick = acMessage.Tick;
    std::copy(std::begin(acMessage.Origin), std::end(acMessage.Origin), std::begin(sample.Origin));
    sample.Bodies = acMessage.Bodies;
    ragdoll.RingNext = (ragdoll.RingNext + 1) % size;
    ragdoll.RingCount = (std::min)(ragdoll.RingCount + 1, size);
    ragdoll.Asleep = false;
}

void CorpseRagdollService::OnUpdate(const UpdateEvent&) noexcept
{
    if (!m_transport.IsConnected())
    {
        m_applyOnMainFrame.store(false, std::memory_order_relaxed);
        return;
    }
    const auto now = NowMs();
    if (now >= m_nextTickMs)
    {
        m_nextTickMs = now + kStreamMs;
        CaptureOwned(now);
    }
    m_applyOnMainFrame.store(true, std::memory_order_relaxed);
}

void CorpseRagdollService::CaptureOwned(const uint64_t aNowMs) noexcept
{
    Set<uint32_t> seen;
    auto view = m_world.view<FormIdComponent, LocalComponent>();
    for (auto entity : view)
    {
        auto* pActor = Cast<Actor>(TESForm::GetById(view.get<FormIdComponent>(entity).Id));
        if (!pActor || !pActor->GetNiNode() || !PhysicsOwnsSkeleton(pActor))
            continue;
        const uint32_t serverId = view.get<LocalComponent>(entity).Id;
        Vector<RigidBody*> bodies;
        if (!GetRagdollBodies(pActor, bodies))
            continue;
        seen.insert(serverId);

        auto& owned = m_owned[serverId];
        const bool moving = Moving(bodies);
        if (moving)
        {
            owned.SettledSinceMs = 0;
            owned.SentSettled = false;
        }
        else
        {
            if (!owned.SettledSinceMs)
                owned.SettledSinceMs = aNowMs;
            // Keep streaming briefly after it stops so the follower lands on the resting pose,
            // then resend it every few seconds for late arrivals.
            if (owned.SentSettled && aNowMs - owned.LastSentMs < kResendMs)
                continue;
        }

        CorpseRagdollRequest request{};
        request.ServerId = serverId;
        request.Tick = PoseCopyAuthority::GetCurrentTick();
        request.Origin[0] = pActor->position.x;
        request.Origin[1] = pActor->position.y;
        request.Origin[2] = pActor->position.z;
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
            spdlog::info("Ragdoll {:X} (server {:X}): streaming {} bodies", pActor->formID, serverId, bodies.size());
        owned.LastSentMs = aNowMs;
        if (!moving && aNowMs - owned.SettledSinceMs >= kSettleMs)
            owned.SentSettled = true;
    }
    for (auto it = m_owned.begin(); it != m_owned.end();)
        it = seen.contains(it->first) ? std::next(it) : m_owned.erase(it);
}

void CorpseRagdollService::ApplyRemote(const uint64_t aNowMs) noexcept
{
    const double presentationTime = PoseCopyAuthority::GetPresentationTimeMs();
    const auto presentation = static_cast<uint64_t>(presentationTime);
    for (auto it = m_remote.begin(); it != m_remote.end(); ++it)
    {
        auto& ragdoll = it.value();
        if (!ragdoll.RingCount)
            continue;
        const auto size = static_cast<uint32_t>(ragdoll.Ring.size());
        const auto sample = [&](uint32_t i) -> const Sample& { return ragdoll.Ring[(ragdoll.RingNext + size - ragdoll.RingCount + i) % size]; };
        const auto& newest = sample(ragdoll.RingCount - 1);
        const bool settled = presentation > newest.Tick + kHoldMs;
        if (settled && ragdoll.Asleep && aNowMs < ragdoll.NextCheckMs)
            continue;

        auto* pActor = Utils::GetByServerId<Actor>(it->first);
        if (!pActor || !pActor->GetNiNode())
            continue;
        // Not ragdolling here yet: knock this copy when the presentation time reaches the owner's first
        // ragdoll sample, so it falls from the owner's pose at that moment. Knocked on receipt, it
        // went ragdoll 255 ms early and its bodies jumped to a pose the owner reached later (156
        // units for the running intro prisoner).
        if (!PhysicsOwnsSkeleton(pActor))
        {
            if (!ragdoll.Knocked && presentation >= sample(0).Tick && pActor->currentProcess)
            {
                ragdoll.Knocked = true;
                pActor->currentProcess->KnockExplosion(pActor, &pActor->position, 0.f);
                spdlog::info("Ragdoll {:X}: knocked this copy into ragdoll at the owner's first sample (tick {}, presentation {})",
                    pActor->formID, sample(0).Tick, presentation);
            }
            continue;
        }
        // Ragdolling here before the owner's ragdoll starts on this PC's timeline (the death sync
        // arrives as early as the first sample): leave the bodies until the presentation time gets there.
        if (presentation < sample(0).Tick)
            continue;
        Vector<RigidBody*> bodies;
        if (!GetRagdollBodies(pActor, bodies))
            continue;
        if (bodies.size() != newest.Bodies.size())
        {
            if (!ragdoll.CountMismatchLogged)
                spdlog::warn("Ragdoll {:X}: {} local bodies, owner sent {}", pActor->formID, bodies.size(), newest.Bodies.size());
            ragdoll.CountMismatchLogged = true;
            continue;
        }

        // Owner pose at the presentation tick: interpolate between the bracketing samples, or hold
        // the nearest end of the ring.
        const Sample* pA = &newest;
        const Sample* pB = &newest;
        float t = 0.f;
        if (presentation < sample(0).Tick)
            pA = pB = &sample(0);
        else
        {
            for (uint32_t i = 1; i < ragdoll.RingCount; ++i)
            {
                const auto& a = sample(i - 1);
                const auto& b = sample(i);
                if (presentation > b.Tick || a.Bodies.size() != b.Bodies.size())
                    continue;
                pA = &a;
                pB = &b;
                t = b.Tick > a.Tick ? static_cast<float>((presentationTime - static_cast<double>(a.Tick)) /
                    static_cast<double>(b.Tick - a.Tick)) : 1.f;
                break;
            }
        }

        float worstDrift = 0.f;
        for (size_t i = 0; i < bodies.size(); ++i)
        {
            const auto& x = pA->Bodies[i];
            const auto& y = pB->Bodies[i];
            // World position on the owner's timeline: its origin plus the body offset, both
            // interpolated; the velocity is the owner's motion across this sample span.
            float wanted[3];
            float velocity[3]{};
            const float spanSeconds = pB->Tick > pA->Tick ? static_cast<float>(pB->Tick - pA->Tick) / 1000.f : 0.f;
            for (int axis = 0; axis < 3; ++axis)
            {
                const float from = pA->Origin[axis] + x.Position[axis];
                const float to = pB->Origin[axis] + y.Position[axis];
                wanted[axis] = (from + (to - from) * t) / kHavokToGameUnits;
                if (spanSeconds > 0.f && !settled)
                    velocity[axis] = (to - from) / kHavokToGameUnits / spanSeconds;
            }
            for (int axis = 0; axis < 3; ++axis)
                worstDrift = (std::max)(worstDrift, std::abs((bodies[i]->transform[12 + axis] - wanted[axis]) * kHavokToGameUnits));
            float dot = 0.f;
            for (int k = 0; k < 4; ++k)
                dot += x.Rotation[k] * y.Rotation[k];
            const float sign = dot < 0.f ? -1.f : 1.f;
            float rotation[4];
            float norm = 0.f;
            for (int k = 0; k < 4; ++k)
            {
                rotation[k] = x.Rotation[k] + (sign * y.Rotation[k] - x.Rotation[k]) * t;
                norm += rotation[k] * rotation[k];
            }
            norm = norm > 0.f ? 1.f / std::sqrt(norm) : 1.f;
            for (float& value : rotation)
                value *= norm;
            // Steer, do not place: the owner's motion plus a correction that closes the gap within
            // 0.1 s, as for host-driven carts. Placing each frame snapped the first frame of a fall
            // (49 units for the intro prisoner). Placed only when far off, and while settling.
            const float* current = &bodies[i]->transform[12];
            const float gap[3]{wanted[0] - current[0], wanted[1] - current[1], wanted[2] - current[2]};
            const float gapLength = std::sqrt(gap[0] * gap[0] + gap[1] * gap[1] + gap[2] * gap[2]);
            if (settled || gapLength > 3.f)
                SetBodyPose(bodies[i], wanted, rotation, velocity);
            else
            {
                constexpr float kSteerSeconds = 0.1f;
                for (int axis = 0; axis < 3; ++axis)
                    bodies[i]->linearVelocity[axis] = velocity[axis] + gap[axis] / kSteerSeconds;
                float have[4];
                MatrixToQuaternion(bodies[i]->transform, have);
                // delta = wanted * conjugate(have), as (x, y, z, w)
                const float hx = -have[0], hy = -have[1], hz = -have[2], hw = have[3];
                const float wx = rotation[0], wy = rotation[1], wz = rotation[2], ww = rotation[3];
                float dx = ww * hx + wx * hw + wy * hz - wz * hy;
                float dy = ww * hy - wx * hz + wy * hw + wz * hx;
                float dz = ww * hz + wx * hy - wy * hx + wz * hw;
                const float dw = ww * hw - wx * hx - wy * hy - wz * hz;
                if (dw < 0.f)
                {
                    dx = -dx;
                    dy = -dy;
                    dz = -dz;
                }
                bodies[i]->angularVelocity[0] = dx * 2.f / kSteerSeconds;
                bodies[i]->angularVelocity[1] = dy * 2.f / kSteerSeconds;
                bodies[i]->angularVelocity[2] = dz * 2.f / kSteerSeconds;
            }
        }

        if (!settled && !ragdoll.LiveLogged)
        {
            ragdoll.LiveLogged = true;
            spdlog::info("Ragdoll {:X}: following the owner's ragdoll live ({} bodies, drift {:.2f})", pActor->formID,
                bodies.size(), worstDrift);
        }
        if (settled)
        {
            // The owner stopped streaming: the ragdoll rests at its final pose; sleep it there.
            for (auto* pBody : bodies)
                SleepBody(pBody);
            if (!ragdoll.Asleep && ragdoll.Applications++ < 3)
                spdlog::info("Ragdoll {:X}: resting at the owner's pose (drift was {:.2f})", pActor->formID, worstDrift);
            ragdoll.Asleep = true;
            ragdoll.NextCheckMs = aNowMs + 1000;
        }
    }
}
