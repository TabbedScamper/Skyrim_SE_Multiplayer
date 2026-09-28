#include <TiltedOnlinePCH.h>
#include <ArmorAttachmentTrace.h>
#include <BipedArmorRepair.h>
#include <NetImmerse/NiAVObject.h>

#include <atomic>
#include <intrin.h>

namespace
{
// Opt-in, read-only observation. Trace sampling only copies bounded POD data.
// LoadParts/CompletePart also host the always-on production armor repair/proof;
// use one detour per native target, since MinHook does not chain duplicate hooks.
// Signatures and call sites: REFERENCE_RESEARCH.md, heads round 2.
using TSetRoot = void(void*, NiAVObject*);
using TLoadParts = void(void*, float, uint8_t);
using TCompletePart = void(void*, uintptr_t, NiAVObject*, int32_t);
using TAttachArmor = NiAVObject*(void*, NiAVObject*, NiAVObject*, int32_t, uint8_t, uint8_t, NiAVObject*);
using TRemovePart = void(void*, void*, uint8_t, int32_t);
using TDetachPart = void(const uint32_t*, NiAVObject*);
TSetRoot* s_setRoot{};
TLoadParts* s_loadParts{};
TCompletePart* s_completePart{};
TAttachArmor* s_attachArmor{};
TRemovePart* s_removePart{};
TDetachPart* s_detachPart{};
bool s_enabled{}; // Set once before hooks are installed.
constexpr uint32_t cSlotCount = 42;
constexpr uint32_t cCapacity = 1024;
constexpr uint32_t cDrainLimit = 4;

bool WatchSlot(int32_t aRequested, uint32_t aIndex) noexcept
{
    // All native per-slot events are retained. Whole-biped boundaries only need
    // the four clothing slots absent in the paired captures, bounding parent reads.
    return aRequested >= 0 || aIndex == 1 || aIndex == 2 || aIndex == 3 || aIndex == 7;
}

// Native layout, copied in one read rather than dozens of scalar reads.
struct PartView
{
    uintptr_t Item, Addon, Model, Texture, Clone;
    uint8_t Unused28[0x40];
    uint8_t Skinned;
    uint8_t Unused69[0xF];
};
struct BipedView
{
    uint64_t RefCount;
    uintptr_t Root;
    PartView Current[cSlotCount];
    PartView Buffered[cSlotCount];
    uint32_t OwnerHandle, Padding;
};
static_assert(sizeof(PartView) == 0x78 && offsetof(PartView, Skinned) == 0x68);
static_assert(offsetof(BipedView, Buffered) == 0x13C0 && offsetof(BipedView, OwnerHandle) == 0x2770);
static_assert(sizeof(BipedView) == 0x2778);

template <class T> bool ReadAt(uintptr_t aBase, size_t aOffset, T& arValue) noexcept
{
    SIZE_T copied{};
    return aBase && ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(aBase + aOffset),
        &arValue, sizeof(T), &copied) && copied == sizeof(T);
}

struct PartSample
{
    uintptr_t Item{}, Model{}, Clone{}, Parent{};
    uint8_t Skinned{};
    bool ParentReadable{};
};
struct SlotSample
{
    PartSample Current, Buffered;
};
struct BipedSample
{
    uintptr_t Root{}, RootParent{};
    uint32_t OwnerHandle{};
    bool Readable{}, RootParentReadable{}, RootUnchanged{};
    SlotSample Slots[cSlotCount];
};
enum class Event : uint8_t { SetRoot, LoadParts, CompletePart, AttachArmor, RemovePart, DetachPart };
struct Record
{
    // 0 free, 1 writer owns the record, 2 published for the game thread.
    std::atomic<uint32_t> State{};
    uint64_t Sequence{}, BeginTicks{}, EndTicks{};
    uintptr_t Biped{}, Caller{}, Input{}, Destination{}, Result{}, ResultParent{};
    uintptr_t InputParent{};
    uint32_t DetachOwner{};
    uint32_t Thread{};
    int32_t Slot{};
    Event Kind{};
    bool ResultParentReadable{};
    bool InputParentReadable{}, DetachOwnerReadable{};
    BipedSample Before, After;
};
// Fixed storage; no pointers in a record are dereferenced after publication.
Record s_records[cCapacity];
std::atomic<uint64_t> s_ticket{};
std::atomic<uint32_t> s_dropped{};
static_assert(std::atomic<uint64_t>::is_always_lock_free);
static_assert(std::atomic<uint32_t>::is_always_lock_free);

PartSample SamplePart(const PartView& aPart) noexcept
{
    PartSample result;
    result.Item = aPart.Item;
    result.Model = aPart.Model;
    result.Clone = aPart.Clone;
    result.Skinned = aPart.Skinned;
    result.ParentReadable = ReadAt(result.Clone, 0x30, result.Parent);
    return result;
}

void SampleBiped(uintptr_t aBiped, int32_t aSlot, BipedSample& aSample) noexcept
{
    // A bulk read is not an atomic scene snapshot. RootUnchanged only detects
    // a root switch during sampling; it cannot certify coherent child links.
    BipedView view;
    aSample.Readable = ReadAt(aBiped, 0, view);
    aSample.Root = aSample.RootParent = 0;
    aSample.OwnerHandle = 0;
    aSample.RootParentReadable = aSample.RootUnchanged = false;
    if (!aSample.Readable)
        return;
    aSample.Root = view.Root;
    aSample.OwnerHandle = view.OwnerHandle;
    aSample.RootParentReadable = ReadAt(view.Root, 0x30, aSample.RootParent);
    const uint32_t first = aSlot < 0 ? 0 : static_cast<uint32_t>(aSlot);
    const uint32_t end = aSlot < 0 ? cSlotCount : first + 1;
    for (uint32_t i = first; i < end; ++i)
    {
        if (!WatchSlot(aSlot, i))
            continue;
        aSample.Slots[i].Current = SamplePart(view.Current[i]);
        aSample.Slots[i].Buffered = SamplePart(view.Buffered[i]);
    }
    uintptr_t rootAgain{};
    aSample.RootUnchanged = ReadAt(aBiped, 8, rootAgain) && rootAgain == view.Root;
}

Record* Begin(Event aKind, void* apBiped, int32_t aSlot, uintptr_t aCaller,
    uintptr_t aInput = 0, uintptr_t aDestination = 0) noexcept
{
    if (!s_enabled)
        return nullptr;
    // One attempt, never wait on another loader or on the game thread.
    const auto ticket = s_ticket.fetch_add(1, std::memory_order_relaxed);
    auto& record = s_records[ticket % cCapacity];
    uint32_t expected = 0;
    if (!record.State.compare_exchange_strong(expected, 1, std::memory_order_acquire))
    {
        s_dropped.fetch_add(1, std::memory_order_relaxed);
        return nullptr;
    }
    record.Sequence = ticket + 1;
    record.Kind = aKind;
    record.Biped = reinterpret_cast<uintptr_t>(apBiped);
    record.Caller = aCaller;
    record.Slot = aSlot;
    record.Input = aInput;
    record.Destination = aDestination;
    record.Result = record.ResultParent = 0;
    record.ResultParentReadable = false;
    record.InputParent = 0;
    record.DetachOwner = 0;
    record.InputParentReadable = record.DetachOwnerReadable = false;
    record.Thread = GetCurrentThreadId();
    LARGE_INTEGER ticks;
    QueryPerformanceCounter(&ticks);
    record.BeginTicks = ticks.QuadPart;
    SampleBiped(record.Biped, aSlot, record.Before);
    return &record;
}

void End(Record* apRecord, NiAVObject* apResult = nullptr) noexcept
{
    if (!apRecord)
        return;
    apRecord->Result = reinterpret_cast<uintptr_t>(apResult);
    apRecord->ResultParentReadable = ReadAt(apRecord->Result, 0x30, apRecord->ResultParent);
    SampleBiped(apRecord->Biped, apRecord->Slot, apRecord->After);
    LARGE_INTEGER ticks;
    QueryPerformanceCounter(&ticks);
    apRecord->EndTicks = ticks.QuadPart;
    apRecord->State.store(2, std::memory_order_release);
}

void HookSetRoot(void* apBiped, NiAVObject* apRoot)
{
    auto* record = Begin(Event::SetRoot, apBiped, -1, reinterpret_cast<uintptr_t>(_ReturnAddress()),
        reinterpret_cast<uintptr_t>(apRoot));
    s_setRoot(apBiped, apRoot);
    End(record);
}

void HookLoadParts(void* apBiped, float aWeight, uint8_t aFlags)
{
    auto* record = Begin(Event::LoadParts, apBiped, -1, reinterpret_cast<uintptr_t>(_ReturnAddress()), aFlags);
    const auto rebuiltSlots = PrepareBipedArmorLoad(apBiped);
    s_loadParts(apBiped, aWeight, aFlags);
    End(record);
    TraceSpawnedCopyArmor(apBiped, "LoadParts", rebuiltSlots);
}

void HookCompletePart(void* apBiped, uintptr_t aModel, NiAVObject* apArmor, int32_t aSlot)
{
    auto* record = aSlot >= 0 && aSlot < cSlotCount ?
        Begin(Event::CompletePart, apBiped, aSlot, reinterpret_cast<uintptr_t>(_ReturnAddress()),
            aModel, reinterpret_cast<uintptr_t>(apArmor)) : nullptr;
    s_completePart(apBiped, aModel, apArmor, aSlot);
    End(record);
    // Async completion can happen after LoadParts' proof. Neither depends on
    // received worn snapshots, remote assignment, or opt-in trace recording.
    if (aSlot == 2)
        TraceSpawnedCopyArmor(apBiped, "CompletePart");
}

NiAVObject* HookAttachArmor(void* apBiped, NiAVObject* apArmor, NiAVObject* apDestination,
    int32_t aSlot, uint8_t aFirstPerson, uint8_t aUnknown, NiAVObject* apModel)
{
    auto* record = aSlot >= 0 && aSlot < cSlotCount ?
        Begin(Event::AttachArmor, apBiped, aSlot, reinterpret_cast<uintptr_t>(_ReturnAddress()),
            reinterpret_cast<uintptr_t>(apArmor), reinterpret_cast<uintptr_t>(apDestination)) : nullptr;
    auto* result = s_attachArmor(apBiped, apArmor, apDestination, aSlot, aFirstPerson, aUnknown, apModel);
    End(record, result);
    return result;
}

void HookRemovePart(void* apBiped, void* apPart, uint8_t aClear, int32_t aReplacement)
{
    // Both current and buffered BIPOBJECT entries use this native removal path.
    // Observe only entries containing a clone; empty reset slots add no evidence.
    const auto offset = reinterpret_cast<uintptr_t>(apPart) - reinterpret_cast<uintptr_t>(apBiped);
    const bool isEntry = offset >= 0x10 && offset < 0x2770 && (offset - 0x10) % 0x78 == 0;
    uintptr_t clone{};
    auto* record = isEntry && ReadAt(reinterpret_cast<uintptr_t>(apPart), 0x20, clone) && clone ?
        Begin(Event::RemovePart, apBiped, static_cast<int32_t>((offset - 0x10) / 0x78 % cSlotCount),
            reinterpret_cast<uintptr_t>(_ReturnAddress()), reinterpret_cast<uintptr_t>(apPart), aClear) : nullptr;
    s_removePart(apBiped, apPart, aClear, aReplacement);
    End(record);
}

void HookDetachPart(const uint32_t* apOwner, NiAVObject* apClone)
{
    // 15661 can enqueue removal; 15660 executes the eventual parent detach.
    // This boundary sees both immediate and queued calls without resolving handles.
    auto* record = Begin(Event::DetachPart, nullptr, -1, reinterpret_cast<uintptr_t>(_ReturnAddress()),
        reinterpret_cast<uintptr_t>(apClone));
    if (record)
    {
        record->DetachOwnerReadable = ReadAt(reinterpret_cast<uintptr_t>(apOwner), 0, record->DetachOwner);
        record->InputParentReadable = ReadAt(record->Input, 0x30, record->InputParent);
    }
    s_detachPart(apOwner, apClone);
    End(record, apClone);
}

void LogSample(const Record& aRecord, const char* apPhase, const BipedSample& aSample)
{
    spdlog::info("ArmorTrace seq={} phase={} biped={:X} ownerHandle={:X} readable={} root={:X} "
        "rootParent={:X} rootParentReadable={} rootUnchanged={} nonAtomic=1",
        aRecord.Sequence, apPhase, aRecord.Biped, aSample.OwnerHandle, aSample.Readable, aSample.Root,
        aSample.RootParent, aSample.RootParentReadable, aSample.RootUnchanged);
    if (!aSample.Readable)
        return;
    const uint32_t first = aRecord.Slot < 0 ? 0 : static_cast<uint32_t>(aRecord.Slot);
    const uint32_t end = aRecord.Slot < 0 ? cSlotCount : first + 1;
    for (uint32_t i = first; i < end; ++i)
    {
        if (!WatchSlot(aRecord.Slot, i))
            continue;
        const auto& slot = aSample.Slots[i];
        if (aRecord.Slot < 0 && !slot.Current.Item && !slot.Current.Clone && !slot.Buffered.Item && !slot.Buffered.Clone)
            continue;
        spdlog::info("ArmorTrace seq={} phase={} slot={} item={:X} model={:X} clone={:X} parent={:X} "
            "parentReadable={} skinned={} bufferedItem={:X} bufferedModel={:X} bufferedClone={:X} "
            "bufferedParent={:X} bufferedParentReadable={} bufferedSkinned={}",
            aRecord.Sequence, apPhase, i, slot.Current.Item, slot.Current.Model, slot.Current.Clone,
            slot.Current.Parent, slot.Current.ParentReadable, slot.Current.Skinned,
            slot.Buffered.Item, slot.Buffered.Model, slot.Buffered.Clone, slot.Buffered.Parent,
            slot.Buffered.ParentReadable, slot.Buffered.Skinned);
    }
}
}

void DrainArmorAttachmentTrace() noexcept
{
    if (!s_enabled)
        return;
    static uint32_t cursor{};
    uint32_t drained{};
    const auto start = GetTickCount64();
    for (uint32_t scanned = 0; scanned < cCapacity && drained < cDrainLimit; ++scanned)
    {
        auto& record = s_records[cursor++ % cCapacity];
        if (record.State.load(std::memory_order_acquire) != 2)
            continue;
        constexpr const char* names[] = {"SetRoot", "LoadParts", "CompletePart", "AttachArmor", "RemovePart", "DetachPart"};
        spdlog::info("ArmorTrace seq={} event={} tid={} caller={:X} biped={:X} slot={} input={:X} destination={:X} "
            "beginTicks={} endTicks={} result={:X} resultParent={:X} resultParentReadable={} "
            "inputParent={:X} inputParentReadable={} detachOwner={:X} detachOwnerReadable={}",
            record.Sequence, names[static_cast<size_t>(record.Kind)], record.Thread, record.Caller, record.Biped,
            record.Slot, record.Input, record.Destination, record.BeginTicks, record.EndTicks,
            record.Result, record.ResultParent, record.ResultParentReadable,
            record.InputParent, record.InputParentReadable, record.DetachOwner, record.DetachOwnerReadable);
        LogSample(record, "before", record.Before);
        LogSample(record, "after", record.After);
        record.State.store(0, std::memory_order_release);
        ++drained;
        // This is a soft budget; an individual logger call can still block.
        if (GetTickCount64() - start >= 1)
            break;
    }
    const auto dropped = s_dropped.exchange(0, std::memory_order_relaxed);
    if (dropped)
        spdlog::warn("ArmorTrace gap dropped={}; history incomplete, do not infer absence of a native call", dropped);
}

static TiltedPhoques::Initializer s_armorAttachmentTrace([]()
{
    char enabled[2]{};
    bool requested = GetEnvironmentVariableA("SEAMLESS_ARMOR_TRACE", enabled, sizeof(enabled)) == 1 && enabled[0] == '1';
#ifdef SEAMLESS_HARNESS
    wchar_t gamePath[MAX_PATH]{};
    if (GetModuleFileNameW(nullptr, gamePath, MAX_PATH))
        requested = requested || std::filesystem::exists(std::filesystem::path(gamePath).parent_path() /
            "Data" / "SkyrimTogetherReborn" / "harness-armor.enabled");
#endif
    POINTER_SKYRIMSE(TSetRoot, setRoot, 15657);
    POINTER_SKYRIMSE(TLoadParts, loadParts, 15678);
    POINTER_SKYRIMSE(TCompletePart, completePart, 15701);
    POINTER_SKYRIMSE(TAttachArmor, attachArmor, 15712);
    POINTER_SKYRIMSE(TRemovePart, removePart, 15661);
    POINTER_SKYRIMSE(TDetachPart, detachPart, 15660);
    s_setRoot = setRoot.Get();
    s_loadParts = loadParts.Get();
    s_completePart = completePart.Get();
    s_attachArmor = attachArmor.Get();
    s_removePart = removePart.Get();
    s_detachPart = detachPart.Get();
    s_enabled = requested;
    TP_HOOK(&s_loadParts, HookLoadParts);
    TP_HOOK(&s_completePart, HookCompletePart);
    spdlog::info("Biped armor repair enabled version=2: guarded native stale-cache rebuild and spawned-copy proof");
    if (!requested)
        return;
    TP_HOOK(&s_setRoot, HookSetRoot);
    TP_HOOK(&s_attachArmor, HookAttachArmor);
    TP_HOOK(&s_removePart, HookRemovePart);
    TP_HOOK(&s_detachPart, HookDetachPart);
    LARGE_INTEGER frequency;
    QueryPerformanceFrequency(&frequency);
    spdlog::info("ArmorTrace enabled version=2 capacity={} drainPerTick={} bytes={} gameBase={:X} qpcFrequency={} "
        "perSlot=all rootLoadSlots=1,2,3,7; read-only, non-atomic samples, game-thread drain",
        cCapacity, cDrainLimit, sizeof(s_records), reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr)), frequency.QuadPart);
});
