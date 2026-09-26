#include <TiltedOnlinePCH.h>

#include <Games/Skyrim/Audio/AudioPreview.h>

#include <BSGraphics/BSGraphicsRenderer.h>
#include <Forms/TESForm.h>

#include <random>
#include <vector>

namespace
{
using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;

// CommonLibSSE-NG BSSoundHandle (0xC).
struct SoundHandle
{
    uint32_t SoundId{0xFFFFFFFF};
    bool AssumeSuccess{false};
    uint8_t Pad05{0};
    uint16_t Pad06{0};
    uint32_t State{0};
};
static_assert(sizeof(SoundHandle) == 0xC);

// BGSSoundCategory form IDs (Skyrim.esm SNCT records).
constexpr uint32_t kSfx = 0x000172A1;        // AudioCategorySFX (Effects slider)
constexpr uint32_t kFootsteps = 0x000F5FFC;  // AudioCategoryFST (child of SFX)
constexpr uint32_t kVoice = 0x000876BD;      // AudioCategoryVOCGeneral
constexpr uint32_t kMusic = 0x00071E64;      // AudioCategoryMUS
constexpr uint32_t kAmbience = 0x0007F80B;   // AudioCategoryAMB
constexpr uint32_t kAmbienceR = 0x00071E63;  // AudioCategoryAMBr
constexpr uint32_t kMagic = 0x0001A0BD;      // AudioCategoryMAG
constexpr uint32_t kInterface = 0x00064451;  // AudioCategoryUI
constexpr uint32_t kKillMove = 0x000EA51F;   // AudioCategoryNPCKillMove

// Categories silenced so only the adjusted channel is heard. A parent of the
// previewed category is never muted (volumes multiply down the tree).
std::vector<uint32_t> MutedFor(const std::string& acChannel)
{
    if (acChannel == "effects")
        return {kFootsteps, kVoice, kMusic, kAmbience, kAmbienceR, kInterface};
    if (acChannel == "footsteps")
        return {kVoice, kMusic, kAmbience, kAmbienceR, kMagic, kInterface, kKillMove};
    if (acChannel == "voice")
        return {kSfx, kMusic};
    if (acChannel == "music")
        return {kSfx, kVoice};
    return {}; // master: everything plays
}

// Sound descriptor pools (Skyrim.esm SNDR EDIDs in the matching category).
const std::vector<const char*> kEffects = {
    "WPNSwing2Hand", "WPNImpactBlade2HandVsMetalSkin", "WPNImpactBladeVsMetalSkin", "WPNSwingBlunt1Hand",
    "WPNImpactBlunt2HandVsWood", "WPNImpactBladeVsIceSD", "WPNImpactArrowVsShieldHeavySD"};
// Walking surfaces: {left, right}.
const std::vector<std::pair<const char*, const char*>> kSurfaces = {
    {"FSTPlayerStoneSolidWalkLSD", "FSTPlayerStoneSolidWalkRSD"}, {"FSTPlayerDirtWalkLSD", "FSTPlayerDirtWalkRSD"},
    {"FSTPlayerGrassWalkLSD", "FSTPlayerGrassWalkRSD"}, {"FSTPlayerWoodWalkLSD", "FSTPlayerWoodWalkRSD"},
    {"FSTPlayerGravelWalkLSD", "FSTPlayerGravelWalkRSD"}, {"FSTPlayerSnowWalkLSD", "FSTPlayerSnowWalkRSD"}};
// Real dialogue from Skyrim - Voices_en0.bsa, spread over voice types.
const std::vector<const char*> kVoiceLines = {
    "Data\\Sound\\Voice\\Skyrim.esm\\maleguard\\dialoguecr_dgcrimepayfine_00020fa9_2.wav",
    "Data\\Sound\\Voice\\Skyrim.esm\\maleguard\\dialoguewh_dialoguewhiteru_000d1981_1.wav",
    "Data\\Sound\\Voice\\Skyrim.esm\\malenord\\dialoguewh__000c02dd_1.wav",
    "Data\\Sound\\Voice\\Skyrim.esm\\malenord\\dialoguewh__000c06de_3.wav",
    "Data\\Sound\\Voice\\Skyrim.esm\\femalenord\\dialoguewh_dialoguewhiteru_00093132_4.wav",
    "Data\\Sound\\Voice\\Skyrim.esm\\femalenord\\dialogueiv_dialogueivarste_000daaf3_1.wav",
    "Data\\Sound\\Voice\\Skyrim.esm\\maleeventoned\\dialoguegeneric__0006d93b_1.wav",
    "Data\\Sound\\Voice\\Skyrim.esm\\maleeventoned\\dialoguewh__0007ebdf_1.wav",
    "Data\\Sound\\Voice\\Skyrim.esm\\femaleeventoned\\dialoguegeneric__0006d949_1.wav",
    "Data\\Sound\\Voice\\Skyrim.esm\\femaleeventoned\\dialoguege_dialoguegeneric_0006ae39_1.wav",
    "Data\\Sound\\Voice\\Skyrim.esm\\malecommoner\\dialoguegeneric__0006d948_1.wav",
    "Data\\Sound\\Voice\\Skyrim.esm\\malecommoner\\dialogueso_dialoguesoljund_0006a886_1.wav",
    "Data\\Sound\\Voice\\Skyrim.esm\\femalecommoner\\dialoguesolitude__000bfaf7_1.wav",
    "Data\\Sound\\Voice\\Skyrim.esm\\femalecommoner\\dialoguemorthal__00042e3a_1.wav"};

// ---- playing music track (BGSMusicSingleTrack, vtable ID 191021) ----
// Handles at +0x68 and +0x74 (FUN_140326690 creates them, FUN_140326750
// fades/pauses both). Palette tracks play single tracks, so this covers them.
constexpr size_t kTrackHandleA = 0x68;
constexpr size_t kTrackHandleB = 0x74;
using TTrackFn = void (*)(void*);
TTrackFn s_realTrackPlay = nullptr;
TTrackFn s_realTrackStop = nullptr;
void* s_currentTrack = nullptr;
float s_musicLevelAtTrackStart = 1.f;
bool s_musicMutedForPreview = false;

float MusicUserLevel() noexcept
{
    // BGSSoundCategory user volume (BSISoundCategory +0x20 = form +0x50).
    auto* pForm = TESForm::GetById(0x00071E64); // AudioCategoryMUS
    return pForm ? *reinterpret_cast<const float*>(reinterpret_cast<uint8_t*>(pForm) + 0x50) : 1.f;
}

void HookTrackPlay(void* apTrack)
{
    s_realTrackPlay(apTrack);
    s_currentTrack = apTrack;
    s_musicLevelAtTrackStart = MusicUserLevel();
    AudioPreview::SyncMusicVolume();
}

void HookTrackStop(void* apTrack)
{
    s_realTrackStop(apTrack);
    if (apTrack == s_currentTrack)
        s_currentTrack = nullptr;
}

struct State
{
    std::string Channel;
    Clock::time_point Deadline{};
    Clock::time_point NextSound{};
    SoundHandle Handle{};
    std::vector<std::pair<uint32_t, float>> Muted; // category, previous volume
    size_t Surface = 0;
    int Step = 0;
    bool Active = false;
} s_state;

std::mt19937& Rng()
{
    static std::mt19937 s_rng(std::random_device{}());
    return s_rng;
}

template <class T> const T& Pick(const std::vector<T>& acPool)
{
    return acPool[std::uniform_int_distribution<size_t>(0, acPool.size() - 1)(Rng())];
}

// BGSSoundCategory's BSISoundCategory interface at +0x30: slot 2 Get, slot 3 Set.
void* CategoryInterface(uint32_t aFormId) noexcept
{
    auto* pForm = TESForm::GetById(aFormId);
    return pForm ? reinterpret_cast<uint8_t*>(pForm) + 0x30 : nullptr;
}

void SetCategoryVolume(uint32_t aFormId, float aValue) noexcept
{
    AudioPreview::SetCategoryVolumeVanilla(aFormId, aValue);
}

void RestoreMuted() noexcept
{
    const bool any = !s_state.Muted.empty();
    for (const auto& [formId, volume] : s_state.Muted)
        SetCategoryVolume(formId, volume);
    s_state.Muted.clear();
    (void)any;
    s_musicMutedForPreview = false;
    AudioPreview::SyncMusicVolume();
}

void MuteOthers(const std::string& acChannel) noexcept
{
    RestoreMuted();
    s_musicMutedForPreview = acChannel != "music" && acChannel != "master";
    AudioPreview::SyncMusicVolume();
    for (const auto formId : MutedFor(acChannel))
    {
        if (auto* pInterface = CategoryInterface(formId))
        {
            // The user-set level (form+0x50), not the effective product with parents.
            s_state.Muted.emplace_back(formId, *reinterpret_cast<const float*>(static_cast<uint8_t*>(pInterface) + 0x20));
            SetCategoryVolume(formId, 0.f);
        }
    }
}

void StopSound() noexcept
{
    using TFadeOut = bool (*)(SoundHandle*, uint16_t);
    static VersionDbPtr<void> s_fadeOut(67646); // BSSoundHandle::FadeOutAndRelease
    if (s_state.Handle.SoundId != 0xFFFFFFFF)
        reinterpret_cast<TFadeOut>(s_fadeOut.GetPtr())(&s_state.Handle, 120);
    s_state.Handle = {};
}

// 2D output models (Skyrim.esm SOPM records without an attenuation block):
// previews play "in the head" like UI sounds, independent of any camera or
// listener position (the title screen has no gameplay camera).
constexpr uint32_t kOutputStereo = 0x0007EDCA;     // SOMStereo

void PlayDescriptor(const char* apEditorId, uint32_t aOutputModel = kOutputStereo) noexcept
{
    using TGetSingleton = void* (*)();
    using TGetByName = void (*)(void*, SoundHandle&, const char*, uint32_t);
    using TSetOutputModel = void (*)(SoundHandle*, const void*);
    using TPlay = bool (*)(SoundHandle*);
    static VersionDbPtr<void> s_getSingleton(67652); // BSAudioManager::GetSingleton
    static VersionDbPtr<void> s_getByName(67665);    // BSAudioManager::GetSoundHandleByName
    static VersionDbPtr<void> s_setOutputModel(67624); // BSSoundHandle::SetOutputModel
    static VersionDbPtr<void> s_play(67616);         // BSSoundHandle::Play

    auto* pManager = reinterpret_cast<TGetSingleton>(s_getSingleton.GetPtr())();
    if (!pManager)
        return;
    SoundHandle handle{};
    // 0x10: the flags CommonLib-based plugins pass for UI-style playback.
    reinterpret_cast<TGetByName>(s_getByName.GetPtr())(pManager, handle, apEditorId, 0x10);
    if (handle.SoundId == 0xFFFFFFFF)
    {
        spdlog::warn("Audio preview: sound descriptor {} not found", apEditorId);
        return;
    }
    // BGSSoundOutput's BSISoundOutputModel interface is at +0x20 (CommonLibSSE-NG).
    if (auto* pOutput = TESForm::GetById(aOutputModel))
        reinterpret_cast<TSetOutputModel>(s_setOutputModel.GetPtr())(&handle, reinterpret_cast<uint8_t*>(pOutput) + 0x20);
    reinterpret_cast<TPlay>(s_play.GetPtr())(&handle);
    s_state.Handle = handle; // one-shots overlap naturally; only the latest is faded on stop
}

// Plays a dialogue file the way Actor speech does (STR_Actor_speakSoundFunction):
// path normalised under "sound\\" (ID 69822), BSResource::ID from it (ID 69971),
// BSAudioManager::GetSoundHandleByFile (ID 67664) with flags 0x10 / priority 0x40.
// Dialogue's own category chain includes AudioCategoryPausedDuringMenu, so in
// any menu it would start paused; it is moved to PausedDuringMenuLoad (master
// only, not paused in menus) and its handle volume set to what the Voice
// slider gives in the world: VOCGeneral static 0.9 x user level.
void PlayVoiceFile(const char* apPath) noexcept
{
    using TNormalize = const char* (*)(char*, uint32_t, const char*, const char*);
    using TMakeId = void (*)(void*, const char*);
    using TGetSingleton = void* (*)();
    using TGetByFile = void (*)(void*, SoundHandle&, const void*, uint32_t, uint32_t, const char*);
    using TSetOutputModel = void (*)(SoundHandle*, const void*);
    using TSetCategory = void (*)(SoundHandle*, const void*, uint16_t);
    using TSetVolume = bool (*)(SoundHandle*, float);
    using TPlay = bool (*)(SoundHandle*);
    static VersionDbPtr<void> s_normalize(69822);
    static VersionDbPtr<void> s_makeId(69971);
    static VersionDbPtr<void> s_getSingleton(67652);
    static VersionDbPtr<void> s_getByFile(67664);
    static VersionDbPtr<void> s_setOutputModel(67624);
    static VersionDbPtr<void> s_setCategory(67623);
    static VersionDbPtr<void> s_setVolume(67626);
    static VersionDbPtr<void> s_play(67616);

    auto* pManager = reinterpret_cast<TGetSingleton>(s_getSingleton.GetPtr())();
    if (!pManager)
        return;
    char normalized[0x104]{};
    const char* pNormalized = reinterpret_cast<TNormalize>(s_normalize.GetPtr())(normalized, sizeof(normalized), apPath, "sound\\");
    alignas(8) uint8_t resourceId[0x10]{}; // BSResource::ID (file, ext, dir hashes)
    reinterpret_cast<TMakeId>(s_makeId.GetPtr())(resourceId, pNormalized ? pNormalized : normalized);

    SoundHandle handle{};
    reinterpret_cast<TGetByFile>(s_getByFile.GetPtr())(pManager, handle, resourceId, 0x10, 0x40, apPath);
    if (handle.SoundId == 0xFFFFFFFF)
    {
        spdlog::warn("Audio preview: voice file {} not found", apPath);
        return;
    }
    if (auto* pOutput = TESForm::GetById(0x000B5183)) // SOMDialogue2D
        reinterpret_cast<TSetOutputModel>(s_setOutputModel.GetPtr())(&handle, reinterpret_cast<uint8_t*>(pOutput) + 0x20);
    if (auto* pCategory = TESForm::GetById(0x0010AA60)) // AudioCategoryPausedDuringMenuLoad
        reinterpret_cast<TSetCategory>(s_setCategory.GetPtr())(&handle, reinterpret_cast<uint8_t*>(pCategory) + 0x30, 0);
    float voice = 1.f;
    if (auto* pVoice = TESForm::GetById(kVoice))
        voice = *reinterpret_cast<const float*>(reinterpret_cast<uint8_t*>(pVoice) + 0x50);
    reinterpret_cast<TSetVolume>(s_setVolume.GetPtr())(&handle, 0.9f * voice);
    reinterpret_cast<TPlay>(s_play.GetPtr())(&handle);
    spdlog::debug("Audio preview: voice line at {:.2f} ({})", 0.9f * voice, apPath);
    StopSound(); // one line at a time
    s_state.Handle = handle;
}

// Plays the next sample for the channel and returns the delay until the one after.
std::chrono::milliseconds PlayNext(const std::string& acChannel) noexcept
{
    std::string channel = acChannel;
    if (channel == "master")
    {
        static const std::vector<std::string> kMix = {"effects", "effects", "footsteps", "voice"};
        channel = Pick(kMix);
    }
    if (channel == "effects")
    {
        PlayDescriptor(Pick(kEffects));
        return std::chrono::milliseconds(std::uniform_int_distribution<int>(550, 900)(Rng()));
    }
    if (channel == "footsteps")
    {
        if (s_state.Step % 8 == 0)
            s_state.Surface = std::uniform_int_distribution<size_t>(0, kSurfaces.size() - 1)(Rng());
        const auto& surface = kSurfaces[s_state.Surface];
        PlayDescriptor(s_state.Step % 2 == 0 ? surface.first : surface.second);
        ++s_state.Step;
        return 480ms; // walking cadence
    }
    if (channel == "voice")
    {
        PlayVoiceFile(Pick(kVoiceLines));
        return std::chrono::milliseconds(std::uniform_int_distribution<int>(3600, 4600)(Rng()));
    }
    return 500ms; // music: nothing to play, the current track is the preview
}

HWND GameWindow() noexcept
{
    const auto* pWindow = BSGraphics::GetMainWindow();
    return pWindow ? pWindow->hWnd : nullptr;
}
} // namespace

namespace AudioPreview
{
void KeepAlive(const std::string& acChannel) noexcept
{
    const auto now = Clock::now();
    s_state.Deadline = now + 1500ms;
    if (!s_state.Active || s_state.Channel != acChannel)
    {
        if (s_state.Active)
            StopSound();
        s_state.Channel = acChannel;
        s_state.Active = true;
        s_state.Step = 0;
        s_state.NextSound = now;
        MuteOthers(acChannel);
        if (auto hWnd = GameWindow())
            SetTimer(hWnd, kTimerId, 40, nullptr);
        Tick();
    }
    else
    {
        // A slider change re-applies every category volume; keep the others silent
        // (their recorded volumes are what Stop() restores).
        for (const auto& [formId, volume] : s_state.Muted)
            SetCategoryVolume(formId, 0.f);
    }
}

void SetCategoryVolumeVanilla(uint32_t aCategoryFormId, float aValue) noexcept
{
    // Scaleform FxDelegate call layout used by the handler (see SetMasterVolume
    // in GameSettingsService): args[0] option id, args[1] value, as doubles.
    struct Arguments
    {
        uint8_t Padding0[0x10]{};
        double OptionId{};
        uint8_t Padding1[0x10]{};
        double Value{};
    } arguments;
    struct Callback
    {
        uint8_t Padding[0x28]{};
        void* Args{};
    } callback;
    arguments.OptionId = static_cast<double>(aCategoryFormId);
    arguments.Value = static_cast<double>(std::clamp(aValue, 0.f, 1.f));
    callback.Args = &arguments;
    static VersionDbPtr<void> s_optionChange(53310);
    reinterpret_cast<void (*)(void*)>(s_optionChange.GetPtr())(&callback);
}

void SyncMusicVolume() noexcept
{
    using TIsValid = bool (*)(const SoundHandle*);
    using TSetVolume = bool (*)(SoundHandle*, float);
    static VersionDbPtr<void> s_isValid(67621);   // BSSoundHandle::IsValid
    static VersionDbPtr<void> s_setVolume(67626); // BSSoundHandle::SetVolume

    // Title-screen theme: the Main Menu plays it itself (FUN_14095ea10, ID 52185)
    // into a global handle (ID 383021) with no sound category, so no category
    // slider reaches it - only master. Its level is the handle volume itself.
    static VersionDbPtr<SoundHandle> s_titleMusic(383021);
    if (auto* pTitle = s_titleMusic.Get(); pTitle && reinterpret_cast<TIsValid>(s_isValid.GetPtr())(pTitle))
        reinterpret_cast<TSetVolume>(s_setVolume.GetPtr())(pTitle, s_musicMutedForPreview ? 0.f : MusicUserLevel());

    if (!s_currentTrack)
        return;
    // The category already applied the start-of-track level; scale by the change since.
    const float start = std::max(0.01f, s_musicLevelAtTrackStart);
    const float gain = s_musicMutedForPreview ? 0.f : std::clamp(MusicUserLevel() / start, 0.f, 1.f / start);
    for (const size_t offset : {kTrackHandleA, kTrackHandleB})
    {
        auto* pHandle = reinterpret_cast<SoundHandle*>(static_cast<uint8_t*>(s_currentTrack) + offset);
        if (reinterpret_cast<TIsValid>(s_isValid.GetPtr())(pHandle))
            reinterpret_cast<TSetVolume>(s_setVolume.GetPtr())(pHandle, gain);
    }
}

void NotifyCategoryVolumesChanged() noexcept
{
    using TGetSingleton = void* (*)();
    using TCompose = void (*)(void*, uint32_t, uint64_t, uint64_t, uint64_t);
    static VersionDbPtr<void> s_getSingleton(67652); // BSAudioManager::GetSingleton
    static VersionDbPtr<void> s_compose(67716);      // BSAudioManager message wrapper
    if (auto* pManager = reinterpret_cast<TGetSingleton>(s_getSingleton.GetPtr())())
        reinterpret_cast<TCompose>(s_compose.GetPtr())(pManager, 0x10, 0, 0, 0);
}

void Stop() noexcept
{
    if (!s_state.Active)
        return;
    s_state.Active = false;
    if (auto hWnd = GameWindow())
        KillTimer(hWnd, kTimerId);
    StopSound();
    RestoreMuted();
}

void Tick() noexcept
{
    if (!s_state.Active)
        return;
    const auto now = Clock::now();
    if (now >= s_state.Deadline)
    {
        Stop();
        return;
    }
    if (now >= s_state.NextSound)
        s_state.NextSound = now + PlayNext(s_state.Channel);
}
} // namespace AudioPreview

// Main Menu music start (FUN_14095ea10, ID 52185): apply the saved Music
// level to the title theme as soon as it starts. Register arguments are
// forwarded untouched.
using TTitleMusic = void (*)(void*, void*, void*, void*);
static TTitleMusic RealTitleMusic = nullptr;
static void HookTitleMusic(void* a1, void* a2, void* a3, void* a4)
{
    RealTitleMusic(a1, a2, a3, a4);
    AudioPreview::SyncMusicVolume();
}

static TiltedPhoques::Initializer s_musicTrackHooks(
    []()
    {
        static VersionDbPtr<void> s_titleMusicStart(52185);
        RealTitleMusic = reinterpret_cast<TTitleMusic>(s_titleMusicStart.GetPtr());
        if (RealTitleMusic)
            TP_HOOK(&RealTitleMusic, HookTitleMusic);

        static VersionDbPtr<void*> s_vtable(191021); // BGSMusicSingleTrack vtable
        auto** pVtable = s_vtable.Get();
        if (!pVtable)
            return;
        DWORD oldProtect{};
        if (!VirtualProtect(pVtable + 2, 2 * sizeof(void*), PAGE_READWRITE, &oldProtect))
            return;
        s_realTrackPlay = reinterpret_cast<TTrackFn>(pVtable[2]); // DoPlay (FUN_140326690)
        s_realTrackStop = reinterpret_cast<TTrackFn>(pVtable[3]); // DoPause/Finish (FUN_140326750)
        pVtable[2] = reinterpret_cast<void*>(&HookTrackPlay);
        pVtable[3] = reinterpret_cast<void*>(&HookTrackStop);
        VirtualProtect(pVtable + 2, 2 * sizeof(void*), oldProtect, &oldProtect);
    });
