#include <Combat/PlayerCombat.h>
#include <Actor.h>
#include <PlayerCharacter.h>
#include <AI/AIProcess.h>
#include <Misc/ActorValueOwner.h>
#include <World.h>
#include <Games/ActorExtension.h>
#include <mutex>
#include <unordered_map>

namespace
{
constexpr uint8_t kEntries[]{15, 47, 48, 57};
bool IsDetectionEntry(uint8_t aEntry)
{
    return std::find(std::begin(kEntries), std::end(kEntries), aEntry) != std::end(kEntries);
}

// CommonLib BGSPerkEntry/BGSEntryPointPerkEntry layout. Only entry point records are read.
struct Entry
{
    void** Vtable;
    uint8_t Rank;
    uint8_t Priority;
    uint8_t Padding[6];
    uint8_t EntryPoint;
    uint8_t Data[7];
    void* FunctionData;
    void* Conditions;
    TESForm* Perk;
};
static_assert(offsetof(Entry, Perk) == 0x28);
struct EntryVisitor
{
    virtual uint32_t Visit(Entry* apEntry) = 0;
};

using HasEntries = bool(Actor*, uint8_t);
using VisitEntries = void(Actor*, uint8_t, EntryVisitor*);
HasEntries* s_hasEntries{};
VisitEntries* s_visitEntries{};

struct Snapshot
{
    PlayerCombatState State;
    Actor* ActorPtr{}; // matched to the current native target before use
    std::vector<Entry*> Entries;
    uint64_t ReceivedAt{};
};
std::mutex s_mutex;
std::unordered_map<uint32_t, Snapshot> s_snapshots;
thread_local const Snapshot* s_detection{};
std::atomic<int32_t> s_meterLevel{-1000};
std::atomic<uint64_t> s_meterTime{};

GameArray<Entry*>* PerkEntries(TESForm* apForm)
{
    // PERK = 92. Verified BGSPerk layout in CommonLib and entry accessor natives.
    return apForm && static_cast<uint8_t>(apForm->formType) == 92 ?
        reinterpret_cast<GameArray<Entry*>*>(reinterpret_cast<uint8_t*>(apForm) + 0x60) : nullptr;
}

bool HookHasEntries(Actor* apActor, uint8_t aEntry)
{
    if (s_detection && s_detection->ActorPtr == apActor && IsDetectionEntry(aEntry))
        return std::any_of(s_detection->Entries.begin(), s_detection->Entries.end(),
            [aEntry](auto* entry) { return entry->EntryPoint == aEntry; });
    return s_hasEntries(apActor, aEntry);
}

void HookVisitEntries(Actor* apActor, uint8_t aEntry, EntryVisitor* apVisitor)
{
    if (s_detection && s_detection->ActorPtr == apActor && IsDetectionEntry(aEntry))
    {
        for (auto* entry : s_detection->Entries)
            if (entry->EntryPoint == aEntry && apVisitor->Visit(entry) != 1)
                break;
        return;
    }
    s_visitEntries(apActor, aEntry, apVisitor);
}

using CalculateDetection = void(Actor*, Actor*, int32_t*, uint8_t*, uint8_t*, uint32_t*, NiPoint3*, float*, float*, float*);
CalculateDetection* s_calculateDetection{};
void HookCalculateDetection(Actor* apObserver, Actor* apTarget, int32_t* apLevel,
    uint8_t* apView, uint8_t* apLOS, uint32_t* apLOSResult, NiPoint3* apPosition,
    float* apSound, float* apVisual, float* apSkill)
{
    Snapshot snapshot;
    bool found = false;
    if (apTarget)
    {
        std::lock_guard lock(s_mutex);
        const auto it = s_snapshots.find(apTarget->formID);
        if (it != s_snapshots.end() && it->second.ActorPtr == apTarget &&
            GetTickCount64() - it->second.ReceivedAt < 2000)
        {
            snapshot = it->second;
            found = true;
        }
    }
    const auto* previous = s_detection;
    s_detection = found ? &snapshot : nullptr;
    s_calculateDetection(apObserver, apTarget, apLevel, apView, apLOS, apLOSResult,
        apPosition, apSound, apVisual, apSkill);
    s_detection = previous;
}

using ActorBool = bool(Actor*);
ActorBool* s_isMoving{};
ActorBool* s_isRunning{};
ActorBool* s_isSneaking{};
bool HookIsMoving(Actor* apActor)
{
    return s_detection && s_detection->ActorPtr == apActor ? s_detection->State.Moving : s_isMoving(apActor);
}
bool HookIsRunning(Actor* apActor)
{
    return s_detection && s_detection->ActorPtr == apActor ? s_detection->State.Running : s_isRunning(apActor);
}
bool HookIsSneaking(Actor* apActor)
{
    return s_detection && s_detection->ActorPtr == apActor ? s_detection->State.Sneaking : s_isSneaking(apActor);
}
using LightLevel = float(AIProcess*);
LightLevel* s_lightLevel{};
float HookLightLevel(AIProcess* apProcess)
{
    return s_detection && s_detection->ActorPtr->currentProcess == apProcess ? s_detection->State.Light : s_lightLevel(apProcess);
}
using ArmorWeight = float(Actor*);
ArmorWeight* s_armorWeight{};
float HookArmorWeight(Actor* apActor)
{
    return s_detection && s_detection->ActorPtr == apActor ? s_detection->State.ArmorWeight : s_armorWeight(apActor);
}
using ClampedValue = float(ActorValueOwner*, uint32_t);
ClampedValue* s_clampedValue{};
float HookClampedValue(ActorValueOwner* apOwner, uint32_t aValue)
{
    float value = s_clampedValue(apOwner, aValue);
    if (s_detection && apOwner == &s_detection->ActorPtr->actorValueOwner && aValue == 15)
    {
        // The detection formula scales the actual player by these GMSTs. Copies need the same scale.
        POINTER_SKYRIMSE(float, skillBase, 371398);
        POINTER_SKYRIMSE(float, skillMult, 371401);
        value = value * *skillMult + *skillBase;
    }
    return value;
}

using UpdateMeter = void(int32_t, bool);
UpdateMeter* s_updateMeter{};
void HookUpdateMeter(int32_t aLevel, bool aSneaking)
{
    const auto time = s_meterTime.load(std::memory_order_acquire);
    if (time && GetTickCount64() - time < 2000)
        aLevel = (std::max)(0, s_meterLevel.load(std::memory_order_relaxed));
    s_updateMeter(aLevel, aSneaking);
}

TiltedPhoques::Initializer s_hooks([]()
{
    POINTER_SKYRIMSE(CalculateDetection, calculateDetection, 37774);
    POINTER_SKYRIMSE(HasEntries, hasEntries, 37701);
    POINTER_SKYRIMSE(VisitEntries, visitEntries, 37702);
    POINTER_SKYRIMSE(ActorBool, isMoving, 37953);
    POINTER_SKYRIMSE(ActorBool, isRunning, 37234);
    POINTER_SKYRIMSE(ActorBool, isSneaking, 24917);
    POINTER_SKYRIMSE(LightLevel, lightLevel, 39509);
    POINTER_SKYRIMSE(ArmorWeight, armorWeight, 38286);
    POINTER_SKYRIMSE(ClampedValue, clampedValue, 27284);
    POINTER_SKYRIMSE(UpdateMeter, updateMeter, 51623);
    s_calculateDetection = calculateDetection.Get();
    s_hasEntries = hasEntries.Get();
    s_visitEntries = visitEntries.Get();
    s_isMoving = isMoving.Get();
    s_isRunning = isRunning.Get();
    s_isSneaking = isSneaking.Get();
    s_lightLevel = lightLevel.Get();
    s_armorWeight = armorWeight.Get();
    s_clampedValue = clampedValue.Get();
    s_updateMeter = updateMeter.Get();
    TP_HOOK(&s_calculateDetection, HookCalculateDetection);
    TP_HOOK(&s_hasEntries, HookHasEntries);
    TP_HOOK(&s_visitEntries, HookVisitEntries);
    TP_HOOK(&s_isMoving, HookIsMoving);
    TP_HOOK(&s_isRunning, HookIsRunning);
    TP_HOOK(&s_isSneaking, HookIsSneaking);
    TP_HOOK(&s_lightLevel, HookLightLevel);
    TP_HOOK(&s_armorWeight, HookArmorWeight);
    TP_HOOK(&s_clampedValue, HookClampedValue);
    TP_HOOK(&s_updateMeter, HookUpdateMeter);
});
}

namespace PlayerCombat
{
bool Capture(Actor* apActor, PlayerCombatState& aState)
{
    if (!apActor || !apActor->currentProcess)
        return false;
    aState.Sneaking = s_isSneaking(apActor);
    aState.Moving = s_isMoving(apActor);
    aState.Running = s_isRunning(apActor);
    aState.Speed = (std::max)(0.f, apActor->GetSpeed());
    aState.Light = s_lightLevel(apActor->currentProcess);
    aState.ArmorWeight = s_armorWeight(apActor);
    for (size_t i = 0; i < aState.Values.size(); ++i)
        aState.Values[i] = apActor->GetActorValue(PlayerCombatState::ActorValues[i]);
    struct CaptureVisitor final : EntryVisitor
    {
        PlayerCombatState& State;
        bool Valid{true};
        explicit CaptureVisitor(PlayerCombatState& aState) : State(aState) {}
        uint32_t Visit(Entry* apEntry) override
        {
            if (!apEntry || !IsDetectionEntry(apEntry->EntryPoint))
                return 1;
            const auto* entries = PerkEntries(apEntry->Perk);
            if (!entries)
                return 1;
            for (uint32_t i = 0; i < entries->length; ++i)
            {
                if ((*entries)[i] != apEntry)
                    continue;
                PlayerCombatState::Perk perk;
                if (i >= 4096 || State.Perks.size() >= PlayerCombatState::MaxPerks ||
                    !World::Get().GetModSystem().GetServerModId(apEntry->Perk->formID, perk.Id))
                {
                    Valid = false;
                    return 0;
                }
                perk.EntryIndex = static_cast<uint16_t>(i);
                if (std::find(State.Perks.begin(), State.Perks.end(), perk) == State.Perks.end())
                    State.Perks.push_back(perk);
                break;
            }
            return 1;
        }
    } visitor(aState);
    for (auto entry : kEntries)
        s_visitEntries(apActor, entry, &visitor);
    return visitor.Valid;
}

void Publish(Actor* apActor, const PlayerCombatState& aState)
{
    Snapshot snapshot{aState, apActor, {}, GetTickCount64()};
    for (const auto& perk : aState.Perks)
    {
        const auto* entries = PerkEntries(TESForm::GetById(World::Get().GetModSystem().GetGameId(perk.Id)));
        if (!entries || perk.EntryIndex >= entries->length)
            continue;
        auto* entry = (*entries)[perk.EntryIndex];
        if (!entry)
            continue;
        using GetType = uint32_t(Entry*);
        if (reinterpret_cast<GetType*>(entry->Vtable[4])(entry) == 2 && IsDetectionEntry(entry->EntryPoint))
            snapshot.Entries.push_back(entry);
    }
    // Keep the owner's native visitation order, including equal-priority entries.
    std::lock_guard lock(s_mutex);
    s_snapshots.insert_or_assign(apActor->formID, std::move(snapshot));
}

void Forget(uint32_t aFormId)
{
    std::lock_guard lock(s_mutex);
    s_snapshots.erase(aFormId);
}
void SetTeammate(Actor* apActor, bool aValue)
{
    using SetPlayerTeammate = void(Actor*, bool, bool);
    POINTER_SKYRIMSE(SetPlayerTeammate, setTeammate, 37717);
    setTeammate(apActor, aValue, (apActor->flags2 & 0x80) != 0);
}
void Clear()
{
    std::lock_guard lock(s_mutex);
    s_snapshots.clear();
    s_meterTime.store(0, std::memory_order_release);
}
int32_t DetectionLevel(Actor* apObserver, Actor* apTarget)
{
    using RequestLevel = int32_t(Actor*, Actor*, uint32_t);
    POINTER_SKYRIMSE(RequestLevel, requestLevel, 37764);
    return requestLevel(apObserver, apTarget, 3);
}
std::vector<Actor*> Observers()
{
    // ProcessLists::RequestHighestDetectionLevelAgainstActor, ID 41408.
    POINTER_SKYRIMSE(uint8_t*, processLists, 400315);
    std::vector<Actor*> actors;
    if (!*processLists)
        return actors;
    const auto& handles = *reinterpret_cast<GameArray<uint32_t>*>(*processLists + 0x30);
    for (auto handle : handles)
    {
        auto* actor = Cast<Actor>(TESObjectREFR::GetByHandle(handle));
        if (actor && actor->currentProcess && actor->currentProcess->movementType == 0 &&
            !actor->IsDead() && !actor->IsDisabled() && !actor->IsDeleted() &&
            (actor->flags1 & 2) && !(actor->flags1 & 0x200) && !(actor->flags & (1u << 21)) &&
            !actor->GetExtension()->IsRemote() && !actor->GetExtension()->IsPlayer())
            actors.push_back(actor);
    }
    return actors;
}
bool HasSnapshot(Actor* apActor)
{
    std::lock_guard lock(s_mutex);
    const auto it = s_snapshots.find(apActor->formID);
    return it != s_snapshots.end() && it->second.ActorPtr == apActor &&
        GetTickCount64() - it->second.ReceivedAt < 2000;
}
void GetAwareness(Actor* apTarget, int32_t& aLevel, uint32_t& aLOSCount)
{
    aLevel = -1000;
    aLOSCount = 0;
    using GetState = const uint8_t*(Actor*, Actor*, uint32_t);
    POINTER_SKYRIMSE(GetState, getState, 37758);
    for (auto* actor : Observers())
    {
        // Match the native player's exclusions for teammates and commanded allies.
        if (actor == apTarget || (actor->flags1 & (1u << 26)) || (actor->flags2 & (1u << 26)))
            continue;
        if (auto* commander = actor->GetCommandingActor(); commander && commander->GetExtension()->IsPlayer())
            continue;
        const auto* state = getState(actor, apTarget, 3);
        if (!state)
            continue;
        aLevel = (std::max)(aLevel, *reinterpret_cast<const int32_t*>(state + 0x10));
        if (state[0x15])
            ++aLOSCount;
    }
}
void SetMeter(int32_t aLevel, uint32_t)
{
    s_meterLevel.store(aLevel, std::memory_order_relaxed);
    s_meterTime.store(GetTickCount64(), std::memory_order_release);
}
uint8_t GetMeterLevel(Actor* apTarget, int32_t aDetectionLevel)
{
    if (aDetectionLevel > 0)
        return 100;
    // PlayerCharacter's HUD update (40593) uses 100 - combat search stealth points.
    // Query by target, since a streamed player need not run its own CombatController.
    using StealthPoints = float(void*, Actor*);
    POINTER_SKYRIMSE(void*, manager, 405246);
    POINTER_SKYRIMSE(StealthPoints, stealthPoints, 46877);
    if (!*manager)
        return 0;
    return static_cast<uint8_t>(std::clamp(100 - static_cast<int32_t>(stealthPoints(*manager, apTarget)), 0, 100));
}
}
