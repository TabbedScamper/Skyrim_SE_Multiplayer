#include <TiltedOnlinePCH.h>

#include <Games/Skyrim/LipSyncHooks.h>
#include <Games/Skyrim/Actor.h>
#include <Games/ActorExtension.h>
#include <Games/Misc/MenuTopicManager.h>
#include <DefaultObjectManager.h>
#include <Events/ActorRemovedEvent.h>
#include <Events/DisconnectedEvent.h>
#include <Events/PartyLeftEvent.h>
#include <Events/PreUpdateEvent.h>
#include <World.h>

#include <array>
#include <atomic>
#include <memory>
#include <unordered_map>
#include <chrono>


namespace
{
// Count added work, excluding the chained native face-node update.
struct LipCost
{
    std::chrono::steady_clock::time_point Started{HostFrameCost::Begin()};
    bool Running{true};
    void Stop()
    {
        if (Running)
        {
            HostFrameCost::End(3, Started);
            Running = false;
        }
    }
    ~LipCost() { Stop(); }
};
// Research, SkyrimSE 1.7.104 (kept here because this task's edit allowlist
// excludes docs/REFERENCE_RESEARCH.md):
// CommonLibSSE-NG, inspected BSFaceGenAnimationData.h, BSFaceGenNiNode.h,
// BSFaceGenKeyframeMultiple.h, BSSoundInfo.h and src/RE/B/BSSoundHandle.cpp:
// https://github.com/alandtse/CommonLibSSE-NG/tree/a898f469851c464d05137bb74b069dd234897643
// Adopt native 16-phoneme morphs and the sound playback clock. These are not
// Havok pose bones, so PoseCopyAuthority needs no face-bone exclusion.
// EngineFixes src/fixes/lip_sync.h, inspected at dc5a5c0:
// https://github.com/alandtse/EngineFixesSkyrim64/blob/dc5a5c0adacbcace8cf8479c1eff9cb5714e1593/src/fixes/lip_sync.h
// Its modifier interpolation patch (16267) does not arbitrate voice ownership;
// neither that patch nor amplitude-generated phonemes solve a wrong .lip file.
// Native functions read in the corpus, with ABI checked in the executable:
// 37542 / 14068D490: SpeakSound attaches AIProcess's cached dialogue resource
// to face+228, then resets the independent modifier/phoneme clocks. Volume
// does not gate these clocks. Replaying through it also replaces process sound
// slot 0 (39400 / 1406F6BD0), corrupting the supposedly preserved scene handle.
// 16254 / 140242A60: async .lip/.fuz request; 16318 / 140246FE0: release.
// 26598 / 140424AE0: face update; 26998 / 14043A4E0: face-node morph scheduling.
// 16266 / 140243790: sample the 16 phonemes; 67639 / 140CC9C80 calls
// 67702 / 140CCC950, reading sound-info+18 under the audio read lock.
// The replay owns its resource and audio handle. Only final speech phonemes
// are replaced; native dialogue, expressions, blink and scene waits still run.

struct SoundHandle
{
    uint32_t Id{0xFFFFFFFFu};
    uint32_t AssumeSuccess{};
    uint32_t State{};
};
static_assert(sizeof(SoundHandle) == 0xC);

struct LipData
{
    uint32_t FrameCount;
    int32_t Offset;
};

struct LipResource
{
    uint8_t Pad00[0xC];
    volatile LONG StateAndReferences;
    uint8_t Pad10[0x18];
    LipData* Data;
};
static_assert(offsetof(LipResource, Data) == 0x28);

struct Keyframe
{
    void* Vtable;
    uint32_t Type;
    float Time;
    float* Values;
    uint32_t Count;
    bool Updated;
    uint8_t Pad1D[3];
};
static_assert(sizeof(Keyframe) == 0x20);

// Narrow views, verified against both native accesses and CommonLib layouts.
struct FaceData
{
    uint8_t Pad00[0x140];
    Keyframe Phonemes;
    uint8_t Pad160[0x217 - 0x160];
    bool Changed;
    bool Update;
    uint8_t Pad219[7];
    BSRecursiveLock Lock;
};
static_assert(offsetof(FaceData, Lock) == 0x220);

struct FaceNode
{
    uint8_t Pad00[0x150];
    FaceData* Animation;
    float LastTime;
    uint32_t ActorHandle;
    uint16_t Flags;
};
static_assert(offsetof(FaceNode, ActorHandle) == 0x15C);
static_assert(offsetof(FaceNode, Flags) == 0x160);

using TSoundBool = bool(SoundHandle*);
using TSoundTime = uint32_t(SoundHandle*);
using TReleaseLip = void(LipResource*);
using TSampleLip = bool(LipData*, float, float*);
using TFaceUpdate = bool(FaceData*, float, bool);
using TFaceNodeUpdate = void(FaceNode*, void*);
TFaceUpdate* RealFaceUpdate{};
TFaceNodeUpdate* RealFaceNodeUpdate{};

POINTER_SKYRIMSE(TSoundBool, s_isPlaying, 67620);
POINTER_SKYRIMSE(TSoundBool, s_isValid, 67621);
POINTER_SKYRIMSE(TSoundBool, s_stop, 67619);
POINTER_SKYRIMSE(TSoundBool, s_releaseSound, 67643);
POINTER_SKYRIMSE(TSoundTime, s_position, 67639);
POINTER_SKYRIMSE(TReleaseLip, s_releaseLip, 16318);
POINTER_SKYRIMSE(TSampleLip, s_sampleLip, 16266);

uint32_t ResourceState(LipResource* apResource)
{
    return apResource ? (InterlockedCompareExchange(&apResource->StateAndReferences, 0, 0) & 0x70000000u) : 0;
}

LipData* ReadyLip(LipResource* apResource)
{
    const auto state = ResourceState(apResource);
    return state == 0x30000000u || state == 0x40000000u ? apResource->Data : nullptr;
}

bool PendingLip(LipResource* apResource)
{
    const auto state = ResourceState(apResource);
    return state == 0x10000000u || state == 0x20000000u || state == 0x70000000u;
}

struct ReplayLine
{
    uint32_t FormId{};
    uint32_t ActorHandle{};
    std::string File;
    LipResource* Resource{};
    SoundHandle Sound;
    bool Started{};
    bool WasValid{};
    bool SampleLogged{};
    uint64_t RequestedAt{GetTickCount64()};
    uint64_t StartedAt{};
    float LeadIn{};

    ~ReplayLine()
    {
        if (Sound.Id != 0xFFFFFFFFu)
        {
            s_stop.Get()(&Sound);
            // Match AIProcess's stop/release pair at 1406F6C10.
            s_releaseSound.Get()(&Sound);
        }
        s_releaseLip.Get()(Resource);
    }
};

// A native handle lookup owns a reference until our render/cleanup call ends.
// TESObjectREFR::GetByHandle drops that reference before returning.
struct ActorReference
{
    TESObjectREFR* Reference{};
    explicit ActorReference(uint32_t aHandle)
    {
        using TLookup = void(uint32_t&, TESObjectREFR*&);
        POINTER_SKYRIMSE(TLookup, lookup, 17201);
        lookup.Get()(aHandle, Reference);
    }
    ~ActorReference()
    {
        if (Reference)
            Reference->handleRefObject.DecRefHandle();
    }
    Actor* Get() const { return Cast<Actor>(Reference); }
};

void StartAudio(ReplayLine& aLine, Actor* apActor)
{
    // The audio half of 37542, without its AIProcess/face/idle mutations.
    using TNormalize = const char*(char*, uint32_t, const char*, const char*);
    using TMakeId = void(void*, const char*);
    using TGetAudio = void*();
    using TGetSound = void(void*, SoundHandle*, const void*, uint32_t, uint32_t, const char*);
    using TIsFuz = bool(LipResource**);
    using TOutput = void(SoundHandle*, const void*);
    using TFollow = void(SoundHandle*, void*);
    using THeadNode = void*(void*);
    using TRegisterVoice = void(Actor*, SoundHandle*);
    using TPlayDelayed = bool(SoundHandle*, uint32_t);
    using TLipDelay = float();
    POINTER_SKYRIMSE(TNormalize, normalize, 69822);
    POINTER_SKYRIMSE(TMakeId, makeId, 69971);
    POINTER_SKYRIMSE(TGetAudio, audio, 67652);
    POINTER_SKYRIMSE(TGetSound, sound, 67664);
    POINTER_SKYRIMSE(TIsFuz, isFuz, 16257);
    POINTER_SKYRIMSE(TOutput, output, 67624);
    POINTER_SKYRIMSE(TFollow, follow, 67636);
    POINTER_SKYRIMSE(THeadNode, head, 39975);
    POINTER_SKYRIMSE(TRegisterVoice, registerVoice, 35124);
    POINTER_SKYRIMSE(TPlayDelayed, play, 67617);
    POINTER_SKYRIMSE(TLipDelay, lipDelay, 26565);

    char normalized[0x104]{};
    alignas(8) uint8_t resourceId[0x10]{};
    makeId.Get()(resourceId, normalize.Get()(normalized, sizeof(normalized), aLine.File.c_str(), "sound\\"));
    sound.Get()(audio.Get()(), &aLine.Sound, resourceId,
        isFuz.Get()(&aLine.Resource) ? 0x8090 : 0x90, 0x80, aLine.File.c_str());

    // Default NPC dialogue output (not a form-specific assumption), exactly
    // the manager+4B8/+C53 branch in 37542. Preserve vanilla voice categories.
    auto* pDefaults = reinterpret_cast<uint8_t*>(&DefaultObjectManager::Get());
    auto* pOutput = pDefaults[0xC53] ? *reinterpret_cast<uint8_t**>(pDefaults + 0x4B8) : nullptr;
    output.Get()(&aLine.Sound, pOutput ? pOutput + 0x20 : nullptr);
    registerVoice.Get()(apActor, &aLine.Sound);
    void* pNode = apActor->currentProcess ? head.Get()(apActor->currentProcess) : nullptr;
    follow.Get()(&aLine.Sound, pNode ? pNode : apActor->GetNiNode());

    auto* pLip = ReadyLip(aLine.Resource);
    aLine.LeadIn = pLip ? lipDelay.Get()() + (pLip->Offset < 0 ? -static_cast<float>(pLip->Offset) / 30.f : 0.f) : 0.f;
    play.Get()(&aLine.Sound, pLip ? static_cast<uint32_t>((std::max)(0.f, aLine.LeadIn) * 1000.f) : 1u);
    aLine.Started = true;
    aLine.StartedAt = GetTickCount64();
    spdlog::info("Network lip replay actor {:X} soundId={} lip={} frames={} waitMs={} file={}",
        aLine.FormId, aLine.Sound.Id, pLip != nullptr, pLip ? pLip->FrameCount : 0,
        aLine.StartedAt - aLine.RequestedAt, aLine.File);
}

struct ReplayStore
{
    std::mutex Lock;
    std::unordered_map<uint32_t, std::unique_ptr<ReplayLine>> Lines;

    ReplayStore()
    {
        // Constructed by Actor::SpeakSound on the presentation/update thread.
        // Subscribe to other events, never modify the active UpdateEvent sink.
        auto& dispatcher = World::Get().GetDispatcher();
        dispatcher.sink<PreUpdateEvent>().connect<&ReplayStore::OnUpdate>(this);
        dispatcher.sink<DisconnectedEvent>().connect<&ReplayStore::OnDisconnected>(this);
        dispatcher.sink<PartyLeftEvent>().connect<&ReplayStore::OnPartyLeft>(this);
        dispatcher.sink<ActorRemovedEvent>().connect<&ReplayStore::OnRemoved>(this);
    }

    void Clear()
    {
        std::lock_guard lock(Lock);
        Lines.clear();
    }
    void OnDisconnected(const DisconnectedEvent&) { Clear(); }
    void OnPartyLeft(const PartyLeftEvent&) { Clear(); }
    void OnRemoved(const ActorRemovedEvent& aEvent)
    {
        std::lock_guard lock(Lock);
        Lines.erase(aEvent.FormId);
    }
    void OnUpdate(const PreUpdateEvent&)
    {
        LipCost cost;
        std::lock_guard lock(Lock);
        const auto now = GetTickCount64();
        for (auto it = Lines.begin(); it != Lines.end();)
        {
            auto& line = *it->second;
            ActorReference reference(line.ActorHandle);
            auto* pActor = reference.Get();
            bool remove = !pActor || pActor->formID != line.FormId ||
                !LipSyncHooks::IsRemoteSpeaker(pActor) || !pActor->GetNiNode();
            if (!remove && !line.Started && !PendingLip(line.Resource))
                StartAudio(line, pActor);
            if (!remove && line.Started)
            {
                const bool valid = s_isValid.Get()(&line.Sound);
                // Queued and paused sounds are not finished sounds. In
                // particular, IsPlaying(false) must not cancel a menu pause.
                remove = !valid && (line.WasValid || now - line.StartedAt > 10000);
                line.WasValid |= valid;
            }
            if (!line.Started && now - line.RequestedAt > 10000)
            {
                spdlog::warn("Network lip replay load timed out actor {:X} file={}", line.FormId, line.File);
                remove = true;
            }
            if (remove)
                it = Lines.erase(it);
            else
                ++it;
        }
    }
};

// Process-lifetime observer: disconnect/unload explicitly releases resources.
// Do not call engine audio/resource destructors during CRT shutdown.
std::atomic<ReplayStore*> s_store{};

struct FaceContext
{
    FaceData* Data{};
    std::array<float, 16> Phonemes{};
};
thread_local const FaceContext* s_faceContext{};

bool ApplyPhonemes(FaceData* apData, const FaceContext& acContext)
{
    auto& final = apData->Phonemes;
    bool changed = false;
    if (final.Values)
    {
        for (uint32_t i = 0; i < final.Count; ++i)
        {
            const float value = i < acContext.Phonemes.size() ? acContext.Phonemes[i] : 0.f;
            changed |= final.Values[i] != value;
            final.Values[i] = value;
        }
        if (changed)
        {
            final.Updated = false;
            apData->Changed = true;
        }
    }
    return changed;
}

bool HookFaceUpdate(FaceData* apData, float aDelta, bool aUpdate)
{
    if (!s_faceContext || s_faceContext->Data != apData)
        return RealFaceUpdate(apData, aDelta, aUpdate);

    BSScopedLock<BSRecursiveLock> lock(apData->Lock);
    const bool nativeChanged = RealFaceUpdate(apData, aDelta, aUpdate);
    LipCost cost;
    const bool changed = ApplyPhonemes(apData, *s_faceContext);
    return nativeChanged || changed;
}

void HookFaceNodeUpdate(FaceNode* apNode, void* apUpdate)
{
    if (!apNode)
        return;
    if (!apNode->Animation || !World::Get().GetTransport().IsConnected() ||
        !World::Get().GetPartyService().IsInParty())
        return RealFaceNodeUpdate(apNode, apUpdate);
    LipCost cost;
    ActorReference reference(apNode->ActorHandle);
    auto* pActor = reference.Get();
    if (!LipSyncHooks::IsRemoteSpeaker(pActor))
    {
        cost.Stop();
        RealFaceNodeUpdate(apNode, apUpdate);
        return;
    }

    FaceContext context{apNode->Animation};
    if (auto* pStore = s_store.load(std::memory_order_acquire))
    {
        std::lock_guard lock(pStore->Lock);
        const auto it = pStore->Lines.find(pActor->formID);
        if (it != pStore->Lines.end() && it->second->ActorHandle == apNode->ActorHandle)
        {
            auto& line = *it->second;
            auto* pLip = ReadyLip(line.Resource);
            if (line.Started && pLip && pLip->FrameCount && s_isPlaying.Get()(&line.Sound))
            {
                const auto position = s_position.Get()(&line.Sound);
                s_sampleLip.Get()(pLip, position * 0.001f + line.LeadIn, context.Phonemes.data());
                if (!line.SampleLogged)
                {
                    line.SampleLogged = true;
                    spdlog::info("Network lip sample actor {:X} soundId={} playbackMs={} file={}",
                        line.FormId, line.Sound.Id, position, line.File);
                }
            }
        }
    }

    // The node schedules morph application based on FaceUpdate's return.
    // Override before that decision, including frames with no native speech.
    // Bit 2 selects a full face update versus reuse of the cached result.
    // Preserve it: forcing a full update on a second pass advances blink and
    // expression clocks twice. The cached path only needs its final phonemes.
    const auto* previous = s_faceContext;
    s_faceContext = &context;
    {
        BSScopedLock<BSRecursiveLock> lock(apNode->Animation->Lock);
        if ((apNode->Flags & 4) != 0 || ApplyPhonemes(apNode->Animation, context))
            apNode->Animation->Update = true;
    }
    cost.Stop();
    RealFaceNodeUpdate(apNode, apUpdate);
    s_faceContext = previous;
}
}

namespace LipSyncHooks
{
bool IsRemoteSpeaker(Actor* apActor) noexcept
{
    auto& world = World::Get();
    const auto* extension = apActor ? apActor->GetExtension() : nullptr;
    return extension && world.GetTransport().IsConnected() && world.GetPartyService().IsInParty() &&
        extension->IsRemote() && !extension->IsPlayer() &&
        !MenuTopicManager::IsPlayerDialogueSpeaker(apActor);
}

void Replay(Actor* apActor, const char* apFile)
{
    if (!apFile || !*apFile || !apActor->GetNiNode())
        return;

    static auto* pStore = new ReplayStore;
    s_store.store(pStore, std::memory_order_release);
    auto line = std::make_unique<ReplayLine>();
    line->FormId = apActor->formID;
    line->ActorHandle = apActor->GetHandle().handle.iBits;
    line->File = apFile;
    using TRequestLip = void(const char*, LipResource**);
    POINTER_SKYRIMSE(TRequestLip, requestLip, 16254);
    requestLip.Get()(apFile, &line->Resource);

    std::lock_guard lock(pStore->Lock);
    // Replace only the previous presentation line, never the silent native
    // scene handle. A new owner line is an interruption of the old replay.
    pStore->Lines.erase(line->FormId);
    if (!PendingLip(line->Resource))
        StartAudio(*line, apActor);
    pStore->Lines.emplace(line->FormId, std::move(line));
}
}

static TiltedPhoques::Initializer s_lipSyncHooks([]()
{
    POINTER_SKYRIMSE(TFaceUpdate, update, 26598);
    POINTER_SKYRIMSE(TFaceNodeUpdate, updateNode, 26998);
    RealFaceUpdate = update.Get();
    RealFaceNodeUpdate = updateNode.Get();
    // Resolve shared relocations before face workers use their lazy caches.
    s_isPlaying.Get();
    s_isValid.Get();
    s_stop.Get();
    s_releaseSound.Get();
    s_position.Get();
    s_releaseLip.Get();
    s_sampleLip.Get();
    TP_HOOK(&RealFaceUpdate, HookFaceUpdate);
    TP_HOOK(&RealFaceNodeUpdate, HookFaceNodeUpdate);
});
