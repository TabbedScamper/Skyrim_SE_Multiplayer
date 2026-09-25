#include <TiltedOnlinePCH.h>

#include <Games/Skyrim/Audio/AudioPreview.h>

#include <Camera/PlayerCamera.h>
#include <NetImmerse/NiNode.h>

#include <unordered_map>

namespace
{
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

// Sound descriptors from Skyrim.esm, chosen per category (SNDR EDID -> SNCT):
//   WPNImpactBlade2HandVsMetalSkin   AudioCategorySFX
//   FSTPlayerBarefootStoneWalkL/RSD  AudioCategoryFST
//   NPCDraugrVoiceTaunt              AudioCategoryVOC (only SNDR under VOCGeneral)
const char* DescriptorFor(const std::string& acChannel, bool aAlternate) noexcept
{
    if (acChannel == "effects" || acChannel == "master")
        return "WPNImpactBlade2HandVsMetalSkin";
    if (acChannel == "footsteps")
        return aAlternate ? "FSTPlayerBarefootStoneWalkRSD" : "FSTPlayerBarefootStoneWalkLSD";
    if (acChannel == "voice")
        return "NPCDraugrVoiceTaunt";
    return nullptr;
}

// Voice lines are long; everything else is a one-shot.
std::chrono::milliseconds IntervalFor(const std::string& acChannel) noexcept
{
    using namespace std::chrono_literals;
    if (acChannel == "voice")
        return 2500ms;
    if (acChannel == "footsteps")
        return 280ms;
    return 450ms;
}

struct ChannelState
{
    std::chrono::steady_clock::time_point Next{};
    SoundHandle Handle{};
    bool Alternate{false};
};
std::unordered_map<std::string, ChannelState> s_channels;
} // namespace

namespace AudioPreview
{
void Play(const std::string& acChannel) noexcept
{
    auto& channel = s_channels[acChannel];
    const auto now = std::chrono::steady_clock::now();
    const char* pDescriptor = DescriptorFor(acChannel, channel.Alternate);
    if (!pDescriptor || now < channel.Next)
        return;
    channel.Next = now + IntervalFor(acChannel);
    channel.Alternate = !channel.Alternate;

    using TGetSingleton = void* (*)();
    using TGetByName = void (*)(void*, SoundHandle&, const char*, uint32_t);
    using TSetPosition = bool (*)(SoundHandle*, NiPoint3);
    using TPlay = bool (*)(SoundHandle*);
    using TFadeOut = bool (*)(SoundHandle*, uint16_t);
    static VersionDbPtr<void> s_getSingleton(67652); // BSAudioManager::GetSingleton
    static VersionDbPtr<void> s_getByName(67665);    // BSAudioManager::GetSoundHandleByName
    static VersionDbPtr<void> s_setPosition(67631);  // BSSoundHandle::SetPosition
    static VersionDbPtr<void> s_play(67616);         // BSSoundHandle::Play
    static VersionDbPtr<void> s_fadeOut(67646);      // BSSoundHandle::FadeOutAndRelease

    auto* pManager = reinterpret_cast<TGetSingleton>(s_getSingleton.GetPtr())();
    if (!pManager)
        return;

    // One sample per channel at a time.
    if (channel.Handle.SoundId != 0xFFFFFFFF)
        reinterpret_cast<TFadeOut>(s_fadeOut.GetPtr())(&channel.Handle, 80);
    channel.Handle = {};

    // 0x10: the flags CommonLib-based plugins pass for UI-style playback.
    reinterpret_cast<TGetByName>(s_getByName.GetPtr())(pManager, channel.Handle, pDescriptor, 0x10);
    if (channel.Handle.SoundId == 0xFFFFFFFF)
    {
        spdlog::warn("Audio preview: sound descriptor {} not found", pDescriptor);
        return;
    }

    // Place the sample at the listener so 3D attenuation never silences it
    // (there is no player on the title screen to follow).
    if (auto* pCamera = PlayerCamera::Get(); pCamera && pCamera->cameraNode)
        reinterpret_cast<TSetPosition>(s_setPosition.GetPtr())(&channel.Handle, pCamera->cameraNode->world.translate);
    reinterpret_cast<TPlay>(s_play.GetPtr())(&channel.Handle);
}
} // namespace AudioPreview
