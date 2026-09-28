#include <TiltedOnlinePCH.h>
#include <WorldAnimation.h>
#include <Services/WorldStateService.h>
#include <Games/References.h>
#include <BSAnimationGraphManager.h>
#include <Misc/BSFixedString.h>
#include <Havok/hkbBehaviorGraph.h>
#include <map>

namespace
{
using namespace WorldAnimationData;

// These virtual slots are AnimationStreamSaveGame/LoadGame's interfaces, read
// from their 1.7.104 vtables. We retain typed values instead of BGS save buffers.
struct SaveStream
{
    virtual ~SaveStream() = default;
    virtual bool TransformValue(void* p) { return Write(Transform, p); }
    virtual bool QuaternionValue(void* p) { return Write(Quaternion, p); }
    virtual bool VectorValue(void* p) { return Write(Vector4, p); }
    virtual bool Vector3Value(void* p) { return Write(Vector3, p); }
    virtual bool FloatValue(void* p) { return Write(Float, p); }
    virtual bool BoolValue(void* p) { return Write(Bool, p); }
    virtual bool ShortValue(void* p) { return Write(Int16, p); }
    virtual bool UShortValue(void* p) { return Write(UInt16, p); }
    virtual bool IntValue(void* p) { return Write(Int32, p); }
    virtual bool UIntValue(void* p) { return Write(UInt32, p); }
    virtual bool StringValue(const char** p)
    {
        const char* value = p && *p ? *p : "";
        const auto size = strnlen(value, 1025);
        if (size > 1024 || Data.size() + size + 3 > MaximumBytes) return Good = false;
        const uint16_t length = static_cast<uint16_t>(size);
        if (!Good) return false;
        Data.push_back(WorldAnimationData::String);
        Append(&length, 2); Append(value, size);
        return true;
    }
    std::vector<uint8_t> Data;
    bool Good{true};
private:
    void Append(const void* p, size_t size)
    {
        const auto* bytes = static_cast<const uint8_t*>(p);
        Data.insert(Data.end(), bytes, bytes + size);
    }
    bool Write(Type type, void* p)
    {
        if (!Good || Data.size() + Sizes[type] + 1 > MaximumBytes) return Good = false;
        Data.push_back(type); Append(p, Sizes[type]);
        return true;
    }
};
struct LoadStream
{
    explicit LoadStream(const std::vector<uint8_t>& data) : Input{data} {}
    virtual ~LoadStream() = default;
    virtual uint8_t Version() { return 42; } // selects N63602's modern (>31) grammar
    virtual bool TransformValue(void* p) { return Read(Transform, p); }
    virtual bool QuaternionValue(void* p) { return Read(Quaternion, p); }
    virtual bool VectorValue(void* p) { return Read(Vector4, p); }
    virtual bool Vector3Value(void* p) { return Read(Vector3, p); }
    virtual bool FloatValue(void* p) { return Read(Float, p); }
    virtual bool BoolValue(void* p) { return Read(Bool, p); }
    virtual bool ShortValue(void* p) { return Read(Int16, p); }
    virtual bool UShortValue(void* p) { return Read(UInt16, p); }
    virtual bool IntValue(void* p) { return Read(Int32, p); }
    virtual bool UIntValue(void* p) { return Read(UInt32, p); }
    virtual bool StringValue(BSFixedString* p) { return Text(p); }
    virtual bool StringValue2(BSFixedString* p) { return Text(p); }
    Reader Input;
private:
    bool Read(Type type, void* p) { return Input.Read(type, p, Sizes[type]); }
    bool Text(BSFixedString* p)
    {
        std::string value;
        if (!Input.Text(value)) return false;
        p->Set(value.c_str());
        return true;
    }
};

struct Manager
{
    explicit Manager(TESObjectREFR* ref) { ref->animationGraphHolder.GetBSAnimationGraph(&Value); }
    ~Manager() { if (Value) Value->Release(); }
    BSAnimationGraphManager* Value{};
};
bool Bounded(BSAnimationGraphManager* manager)
{
    if (!manager || !manager->animationGraphs.size || manager->animationGraphs.size > 16) return false;
    unsigned nodes{};
    for (uint32_t i = 0; i < manager->animationGraphs.size; ++i)
    {
        auto* graph = manager->animationGraphs.Get(i);
        if (!graph || !graph->behaviorGraph || !graph->behaviorGraph->struct98) return false;
        const auto count = graph->behaviorGraph->struct98->count;
        if (count < 0 || count > 1024) return false;
        nodes += count;
    }
    return nodes <= 1024;
}

template<class T> T Field(const void* object, size_t offset)
{
    T value;
    std::memcpy(&value, static_cast<const uint8_t*>(object) + offset, sizeof(value));
    return value;
}
bool CompatibleVariables(BSAnimationGraphManager* manager, const std::vector<uint8_t>& bytes)
{
    // N63635 uses the project DB name->index table, NOT the order of names in
    // an asset. N63602 trusts the incoming word/quad tag when writing that index.
    std::vector<std::map<std::string, bool>> schemas(manager->animationGraphs.size);
    unsigned slots{};
    for (uint32_t g = 0; g < manager->animationGraphs.size; ++g)
    {
        const auto* graph = manager->animationGraphs.Get(g);
        const auto* behavior = graph->behaviorGraph;
        const auto* data = Field<const void*>(behavior, 0x88);
        if (!data || !graph->hkxDB) return false;
        const auto count = Field<int32_t>(data, 0x28);
        const auto* infos = Field<const uint8_t*>(data, 0x20);
        const auto* initial = Field<const void*>(data, 0x70);
        const auto* values = behavior->animationVariables;
        if (count < 0 || count > 1024 || (count && (!infos || !initial || !values))) return false;
        // The loader resets defaults before reading any variables. Check both
        // arrays, including every vector index, before that first mutation.
        for (const void* set : {initial, static_cast<const void*>(values)})
        {
            if (!set) continue;
            const auto words = Field<int32_t>(set, 0x18);
            const auto quads = Field<int32_t>(set, 0x28);
            const auto* wordData = Field<const int32_t*>(set, 0x10);
            if (words != count || quads < 0 || quads > 1024 || (words && !wordData) ||
                (quads && !Field<const void*>(set, 0x20))) return false;
            for (int32_t i = 0; i < count; ++i)
            {
                const auto type = Field<int8_t>(infos + i * 6, 4);
                if (type < 0 || type > 8 || (type > 5 && (wordData[i] < 0 || wordData[i] >= quads))) return false;
            }
        }
        const auto capacity = Field<uint32_t>(graph->hkxDB, 0x7C);
        const auto* entries = Field<const uint8_t*>(graph->hkxDB, 0x98);
        if (capacity > 4096 || slots + capacity > 4096 || (capacity && !entries)) return false;
        slots += capacity;
        for (uint32_t i = 0; i < capacity; ++i)
        {
            const auto* entry = entries + i * 24;
            if (!Field<const void*>(entry, 16)) continue;
            const auto index = Field<int32_t>(entry, 8);
            const auto* name = Field<const char*>(entry, 0);
            if (index < 0 || index >= count || !name || strnlen(name, 1025) > 1024) return false;
            const auto type = Field<int8_t>(infos + index * 6, 4);
            if (type == 5) continue; // pointer values are never transported
            if (!schemas[g].emplace(name, type < 5).second) return false;
        }
    }
    return WorldAnimationData::Validate(bytes, [&](uint32_t graph, const std::string& name, bool word) {
        if (graph >= schemas.size()) return false;
        const auto found = schemas[graph].find(name);
        return found != schemas[graph].end() && found->second == word;
    });
}
TESObjectREFR* Owner(BShkbAnimationGraph* graph)
{
    // N63555 constructor assigns its TESObjectREFR argument at +210. Actor is
    // the SDK's misleading field type; furniture uses this same constructor.
    auto* ref = graph ? Field<TESObjectREFR*>(graph, 0x210) : nullptr;
    return ref && static_cast<uint8_t>(ref->formType) == 61 && ref->formID < 0xFF000000 ? ref : nullptr;
}

struct Dispatch { Dispatch* Previous{}; uint32_t Observed{}; };
thread_local Dispatch* s_dispatch{};
thread_local uint32_t s_updating{};
thread_local bool s_changed{};
thread_local float s_gamebryoDelta{};
using SendManagerFn = int(BSAnimationGraphManager*, BSFixedString*);
using SendGraphFn = bool(BShkbAnimationGraph*, BSFixedString*);
using UpdateGraphFn = void(BShkbAnimationGraph*, void*, void*);
using UpdateNodeFn = void(void*, void*, float);
using ConstructGraphFn = BShkbAnimationGraph*(BShkbAnimationGraph*, TESObjectREFR*, bool);
using SequenceTimeFn = bool(void*, void*, float*);
SendManagerFn* s_sendManager{};
SendGraphFn* s_sendGraph{};
UpdateGraphFn* s_updateGraph{};
UpdateNodeFn* s_updateState{};
UpdateNodeFn* s_updateClip{};
UpdateNodeFn* s_updateGamebryo{};
ConstructGraphFn* s_constructGraph{};
SequenceTimeFn* s_sequenceTime{};

int SendManager(BSAnimationGraphManager* manager, BSFixedString* name)
{
    Dispatch scope{s_dispatch};
    s_dispatch = &scope;
    const auto result = s_sendManager(manager, name);
    s_dispatch = scope.Previous;
    return result;
}
bool SendGraph(BShkbAnimationGraph* graph, BSFixedString* name)
{
    auto* ref = Owner(graph);
    const auto id = ref ? ref->formID : 0;
    const auto accepted = s_sendGraph(graph, name);
    if (accepted && name && id)
        if (!s_dispatch || s_dispatch->Observed != id)
        {
            if (s_dispatch) s_dispatch->Observed = id;
            // This call is inside the native manager lock. It copies only the
            // reference ID and string, preserving native dispatch order.
            WorldStateService::AnimationEvent(id, name->AsAscii());
        }
    return accepted;
}
void UpdateGraph(BShkbAnimationGraph* graph, void* data, void* events)
{
    const auto previous = s_updating;
    const auto changed = s_changed;
    auto* ref = Owner(graph);
    s_updating = ref ? ref->formID : 0;
    s_changed = false;
    s_updateGraph(graph, data, events);
    if (s_updating && s_changed) WorldStateService::AnimationDirty(s_updating);
    s_updating = previous; s_changed = changed;
}
void UpdateState(void* node, void* context, float delta)
{
    if (!s_updating) { s_updateState(node, context, delta); return; }
    // N59335 clears +100 after propagating its change bit. Compare state and
    // transition count as well, so automatic/zero-duration transitions dirty it.
    const bool changed = Field<uint8_t>(node, 0x100) != 0;
    const auto state = Field<int32_t>(node, 0x80);
    const auto transitions = Field<uint32_t>(node, 0xB8);
    s_updateState(node, context, delta);
    s_changed |= changed || state != Field<int32_t>(node, 0x80) || transitions != Field<uint32_t>(node, 0xB8);
}
void UpdateClip(void* node, void* context, float delta)
{
    if (!s_updating) { s_updateClip(node, context, delta); return; }
    const auto atEnd = Field<uint8_t>(node, 0x104);
    s_updateClip(node, context, delta);
    s_changed |= atEnd != Field<uint8_t>(node, 0x104);
}
BShkbAnimationGraph* ConstructGraph(BShkbAnimationGraph* graph, TESObjectREFR* ref, bool flag)
{
    const auto id = ref && static_cast<uint8_t>(ref->formType) == 61 ? ref->formID : 0;
    auto* result = s_constructGraph(graph, ref, flag);
    if (result && id) WorldStateService::AnimationKnown(id);
    return result;
}
void UpdateGamebryo(void* node, void* context, float delta)
{
    const auto previous = s_gamebryoDelta;
    s_gamebryoDelta = delta;
    s_updateGamebryo(node, context, delta);
    s_gamebryoDelta = previous;
}
bool SequenceTime(void* callback, void* node, float* duration)
{
    const auto result = s_sequenceTime(callback, node, duration);
    // N62932 increments fTime before N32773 returns the sequence duration.
    // Detect the one-shot completion crossing, not every subsequent update.
    if (result && s_updating && s_gamebryoDelta > 0 && std::isfinite(*duration) && *duration > 0 && !Field<uint8_t>(node, 0x6D))
    {
        const auto time = Field<float>(node, 0x68);
        s_changed |= time >= *duration && time - s_gamebryoDelta < *duration;
    }
    return result;
}
static TiltedPhoques::Initializer s_hooks([] {
    POINTER_SKYRIMSE(SendManagerFn, sendManager, 63362);
    POINTER_SKYRIMSE(SendGraphFn, sendGraph, 63591);
    POINTER_SKYRIMSE(UpdateGraphFn, updateGraph, 63587);
    POINTER_SKYRIMSE(UpdateNodeFn, updateState, 59335);
    POINTER_SKYRIMSE(UpdateNodeFn, updateClip, 59253);
    POINTER_SKYRIMSE(UpdateNodeFn, updateGamebryo, 62932);
    POINTER_SKYRIMSE(ConstructGraphFn, constructGraph, 63555);
    POINTER_SKYRIMSE(SequenceTimeFn, sequenceTime, 32773);
    s_sendManager = sendManager.Get(); TP_HOOK(&s_sendManager, SendManager);
    s_sendGraph = sendGraph.Get(); TP_HOOK(&s_sendGraph, SendGraph);
    s_updateGraph = updateGraph.Get(); TP_HOOK(&s_updateGraph, UpdateGraph);
    s_updateState = updateState.Get(); TP_HOOK(&s_updateState, UpdateState);
    s_updateClip = updateClip.Get(); TP_HOOK(&s_updateClip, UpdateClip);
    s_updateGamebryo = updateGamebryo.Get(); TP_HOOK(&s_updateGamebryo, UpdateGamebryo);
    s_constructGraph = constructGraph.Get(); TP_HOOK(&s_constructGraph, ConstructGraph);
    s_sequenceTime = sequenceTime.Get(); TP_HOOK(&s_sequenceTime, SequenceTime);
});
}

bool WorldAnimation::Eligible(TESObjectREFR* ref) noexcept
{
    // Enable-parent ownership applies to enable/disable writes, not to a child's
    // own animation graph. Actor graphs keep their existing replication path.
    return ref && static_cast<uint8_t>(ref->formType) == 61 && ref->baseForm && ref->formID &&
        ref->formID < 0xFF000000 && !ref->IsTemporary() && !ref->IsDeleted();
}
bool WorldAnimation::Capture(TESObjectREFR* ref, std::vector<uint8_t>& bytes) noexcept
{
    if (!Eligible(ref) || !ref->GetNiNode()) return false;
    Manager manager(ref);
    if (!manager.Value) return false;
    BSScopedLock<BSRecursiveLock> lock(manager.Value->lock);
    if (!Bounded(manager.Value)) return false;
    using SaveFn = bool(BSAnimationGraphManager*, SaveStream*, bool);
    POINTER_SKYRIMSE(SaveFn, save, 63368);
    SaveStream stream;
    // true includes clip progress: N65148 otherwise omits clip times.
    if (!save.Get()(manager.Value, &stream, true) || !stream.Good || !WorldAnimationData::Valid(stream.Data)) return false;
    bytes = std::move(stream.Data);
    return true;
}
bool WorldAnimation::Apply(TESObjectREFR* ref, const WorldState& state) noexcept
{
    if (!Eligible(ref) || !ref->GetNiNode() || ref->IsDisabled()) return false;
    if (state.Kind == WorldStateKind::AnimationEvent)
    {
        BSFixedString event(state.Animation.c_str());
        const auto accepted = ref->SendAnimationEvent(&event);
        event.Release();
        return accepted;
    }
    if (state.Kind != WorldStateKind::AnimationSnapshot || !WorldAnimationData::Valid(state.AnimationData)) return false;
    Manager manager(ref);
    if (!manager.Value) return false;
    BSScopedLock<BSRecursiveLock> lock(manager.Value->lock);
    if (!Bounded(manager.Value)) return false;
    Reader header{state.AnimationData};
    if (header.Count() != manager.Value->animationGraphs.size) return false;
    if (!CompatibleVariables(manager.Value, state.AnimationData)) return false;
    // N63369 -> N63602 restores states/clip positions and generates a zero-delta
    // pose via N63587, including the final frame. No event-history fast-forward.
    using LoadFn = bool(BSAnimationGraphManager*, LoadStream*);
    POINTER_SKYRIMSE(LoadFn, load, 63369);
    LoadStream stream(state.AnimationData);
    const bool result = load.Get()(manager.Value, &stream);
    return result && stream.Input.Good && stream.Input.Position == state.AnimationData.size();
}
