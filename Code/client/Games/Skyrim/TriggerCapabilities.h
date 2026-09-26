#pragma once

#include <cstdint>
#include <initializer_list>
#include <string_view>

// Declaration-only policy. No live Papyrus property values are read. Unknown
// scripts (including subclasses of a known helper) must not inherit its exemption.
namespace TriggerCapabilities
{
enum class Bucket : uint8_t
{
    None,
    ActorWake,
    Activation,
    ActorEnable,
    Unknown,
    QuestOrScene,
    DoorOrMovement
};

constexpr bool Equal(std::string_view a, std::string_view b) noexcept
{
    if (a.size() != b.size())
        return false;
    for (size_t i = 0; i < a.size(); ++i)
    {
        const auto lower = [](char c) { return c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c; };
        if (lower(a[i]) != lower(b[i]))
            return false;
    }
    return true;
}

constexpr Bucket Merge(Bucket a, Bucket b) noexcept { return a > b ? a : b; }
constexpr bool Hold(Bucket a) noexcept { return a >= Bucket::Unknown; }
constexpr const char* Name(Bucket a) noexcept
{
    switch (a)
    {
    case Bucket::None: return "no-script";
    case Bucket::ActorWake: return "actor-wake";
    case Bucket::Activation: return "activation";
    case Bucket::ActorEnable: return "actor-enable";
    case Bucket::QuestOrScene: return "quest-or-scene";
    case Bucket::DoorOrMovement: return "door-or-movement";
    default: return "unknown";
    }
}

constexpr Bucket Declaration(std::string_view aName) noexcept
{
    // Audited generic helpers in scripts-all. Exact names, never substring
    // matches: an "ambush" script can also start a scene or close a door.
    for (const auto name : {"defaultForceEvaluatePackageTrigger", "DEFAULTlinkRefStartCombatPlayer",
             "defaultSetLinkAVVar", "defaultSetMultiAVTriggerScript", "NorSarcophagusTopAnim01SCRIPT"})
        if (Equal(aName, name))
            return Bucket::ActorWake;
    for (const auto name : {"defaultActivateSelf", "defaultActivateLinkDoOnceSCRIPT",
             "defaultActivateActivateLinkedRefOnce"})
        if (Equal(aName, name))
            return Bucket::Activation;
    for (const auto name : {"defaultEnableEncLinkedRef", "defaultEnableDisableLinkedRef",
             "defaultPlayerEnableDisableLinkedRef"})
        if (Equal(aName, name))
            return Bucket::ActorEnable;
    for (const auto name : {"defaultActivateOpenLinkedRef", "defaultBlockActivation",
             "defaultOnActivateBlockActivation", "defaultOnActivateBlockPlayerActivate"})
        if (Equal(aName, name))
            return Bucket::DoorOrMovement;
    return Bucket::Unknown;
}

constexpr Bucket PropertyType(std::string_view aName) noexcept
{
    if (Equal(aName, "Quest") || Equal(aName, "Scene") || Equal(aName, "ReferenceAlias"))
        return Bucket::QuestOrScene;
    return Bucket::None;
}
}
