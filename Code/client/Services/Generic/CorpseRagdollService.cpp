#include <Services/CorpseRagdollService.h>
#include <Services/CharacterService.h>
#include <Games/Overrides.h>

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
#include <Forms/TESObjectCELL.h>

#include <chrono>
#include <cmath>
#include <optional>
#include <glm/gtc/quaternion.hpp>

namespace
{
using namespace ActorPoseDiagnosticViews;

// Native/source decisions and capture limitations: docs/REFERENCE_RESEARCH.md.
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
std::array<std::atomic<uint32_t>, 64> s_pendingActors{};
std::atomic<uint32_t> s_pendingActorWrite{};
std::mutex s_followingLock;
std::unordered_map<uint32_t, uint64_t> s_followingSinceMs; // form id -> last sample received
std::unordered_map<uint32_t, uint32_t> s_followingServerIds;
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
constexpr uint64_t kStreamMs = 33;
constexpr uint64_t kResendMs = 5000;
constexpr size_t kStreamsPerFrame = 64;
constexpr size_t kDiscoverPerFrame = 8;
constexpr uint64_t kResidualLogMs = 2000;

bool Dynamic(uint8_t aMotion) noexcept
{
    return aMotion == 1 || aMotion == 2 || aMotion == 3 || aMotion == 6;
}

// A copy this close to the owner's settled pose may sleep, and stays asleep until it leaves the band.
constexpr float kSettleNearPosition = 2.f; // game units per body
constexpr float kSettleNearAngle = 10.f;   // degrees per body

bool Resting(const RigidBody* apBody) noexcept
{
    float linear{}, angular{};
    for (int axis = 0; axis < 3; ++axis)
    {
        linear += apBody->linearVelocity[axis] * apBody->linearVelocity[axis];
        angular += apBody->angularVelocity[axis] * apBody->angularVelocity[axis];
    }
    // One rest definition on both PCs. NaNs fail closed instead of passing rest.
    return linear < kSettledLinear * kSettledLinear && angular < kSettledAngular * kSettledAngular;
}

// E61307/E60918: a world pointer can remain set while removal is pending.
// Only use this gate under the native wrapper lock at a solver boundary.
bool WorldIdle(void* apWorld) noexcept
{
    const auto* bytes = static_cast<const uint8_t*>(apWorld);
    return apWorld && *reinterpret_cast<const int*>(bytes + 0xF0) == 0 &&
        *reinterpret_cast<const int*>(bytes + 0xF4) == 0 &&
        *reinterpret_cast<const int*>(bytes + 0xF8) == 0 && bytes[0x100] == 0;
}

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

// Read-only evidence at the native consumer, not a repair or a lifetime claim.
// 64158 / 140BF4CF0 removes constraints then calls 61307 / 140B55500.
// 64677 / 140C097C0 sorts 16-bit node indices and writes through node+10.
// Fixed-size snapshots never retain native objects or acquire a world/registry lock.
// Formatting and sink I/O run only from OnMainFrame; hooks publish POD records.
constexpr size_t kAuditBodies = 128;
std::atomic<uint64_t> s_auditSequence{};
thread_local void* t_removingGraph{};

template <class T> bool ReadAt(uintptr_t aBase, size_t aOffset, T& aValue) noexcept
{
    return aBase && aBase <= UINTPTR_MAX - aOffset &&
        ReadNative(reinterpret_cast<const void*>(aBase + aOffset), aValue);
}

struct BodyAudit
{
    uintptr_t Body{}, World{}, Shape{}, Broadphase{}, Back{};
    uint32_t Handle{}, Uid{};
    int NodeCount{}, Critical{};
    uint8_t Motion{}, BlockPending{};
    bool Readable{}, Valid{};
};

bool ReadNode(uintptr_t aBroadphase, uint32_t aHandle, uintptr_t& aBack, int& aCount) noexcept
{
    uintptr_t nodes{};
    return ReadAt(aBroadphase, 0xB0, nodes) && ReadAt(aBroadphase, 0xB8, aCount) &&
        aCount > 0 && aHandle < static_cast<uint32_t>(aCount) && aHandle <= UINT16_MAX &&
        ReadAt(nodes, static_cast<size_t>(aHandle) * 0x18 + 0x10, aBack);
}

BodyAudit SnapshotBody(uintptr_t aBody) noexcept
{
    BodyAudit result{};
    result.Body = aBody;
    // One copy for the native body fields, without dereferencing a borrowed pointer.
    std::array<uint8_t, 0x164> bytes{};
    if (!ReadNative(reinterpret_cast<const void*>(aBody), bytes))
        return result;
    std::memcpy(&result.World, bytes.data() + 0x10, sizeof(result.World));
    std::memcpy(&result.Shape, bytes.data() + 0x20, sizeof(result.Shape));
    std::memcpy(&result.Handle, bytes.data() + 0x44, sizeof(result.Handle));
    std::memcpy(&result.Uid, bytes.data() + 0x13C, sizeof(result.Uid));
    result.Motion = bytes[0x160];
    result.Readable = true;
    if (!result.World || !result.Shape)
        return result; // Native 61307 skips shapeless bodies; worldless is not enlisted.
    result.Valid = ReadAt(result.World, 0xF8, result.Critical) &&
        ReadAt(result.World, 0x100, result.BlockPending) &&
        ReadAt(result.World, 0x88, result.Broadphase) &&
        ReadNode(result.Broadphase, result.Handle, result.Back, result.NodeCount) &&
        result.Handle != 0 && result.Back == aBody + 0x44;
    return result;
}

struct RemovalAudit
{
    uint64_t Sequence{};
    uintptr_t Instance{}, Graph{}, Holder{};
    uint32_t FormId{};
    int Count{};
    DWORD Thread{};
    std::array<BodyAudit, kAuditBodies> Bodies{};
};
thread_local const RemovalAudit* t_removalAudit{};

// Lexically paired scopes also preserve an outer invocation during reentrant
// removal. No callback pairing, heap stack, or worker-owned native reference.
template <class T> struct AuditScope
{
    T& Slot;
    T Previous;
    AuditScope(T& aSlot, T aValue) : Slot(aSlot), Previous(aSlot) { Slot = aValue; }
    ~AuditScope() { Slot = Previous; }
};

void LogBody(uint64_t aSequence, size_t aIndex, const char* apStage, const BodyAudit& aBody)
{
    spdlog::warn("DoorCrashAudit body: seq={} stage={} index={} body={:X} world={:X} shape={:X} uid={} motion={} critical={} blocked={} broadphase={:X} handle={} nodes={} back={:X} readable={} valid={}",
        aSequence, apStage, aIndex, aBody.Body, aBody.World, aBody.Shape, aBody.Uid,
        aBody.Motion, aBody.Critical, aBody.BlockPending, aBody.Broadphase, aBody.Handle, aBody.NodeCount,
        aBody.Back, aBody.Readable, aBody.Valid);
}

// A bounded, nonwaiting mailbox. Static payloads are included by the unchanged
// MiniDumpWithDataSegs setting, even if native removal faults before the next frame.
// State: 0 reusable, 1 writing, 2 published, 3 draining. No producer retries.
struct AuditRecord
{
    RemovalAudit Entry{};
    std::array<BodyAudit, kAuditBodies> Consumer{};
    std::array<uintptr_t, kAuditBodies> Handles{}, Backs{};
    std::array<uint32_t, kAuditBodies> Ids{};
    std::array<bool, kAuditBodies> Valid{};
    std::array<void*, 16> Stack{};
    uintptr_t Broadphase{};
    int Count{}, NodeCount{}, Result{};
    USHORT Depth{};
    uint8_t Kind{}; // 1 entry, 2 consumer, 3 return
    bool Suspect{}, Complete{}, Duplicate{};
};
struct AuditSlot
{
    std::atomic<uint32_t> State{};
    AuditRecord Record{};
};
constexpr size_t kAuditSlots = 16;
std::array<AuditSlot, kAuditSlots> s_doorCrashAudit{};
std::atomic<uint64_t> s_auditWrite{}, s_auditDropped{};
static_assert(std::atomic<uint32_t>::is_always_lock_free && std::atomic<uint64_t>::is_always_lock_free);

AuditSlot* ClaimAudit() noexcept
{
    auto& slot = s_doorCrashAudit[s_auditWrite.fetch_add(1, std::memory_order_relaxed) % kAuditSlots];
    auto expected = slot.State.load(std::memory_order_relaxed);
    if ((expected != 0 && expected != 2) ||
        !slot.State.compare_exchange_strong(expected, 1, std::memory_order_acquire))
    {
        s_auditDropped.fetch_add(1, std::memory_order_relaxed);
        return nullptr;
    }
    if (expected == 2) // Keep the newest evidence during a long cell unload.
        s_auditDropped.fetch_add(1, std::memory_order_relaxed);
    slot.Record = {};
    return &slot;
}

void DrainDoorCrashAudit()
{
    for (auto& slot : s_doorCrashAudit)
    {
        uint32_t expected = 2;
        if (!slot.State.compare_exchange_strong(expected, 3, std::memory_order_acquire))
            continue;
        const auto& record = slot.Record;
        const auto& audit = record.Entry;
        const int captured = std::clamp(audit.Count, 0, static_cast<int>(kAuditBodies));
        if (record.Kind == 1)
        {
            spdlog::info("DoorCrashAudit remove-begin: seq={} thread={} graph={:X} holder={:X} form={:X} instance={:X} count={} captured={} suspect={}",
                audit.Sequence, audit.Thread, audit.Graph, audit.Holder, audit.FormId, audit.Instance, audit.Count, captured, record.Suspect);
            if (record.Suspect)
                for (int i = 0; i < captured; ++i)
                    LogBody(audit.Sequence, i, "entry", audit.Bodies[i]);
        }
        else if (record.Kind == 2)
        {
            spdlog::error("DoorCrashAudit broadphase-suspect: seq={} thread={} broadphase={:X} count={} captured={} complete={} nodes={} duplicate={} scoped=true (native call unchanged)",
                audit.Sequence, audit.Thread, record.Broadphase, record.Count,
                std::clamp(record.Count, 0, static_cast<int>(kAuditBodies)), record.Complete, record.NodeCount, record.Duplicate);
            for (int i = 0; i < captured; ++i)
            {
                LogBody(audit.Sequence, i, "entry-before-constraints", audit.Bodies[i]);
                LogBody(audit.Sequence, i, "broadphase-consumer", record.Consumer[i]);
            }
            for (int i = 0; i < std::clamp(record.Count, 0, static_cast<int>(kAuditBodies)); ++i)
                spdlog::error("DoorCrashAudit handle: seq={} index={} address={:X} id={} back={:X} valid={}",
                    audit.Sequence, i, record.Handles[i], record.Ids[i], record.Backs[i], record.Valid[i]);
            for (USHORT i = 0; i < record.Depth; ++i)
                spdlog::error("DoorCrashAudit stack: seq={} frame={} pc={}", audit.Sequence, i, fmt::ptr(record.Stack[i]));
        }
        else if (record.Kind == 3)
            spdlog::info("DoorCrashAudit remove-end: seq={} result={}", audit.Sequence, record.Result);
        slot.State.store(0, std::memory_order_release);
    }
    const auto dropped = s_auditDropped.exchange(0, std::memory_order_relaxed);
    if (dropped)
        spdlog::warn("DoorCrashAudit dropped: records={} (bounded mailbox; evidence incomplete)", dropped);
}

// 63622 / 140BD3A00 returns bool in AL; virtual caller 32952 tests AL.
using TRemoveGraph = bool(void*);
TRemoveGraph* RealRemoveGraph{};
bool HookRemoveGraph(void* apGraph)
{
    AuditScope<void*> scope(t_removingGraph, apGraph);
    return RealRemoveGraph(apGraph);
}

// 64158 / 140BF4CF0 returns hkResult in EAX, with no hidden result pointer.
using TRemoveRagdoll = int32_t(void*);
TRemoveRagdoll* RealRemoveRagdoll{};
int32_t HookRemoveRagdoll(void* apInstance)
{
    // Invalidate before native removal can queue while body.world remains nonnull.
    // The read-only door audit below and native call remain unchanged.
    CorpseRagdollService::InvalidateInstance(apInstance);
    auto* slot = ClaimAudit();
    if (!slot)
    {
        AuditScope<const RemovalAudit*> scope(t_removalAudit, nullptr);
        return RealRemoveRagdoll(apInstance);
    }
    RemovalAudit audit{};
    audit.Sequence = s_auditSequence.fetch_add(1, std::memory_order_relaxed) + 1;
    audit.Thread = GetCurrentThreadId();
    audit.Instance = reinterpret_cast<uintptr_t>(apInstance);
    audit.Graph = reinterpret_cast<uintptr_t>(t_removingGraph);
    uintptr_t driver{}, instance{};
    if (!ReadAt(audit.Graph, 0xF0, driver) || !ReadAt(driver, 0x88, instance) || instance != audit.Instance)
        audit.Graph = 0;
    ReadAt(audit.Graph, 0x210, audit.Holder);
    ReadAt(audit.Holder, offsetof(TESForm, formID), audit.FormId);
    HavokArray<uintptr_t> bodies{};
    const bool readable = ReadAt(audit.Instance, 0x10, bodies);
    audit.Count = readable ? bodies.size : -1;
    const int captured = std::clamp(audit.Count, 0, static_cast<int>(kAuditBodies));
    bool invalid = !readable || audit.Count <= 0 || audit.Count > static_cast<int>(kAuditBodies);
    for (int i = 0; i < captured; ++i)
    {
        uintptr_t body{};
        ReadAt(reinterpret_cast<uintptr_t>(bodies.data), i * sizeof(uintptr_t), body);
        audit.Bodies[i] = SnapshotBody(body);
        const auto& value = audit.Bodies[i];
        invalid |= !value.Readable || (value.World && value.Shape && !value.Valid);
    }
    slot->Record.Entry = audit;
    slot->Record.Kind = 1;
    slot->Record.Suspect = invalid;
    slot->State.store(2, std::memory_order_release); // publish before the potentially fatal call
    AuditScope<const RemovalAudit*> scope(t_removalAudit, &audit);
    const auto result = RealRemoveRagdoll(apInstance);
    if (auto* end = ClaimAudit())
    {
        end->Record.Kind = 3;
        end->Record.Entry.Sequence = audit.Sequence;
        end->Record.Result = result;
        end->State.store(2, std::memory_order_release);
    }
    return result;
}

// 64677 / 140C097C0 virtual slot 9: (broadphase, hkArray<handle*>*, pairs*).
// It loads handle pointers with stride 8, then builds its own 16-bit sort array.
using TRemoveObjects = void(void*, const HavokArray<uintptr_t>*, void*);
TRemoveObjects* RealRemoveObjects{};
void HookRemoveObjects(void* apBroadphase, const HavokArray<uintptr_t>* apHandles, void* apPairs)
{
    const auto* audit = t_removalAudit;
    if (!audit)
    {
        RealRemoveObjects(apBroadphase, apHandles, apPairs);
        return; // Unrelated clutter removals do not perform snapshots or logging.
    }
    auto* slot = ClaimAudit();
    if (!slot)
    {
        RealRemoveObjects(apBroadphase, apHandles, apPairs);
        return;
    }
    auto& record = slot->Record;
    record.Kind = 2;
    record.Entry = *audit;
    record.Broadphase = reinterpret_cast<uintptr_t>(apBroadphase);
    HavokArray<uintptr_t> handles{};
    const bool readable = ReadNative(apHandles, handles);
    record.Count = readable ? handles.size : -1;
    const int captured = std::clamp(record.Count, 0, static_cast<int>(kAuditBodies));
    record.Complete = readable && record.Count >= 0 && record.Count == captured;
    record.Suspect = !readable || record.Count <= 0 || !record.Complete;
    std::array<uint32_t, kAuditBodies> sorted{};
    for (int i = 0; i < captured; ++i)
    {
        record.Valid[i] = ReadAt(reinterpret_cast<uintptr_t>(handles.data), i * sizeof(uintptr_t), record.Handles[i]) &&
            ReadAt(record.Handles[i], 0, record.Ids[i]) &&
            ReadNode(record.Broadphase, record.Ids[i], record.Backs[i], record.NodeCount) &&
            record.Ids[i] != 0 && record.Backs[i] == record.Handles[i];
        record.Suspect |= !record.Valid[i];
        sorted[i] = record.Ids[i];
    }
    std::sort(sorted.begin(), sorted.begin() + captured);
    record.Duplicate = std::adjacent_find(sorted.begin(), sorted.begin() + captured) != sorted.begin() + captured;
    record.Suspect |= record.Duplicate;
    if (record.Suspect)
    {
        for (int i = 0; i < std::clamp(audit->Count, 0, static_cast<int>(kAuditBodies)); ++i)
            record.Consumer[i] = SnapshotBody(audit->Bodies[i].Body);
        record.Depth = CaptureStackBackTrace(0, static_cast<DWORD>(record.Stack.size()), record.Stack.data(), nullptr);
        slot->State.store(2, std::memory_order_release);
    }
    else
        slot->State.store(0, std::memory_order_release);
    RealRemoveObjects(apBroadphase, apHandles, apPairs);
}

// Dying, dead, knocked down or ragdolling (ActorState1 lifeState bits 21-24, knockState 25-27).
// Owner side: when each captured actor's life state first left alive (stream start latency log).
std::unordered_map<uint32_t, uint64_t> s_diedAtMs;

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

// Discovery runs while the graph manager (or main-thread head clone) owns the
// objects. Retain before inspecting body state, and through the caller's use.
struct RetainedBodies : Vector<RigidBody*>
{
    Vector<void*> References;
    bool Retain(void* apObject)
    {
        if (!apObject)
            return false;
        using TReference = void(void*);
        POINTER_SKYRIMSE(TReference, addReference, 57010);
        addReference.Get()(apObject);
        References.push_back(apObject);
        return true;
    }
    ~RetainedBodies()
    {
        using TReference = void(void*);
        POINTER_SKYRIMSE(TReference, removeReference, 57011);
        for (auto it = References.rbegin(); it != References.rend(); ++it)
            removeReference.Get()(*it);
    }
};

// The active behavior graph's ragdoll rigid bodies (hkpRigidBody*), in ragdoll order.
bool GetRagdollBodies(Actor* apActor, RetainedBodies& aBodies, const char** apReason = nullptr,
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
    if (!aBodies.Retain(graph.characterInstance.ragdollDriver) || !ReadNative(graph.characterInstance.ragdollDriver, driver))
        return fail("no ragdoll driver");
    if (apDriver)
        *apDriver = graph.characterInstance.ragdollDriver;
    if (!aBodies.Retain(driver.ragdoll) || !ReadNative(driver.ragdoll, ragdoll))
        return fail("no ragdoll instance");
    if (ragdoll.rigidBodies.size <= 0 || ragdoll.rigidBodies.size > static_cast<int32_t>(CorpseRagdollRequest::kMaxBodies))
        return fail("bad ragdoll body count");
    for (int32_t i = 0; i < ragdoll.rigidBodies.size; ++i)
    {
        void* pBody{};
        RigidBody probe{};
        if (!ReadNative(ragdoll.rigidBodies.data + i, pBody) || !aBodies.Retain(pBody) || !ReadNative(pBody, probe))
            return fail("body unreadable");
        if (!probe.world)
            return fail("bodies not in the physics world");
        aBodies.push_back(static_cast<RigidBody*>(pBody));
    }
    return true;
}

bool GetHeadBody(Actor* apActor, RetainedBodies& aBodies) noexcept
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
    if (!wrapper || !ReadNative(static_cast<uint8_t*>(wrapper) + 0x10, rigid) || !aBodies.Retain(rigid) || !ReadNative(rigid, state) || !state.world)
        return false;
    aBodies.push_back(rigid);
    return true;
}

struct PhysicsLock
{
    void* Wrapper{};
    void* World{};
    explicit PhysicsLock(const Vector<RigidBody*>& acBodies)
    {
        if (acBodies.empty() || !acBodies.front()->world)
            return;
        auto* world = acBodies.front()->world;
        World = world;
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
        for (auto* body : acBodies)
            if (body->world != World)
            {
                POINTER_SKYRIMSE(TLock, unlock, 77929);
                unlock.Get()(Wrapper);
                Wrapper = nullptr;
                break;
            }
    }
    explicit PhysicsLock(void* apWorld)
    {
        World = apWorld;
        // The native step caller owns this world through its return. Never lock
        // through a stale retained binding's raw world pointer from a worker.
        if (!apWorld || !ReadNative(static_cast<uint8_t*>(apWorld) + 0x430, Wrapper) || !Wrapper)
            return;
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
    using TSetMotion = void(void*, uint32_t, uint32_t, uint32_t);
    POINTER_SKYRIMSE(TSetMotion, setMotion, 60908);
    if (apBody->motionType != aMotion)
        setMotion.Get()(apBody, aMotion, 1, 0);
}

// The ragdoll is simulating: its bodies are in the world and not keyframed to the animation (a
// death animation keyframes them, or keeps them out of the world, before the ragdoll takes over).
bool RagdollSimulating(Actor* apActor, RetainedBodies& aBodies, const char** apReason = nullptr) noexcept
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
        if (!Resting(pBody))
            return true;
    }
    return false;
}

struct BodyError
{
    float Position{}, Angle{};
    float ActualPosition[3]{}, ActualRotation[4]{};
    QsTransform Target{};
    uint8_t Motion{}, OwnerMotion{};
};
BodyError MeasureBody(RigidBody* apBody, const QsTransform& aTarget, uint8_t aOwnerMotion)
{
    BodyError error{};
    error.Target = aTarget;
    error.Motion = apBody->motionType;
    error.OwnerMotion = aOwnerMotion;
    for (int axis = 0; axis < 3; ++axis)
    {
        error.ActualPosition[axis] = apBody->transform[12 + axis] * kHavokToGameUnits;
        const float delta = aTarget.translation[axis] * kHavokToGameUnits - error.ActualPosition[axis];
        error.Position += delta * delta;
    }
    error.Position = std::sqrt(error.Position);
    MatrixToQuaternion(apBody->transform, error.ActualRotation);
    float dot{};
    for (int axis = 0; axis < 4; ++axis)
        dot += aTarget.rotation[axis] * error.ActualRotation[axis];
    error.Angle = 2.f * std::acos(std::clamp(std::abs(dot), 0.f, 1.f)) * 57.29578f;
    return error;
}
struct RagdollObservation
{
    uint32_t FormId{}, ServerId{}, Limb{}, Count{}, Thread{};
    uint64_t Tick{}, WallMs{}, SampleAgeMs{};
    double PresentationMs{};
    bool OwnerSettled{}, FollowerResting{}, Exact{}, Log{};
    uint32_t Snaps{};
    std::array<BodyError, CorpseRagdollRequest::kMaxBodies> Bodies{};
};
struct ObservationSlot
{
    std::atomic<uint32_t> State{};
    RagdollObservation Value{};
};
std::array<ObservationSlot, 64> s_observations;
std::atomic<uint64_t> s_observationWrite{}, s_observationDropped{};
std::deque<std::string> s_captureRecords; // main-thread producer/consumer only
uint64_t s_captureDropped{};
uint64_t s_captureSinceMs{}, s_observationDroppedBase{};

void PublishObservation(const RagdollObservation& aValue)
{
    auto& slot = s_observations[s_observationWrite.fetch_add(1) % s_observations.size()];
    uint32_t expected = 0;
    if (!slot.State.compare_exchange_strong(expected, 1, std::memory_order_acquire))
    {
        ++s_observationDropped;
        return;
    }
    slot.Value = aValue;
    slot.State.store(2, std::memory_order_release);
}
void DrainObservations()
{
    static size_t cursor{};
    size_t drained{};
    for (size_t visited = 0; visited < s_observations.size() && drained < 8; ++visited)
    {
        auto& slot = s_observations[cursor++ % s_observations.size()];
        uint32_t expected = 2;
        if (!slot.State.compare_exchange_strong(expected, 3, std::memory_order_acquire))
            continue;
        const auto value = slot.Value;
        slot.State.store(0, std::memory_order_release);
        ++drained;
        float position{}, angle{};
        std::string bodies = "[";
        for (uint32_t i = 0; i < value.Count; ++i)
        {
            const auto& b = value.Bodies[i];
            position = (std::max)(position, b.Position);
            angle = (std::max)(angle, b.Angle);
            bodies += fmt::format("{}{{\"body\":{},\"positionError\":{},\"rotationErrorDeg\":{},\"p\":[{},{},{}],\"q\":[{},{},{},{}],\"targetP\":[{},{},{}],\"targetQ\":[{},{},{},{}],\"motion\":{},\"ownerMotion\":{}}}",
                i ? "," : "", i, b.Position, b.Angle,
                b.ActualPosition[0], b.ActualPosition[1], b.ActualPosition[2],
                b.ActualRotation[0], b.ActualRotation[1], b.ActualRotation[2], b.ActualRotation[3],
                b.Target.translation[0] * kHavokToGameUnits, b.Target.translation[1] * kHavokToGameUnits, b.Target.translation[2] * kHavokToGameUnits,
                b.Target.rotation[0], b.Target.rotation[1], b.Target.rotation[2], b.Target.rotation[3], b.Motion, b.OwnerMotion);
        }
        bodies += ']';
        if (value.Log)
            spdlog::info("Ragdoll {:X} limb {}: dynamic residual {:.3f} u/{:.3f} deg, {} bodies tick {} owner-settled={} follower-resting={} age={} ms snaps={} (post-solver BEFORE correction)",
                value.FormId, value.Limb, position, angle, value.Count, value.Tick, value.OwnerSettled, value.FollowerResting, value.SampleAgeMs,
                value.Snaps);
        if (value.Exact)
            spdlog::info("Ragdoll {:X} limb {}: exact owner settle at tick {} after both rested; pre-correction {:.3f} u/{:.3f} deg",
                value.FormId, value.Limb, value.Tick, position, angle);
        if (!s_captureSinceMs || value.WallMs < s_captureSinceMs)
            continue;
        if (s_captureRecords.size() == 64)
        {
            s_captureRecords.pop_front();
            ++s_captureDropped;
        }
        s_captureRecords.push_back(fmt::format("{{\"kind\":\"ragdoll_error\",\"id\":{},\"serverId\":{},\"limb\":{},\"wallMs\":{},\"tick\":{},\"presentationMs\":{},\"sampleAgeMs\":{},\"thread\":{},\"ownerSettled\":{},\"followerResting\":{},\"exactAfterMeasurement\":{},\"phase\":\"post-solver-before-correction\",\"dropped\":{},\"bodyCount\":{},\"truncated\":false,\"bodies\":{}}}",
            value.FormId, value.ServerId, value.Limb, value.WallMs, value.Tick, value.PresentationMs, value.SampleAgeMs,
            value.Thread, value.OwnerSettled, value.FollowerResting, value.Exact,
            s_observationDropped.load() - s_observationDroppedBase, value.Count, bodies));
    }
}
// Worn-marker probe (naked flash at death, runs 20260928-095932 / -101047): a dying copy's worn list empties 33..66 ms
// after its death with no unequip call. The worn state is ExtraWorn / ExtraWornLeft extra data; every removal deletes the
// object through its vtable slot 0 (scalar deleting destructor; ExtraWorn vtable ID 186681, ExtraWornLeft 186683). For
// 400 ms after a copy's death transition, log the full stack of every such deletion to name the native caller.
using TExtraDtor = void*(void*, uint32_t);
TExtraDtor* s_realWornDtor{};
TExtraDtor* s_realWornLeftDtor{};
std::atomic<uint64_t> s_wornProbeUntilMs{};
std::atomic<uint32_t> s_wornProbeLogs{};

void LogWornDeletion(const char* apKind) noexcept
{
    if (GetTickCount64() > s_wornProbeUntilMs.load(std::memory_order_relaxed) ||
        s_wornProbeLogs.fetch_add(1, std::memory_order_relaxed) >= 24)
        return;
    void* frames[28]{};
    const auto count = RtlCaptureStackBackTrace(1, 28, frames, nullptr);
    std::string stack;
    for (USHORT i = 0; i < count; ++i)
    {
        HMODULE module{};
        char name[MAX_PATH]{};
        if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                static_cast<LPCWSTR>(frames[i]), &module) && module)
        {
            GetModuleFileNameA(module, name, MAX_PATH);
            const char* file = strrchr(name, '\\');
            stack += fmt::format(" {}+{:X}", file ? file + 1 : name,
                reinterpret_cast<uintptr_t>(frames[i]) - reinterpret_cast<uintptr_t>(module));
        }
        else
            stack += fmt::format(" ?{:X}", reinterpret_cast<uintptr_t>(frames[i]));
    }
    // The unwinder stops inside game code (the launcher-mapped image has no registered unwind data), so also scan the
    // raw stack: keep values that point into the game's code just after a call instruction (return addresses).
    std::string scan;
    HMODULE game{};
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(s_realWornDtor), &game) && game)
    {
        const auto base = reinterpret_cast<uintptr_t>(game);
        ULONG_PTR low{}, high{};
        GetCurrentThreadStackLimits(&low, &high);
        auto* cursor = reinterpret_cast<const uintptr_t*>(&frames[0]);
        const auto* end = reinterpret_cast<const uintptr_t*>((std::min)(static_cast<uintptr_t>(high),
            reinterpret_cast<uintptr_t>(cursor) + 16384));
        int found = 0;
        for (; cursor + 1 <= end && found < 24; ++cursor)
        {
            const uintptr_t value = *cursor;
            if (value < base + 0x1000 || value >= base + 0x17C0000)
                continue;
            const auto* code = reinterpret_cast<const uint8_t*>(value);
            const bool call = code[-5] == 0xE8 || (code[-6] == 0xFF && (code[-5] & 0x38) == 0x10) ||
                (code[-2] == 0xFF && (code[-1] & 0xF8) == 0xD0) || (code[-3] == 0xFF && (code[-2] & 0x38) == 0x10);
            if (!call)
                continue;
            scan += fmt::format(" {:X}", value - base);
            ++found;
        }
    }
    spdlog::info("Worn marker deleted ({}) on thread {}: stack{} | scan{}", apKind, GetCurrentThreadId(), stack, scan);
}

void* HookWornDtor(void* apThis, uint32_t aFlags)
{
    LogWornDeletion("ExtraWorn");
    return s_realWornDtor(apThis, aFlags);
}

void* HookWornLeftDtor(void* apThis, uint32_t aFlags)
{
    LogWornDeletion("ExtraWornLeft");
    return s_realWornLeftDtor(apThis, aFlags);
}

void PatchVtableSlot0(uint32_t aVtableId, void* apHook, TExtraDtor*& arReal) noexcept
{
    POINTER_SKYRIMSE(void*, vtable, aVtableId);
    auto** table = reinterpret_cast<void**>(vtable.Get());
    DWORD previous{};
    if (table && VirtualProtect(&table[0], sizeof(void*), PAGE_EXECUTE_READWRITE, &previous))
    {
        arReal = reinterpret_cast<TExtraDtor*>(table[0]);
        table[0] = apHook;
        DWORD ignored{};
        VirtualProtect(&table[0], sizeof(void*), previous, &ignored);
    }
}

TiltedPhoques::Initializer s_wornProbeInit([]() {
    PatchVtableSlot0(186681, reinterpret_cast<void*>(&HookWornDtor), s_realWornDtor);
    PatchVtableSlot0(186683, reinterpret_cast<void*>(&HookWornLeftDtor), s_realWornLeftDtor);
});
} // namespace


struct CorpseRagdollService::StepBinding
{
    Vector<RigidBody*> Bodies;
    Vector<uint32_t> Ids;
    void* World{};
    void* Driver{};
    void* Instance{};
    uint32_t ServerId{};
    uint32_t FormId{};
    uint32_t Limb{};
    std::atomic<bool> Active{true};
    // Only touched by this world's serial physics steps.
    bool Logged{};
    bool Asleep{};
    // Hard placements onto the owner pose (first presented sample, then only on large drift).
    bool Placed{};
    uint32_t Snaps{};
    uint64_t RestSinceMs{};
    uint64_t ExactTick{};
    uint64_t NextCaptureMs{};
    uint64_t NextLogMs{};
    void Invalidate() noexcept
    {
        if (Active.exchange(false, std::memory_order_acq_rel))
            PoseCopyAuthority::SetControlledRagdollDriver(Driver, false);
    }
    StepBinding* RetiredNext{};
    inline static std::atomic<StepBinding*> Retired{};
    static void Retire(StepBinding* apBinding) noexcept
    {
        auto* head = Retired.load(std::memory_order_relaxed);
        do { apBinding->RetiredNext = head; }
        while (!Retired.compare_exchange_weak(head, apBinding, std::memory_order_release, std::memory_order_relaxed));
    }
    static void Drain() noexcept
    {
        static StepBinding* pending{}; // main-thread retirement budget
        if (!pending) pending = Retired.exchange(nullptr, std::memory_order_acquire);
        for (size_t retired = 0; pending && retired < kStreamsPerFrame; ++retired)
        {
            auto* binding = pending;
            pending = binding->RetiredNext;
            delete binding;
        }
    }
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

        bool Evaluate(double aTime, size_t aBody, QsTransform& aPose, bool& aSettled, uint64_t& aTick,
            CorpseRagdollBody* apState = nullptr) const noexcept
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
            if (apState)
            {
                *apState = x;
                for (int axis = 0; axis < 3; ++axis)
                {
                    apState->LinearVelocity[axis] = x.LinearVelocity[axis] + (y.LinearVelocity[axis] - x.LinearVelocity[axis]) * t;
                    apState->AngularVelocity[axis] = x.AngularVelocity[axis] + (y.AngularVelocity[axis] - x.AngularVelocity[axis]) * t;
                }
                // Never extrapolate velocity indefinitely across a lost stream.
                if (aSettled || aTime > Samples.back().Tick + 100.0)
                {
                    std::fill_n(apState->LinearVelocity, 3, 0.f);
                    std::fill_n(apState->AngularVelocity, 3, 0.f);
                }
            }
            if (!aSettled && index + 1 == Samples.size() && aTime > a.Tick)
            {
                const auto seconds = static_cast<float>((std::min)(aTime - a.Tick, 100.0) * 0.001);
                for (int axis = 0; axis < 3; ++axis)
                    aPose.translation[axis] += x.LinearVelocity[axis] * seconds;
                const glm::vec3 angular{x.AngularVelocity[0], x.AngularVelocity[1], x.AngularVelocity[2]};
                const float speed = glm::length(angular);
                if (speed > 0.0001f)
                {
                    const auto q = glm::normalize(glm::angleAxis(speed * seconds, angular / speed) *
                        glm::quat{aPose.rotation[3], aPose.rotation[0], aPose.rotation[1], aPose.rotation[2]});
                    aPose.rotation[0] = q.x; aPose.rotation[1] = q.y; aPose.rotation[2] = q.z; aPose.rotation[3] = q.w;
                }
            }
            return true;
        }
    };
    // Stable registry. Main-thread refresh replaces only changed entries; native
    // steps copy a bounded slice under m_stepLock, then drop it before world work.
    std::unordered_map<uint64_t, std::shared_ptr<const Stream>> Streams;
    std::unordered_map<void*, std::deque<std::pair<uint64_t, std::weak_ptr<StepBinding>>>> WorldStreams;
    std::unordered_map<void*, std::weak_ptr<StepBinding>> Instances;
};

void CorpseRagdollService::InvalidateInstance(void* apInstance) noexcept
{
    auto* service = s_ragdollService.load(std::memory_order_acquire);
    if (!service) return;
    std::lock_guard lock(service->m_stepLock);
    if (!service->m_stepFrame) return;
    const auto it = service->m_stepFrame->Instances.find(apInstance);
    if (it != service->m_stepFrame->Instances.end())
    {
        if (auto binding = it->second.lock())
        {
            binding->Invalidate();
        }
        service->m_stepFrame->Instances.erase(it);
    }
}

std::string CorpseRagdollService::DrainRagdollCapture() noexcept
{
    if (s_captureRecords.empty()) return {};
    auto record = std::move(s_captureRecords.front());
    s_captureRecords.pop_front();
    // Report skipped history explicitly; an incomplete capture is never a pass.
    if (s_captureDropped)
    {
        record = fmt::format("{{\"kind\":\"ragdoll_capture_gap\",\"dropped\":{}}}\n", s_captureDropped) + record;
        s_captureDropped = 0;
    }
    return record;
}

void CorpseRagdollService::BeginRagdollCapture() noexcept
{
    s_captureRecords.clear();
    s_captureDropped = 0;
    s_captureSinceMs = NowMs();
    s_observationDroppedBase = s_observationDropped.load();
}

void CorpseRagdollService::OnHavokStep(void* apWorld, float aDeltaTime, bool aAfterStep) noexcept
{
    // Native 61410 and 61417 are serial boundaries for THIS world. The latter
    // has joined its workers before the after-call. No ECS or graph access here.
    struct Step
    {
        std::array<std::shared_ptr<const StepFrame::Stream>, kStreamsPerFrame> Streams;
        size_t Count{};
        void* World{};
        double Time{};
    };
    thread_local std::array<Step, 8> steps;
    thread_local size_t depth{};
    thread_local size_t overflow{};
    auto* service = s_ragdollService.load(std::memory_order_acquire);
    if (!aAfterStep)
    {
        if (depth == steps.size() || overflow) { ++overflow; return; }
        auto& step = steps[depth++];
        step.World = apWorld;
        step.Time = PoseCopyAuthority::GetPresentationTimeMs() + aDeltaTime * 1000.0;
        if (service && service->m_applyOnMainFrame.load(std::memory_order_relaxed))
        {
            std::lock_guard lock(service->m_stepLock);
            if (auto frame = service->m_stepFrame)
            {
                auto found = frame->WorldStreams.find(apWorld);
                if (found != frame->WorldStreams.end())
                {
                    auto& queue = found->second;
                    const auto count = (std::min)(kStreamsPerFrame, queue.size());
                    for (size_t i = 0; i < count; ++i)
                    {
                        const auto ticket = queue.front(); queue.pop_front();
                        const auto entry = frame->Streams.find(ticket.first);
                        const auto generation = ticket.second.lock();
                        if (!generation || entry == frame->Streams.end() || entry->second->Binding != generation ||
                            generation->World != apWorld) continue;
                        queue.push_back(ticket);
                        step.Streams[step.Count++] = entry->second;
                    }
                }
            }
        }
    }
    else if (overflow) { --overflow; return; }
    if (!depth || steps[depth - 1].World != apWorld) return;
    const auto step = steps[depth - 1];
    if (aAfterStep) steps[--depth] = {};
    if (!step.Count || !service || !service->m_applyOnMainFrame.load(std::memory_order_relaxed) ||
        !std::isfinite(aDeltaTime) || aDeltaTime <= 0.f || aDeltaTime > 0.25f) return;
    PhysicsLock lock(apWorld);
    if (!lock.Wrapper || !WorldIdle(apWorld)) return;
    const auto now = NowMs();
    // Each world step visits at most 64 streams, each at most 64 bodies. Other
    // cohorts retain authority and their readback cache until their next slice.
    for (size_t streamIndex = 0; streamIndex < step.Count; ++streamIndex)
    {
        const auto& stream = *step.Streams[streamIndex];
        auto& binding = *stream.Binding;
        if (!binding.Active.load(std::memory_order_acquire) || binding.World != apWorld) continue;
        bool valid = true;
        for (size_t i = 0; i < binding.Bodies.size(); ++i)
            valid &= binding.Bodies[i]->world == apWorld && binding.Bodies[i]->uid == binding.Ids[i];
        if (!valid) { binding.Invalidate(); continue; }
        std::array<QsTransform, CorpseRagdollRequest::kMaxBodies> targets{};
        std::array<CorpseRagdollBody, CorpseRagdollRequest::kMaxBodies> states{};
        bool settled{};
        uint64_t tick{};
        // Validate the entire pose before touching any constrained member.
        for (size_t i = 0; i < binding.Bodies.size(); ++i)
            valid &= stream.Evaluate(step.Time, i, targets[i], settled, tick, &states[i]);
        if (!valid) continue;
        if (!aAfterStep)
        {
            for (size_t i = 0; i < binding.Bodies.size(); ++i)
            {
                auto* body = binding.Bodies[i];
                // Native animation may still have a saved dynamic motion. Restore
                // through 60908, retaining its inertia subtype; never force type 4.
                if (Dynamic(states[i].MotionType) && body->motionType == 4)
                    SetMotion(body, 1);
                valid &= Dynamic(states[i].MotionType) ? Dynamic(body->motionType) :
                    body->motionType == states[i].MotionType;
            }
            if (!valid || !binding.Active.load(std::memory_order_acquire) || !WorldIdle(apWorld)) continue;
            if (settled && binding.Asleep)
            {
                bool aligned = true;
                for (size_t i = 0; i < binding.Bodies.size(); ++i)
                {
                    const auto error = MeasureBody(binding.Bodies[i], targets[i], states[i].MotionType);
                    aligned &= error.Position < kSettleNearPosition && error.Angle < kSettleNearAngle;
                }
                // The same band that let it settle keeps it asleep. With a tighter wake band (1 u/1 deg) the joint
                // solver's relaxation after the exact placement (~1.15 deg) woke it again, and the drive re-settled
                // it every half second for ten minutes: the corpse crept on the follower (B1695, session 2026-09-29).
                if (aligned) continue; // heartbeats must not reactivate an island
                binding.Asleep = false; // resume the entire constrained set
                binding.RestSinceMs = 0;
            }
            // The copy binds wherever its local death left it (50 u / 21 deg off in run 213137) and the capped
            // velocity drive below converges translation but plateaus near 5 deg against the joints. Place the whole
            // constrained set on the owner's pose once at bind, and again only if it drifts far: every body moves
            // together to a joint-consistent pose, carrying the owner's velocities.
            {
                float worstPosition{}, worstAngle{};
                for (size_t i = 0; i < binding.Bodies.size(); ++i)
                {
                    if (binding.Bodies[i]->motionType == 5) continue;
                    const auto error = MeasureBody(binding.Bodies[i], targets[i], states[i].MotionType);
                    worstPosition = (std::max)(worstPosition, error.Position);
                    worstAngle = (std::max)(worstAngle, error.Angle);
                }
                if (!binding.Placed || worstPosition > 20.f || worstAngle > 30.f)
                {
                    bool placed = true;
                    for (size_t i = 0; i < binding.Bodies.size(); ++i)
                    {
                        auto* body = binding.Bodies[i];
                        if (!binding.Active.load(std::memory_order_acquire) || !WorldIdle(apWorld) ||
                            body->world != apWorld || body->uid != binding.Ids[i])
                        {
                            placed = false;
                            break;
                        }
                        if (body->motionType == 5) continue;
                        SetBodyPose(body, targets[i]);
                        for (int axis = 0; axis < 3; ++axis)
                        {
                            body->linearVelocity[axis] = states[i].LinearVelocity[axis];
                            body->angularVelocity[axis] = states[i].AngularVelocity[axis];
                        }
                    }
                    if (!placed) continue;
                    binding.Placed = true;
                    ++binding.Snaps;
                    continue; // placed exactly this step; the drive resumes next step
                }
            }
            for (size_t i = 0; i < binding.Bodies.size(); ++i)
            {
                auto* body = binding.Bodies[i];
                if (!binding.Active.load(std::memory_order_acquire) || !WorldIdle(apWorld) ||
                    body->world != apWorld || body->uid != binding.Ids[i])
                    break; // Native activation callbacks may invalidate the stream.
                if (body->motionType == 5) continue; // preserve native fixed helpers
                const auto error = MeasureBody(body, targets[i], states[i].MotionType);
                binding.Asleep = false;
                const float alpha = 1.f - std::exp(-aDeltaTime / 0.04f);
                QsTransform goal = targets[i];
                glm::vec3 delta{targets[i].translation[0] - body->transform[12],
                    targets[i].translation[1] - body->transform[13], targets[i].translation[2] - body->transform[14]};
                delta *= alpha;
                const float distance = glm::length(delta);
                if (distance > 10.f / kHavokToGameUnits)
                    delta *= (10.f / kHavokToGameUnits) / distance;
                for (int axis = 0; axis < 3; ++axis)
                    goal.translation[axis] = body->transform[12 + axis] + delta[axis] +
                        states[i].LinearVelocity[axis] * aDeltaTime * (1.f - alpha);
                glm::quat current{error.ActualRotation[3], error.ActualRotation[0], error.ActualRotation[1], error.ActualRotation[2]};
                glm::quat target{goal.rotation[3], goal.rotation[0], goal.rotation[1], goal.rotation[2]};
                auto rotation = glm::slerp(current, target, alpha);
                const glm::vec3 angular{states[i].AngularVelocity[0], states[i].AngularVelocity[1], states[i].AngularVelocity[2]};
                const float speed = glm::length(angular);
                if (speed > 0.0001f)
                    rotation = glm::angleAxis(speed * aDeltaTime * (1.f - alpha), angular / speed) * rotation;
                rotation = glm::normalize(rotation);
                goal.rotation[0] = rotation.x; goal.rotation[1] = rotation.y;
                goal.rotation[2] = rotation.z; goal.rotation[3] = rotation.w;
                using TDrive = void(const float*, const float*, float, void*);
                POINTER_SKYRIMSE(TDrive, drive, 62478);
                drive.Get()(goal.translation, goal.rotation, 1.f / aDeltaTime, body);
            }
            continue;
        }
        // Motion can change in native callbacks. Never settle a failed dynamic
        // restoration or claim native fixed helpers were placed successfully.
        for (size_t i = 0; i < binding.Bodies.size(); ++i)
            valid &= Dynamic(states[i].MotionType) ? Dynamic(binding.Bodies[i]->motionType) :
                binding.Bodies[i]->motionType == states[i].MotionType;
        RagdollObservation observation{};
        observation.FormId = binding.FormId; observation.ServerId = binding.ServerId;
        observation.Limb = binding.Limb; observation.Count = static_cast<uint32_t>(binding.Bodies.size());
        observation.Thread = GetCurrentThreadId(); observation.Tick = tick; observation.WallMs = now;
        observation.PresentationMs = step.Time; observation.OwnerSettled = settled;
        observation.SampleAgeMs = static_cast<uint64_t>((std::max)(0.0, step.Time - tick));
        observation.FollowerResting = valid;
        for (size_t i = 0; i < binding.Bodies.size(); ++i)
        {
            auto* body = binding.Bodies[i];
            observation.Bodies[i] = MeasureBody(body, targets[i], states[i].MotionType);
            if (body->motionType == 5)
                observation.FollowerResting &= observation.Bodies[i].Position < 0.01f && observation.Bodies[i].Angle < 0.1f;
            observation.FollowerResting &= Resting(body);
        }
        observation.Snaps = binding.Snaps;
        // The drive injects velocity every step, so Resting() never passes while it runs and the exact settle
        // never fired (run 213137: residual held ~1 u/5 deg, "exact owner settle" absent). Once the owner has
        // settled and the copy is close, count it as rested so the one-time placement + sleep can finish it.
        bool closeToOwner = true;
        for (size_t i = 0; i < binding.Bodies.size(); ++i)
            closeToOwner &= observation.Bodies[i].Position < kSettleNearPosition && observation.Bodies[i].Angle < kSettleNearAngle;
        if (settled && valid && (observation.FollowerResting || closeToOwner))
        {
            if (!binding.RestSinceMs) binding.RestSinceMs = now;
        }
        else { binding.RestSinceMs = 0; binding.Asleep = false; }
        if (settled && !binding.Asleep && binding.RestSinceMs && now - binding.RestSinceMs >= 500 &&
            WorldIdle(apWorld) && binding.Active.load(std::memory_order_acquire))
        {
            // Full-set, one-time settle fallback. No joints are removed/disabled,
            // no moving-body teleport, no writes from postPhysics listeners.
            for (size_t i = 0; i < binding.Bodies.size(); ++i)
            {
                auto* body = binding.Bodies[i];
                // 60918 can drain callback-enqueued removal before returning.
                // A reference preserves allocation, not membership of the next body.
                if (!binding.Active.load(std::memory_order_acquire) || !WorldIdle(apWorld) ||
                    body->world != apWorld || body->uid != binding.Ids[i])
                {
                    valid = false;
                    break;
                }
                if (body->motionType != 5) SetBodyPose(body, targets[i]);
            }
            if (!valid || !binding.Active.load(std::memory_order_acquire) || !WorldIdle(apWorld))
            {
                binding.RestSinceMs = 0;
                continue; // An interrupted set is never reported as an exact settle.
            }
            // E60850 acts on islands. Request once per distinct island in this
            // bounded ragdoll, only after every member passed the rest gate.
            std::array<void*, CorpseRagdollRequest::kMaxBodies> islands{};
            size_t count{};
            for (auto* body : binding.Bodies)
            {
                if (!binding.Active.load(std::memory_order_acquire) || !WorldIdle(apWorld) || body->world != apWorld)
                {
                    valid = false;
                    break;
                }
                auto* island = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(body) + 0x130);
                if (island && std::find(islands.begin(), islands.begin() + count, island) == islands.begin() + count)
                {
                    islands[count++] = island;
                    // E60850 sleeps ALL island entities. Do not force unrelated
                    // moving contacts to sleep, or scan an unbounded island.
                    const auto* bytes = static_cast<const uint8_t*>(island);
                    const auto size = *reinterpret_cast<const int32_t*>(bytes + 0x68);
                    auto** members = *reinterpret_cast<RigidBody***>(const_cast<uint8_t*>(bytes) + 0x60);
                    bool resting = members && size > 0 && size <= 128;
                    for (int32_t index = 0; resting && index < size; ++index)
                    {
                        const auto* member = members[index];
                        // A slow unrelated contact is still not ours to deactivate.
                        // At most 128 members x 64 streamed bodies on this rare path.
                        resting = member && member->world == apWorld && Resting(member) &&
                            std::find(binding.Bodies.begin(), binding.Bodies.end(), member) != binding.Bodies.end();
                    }
                    if (resting) SleepBody(body);
                }
            }
            if (!valid || !binding.Active.load(std::memory_order_acquire) || !WorldIdle(apWorld))
            {
                binding.RestSinceMs = 0;
                continue;
            }
            binding.Asleep = true;
            binding.ExactTick = tick;
            observation.Exact = true;
        }
        observation.Log = !binding.Logged || now >= binding.NextLogMs;
        if (observation.Log || observation.Exact || now >= binding.NextCaptureMs)
        {
            PublishObservation(observation);
            binding.NextCaptureMs = now + (settled ? kResidualLogMs : 100);
            if (observation.Log) { binding.Logged = true; binding.NextLogMs = now + kResidualLogMs; }
        }
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

void CorpseRagdollService::QueueActor(Actor* apActor) noexcept
{
    if (apActor && !apActor->GetExtension()->IsRemote())
        s_pendingActors[s_pendingActorWrite.fetch_add(1, std::memory_order_relaxed) % s_pendingActors.size()].store(apActor->formID, std::memory_order_release);
}

void CorpseRagdollService::OnMainFrame() noexcept
{
    DrainDoorCrashAudit();
    DrainObservations();
    StepBinding::Drain(); // Also drain while disconnected or playback is disabled.
    auto* pService = s_ragdollService.load(std::memory_order_acquire);
    if (!pService)
        return;
    std::lock_guard lock(pService->m_remoteLock);
    if (pService->m_disconnectPending.exchange(false))
        pService->ResetOnMainFrame();
    if (!pService->m_applyOnMainFrame.load(std::memory_order_relaxed))
        return;
    for (auto& pending : s_pendingActors)
        if (const auto formId = pending.exchange(0, std::memory_order_acquire))
            if (const auto entity = Utils::FindLocalEntityByFormId(formId); entity &&
                pService->m_world.all_of<LocalComponent>(*entity))
            {
                const auto serverId = pService->m_world.get<LocalComponent>(*entity).Id;
                if (pService->m_captureEntities.emplace(*entity, serverId).second)
                    pService->m_captureQueue.push_front(*entity);
                pService->m_nextTickMs = 0;
            }
    const auto now = NowMs();
    if (now >= pService->m_nextTickMs)
    {
        pService->m_nextTickMs = now + kStreamMs;
        pService->CaptureOwned(now);
        pService->ApplyRemote(now);
    }
    pService->ApplyDismembers(NowMs());
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
    m_ended.clear();
    m_dismembers.clear();
    m_sentDismembers.clear();
    m_captureQueue.clear();
    m_captureEntities.clear();
    m_remoteQueue.clear();
    m_dismemberQueue.clear();
    m_discoveryCursor = 0;
    {
        std::lock_guard lock(m_stepLock);
        m_stepFrame.reset();
    }
    PoseCopyAuthority::ClearRagdollAuthority();
    {
        std::lock_guard followingLock(s_followingLock);
        s_followingSinceMs.clear();
        s_followingServerIds.clear();
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
    RetainedBodies bodies;
    if (!apActor || !GetRagdollBodies(apActor, bodies))
        return "[]";
    std::string json = "[";
    for (size_t i = 0; i < bodies.size(); ++i)
        json += fmt::format("{}[{:.1f},{:.1f},{:.1f}]", i ? "," : "", bodies[i]->transform[12] * kHavokToGameUnits,
            bodies[i]->transform[13] * kHavokToGameUnits, bodies[i]->transform[14] * kHavokToGameUnits);
    return json + "]";
}

std::string CorpseRagdollService::DescribeRagdollRenderPose(Actor* apActor) noexcept
{
    if (!apActor || !PhysicsOwnsSkeleton(apActor)) return {};
    GraphRef ref;
    if (!apActor->animationGraphHolder.GetBSAnimationGraph(&ref.pManager) || !ref.pManager) return {};
    BSScopedLock<BSRecursiveLock> lock(ref.pManager->lock);
    if (ref.pManager->animationGraphIndex >= ref.pManager->animationGraphs.size) return {};
    AnimationGraph graph{};
    if (!ReadNative(ref.pManager->animationGraphs.Get(ref.pManager->animationGraphIndex), graph)) return {};
    const auto count = (std::min)(graph.boneNodes.length, 128u);
    std::string bones = "[";
    for (uint32_t i = 0; i < count; ++i)
    {
        BoneNodeEntry entry{};
        NiTransform transform{};
        if (!ReadNative(graph.boneNodes.data + i, entry) || !entry.node) continue;
        const char* name{};
        auto* node = static_cast<uint8_t*>(entry.node);
        // Match 63856 / 140BDFA20: nonnegative +8 selects a flattened bone,
        // not the containing tree's root. CommonLib BoneEntry is 0x80 bytes.
        if (static_cast<int32_t>(entry.unk08) >= 0)
        {
            uint32_t length{};
            uint8_t* entries{};
            if (!ReadNative(node + 0x128, length) || entry.unk08 >= length || entry.unk08 >= 4096 ||
                !ReadNative(node + 0x130, entries) || !entries) continue;
            auto* flattened = entries + size_t(entry.unk08) * 0x80;
            if (!ReadNative(flattened + 0x70, node)) continue;
            if (!node)
            {
                if (!ReadNative(flattened + 0x34, transform)) continue;
                ReadNative(flattened + 0x78, name);
            }
        }
        if (node)
        {
            if (!ReadNative(node + offsetof(NiAVObject, world), transform)) continue;
            ReadNative(node + 0x10, name);
        }
        const auto& p = transform.translate;
        const auto& r = transform.rotate.entry;
        // CommonLib NiObjectNET::name +10. Bounded name hash qualifies cross-PC
        // node identity without treating a skeleton index as a rigid-body index.
        std::array<char, 128> nameBytes{};
        uint64_t nameHash = 14695981039346656037ull;
        bool nameComplete = false;
        if (ReadNative(name, nameBytes))
            for (char c : nameBytes)
            {
                if (!c) { nameComplete = true; break; }
                nameHash = (nameHash ^ static_cast<uint8_t>(c)) * 1099511628211ull;
            }
        if (bones.size() > 1) bones += ',';
        bones += fmt::format("{{\"bone\":{},\"nameHash\":{},\"nameComplete\":{},\"p\":[{},{},{}],\"r\":[{},{},{},{},{},{},{},{},{}],\"scale\":{}}}",
            i, nameHash, nameComplete, p.x, p.y, p.z, r[0][0], r[0][1], r[0][2], r[1][0], r[1][1], r[1][2], r[2][0], r[2][1], r[2][2], transform.scale);
    }
    bones += ']';
    const auto serverId = Utils::FindLocalEntityByFormId(apActor->formID);
    auto mappedId = serverId ? Utils::GetServerId(*serverId) : std::optional<uint32_t>{};
    if (!mappedId)
    {
        std::lock_guard followingLock(s_followingLock);
        if (const auto found = s_followingServerIds.find(apActor->formID); found != s_followingServerIds.end())
            mappedId = found->second;
    }
    return fmt::format("{{\"kind\":\"ragdoll_render\",\"id\":{},\"serverId\":{},\"tick\":{},\"presentationMs\":{},\"wallMs\":{},\"remote\":{},\"stateFlags\":{},\"boneCount\":{},\"truncated\":{},\"nonAtomic\":true,\"bones\":{}}}",
        apActor->formID, mappedId.value_or(0), PoseCopyAuthority::GetCurrentTick(),
        PoseCopyAuthority::GetPresentationTimeMs(), NowMs(), apActor->GetExtension()->IsRemote(), apActor->actorState.flags1,
        graph.boneNodes.length, graph.boneNodes.length > count, bones);
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
    if (!m_dismembers.contains(acMessage.ServerId)) m_dismemberQueue.push_back(acMessage.ServerId);
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
    const auto budget = (std::min)(size_t{16}, m_dismemberQueue.size());
    for (size_t visited = 0; visited < budget; ++visited)
    {
        const auto serverId = m_dismemberQueue.front();
        m_dismemberQueue.pop_front();
        auto it = m_dismembers.find(serverId);
        if (it == m_dismembers.end()) continue;
        m_dismemberQueue.push_back(serverId);
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
        RetainedBodies bodies;
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
    const auto key = StreamKey(acMessage.ServerId, acMessage.Limb);
    if (const auto ended = m_ended.find(key); ended != m_ended.end() && acMessage.Tick <= ended->second)
        return;
    if (!m_remote.contains(key)) m_remoteQueue.push_back(key);
    auto& ragdoll = m_remote[key];
    const auto size = static_cast<uint32_t>(ragdoll.Ring.size());
    if (!acMessage.Active)
    {
        if ((!ragdoll.RingCount || acMessage.Tick > ragdoll.Ring[(ragdoll.RingNext + size - 1) % size].Tick) &&
            acMessage.Tick > ragdoll.EndTick)
        {
            ragdoll.EndTick = acMessage.Tick;
            ++ragdoll.Revision;
            m_ended[key] = acMessage.Tick;
        }
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
            if (ragdoll.Binding) ragdoll.Binding->Invalidate();
            ragdoll.RingCount = 0;
            ragdoll.Knocked = false;
            ragdoll.OwnerDying = false;
            ragdoll.RetryTransitionMs = 0;
        }
    }
    ragdoll.EndTick = 0;
    ragdoll.DismemberTick = acMessage.DismemberTick;
    if (acMessage.Limb)
    {
        NotifyDismember event{};
        event.ServerId = acMessage.ServerId;
        event.Limb = acMessage.Limb;
        event.Tick = acMessage.DismemberTick;
        OnDismember(event);
    }
    if (!ragdoll.RingCount)
        spdlog::info("Ragdoll server {:X} limb {}: owner's stream received (tick {}, {} bodies)", acMessage.ServerId,
            acMessage.Limb, acMessage.Tick, acMessage.Bodies.size());
    if (!acMessage.Limb)
    {
        if (auto* actor = Utils::GetByServerId<Actor>(acMessage.ServerId); actor && actor->GetExtension()->IsRemote())
        {
            std::lock_guard followingLock(s_followingLock);
            if (ragdoll.LocalFormId && ragdoll.LocalFormId != actor->formID)
            {
                s_followingSinceMs.erase(ragdoll.LocalFormId);
                s_followingServerIds.erase(ragdoll.LocalFormId);
                PoseCopyAuthority::SetRagdollPending(ragdoll.LocalFormId, false);
                PoseCopyAuthority::SetRagdollSimulating(ragdoll.LocalFormId, false);
            }
            s_followingSinceMs[actor->formID] = NowMs();
            s_followingServerIds[actor->formID] = acMessage.ServerId;
            ragdoll.LocalFormId = actor->formID;
            PoseCopyAuthority::SetRagdollPending(actor->formID, true);
        }
    }
    auto& sample = ragdoll.Ring[ragdoll.RingNext];
    sample.Tick = acMessage.Tick;
    sample.Settled = acMessage.Settled;
    sample.Dying = acMessage.Dying;
    std::copy(std::begin(acMessage.Origin), std::end(acMessage.Origin), std::begin(sample.Origin));
    sample.Heading = acMessage.Heading;
    sample.Bodies = acMessage.Bodies;
    ragdoll.RingNext = (ragdoll.RingNext + 1) % size;
    ragdoll.RingCount = (std::min)(ragdoll.RingCount + 1, size);
    ++ragdoll.Revision;
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
    // Discover only eight local candidates per tick using packed storage, then
    // service known ragdolls independently. No per-tick all-actor census.
    auto& locals = m_world.storage<LocalComponent>();
    for (size_t i = 0, count = (std::min)(kDiscoverPerFrame, locals.size()); i < count; ++i)
    {
        const auto entity = locals.data()[m_discoveryCursor++ % locals.size()];
        if (m_world.all_of<FormIdComponent>(entity) && m_captureEntities.emplace(entity, locals.get(entity).Id).second)
            m_captureQueue.push_back(entity);
    }
    const auto candidates = (std::min)(kStreamsPerFrame / 2, m_captureQueue.size());
    for (size_t candidate = 0; candidate < candidates; ++candidate)
    {
        const auto entity = m_captureQueue.front();
        m_captureQueue.pop_front();
        const uint32_t serverId = m_captureEntities.at(entity);
        const bool local = m_world.valid(entity) && m_world.all_of<FormIdComponent, LocalComponent>(entity) &&
            m_world.get<LocalComponent>(entity).Id == serverId;
        auto* actor = local ? Cast<Actor>(TESForm::GetById(m_world.get<FormIdComponent>(entity).Id)) : nullptr;
        const bool loaded = actor && actor->GetNiNode() && actor->parentCell && actor->parentCell->IsAttached();
        const bool physics = loaded && PhysicsOwnsSkeleton(actor);
        const auto endStream = [&](uint32_t limb)
        {
            const auto found = m_owned.find(StreamKey(serverId, limb));
            if (found == m_owned.end()) return true;
            CorpseRagdollRequest end{};
            end.ServerId = serverId; end.Limb = limb; end.DismemberTick = found.value().DismemberTick;
            end.Tick = (std::max)(PoseCopyAuthority::GetCurrentTick(), found.value().LastTick + 1);
            end.Active = false;
            if (!m_transport.Send(end)) return false;
            m_owned.erase(found);
            return true;
        };
        // The limb event goes out as soon as the head comes off, before the physics gate below: a beheading happens in
        // a killmove, still animated, so the victim's skeleton is not physics-owned yet and the event used to wait
        // for the ragdoll (4.8 s in run 20260928-111810; the follower's head appeared that late).
        uint64_t eventTick{};
        if (actor)
        {
            std::lock_guard lock(s_dismemberLock);
            const auto event = s_localDismembers.find(actor->formID);
            if (event != s_localDismembers.end()) eventTick = event->second.Tick;
        }
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
        // A severed head is loose physics while the body is still in its killmove animation: keep streaming the head
        // (limb 1) then. Gating it on the body's ragdoll left the follower's head flying on local physics for 4.7 s
        // before snapping to the owner's (run 20260928-120033).
        const bool looseHead = loaded && eventTick != 0;
        if (!physics && !looseHead && endStream(0) && endStream(1))
        {
            m_captureEntities.erase(entity);
            continue;
        }
        if (!physics && looseHead)
            endStream(0);
        m_captureQueue.push_back(entity);
        if (!loaded) continue;
        GraphRef graphRef;
        std::optional<BSScopedLock<BSRecursiveLock>> graphLock;
        if (actor->animationGraphHolder.GetBSAnimationGraph(&graphRef.pManager) && graphRef.pManager)
            graphLock.emplace(graphRef.pManager->lock);
        if (((actor->actorState.flags1 >> 21) & 0xF) != 0)
            s_diedAtMs.try_emplace(actor->formID, aNowMs);
        else
            s_diedAtMs.erase(actor->formID);
        for (uint32_t limb = 0; limb <= 1; ++limb)
        {
            if ((!limb && !PhysicsOwnsSkeleton(actor)) || (limb && !eventTick))
                continue;
            const auto key = StreamKey(serverId, limb);
            RetainedBodies bodies;
            // Stream a dying body from its first frame, while its bodies are still keyframed to the death animation:
            // waiting for the ragdoll to take over left the follower's copy on its own death for ~200 ms (Lokir slid
            // 76 u, then snapped onto the owner's body; owner request 2026-09-30: the first sample in real time).
            const auto dyingState = (actor->actorState.flags1 >> 21) & 0xF;
            const bool dying = !limb && (dyingState == 1 || dyingState == 2);
            if (limb ? !GetHeadBody(actor, bodies) : dying ? !GetRagdollBodies(actor, bodies) : !RagdollSimulating(actor, bodies))
                continue;
            PhysicsLock physicsLock(bodies);
            if (!physicsLock.Wrapper || !WorldIdle(bodies.front()->world))
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
            request.Heading = actor->rotation.z;
            for (auto* rigid : bodies)
            {
                CorpseRagdollBody body{};
                for (int axis = 0; axis < 3; ++axis)
                    body.Position[axis] = rigid->transform[12 + axis] * kHavokToGameUnits - request.Origin[axis];
                MatrixToQuaternion(rigid->transform, body.Rotation);
                std::copy_n(rigid->linearVelocity, 3, body.LinearVelocity);
                std::copy_n(rigid->angularVelocity, 3, body.AngularVelocity);
                // Keyframed to the owner's death animation (4): receivers follow it as a dynamic body (their restore
                // path makes a keyframed copy dynamic), so the copy takes the owner's motion from the first frame.
                body.MotionType = rigid->motionType == 4 ? 1 : rigid->motionType;
                request.Bodies.push_back(body);
            }
            if (!m_transport.Send(request))
                continue;
            if (!owned.LastSentMs)
                spdlog::info("Ragdoll {:X} limb {} (server {:X}): streaming {} bodies (life {}, first body motion {}, {} ms after life state left alive)",
                    actor->formID, limb, serverId, bodies.size(), dyingState, bodies.front()->motionType,
                    s_diedAtMs.contains(actor->formID) ? static_cast<int64_t>(aNowMs - s_diedAtMs[actor->formID]) : int64_t{-1});
            owned.LastSentMs = aNowMs;
            owned.LastTick = request.Tick;
            owned.SentSettled = request.Settled;
        }
    }
}

void CorpseRagdollService::ApplyRemote(const uint64_t aNowMs, bool aRelease) noexcept
{
    const double presentationTime = PoseCopyAuthority::GetPresentationTimeMs();
    const auto presentation = static_cast<uint64_t>(presentationTime);
    std::shared_ptr<StepFrame> frame;
    {
        std::lock_guard lock(m_stepLock);
        if (!m_stepFrame) m_stepFrame = std::make_shared<StepFrame>();
        frame = m_stepFrame;
    }
    const auto budget = aRelease ? m_remoteQueue.size() : (std::min)(kStreamsPerFrame, m_remoteQueue.size());
    for (size_t visited = 0; visited < budget; ++visited)
    {
        const auto key = m_remoteQueue.front();
        m_remoteQueue.pop_front();
        auto it = m_remote.find(key);
        if (it == m_remote.end()) continue;
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
        const auto entity = Utils::FindEntityByServerId(serverId);
        const bool locallyOwned = entity && m_world.all_of<LocalComponent>(*entity);
        const bool end = aRelease || (ragdoll.EndTick && presentation >= ragdoll.EndTick) ||
            locallyOwned || !m_transport.IsConnected();
        if (end)
        {
            if (ragdoll.Binding)
                ragdoll.Binding->Invalidate();
            if (ragdoll.Binding) PoseCopyAuthority::SetControlledRagdollDriver(ragdoll.Binding->Driver, false);
            {
                std::lock_guard lock(m_stepLock);
                frame->Streams.erase(key);
                if (ragdoll.Binding && ragdoll.Binding->Instance) frame->Instances.erase(ragdoll.Binding->Instance);
            }
            // Motion remains owned by the engine. No restoration can lock a
            // detached raw world or turn a dynamic corpse back into animation.
            if (ragdoll.LocalFormId && !limb)
            {
                PoseCopyAuthority::SetRagdollSimulating(ragdoll.LocalFormId, false);
                PoseCopyAuthority::SetRagdollPending(ragdoll.LocalFormId, false);
                std::lock_guard lock(s_followingLock);
                s_followingSinceMs.erase(ragdoll.LocalFormId);
                s_followingServerIds.erase(ragdoll.LocalFormId);
            }
            m_remote.erase(it);
            continue;
        }
        m_remoteQueue.push_back(key);
        if (!ragdoll.RingCount)
            continue;
        if (!actor || !actor->GetExtension()->IsRemote())
        {
            if (ragdoll.Binding)
                ragdoll.Binding->Invalidate();
            if (!limb && ragdoll.LocalFormId)
                PoseCopyAuthority::SetRagdollSimulating(ragdoll.LocalFormId, false);
            skip(!actor ? "no actor for server id" : "waiting for remote authority binding");
            continue;
        }
        if (!limb)
        {
            // Bodies follow the owner exactly, but the game draws a ragdoll relative to its actor reference. The
            // copy's reference stayed where its local death happened (393 u off in run 215706: physics 0.00003 u,
            // rendered root 393 u, bones 42 u). Hold the reference on the owner's actor origin; the bodies are
            // not moved (aSyncHavok=false), the step drive keeps them on the owner pose.
            const auto size = static_cast<uint32_t>(ragdoll.Ring.size());
            // The PRESENTED sample (newest at or before the presentation clock), the one the bodies are evaluated
            // at: the newest leads the drawn bodies by the ring depth during the fall (Muse refute-corpse).
            uint32_t presented = (ragdoll.RingNext + size - ragdoll.RingCount) % size;
            for (uint32_t i = 0; i < ragdoll.RingCount; ++i)
            {
                const auto index = (ragdoll.RingNext + size - ragdoll.RingCount + i) % size;
                if (ragdoll.Ring[index].Tick <= presentation)
                    presented = index;
            }
            const auto& newest = ragdoll.Ring[presented];
            // Interpolate the owner's origin and heading at the presentation clock (between the presented sample and
            // the next one) and move the reference there every frame, a bounded step at a time. Snapping it to the
            // newest sample past a 5 u threshold moved it in steps (Lokir 2026-09-30: 33 u at death, then 60 u and a
            // 127 deg turn one second later); the ragdoll is drawn relative to the reference, so each step flashed
            // the body out of place for a frame.
            NiPoint3 origin;
            origin.x = newest.Origin[0]; origin.y = newest.Origin[1]; origin.z = newest.Origin[2];
            float heading = newest.Heading;
            for (uint32_t i = 0; i + 1 < ragdoll.RingCount; ++i)
            {
                const auto& a = ragdoll.Ring[(ragdoll.RingNext + size - ragdoll.RingCount + i) % size];
                const auto& b = ragdoll.Ring[(ragdoll.RingNext + size - ragdoll.RingCount + i + 1) % size];
                if (a.Tick <= presentation && presentation < b.Tick && b.Tick > a.Tick)
                {
                    const float u = static_cast<float>(static_cast<double>(presentation - a.Tick) / static_cast<double>(b.Tick - a.Tick));
                    origin.x = a.Origin[0] + (b.Origin[0] - a.Origin[0]) * u;
                    origin.y = a.Origin[1] + (b.Origin[1] - a.Origin[1]) * u;
                    origin.z = a.Origin[2] + (b.Origin[2] - a.Origin[2]) * u;
                    heading = a.Heading + std::remainder(b.Heading - a.Heading, 6.2831853f) * u;
                    break;
                }
            }
            // Every frame, all at once: the drawn ragdoll sits on the reference, so a partial move is drawn partly
            // wrong. A held or blended owner pose no longer depends on it: PoseCopyAuthority rebases bone 0 onto the
            // owner's own frame (the death-time reference turn flashed the held pose 134 deg away; gating this move
            // instead left the reference 85.7 u behind and jumped the body when the ragdoll took over, 2026-09-30).
            const float dx = origin.x - actor->position.x, dy = origin.y - actor->position.y, dz = origin.z - actor->position.z;
            const float gap = std::sqrt(dx * dx + dy * dy + dz * dz);
            if (std::isfinite(gap) && gap > 0.25f)
            {
                if (gap > 50.f)
                    spdlog::info("Ragdoll {:X} (server {:X}): actor reference {:.1f} u from owner origin, moved to it",
                        actor->formID, serverId, gap);
                // HookSetPosition drops SetPosition on remote actors unless the call is scoped as ours: without this the
                // re-anchor never applied (run 20260928-072831: reference stayed exactly 88.1 u off every frame).
                ScopedReferencesOverride recursionGuard;
                actor->SetPosition(origin, false);
                ++ragdoll.AnchorMoves;
            }
            // Heading too: the rendered body sits on the reference's heading. The owner turns its reference at
            // death (97 -> 306..330 deg for Lokir, then 108 at the settle); the copy stayed at 97, so its drawn
            // body was the owner's pose turned by the difference (151 deg measured = the reference gap, run
            // 20260928-093355), seen as the corpse spinning during the fall.
            const float turn = std::remainder(heading - actor->rotation.z, 6.2831853f);
            if (std::isfinite(turn) && std::abs(turn) > 0.002f)
            {
                if (std::abs(turn) > 1.5f)
                    spdlog::info("Ragdoll {:X} (server {:X}): heading {:.0f} deg from the owner's, turned to match",
                        actor->formID, serverId, turn * 57.29578f);
                actor->SetRotation(actor->rotation.x, actor->rotation.y, heading);
            }
        }
        if (!limb)
        {
            if (ragdoll.LocalFormId && ragdoll.LocalFormId != actor->formID)
            {
                PoseCopyAuthority::SetRagdollSimulating(ragdoll.LocalFormId, false);
                PoseCopyAuthority::SetRagdollPending(ragdoll.LocalFormId, false);
                std::lock_guard lock(s_followingLock);
                s_followingSinceMs.erase(ragdoll.LocalFormId);
                s_followingServerIds.erase(ragdoll.LocalFormId);
            }
            ragdoll.LocalFormId = actor->formID;
            {
                std::lock_guard lock(s_followingLock);
                s_followingSinceMs[actor->formID] = aNowMs;
                s_followingServerIds[actor->formID] = serverId;
            }
            PoseCopyAuthority::SetRagdollPending(actor->formID, true);
            PoseCopyAuthority::SetRagdollSimulating(actor->formID, false);
        }
        const auto* root = actor->GetNiNode();
        if (ragdoll.Root != root)
        {
            ragdoll.Root = root;
            ragdoll.Knocked = false;
            ragdoll.RetryTransitionMs = 0;
            if (ragdoll.Binding)
                ragdoll.Binding->Invalidate();
        }
        if (!root || !actor->parentCell || !actor->parentCell->IsAttached())
        {
            if (ragdoll.Binding) ragdoll.Binding->Invalidate();
            skip("no attached 3D");
            continue;
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
        if (!limb && aNowMs >= ragdoll.RetryTransitionMs &&
            (!ragdoll.Knocked || (ownerDyingNow && !actor->IsDead())))
        {
            if (!actor->currentProcess)
            {
                skip("no AI process");
                continue;
            }
            ragdoll.OwnerDying = ownerDyingNow;
            // "Dying" counted as physics-owned, so a copy still playing its local death animation was never knocked
            // into ragdoll; that animation's root motion kept moving the corpse away from where the owner's fell
            // (Lokir 50-65 u pull every frame, Muse diag-lokir3). Force the ragdoll whenever the copy is not already
            // in a knock/ragdoll state (knockState bits 25-27).
            const bool inRagdollState = ((actor->actorState.flags1 >> 25) & 0x7) != 0;
            if (ragdoll.OwnerDying && !actor->IsDead())
                actor->KillIntoRagdoll();
            else if (!inRagdollState)
            {
                actor->currentProcess->KnockExplosion(actor, &actor->position, 0.f);
                spdlog::info("Ragdoll {:X}: copy was not ragdolling (flags1 {:08X}); knocked into ragdoll to follow the owner",
                    actor->formID, actor->actorState.flags1);
            }
            if (!ragdoll.Knocked)
                ragdoll.WatchSinceMs = aNowMs;
                s_wornProbeUntilMs.store(GetTickCount64() + 400, std::memory_order_relaxed);
            ragdoll.Knocked = true;
            ragdoll.RetryTransitionMs = aNowMs + 250;
            // A dead copy following the owner's ragdoll gets the dead-body collision layer whichever path killed it
            // (a death animation without a knock does not pass the kill hook).
            if (const auto life = (actor->actorState.flags1 >> 21) & 0xF; life == 1 || life == 2)
                CharacterService::NoteRemoteDeath(actor->formID);
        }
        if (!limb && ragdoll.WatchSinceMs && aNowMs - ragdoll.WatchSinceMs < 2000)
        {
            std::string worn;
            for (const auto& item : actor->GetActorInventory().Entries)
                if (item.IsWorn())
                    worn += fmt::format(" {:X}", item.BaseId.BaseId);
            if (worn != ragdoll.WatchWorn)
            {
                spdlog::info("Death watch {:X} +{} ms: worn [{}] life {} knock {} root {}", actor->formID,
                    aNowMs - ragdoll.WatchSinceMs, worn, (actor->actorState.flags1 >> 21) & 0xF,
                    (actor->actorState.flags1 >> 25) & 0x7, static_cast<const void*>(actor->GetNiNode()));
                ragdoll.WatchWorn = worn;
            }
        }
        RetainedBodies bodies;
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
            if (ragdoll.Binding) ragdoll.Binding->Invalidate();
            if (!ragdoll.CountMismatchLogged)
                spdlog::warn("Ragdoll {:X} limb {}: count mismatch, local {}, owner {}; holding owner skeleton",
                    actor->formID, limb, bodies.size(), newest.Bodies.size());
            ragdoll.CountMismatchLogged = true;
            continue;
        }
        PhysicsLock physicsLock(bodies);
        if (!physicsLock.Wrapper || !WorldIdle(bodies.front()->world))
        {
            skip("physics world unavailable or mutation pending");
            continue;
        }
        bool reset = ragdoll.BodyIds.size() != bodies.size() || ragdoll.PhysicsWorld != bodies.front()->world ||
            (ragdoll.Binding && (!ragdoll.Binding->Active.load(std::memory_order_acquire) || ragdoll.Binding->Driver != driver));
        for (size_t i = 0; !reset && i < bodies.size(); ++i)
            reset = ragdoll.BodyIds[i] != bodies[i]->uid || ragdoll.BodyPointers[i] != bodies[i];
        if (reset)
        {
            if (ragdoll.Binding)
                ragdoll.Binding->Invalidate();
            {
                std::lock_guard lock(m_stepLock);
                if (ragdoll.Binding && ragdoll.Binding->Instance) frame->Instances.erase(ragdoll.Binding->Instance);
            }
            ragdoll.Binding.reset();
            ragdoll.PublishedRevision = 0;
            ragdoll.BodyIds.clear();
            ragdoll.BodyPointers.clear();
            ragdoll.MotionTypes.clear();
            ragdoll.PhysicsWorld = bodies.front()->world;
            for (auto* body : bodies)
            {
                ragdoll.BodyIds.push_back(body->uid);
                ragdoll.BodyPointers.push_back(body);
                ragdoll.MotionTypes.push_back(body->motionType);
            }
        }
        if (!ragdoll.Binding)
        {
            ragdoll.Binding = std::shared_ptr<StepBinding>(new StepBinding, &StepBinding::Retire);
            auto& binding = *ragdoll.Binding;
            binding.World = bodies.front()->world;
            binding.FormId = actor->formID;
            binding.Limb = limb;
            binding.Driver = driver;
            binding.ServerId = serverId;
            if (driver) ReadNative(static_cast<uint8_t*>(driver) + 0x88, binding.Instance);
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
            spdlog::info("Ragdoll {:X} limb {} (server {:X}): bound {} bodies for owner placement", actor->formID, limb, serverId, bodies.size());
        }
        if (driver)
        {
            PoseCopyAuthority::SetControlledRagdollDriver(driver, true);
            PoseCopyAuthority::SetRagdollSimulating(actor->formID, true);
            ragdoll.DrivesBody = true;
        }
        if (ragdoll.PublishedRevision == ragdoll.Revision)
            continue;
        StepFrame::Stream stream;
        stream.Binding = ragdoll.Binding;
        for (uint32_t i = 0; i < ragdoll.RingCount; ++i)
            stream.Samples.push_back(sample(i));
        stream.EndTick = ragdoll.EndTick;
        {
            std::lock_guard lock(m_stepLock);
            const auto prior = frame->Streams.find(key);
            if (prior == frame->Streams.end() || prior->second->Binding != stream.Binding)
                frame->WorldStreams[stream.Binding->World].emplace_back(key, stream.Binding);
            if (stream.Binding->Instance) frame->Instances[stream.Binding->Instance] = stream.Binding;
            frame->Streams[key] = std::make_shared<const StepFrame::Stream>(std::move(stream));
        }
        ragdoll.PublishedRevision = ragdoll.Revision;
        ragdoll.LastSkipReason = nullptr;
        ragdoll.CountMismatchLogged = false;
        if (driver)
        {
            PoseCopyAuthority::SetControlledRagdollDriver(driver, true);
            PoseCopyAuthority::SetRagdollSimulating(actor->formID, true);
            ragdoll.DrivesBody = true;
        }
    }

}

static TiltedPhoques::Initializer s_ragdollAuditHooks(
    []()
    {
        POINTER_SKYRIMSE(TRemoveGraph, removeGraph, 63622);
        RealRemoveGraph = removeGraph.Get();
        TP_HOOK(&RealRemoveGraph, HookRemoveGraph);
        POINTER_SKYRIMSE(TRemoveRagdoll, removeRagdoll, 64158);
        RealRemoveRagdoll = removeRagdoll.Get();
        TP_HOOK(&RealRemoveRagdoll, HookRemoveRagdoll);
        POINTER_SKYRIMSE(TRemoveObjects, removeObjects, 64677);
        RealRemoveObjects = removeObjects.Get();
        TP_HOOK(&RealRemoveObjects, HookRemoveObjects);
    });
