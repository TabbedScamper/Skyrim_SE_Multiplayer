#include <Services/CorpseRagdollService.h>

#include <World.h>
#include <Events/UpdateEvent.h>
#include <Events/DisconnectedEvent.h>
#include <Messages/NotifyCorpseRagdoll.h>
#include <Messages/DismemberRequest.h>
#include <Messages/NotifyDismember.h>
#include <Services/TransportService.h>
#include <Components.h>

#include <Games/Skyrim/Actor.h>
#include <Games/ActorExtension.h>
#include <Games/Skyrim/BSAnimationGraphManager.h>
#include <Games/Skyrim/Havok/ActorPoseDiagnosticViews.h>
#include <Games/Skyrim/Havok/PoseCopyAuthority.h>
#include <Utils.h>
#include <AI/AIProcess.h>
#include <NetImmerse/NiNode.h>

#include <chrono>
#include <cmath>
#include <optional>

namespace
{
using namespace ActorPoseDiagnosticViews;

// Round-6 source research (the task's edit list excludes docs/REFERENCE_RESEARCH.md):
// https://github.com/adamhynek/higgs/blob/master/src/physics.cpp
// Adopt native hard-keyframe COM/angular contact velocities and world locking.
// https://github.com/adamhynek/activeragdoll/blob/master/src/main.cpp
// Its controller/blending is useful for physical animation, but rejected here as the
// position authority: the follower must match every streamed body, including at rest.
// Exact-runtime corpus: 61410 / 140B5B0B0 is the step wrapper; 60898 / 140B4CEF0
// places a body's transform and updates the broadphase; 60908 / 140B4D3F0 changes
// motion type (may queue under critical operations); 60850 / 140B4B0C0 requests sleep.
// Havok world units to game units (matches ObjectService).
constexpr float kHavokToGameUnits = 70.f;
std::atomic<CorpseRagdollService*> s_ragdollService{};
std::mutex s_followingLock;
std::unordered_map<uint32_t, uint64_t> s_followingSinceMs; // form id -> last sample received
std::mutex s_dismemberLock;
struct CreatedLimb
{
    uint64_t Tick{};
    NiNode* Node{}; // identity only; resolve through AIProcess before using it
};
std::unordered_map<uint32_t, CreatedLimb> s_localDismembers;
std::unordered_map<uint32_t, NiNode*> s_createdHeads;
std::unordered_map<uint32_t, uint64_t> s_authorizedDismembers;
// A body counts as settled below this speed (Havok units/s, about 2 game units/s).
constexpr float kSettledLinear = 0.03f;
constexpr float kSettledAngular = 0.05f;
constexpr uint64_t kSettleMs = 1500;
constexpr uint64_t kStreamMs = 50;
constexpr uint64_t kResendMs = 5000;

uint64_t StreamKey(uint32_t aServerId, uint32_t aLimb) noexcept
{
    return (static_cast<uint64_t>(aServerId) << 32) | aLimb;
}

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
bool GetRagdollBodies(Actor* apActor, Vector<RigidBody*>& aBodies, const char** apReason = nullptr,
    bool aAddToWorld = false, void** apDriver = nullptr) noexcept
{
    const auto fail = [apReason](const char* apWhy)
    {
        if (apReason)
            *apReason = apWhy;
        return false;
    };
    aBodies.clear();
    GraphRef ref;
    if (!apActor || !apActor->animationGraphHolder.GetBSAnimationGraph(&ref.pManager) || !ref.pManager)
        return fail("no animation graph manager");
    BSScopedLock<BSRecursiveLock> lock(ref.pManager->lock);
    const auto count = ref.pManager->animationGraphs.size;
    const auto index = ref.pManager->animationGraphIndex;
    if (!count || count > 32 || index >= count)
        return fail("no active graph");
    if (aAddToWorld)
    {
        // 63621 / 140BD36A0, graph vtable slot 2. Native code visits EVERY graph.
        // KillImpl and KnockExplosion do not guarantee insertion after a scripted death.
        using TAddRagdoll = bool(void*);
        POINTER_SKYRIMSE(TAddRagdoll, addRagdoll, 63621);
        for (uint32_t i = 0; i < count; ++i)
        {
            auto* graph = ref.pManager->animationGraphs.Get(i);
            AnimationGraph state{};
            RagdollDriver driverState{};
            RagdollInstance instance{};
            if (ReadNative(graph, state) && state.physicsWorld && state.generatorOutputs[0] &&
                ReadNative(state.characterInstance.ragdollDriver, driverState) && ReadNative(driverState.ragdoll, instance) &&
                instance.rigidBodies.size > 0 && instance.rigidBodies.size <= static_cast<int32_t>(CorpseRagdollRequest::kMaxBodies))
                addRagdoll.Get()(graph);
        }
    }
    void* pGraph = ref.pManager->animationGraphs.Get(index);
    AnimationGraph graph{};
    RagdollDriver driver{};
    RagdollInstance ragdoll{};
    if (!ReadNative(pGraph, graph))
        return fail("graph unreadable");
    if (!ReadNative(graph.characterInstance.ragdollDriver, driver))
        return fail("no ragdoll driver");
    if (apDriver)
        *apDriver = graph.characterInstance.ragdollDriver;
    if (!ReadNative(driver.ragdoll, ragdoll))
        return fail("no ragdoll instance");
    if (ragdoll.rigidBodies.size <= 0 || ragdoll.rigidBodies.size > static_cast<int32_t>(CorpseRagdollRequest::kMaxBodies))
        return fail("bad ragdoll body count");
    for (int32_t i = 0; i < ragdoll.rigidBodies.size; ++i)
    {
        void* pBody{};
        RigidBody probe{};
        if (!ReadNative(ragdoll.rigidBodies.data + i, pBody) || !ReadNative(pBody, probe))
            return fail("body unreadable");
        if (!probe.world)
            return fail("bodies not in the physics world");
        aBodies.push_back(static_cast<RigidBody*>(pBody));
    }
    return true;
}

bool GetHeadBody(Actor* apActor, Vector<RigidBody*>& aBodies) noexcept
{
    aBodies.clear();
    auto* node = apActor->GetDetachedLimbNode(1);
    {
        std::lock_guard lock(s_dismemberLock);
        const auto it = s_createdHeads.find(apActor->formID);
        if (!node || it == s_createdHeads.end() || it->second != node)
            return false;
    }
    // 37640 stores the CLONED head with 39973. 39974 can return the original limb
    // before that, hence the creation-hook identity check above. No transient form ID is used.
    using TCollision = void*(NiNode*);
    using TBody = void*(void*);
    POINTER_SKYRIMSE(TCollision, collision, 26022);
    POINTER_SKYRIMSE(TBody, body, 20014);
    auto* object = collision.Get()(node);
    auto* wrapper = object ? body.Get()(object) : nullptr;
    RigidBody* rigid{};
    RigidBody state{};
    if (!wrapper || !ReadNative(static_cast<uint8_t*>(wrapper) + 0x10, rigid) || !ReadNative(rigid, state) || !state.world)
        return false;
    aBodies.push_back(rigid);
    return true;
}

struct PhysicsLock
{
    void* Wrapper{};
    explicit PhysicsLock(const Vector<RigidBody*>& acBodies)
    {
        if (acBodies.empty() || !acBodies.front()->world)
            return;
        auto* world = acBodies.front()->world;
        for (auto* body : acBodies)
            if (body->world != world)
                return;
        // ahkpWorld::userData +430 is the bhkWorld. 77928/77929 lock its +C598.
        if (!ReadNative(static_cast<uint8_t*>(world) + 0x430, Wrapper) || !Wrapper)
        {
            Wrapper = nullptr;
            return;
        }
        using TLock = void(void*);
        POINTER_SKYRIMSE(TLock, lock, 77928);
        lock.Get()(Wrapper);
    }
    ~PhysicsLock()
    {
        if (Wrapper)
        {
            using TUnlock = void(void*);
            POINTER_SKYRIMSE(TUnlock, unlock, 77929);
            unlock.Get()(Wrapper);
        }
    }
};

void SetMotion(RigidBody* apBody, uint8_t aMotion) noexcept
{
    using TSetMotion = void(void*, uint8_t, uint8_t, uint8_t);
    POINTER_SKYRIMSE(TSetMotion, setMotion, 60908);
    if (apBody->motionType != aMotion)
        setMotion.Get()(apBody, aMotion, 1, 0);
}

// The ragdoll is simulating: its bodies are in the world and not keyframed to the animation (a
// death animation keyframes them, or keeps them out of the world, before the ragdoll takes over).
bool RagdollSimulating(Actor* apActor, Vector<RigidBody*>& aBodies, const char** apReason = nullptr) noexcept
{
    if (!GetRagdollBodies(apActor, aBodies, apReason))
        return false;
    if (aBodies.empty() || aBodies[0]->motionType == 4)
    {
        if (apReason)
            *apReason = "bodies keyframed to the animation";
        return false;
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

// Caller holds the bhkWorld write lock, including velocity clearing and deactivation.
void SetBodyPose(RigidBody* apBody, const QsTransform& acPose, bool aClearVelocity = true) noexcept
{
    using TSetPose = void(void*, const float*, const float*);
    POINTER_SKYRIMSE(TSetPose, setPose, 60898);
    setPose.Get()(apBody, acPose.translation, acPose.rotation);
    if (aClearVelocity)
    {
        std::fill(std::begin(apBody->linearVelocity), std::end(apBody->linearVelocity), 0.f);
        std::fill(std::begin(apBody->angularVelocity), std::end(apBody->angularVelocity), 0.f);
    }
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

struct CorpseRagdollService::StepBinding
{
    Vector<RigidBody*> Bodies;
    Vector<uint32_t> Ids;
    void* World{};
    void* Driver{};
    uint32_t FormId{};
    uint32_t Limb{};
    std::atomic<bool> Active{true};
    // Only touched by this world's serial physics steps.
    bool Logged{};
    bool Asleep{};
    uint64_t NextLogMs{};
    ~StepBinding()
    {
        using TReference = void(void*);
        POINTER_SKYRIMSE(TReference, removeReference, 57011);
        for (auto* body : Bodies)
            removeReference.Get()(body);
        if (Driver)
            removeReference.Get()(Driver);
    }
};

struct CorpseRagdollService::StepFrame
{
    struct Stream
    {
        std::shared_ptr<StepBinding> Binding;
        Vector<Sample> Samples; // oldest first, immutable after publication
        uint64_t EndTick{};

        bool Evaluate(double aTime, size_t aBody, QsTransform& aPose, bool& aSettled, uint64_t& aTick) const noexcept
        {
            if (Samples.empty() || aTime < Samples.front().Tick || (EndTick && aTime >= EndTick))
                return false;
            size_t index = 0;
            while (index + 1 < Samples.size() && Samples[index + 1].Tick <= aTime)
                ++index;
            const auto& a = Samples[index];
            // State belongs to the latest PRESENTED sample. A future heartbeat must
            // not wake a settled body for the presentation delay every five seconds.
            aSettled = a.Settled;
            aTick = a.Tick;
            const auto& b = aSettled || index + 1 == Samples.size() ? a : Samples[index + 1];
            if (aBody >= a.Bodies.size() || aBody >= b.Bodies.size())
                return false;
            const float t = a.Tick == b.Tick ? 0.f : std::clamp(
                static_cast<float>((aTime - a.Tick) / static_cast<double>(b.Tick - a.Tick)), 0.f, 1.f);
            const auto& x = a.Bodies[aBody];
            const auto& y = b.Bodies[aBody];
            aPose = {};
            for (int axis = 0; axis < 3; ++axis)
            {
                const float from = a.Origin[axis] + x.Position[axis];
                const float to = b.Origin[axis] + y.Position[axis];
                aPose.translation[axis] = (from + (to - from) * t) / kHavokToGameUnits;
            }
            float dot = 0.f;
            for (int axis = 0; axis < 4; ++axis)
                dot += x.Rotation[axis] * y.Rotation[axis];
            float norm = 0.f;
            for (int axis = 0; axis < 4; ++axis)
            {
                aPose.rotation[axis] = x.Rotation[axis] + ((dot < 0.f ? -1.f : 1.f) * y.Rotation[axis] - x.Rotation[axis]) * t;
                norm += aPose.rotation[axis] * aPose.rotation[axis];
                aPose.scale[axis] = 1.f;
            }
            if (!std::isfinite(norm) || norm < 0.0001f)
                return false;
            for (float& value : aPose.rotation)
                value /= std::sqrt(norm);
            return true;
        }
    };
    Vector<Stream> Streams;
};

void CorpseRagdollService::OnHavokStep(void* apWorld, float aDeltaTime, bool aAfterStep) noexcept
{
    // Keep the same frame/time across the solver. Retained bodies survive unload;
    // world + uid checks prevent applying an old binding to a replacement object.
    struct Step
    {
        std::shared_ptr<StepFrame> Frame;
        void* World{};
        double Time{};
    };
    thread_local Vector<Step> steps;
    auto* service = s_ragdollService.load(std::memory_order_acquire);
    if (!aAfterStep)
    {
        Step step;
        step.World = apWorld;
        step.Time = PoseCopyAuthority::GetPresentationTimeMs();
        if (service && service->m_applyOnMainFrame.load(std::memory_order_relaxed))
        {
            std::lock_guard lock(service->m_stepLock);
            step.Frame = service->m_stepFrame;
        }
        steps.push_back(std::move(step));
    }
    if (steps.empty() || steps.back().World != apWorld)
        return;
    const auto step = steps.back();
    if (aAfterStep)
        steps.pop_back();
    if (!step.Frame || !service || !service->m_applyOnMainFrame.load(std::memory_order_relaxed) ||
        !std::isfinite(aDeltaTime) || aDeltaTime <= 0.f)
        return;
    for (const auto& stream : step.Frame->Streams)
    {
        auto& binding = *stream.Binding;
        if (!binding.Active.load(std::memory_order_acquire) || binding.World != apWorld)
            continue;
        bool valid = true;
        for (size_t i = 0; i < binding.Bodies.size(); ++i)
            valid &= binding.Bodies[i]->world == apWorld && binding.Bodies[i]->uid == binding.Ids[i];
        if (!valid)
            continue;
        PhysicsLock lock(binding.Bodies);
        if (!lock.Wrapper || !binding.Active.load(std::memory_order_acquire))
            continue;
        for (auto* body : binding.Bodies)
        {
            SetMotion(body, 4);
            valid &= body->motionType == 4;
        }
        if (!valid)
            continue; // world critical operations can queue the motion changes
        bool settled = false;
        uint64_t tick{};
        float worstBefore = 0.f;
        float worstAfter = 0.f;
        float worstAngle = 0.f;
        size_t placed = 0;
        for (size_t i = 0; i < binding.Bodies.size(); ++i)
        {
            QsTransform target{};
            if (!stream.Evaluate(step.Time, i, target, settled, tick))
                break;
            auto* body = binding.Bodies[i];
            QsTransform start = target;
            bool previousSettled{};
            uint64_t previousTick{};
            if (!aAfterStep && !settled)
                stream.Evaluate(step.Time - aDeltaTime * 1000.0, i, start, previousSettled, previousTick);
            float error = 0.f;
            for (int axis = 0; axis < 3; ++axis)
            {
                const float delta = target.translation[axis] - body->transform[12 + axis];
                error += delta * delta;
            }
            worstBefore = (std::max)(worstBefore, std::sqrt(error) * kHavokToGameUnits);
            SetBodyPose(body, aAfterStep || settled ? target : start, !aAfterStep || settled);
            if (!aAfterStep && !settled)
            {
                // HIGGS physics.cpp uses this hard-keyframe utility. 62478 / 140B9A9C0
                // derives COM-correct contact velocities and shortest-arc angular velocity.
                // 60898 / 140B4CEF0 remains the position authority, regardless of contacts.
                using THardKeyframe = void(const float*, const float*, float, void*);
                POINTER_SKYRIMSE(THardKeyframe, hardKeyframe, 62478);
                hardKeyframe.Get()(target.translation, target.rotation, 1.f / aDeltaTime, body);
            }
            if (settled)
                SleepBody(body);
            error = 0.f;
            for (int axis = 0; axis < 3; ++axis)
            {
                const float delta = target.translation[axis] - body->transform[12 + axis];
                error += delta * delta;
            }
            worstAfter = (std::max)(worstAfter, std::sqrt(error) * kHavokToGameUnits);
            float rotation[4];
            MatrixToQuaternion(body->transform, rotation);
            float dot = 0.f;
            for (int axis = 0; axis < 4; ++axis)
                dot += target.rotation[axis] * rotation[axis];
            worstAngle = (std::max)(worstAngle, 2.f * std::acos(std::clamp(std::abs(dot), 0.f, 1.f)) * 57.29578f);
            ++placed;
        }
        if (!aAfterStep || placed != binding.Bodies.size())
            continue;
        const auto now = NowMs();
        if (!binding.Logged || now >= binding.NextLogMs)
        {
            spdlog::info("Ragdoll {:X} limb {}: physics-step keyframe placed {} bodies at tick {}, pre-gap {:.2f}, residual {:.3f} u/{:.3f} deg (driver {})",
                binding.FormId, binding.Limb, placed, tick, worstBefore, worstAfter, worstAngle, fmt::ptr(binding.Driver));
            binding.Logged = true;
            binding.NextLogMs = now + kResendMs;
        }
        if (settled && !binding.Asleep)
            spdlog::info("Ragdoll {:X} limb {}: exact owner settle at tick {} in physics step", binding.FormId, binding.Limb, tick);
        binding.Asleep = settled;
    }
}

void CorpseRagdollService::PlaceForReadback(void* apDriver) noexcept
{
    auto* service = s_ragdollService.load(std::memory_order_acquire);
    if (!service || !service->m_applyOnMainFrame.load(std::memory_order_relaxed))
        return;
    std::shared_ptr<StepFrame> frame;
    {
        std::lock_guard lock(service->m_stepLock);
        frame = service->m_stepFrame;
    }
    if (!frame)
        return;
    const double presentation = PoseCopyAuthority::GetPresentationTimeMs();
    for (const auto& stream : frame->Streams)
    {
        const auto& binding = *stream.Binding;
        if (binding.Driver != apDriver || !binding.Active.load(std::memory_order_acquire))
            continue;
        bool valid = true;
        for (size_t i = 0; i < binding.Bodies.size(); ++i)
            valid &= binding.Bodies[i]->world == binding.World && binding.Bodies[i]->uid == binding.Ids[i] &&
                binding.Bodies[i]->motionType == 4;
        if (!valid)
            return;
        PhysicsLock lock(binding.Bodies);
        if (!lock.Wrapper || !binding.Active.load(std::memory_order_acquire))
            return;
        for (size_t i = 0; i < binding.Bodies.size(); ++i)
        {
            QsTransform target{};
            bool settled{};
            uint64_t tick{};
            if (!stream.Evaluate(presentation, i, target, settled, tick))
                return;
            SetBodyPose(binding.Bodies[i], target, settled);
            if (settled)
                SleepBody(binding.Bodies[i]);
        }
        return;
    }
}

CorpseRagdollService::CorpseRagdollService(World& aWorld, entt::dispatcher& aDispatcher, TransportService& aTransport) noexcept
    : m_world(aWorld)
    , m_transport(aTransport)
{
    s_ragdollService.store(this, std::memory_order_release);
    m_updateConnection = aDispatcher.sink<UpdateEvent>().connect<&CorpseRagdollService::OnUpdate>(this);
    m_disconnectConnection = aDispatcher.sink<DisconnectedEvent>().connect<&CorpseRagdollService::OnDisconnected>(this);
    m_ragdollConnection = aDispatcher.sink<NotifyCorpseRagdoll>().connect<&CorpseRagdollService::OnCorpseRagdoll>(this);
    m_dismemberConnection = aDispatcher.sink<NotifyDismember>().connect<&CorpseRagdollService::OnDismember>(this);
}

void CorpseRagdollService::OnMainFrame() noexcept
{
    auto* pService = s_ragdollService.load(std::memory_order_acquire);
    if (!pService)
        return;
    std::lock_guard lock(pService->m_remoteLock);
    if (pService->m_disconnectPending.exchange(false))
        pService->ResetOnMainFrame();
    if (!pService->m_applyOnMainFrame.load(std::memory_order_relaxed))
        return;
    const auto now = NowMs();
    if (now >= pService->m_nextTickMs)
    {
        pService->m_nextTickMs = now + kStreamMs;
        pService->CaptureOwned(now);
    }
    pService->ApplyDismembers(NowMs());
    pService->ApplyRemote(NowMs());
}

void CorpseRagdollService::OnDisconnected(const DisconnectedEvent&) noexcept
{
    m_applyOnMainFrame.store(false, std::memory_order_relaxed);
    m_disconnectPending.store(true);
}

void CorpseRagdollService::ResetOnMainFrame() noexcept
{
    ApplyRemote(NowMs(), true);
    m_owned.clear();
    m_remote.clear();
    m_dismembers.clear();
    m_sentDismembers.clear();
    PoseCopyAuthority::ClearRagdollAuthority();
    {
        std::lock_guard followingLock(s_followingLock);
        s_followingSinceMs.clear();
    }
    {
        std::lock_guard dismemberLock(s_dismemberLock);
        s_localDismembers.clear();
        s_createdHeads.clear();
        s_authorizedDismembers.clear();
    }
}

std::string CorpseRagdollService::DescribeRagdollBodies(Actor* apActor) noexcept
{
    Vector<RigidBody*> bodies;
    if (!apActor || !GetRagdollBodies(apActor, bodies))
        return "[]";
    std::string json = "[";
    for (size_t i = 0; i < bodies.size(); ++i)
        json += fmt::format("{}[{:.1f},{:.1f},{:.1f}]", i ? "," : "", bodies[i]->transform[12] * kHavokToGameUnits,
            bodies[i]->transform[13] * kHavokToGameUnits, bodies[i]->transform[14] * kHavokToGameUnits);
    return json + "]";
}

bool CorpseRagdollService::IsFollowingOwner(const uint32_t aFormId) noexcept
{
    std::lock_guard lock(s_followingLock);
    const auto it = s_followingSinceMs.find(aFormId);
    return it != s_followingSinceMs.end();
}

void CorpseRagdollService::RecordDismember(Actor* apActor, bool aCreated) noexcept
{
    auto* service = s_ragdollService.load(std::memory_order_acquire);
    if (!service || !service->m_transport.IsConnected())
        return;
    auto* currentNode = apActor->GetDetachedLimbNode(1);
    auto* node = aCreated ? currentNode : nullptr;
    using TIsLimbGone = bool(Actor*, uint32_t);
    POINTER_SKYRIMSE(TIsLimbGone, isLimbGone, 19765);
    const bool alreadyGone = isLimbGone.Get()(apActor, 1);
    std::lock_guard lock(s_dismemberLock);
    if (aCreated && node)
    {
        s_createdHeads[apActor->formID] = node;
        spdlog::info("Dismember {:X}: native head clone created (37640), binding through 39974", apActor->formID);
    }
    if (apActor->GetExtension()->IsRemote())
        return;
    auto& event = s_localDismembers[apActor->formID];
    if (!aCreated && !alreadyGone && event.Node && event.Node != currentNode)
    {
        event = {};
        s_createdHeads.erase(apActor->formID);
    }
    if (!event.Tick || (node && event.Node && event.Node != node))
        event.Tick = PoseCopyAuthority::GetCurrentTick();
    if (node)
        event.Node = node;
}

bool CorpseRagdollService::IsDismemberAuthorized(uint32_t aFormId) noexcept
{
    std::lock_guard lock(s_dismemberLock);
    return s_authorizedDismembers.contains(aFormId);
}

void CorpseRagdollService::OnDismember(const NotifyDismember& acMessage) noexcept
{
    if (!acMessage.IsValid() || acMessage.Limb != 1 || !acMessage.Tick)
        return;
    std::lock_guard lock(m_remoteLock);
    auto& event = m_dismembers[acMessage.ServerId];
    if (event.Tick >= acMessage.Tick)
        return;
    event = {};
    event.Tick = acMessage.Tick;
    if (auto* actor = Utils::GetByServerId<Actor>(acMessage.ServerId))
    {
        std::lock_guard dismemberLock(s_dismemberLock);
        s_authorizedDismembers.erase(actor->formID);
    }
    spdlog::info("Dismember server {:X}: owner limb {} event received at tick {}", acMessage.ServerId, acMessage.Limb, acMessage.Tick);
}

void CorpseRagdollService::ApplyDismembers(uint64_t aNowMs) noexcept
{
    const auto presentation = static_cast<uint64_t>(PoseCopyAuthority::GetPresentationTimeMs());
    for (auto it = m_dismembers.begin(); it != m_dismembers.end(); ++it)
    {
        const auto serverId = it->first;
        auto& event = it.value();
        auto* actor = Utils::GetByServerId<Actor>(serverId);
        if (!actor || !actor->GetExtension()->IsRemote() || !actor->GetNiNode() ||
            presentation < event.Tick || aNowMs < event.RetryAtMs)
            continue;
        auto* root = actor->GetNiNode();
        if (event.Root != root)
        {
            event.Root = root;
            event.Applied = false;
        }
        Vector<RigidBody*> bodies;
        if (event.Applied && GetHeadBody(actor, bodies))
            continue;
        {
            std::lock_guard lock(s_dismemberLock);
            s_authorizedDismembers[actor->formID] = event.Tick;
        }
        actor->ApplyRemoteDecapitation();
        event.Applied = true;
        event.RetryAtMs = aNowMs + 250;
        if (!event.Logged)
        {
            event.Logged = true;
            spdlog::info("Dismember {:X}: replayed owner limb 1 at presentation {} (event {})", actor->formID, presentation, event.Tick);
        }
    }
}

void CorpseRagdollService::OnCorpseRagdoll(const NotifyCorpseRagdoll& acMessage) noexcept
{
    if (!acMessage.IsValid() || acMessage.Limb > 1)
        return;
    std::lock_guard lock(m_remoteLock);
    if (acMessage.Limb)
    {
        NotifyDismember event{};
        event.ServerId = acMessage.ServerId;
        event.Limb = acMessage.Limb;
        event.Tick = acMessage.DismemberTick;
        OnDismember(event);
    }
    const auto key = StreamKey(acMessage.ServerId, acMessage.Limb);
    auto& ragdoll = m_remote[key];
    const auto size = static_cast<uint32_t>(ragdoll.Ring.size());
    if (!acMessage.Active)
    {
        if ((!ragdoll.RingCount || acMessage.Tick > ragdoll.Ring[(ragdoll.RingNext + size - 1) % size].Tick) &&
            acMessage.Tick > ragdoll.EndTick)
            ragdoll.EndTick = acMessage.Tick;
        return;
    }
    if (acMessage.Tick <= ragdoll.EndTick)
        return;
    if (ragdoll.RingCount)
    {
        const auto& newest = ragdoll.Ring[(ragdoll.RingNext + size - 1) % size];
        if (acMessage.Tick <= newest.Tick || (ragdoll.EndTick && acMessage.Tick <= ragdoll.EndTick))
            return;
        if (newest.Bodies.size() != acMessage.Bodies.size() || ragdoll.DismemberTick != acMessage.DismemberTick || ragdoll.EndTick)
        {
            ragdoll.RingCount = 0;
            ragdoll.Knocked = false;
        }
    }
    ragdoll.EndTick = 0;
    ragdoll.DismemberTick = acMessage.DismemberTick;
    if (!ragdoll.RingCount)
        spdlog::info("Ragdoll server {:X} limb {}: owner's stream received (tick {}, {} bodies)", acMessage.ServerId,
            acMessage.Limb, acMessage.Tick, acMessage.Bodies.size());
    if (!acMessage.Limb)
    {
        if (auto* actor = Utils::GetByServerId<Actor>(acMessage.ServerId))
        {
            std::lock_guard followingLock(s_followingLock);
            s_followingSinceMs[actor->formID] = NowMs();
            PoseCopyAuthority::SetRagdollPending(actor->formID, true);
        }
    }
    auto& sample = ragdoll.Ring[ragdoll.RingNext];
    sample.Tick = acMessage.Tick;
    sample.Settled = acMessage.Settled;
    sample.Dying = acMessage.Dying;
    std::copy(std::begin(acMessage.Origin), std::end(acMessage.Origin), std::begin(sample.Origin));
    sample.Bodies = acMessage.Bodies;
    ragdoll.RingNext = (ragdoll.RingNext + 1) % size;
    ragdoll.RingCount = (std::min)(ragdoll.RingCount + 1, size);
}

void CorpseRagdollService::OnUpdate(const UpdateEvent&) noexcept
{
    if (!m_transport.IsConnected())
    {
        m_applyOnMainFrame.store(false, std::memory_order_relaxed);
        return;
    }
    m_applyOnMainFrame.store(true, std::memory_order_relaxed);
}

void CorpseRagdollService::CaptureOwned(const uint64_t aNowMs) noexcept
{
    std::unordered_map<uint32_t, CreatedLimb> events;
    {
        std::lock_guard lock(s_dismemberLock);
        events = s_localDismembers;
    }
    Set<uint64_t> seen;
    auto view = m_world.view<FormIdComponent, LocalComponent>();
    for (auto entity : view)
    {
        auto* actor = Cast<Actor>(TESForm::GetById(view.get<FormIdComponent>(entity).Id));
        if (!actor || !actor->GetNiNode())
            continue;
        const uint32_t serverId = view.get<LocalComponent>(entity).Id;
        GraphRef graphRef;
        std::optional<BSScopedLock<BSRecursiveLock>> graphLock;
        if (actor->animationGraphHolder.GetBSAnimationGraph(&graphRef.pManager) && graphRef.pManager)
            graphLock.emplace(graphRef.pManager->lock);
        const auto event = events.find(actor->formID);
        const uint64_t eventTick = event != events.end() ? event->second.Tick : 0;
        if (eventTick && m_sentDismembers[serverId] != eventTick)
        {
            DismemberRequest request{};
            request.ServerId = serverId;
            request.Tick = eventTick;
            if (m_transport.Send(request))
            {
                m_sentDismembers[serverId] = eventTick;
                spdlog::info("Dismember {:X} (server {:X}): sent reliable limb 1 event at {}", actor->formID, serverId, eventTick);
            }
        }
        for (uint32_t limb = 0; limb <= 1; ++limb)
        {
            if ((!limb && !PhysicsOwnsSkeleton(actor)) || (limb && !eventTick))
                continue;
            const auto key = StreamKey(serverId, limb);
            seen.insert(key);
            Vector<RigidBody*> bodies;
            if (limb ? !GetHeadBody(actor, bodies) : !RagdollSimulating(actor, bodies))
                continue;
            PhysicsLock physicsLock(bodies);
            if (!physicsLock.Wrapper)
                continue;
            auto& owned = m_owned[key];
            if (owned.DismemberTick != (limb ? eventTick : 0))
                owned = {};
            owned.DismemberTick = limb ? eventTick : 0;
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
                if (owned.SentSettled && aNowMs - owned.LastSentMs < kResendMs)
                    continue;
            }
            CorpseRagdollRequest request{};
            request.ServerId = serverId;
            request.Tick = PoseCopyAuthority::GetCurrentTick();
            request.Limb = limb;
            request.DismemberTick = owned.DismemberTick;
            request.Settled = !moving && aNowMs - owned.SettledSinceMs >= kSettleMs;
            const auto lifeState = (actor->actorState.flags1 >> 21) & 0xF;
            request.Dying = lifeState == 1 || lifeState == 2;
            request.Origin[0] = actor->position.x;
            request.Origin[1] = actor->position.y;
            request.Origin[2] = actor->position.z;
            for (auto* rigid : bodies)
            {
                CorpseRagdollBody body{};
                for (int axis = 0; axis < 3; ++axis)
                    body.Position[axis] = rigid->transform[12 + axis] * kHavokToGameUnits - request.Origin[axis];
                MatrixToQuaternion(rigid->transform, body.Rotation);
                request.Bodies.push_back(body);
            }
            if (!m_transport.Send(request))
                continue;
            if (!owned.LastSentMs)
                spdlog::info("Ragdoll {:X} limb {} (server {:X}): streaming {} bodies", actor->formID, limb, serverId, bodies.size());
            owned.LastSentMs = aNowMs;
            owned.SentSettled = request.Settled;
        }
    }
    for (auto it = m_owned.begin(); it != m_owned.end();)
    {
        if (seen.contains(it->first))
        {
            ++it;
            continue;
        }
        CorpseRagdollRequest end{};
        end.ServerId = static_cast<uint32_t>(it->first >> 32);
        end.Limb = static_cast<uint32_t>(it->first);
        end.DismemberTick = it.value().DismemberTick;
        end.Tick = PoseCopyAuthority::GetCurrentTick();
        end.Active = false;
        if (m_transport.Send(end))
            it = m_owned.erase(it);
        else
            ++it;
    }
}

void CorpseRagdollService::ApplyRemote(const uint64_t aNowMs, bool aRelease) noexcept
{
    const double presentationTime = PoseCopyAuthority::GetPresentationTimeMs();
    const auto presentation = static_cast<uint64_t>(presentationTime);
    Vector<void*> controlledDrivers;
    auto frame = std::make_shared<StepFrame>();
    for (auto it = m_remote.begin(); it != m_remote.end();)
    {
        const uint32_t serverId = static_cast<uint32_t>(it->first >> 32);
        const uint32_t limb = static_cast<uint32_t>(it->first);
        auto& ragdoll = it.value();
        auto* actor = Utils::GetByServerId<Actor>(serverId);
        const auto skip = [&](const char* reason)
        {
            if (ragdoll.LastSkipReason != reason)
            {
                ragdoll.LastSkipReason = reason;
                spdlog::warn("Ragdoll server {:X} limb {}: waiting ({})", serverId, limb, reason);
            }
        };
        const bool end = aRelease || (ragdoll.EndTick && presentation >= ragdoll.EndTick) ||
            (actor && !actor->GetExtension()->IsRemote()) || !m_transport.IsConnected();
        if (end)
        {
            if (ragdoll.Binding)
                ragdoll.Binding->Active.store(false, std::memory_order_release);
            // Release retained bodies even when the actor has left the ECS or lost 3D.
            // Rediscovering through the actor can instead find a replacement graph.
            if (ragdoll.Binding)
            {
                const auto& bodies = ragdoll.Binding->Bodies;
                PhysicsLock physicsLock(bodies);
                if (physicsLock.Wrapper)
                    for (size_t i = 0; i < bodies.size() && i < ragdoll.BodyIds.size(); ++i)
                        if (bodies[i]->uid == ragdoll.BodyIds[i] && bodies[i] == ragdoll.BodyPointers[i] &&
                            bodies[i]->world == ragdoll.PhysicsWorld)
                            SetMotion(bodies[i], ragdoll.MotionTypes[i]);
            }
            if (ragdoll.LocalFormId && !limb)
            {
                PoseCopyAuthority::SetRagdollSimulating(ragdoll.LocalFormId, false);
                PoseCopyAuthority::SetRagdollPending(ragdoll.LocalFormId, false);
                std::lock_guard lock(s_followingLock);
                s_followingSinceMs.erase(ragdoll.LocalFormId);
            }
            it = m_remote.erase(it);
            continue;
        }
        ++it;
        if (!ragdoll.RingCount)
            continue;
        if (!actor || !actor->GetNiNode())
        {
            if (!limb && ragdoll.LocalFormId)
                PoseCopyAuthority::SetRagdollSimulating(ragdoll.LocalFormId, false);
            skip(!actor ? "no actor for server id" : "no 3D");
            continue;
        }
        if (!limb)
        {
            if (ragdoll.LocalFormId && ragdoll.LocalFormId != actor->formID)
            {
                PoseCopyAuthority::SetRagdollSimulating(ragdoll.LocalFormId, false);
                PoseCopyAuthority::SetRagdollPending(ragdoll.LocalFormId, false);
                std::lock_guard lock(s_followingLock);
                s_followingSinceMs.erase(ragdoll.LocalFormId);
            }
            ragdoll.LocalFormId = actor->formID;
            {
                std::lock_guard lock(s_followingLock);
                s_followingSinceMs[actor->formID] = aNowMs;
            }
            PoseCopyAuthority::SetRagdollPending(actor->formID, true);
            PoseCopyAuthority::SetRagdollSimulating(actor->formID, false);
        }
        const auto size = static_cast<uint32_t>(ragdoll.Ring.size());
        const auto sample = [&](uint32_t i) -> const Sample& { return ragdoll.Ring[(ragdoll.RingNext + size - ragdoll.RingCount + i) % size]; };
        const auto& newest = sample(ragdoll.RingCount - 1);
        if (presentation < sample(0).Tick || (limb && presentation < ragdoll.DismemberTick))
        {
            skip("waiting for first presentation sample");
            continue;
        }
        bool ownerDyingNow = false;
        for (uint32_t i = 0; i < ragdoll.RingCount; ++i)
            ownerDyingNow |= sample(i).Dying && sample(i).Tick <= presentation;
        if (!limb && (!ragdoll.Knocked || (ownerDyingNow && !ragdoll.OwnerDying)))
        {
            if (!actor->currentProcess)
            {
                skip("no AI process");
                continue;
            }
            ragdoll.OwnerDying = ownerDyingNow;
            if (ragdoll.OwnerDying)
                actor->KillIntoRagdoll();
            else if (!PhysicsOwnsSkeleton(actor))
                actor->currentProcess->KnockExplosion(actor, &actor->position, 0.f);
            ragdoll.Knocked = true;
        }
        Vector<RigidBody*> bodies;
        void* driver{};
        GraphRef graphRef;
        std::optional<BSScopedLock<BSRecursiveLock>> graphLock;
        if (actor->animationGraphHolder.GetBSAnimationGraph(&graphRef.pManager) && graphRef.pManager)
            graphLock.emplace(graphRef.pManager->lock);
        const char* reason = "waiting for native head clone/body to enter world";
        bool ready = limb ? GetHeadBody(actor, bodies) : GetRagdollBodies(actor, bodies, &reason, false, &driver);
        if (!ready && !limb)
        {
            ready = GetRagdollBodies(actor, bodies, &reason, true, &driver);
            if (ready)
                spdlog::info("Ragdoll {:X}: added graph ragdoll to world through 63621 ({} bodies)", actor->formID, bodies.size());
        }
        if (!ready)
        {
            skip(reason);
            continue;
        }
        if (bodies.size() != newest.Bodies.size())
        {
            if (!ragdoll.CountMismatchLogged)
                spdlog::warn("Ragdoll {:X} limb {}: count mismatch, local {}, owner {}; holding owner skeleton",
                    actor->formID, limb, bodies.size(), newest.Bodies.size());
            ragdoll.CountMismatchLogged = true;
            continue;
        }
        PhysicsLock physicsLock(bodies);
        if (!physicsLock.Wrapper)
        {
            skip("physics world unavailable for keyframe binding");
            continue;
        }
        bool reset = ragdoll.BodyIds.size() != bodies.size() || ragdoll.PhysicsWorld != bodies.front()->world;
        for (size_t i = 0; !reset && i < bodies.size(); ++i)
            reset = ragdoll.BodyIds[i] != bodies[i]->uid || ragdoll.BodyPointers[i] != bodies[i];
        if (reset)
        {
            if (ragdoll.Binding)
                ragdoll.Binding->Active.store(false, std::memory_order_release);
            ragdoll.Binding.reset();
            ragdoll.BodyIds.clear();
            ragdoll.BodyPointers.clear();
            ragdoll.MotionTypes.clear();
            ragdoll.PhysicsWorld = bodies.front()->world;
            for (auto* body : bodies)
            {
                ragdoll.BodyIds.push_back(body->uid);
                ragdoll.BodyPointers.push_back(body);
                ragdoll.MotionTypes.push_back(body->motionType == 4 ? 1 : body->motionType);
            }
        }
        if (!ragdoll.Binding)
        {
            ragdoll.Binding = std::make_shared<StepBinding>();
            auto& binding = *ragdoll.Binding;
            binding.World = bodies.front()->world;
            binding.FormId = actor->formID;
            binding.Limb = limb;
            binding.Driver = driver;
            // 57010 / 140AA26B0 and 57011 / 140AA2770: native references keep
            // a removed/reloaded body's memory alive until the step snapshot retires.
            using TReference = void(void*);
            POINTER_SKYRIMSE(TReference, addReference, 57010);
            if (driver)
                addReference.Get()(driver);
            for (auto* body : bodies)
            {
                addReference.Get()(body);
                binding.Bodies.push_back(body);
                binding.Ids.push_back(body->uid);
            }
        }
        StepFrame::Stream stream;
        stream.Binding = ragdoll.Binding;
        for (uint32_t i = 0; i < ragdoll.RingCount; ++i)
            stream.Samples.push_back(sample(i));
        stream.EndTick = ragdoll.EndTick;
        frame->Streams.push_back(std::move(stream));
        ragdoll.LastSkipReason = nullptr;
        ragdoll.CountMismatchLogged = false;
        if (driver)
        {
            controlledDrivers.push_back(driver);
            PoseCopyAuthority::SetRagdollSimulating(actor->formID, true);
        }
    }
    // Never hold m_stepLock while taking a graph/world lock. The physics hook only
    // copies this pointer, then works on retained native bodies, without the ECS.
    {
        std::lock_guard lock(m_stepLock);
        m_stepFrame.swap(frame);
    }
    PoseCopyAuthority::SetControlledRagdollDrivers(controlledDrivers);
}
