// Skyrim Together glue for the vendored Auto Audio Output Switch (GPL-3.0-or-later).
#include "PCH.h"

#include "AudioSwitch.h"
#include "Settings.h"
#include "utils/Logger.h"

#include <Games/Skyrim/Audio/AudioDeviceSelection.h>

namespace REL
{
void safe_write(std::uintptr_t aAddress, const void* apData, std::size_t aSize) noexcept
{
    DWORD oldProtect{};
    auto* pTarget = reinterpret_cast<void*>(aAddress);
    if (!VirtualProtect(pTarget, aSize, PAGE_EXECUTE_READWRITE, &oldProtect))
        return;
    std::memcpy(pTarget, apData, aSize);
    VirtualProtect(pTarget, aSize, oldProtect, &oldProtect);
    FlushInstructionCache(GetCurrentProcess(), pTarget, aSize);
}
} // namespace REL

namespace SKSE
{
std::uintptr_t Trampoline::WriteCall5(std::uintptr_t aSite, std::uintptr_t aTarget) noexcept
{
    const auto* pSite = reinterpret_cast<const std::uint8_t*>(aSite);
    if (pSite[0] != 0xE8)
    {
        logger::error("call hook at {:X} skipped: not a rel32 call", aSite);
        return 0;
    }
    std::int32_t oldRel{};
    std::memcpy(&oldRel, pSite + 1, sizeof(oldRel));
    const std::uintptr_t previous = aSite + 5 + static_cast<std::intptr_t>(oldRel);

    const auto displacement = static_cast<std::intptr_t>(aTarget) - static_cast<std::intptr_t>(aSite + 5);
    if (displacement < INT32_MIN || displacement > INT32_MAX)
    {
        // The client is linked into the launcher image right above the game, so
        // this is not expected; refuse rather than write a truncated call.
        logger::error("call hook at {:X} skipped: target {:X} is out of rel32 range", aSite, aTarget);
        return 0;
    }
    const auto rel = static_cast<std::int32_t>(displacement);
    REL::safe_write(aSite + 1, &rel, sizeof(rel));
    return previous;
}
} // namespace SKSE

namespace RE
{
BSAudioManager* BSAudioManager::GetSingleton() noexcept
{
    // CommonLibSSE-NG RELOCATION_ID(66391, 67652); returns the manager global.
    using TGetSingleton = BSAudioManager* (*)();
    static VersionDbPtr<void> s_getSingleton(67652);
    auto* pFunction = reinterpret_cast<TGetSingleton>(s_getSingleton.GetPtr());
    return pFunction ? pFunction() : nullptr;
}
} // namespace RE

namespace settings
{
std::string GetPreferredDevice()
{
    return AudioDeviceSelection::GetPreferredDevice();
}
} // namespace settings

static TiltedPhoques::Initializer s_audioSwitch(
    []()
    {
        // Before the game creates its XAudio2 engine, as the upstream plugin
        // does from SKSEPluginLoad.
        audioswitch::Install();
        audioswitch::StartWatcher();
    });
