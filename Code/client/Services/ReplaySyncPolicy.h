#pragma once
// Verbatim offline copies. Source drift is checked by run_replay_checks.py.
#include <cstdint>
#include <cstddef>
namespace ReplaySync
{
struct PlaybackWindow
{
    uint32_t A{}, B{};
    float Fraction{};
    bool Available{}, Hold{};
};
template <class TickAt>
PlaybackWindow SelectPlaybackWindow(uint32_t aCount, double aTime, TickAt&& aTick)
{
    if (!aCount)
        return {};
    PlaybackWindow result{aCount - 1, aCount - 1, 0.f, true, aTime - static_cast<double>(aTick(aCount - 1)) > 300.0};
    if (aTime <= static_cast<double>(aTick(0)))
        result.A = result.B = 0;
    else
        for (uint32_t i = 1; i < aCount; ++i)
        {
            const auto end = aTick(i);
            if (aTime > static_cast<double>(end))
                continue;
            const auto start = aTick(i - 1);
            result.A = i - 1;
            result.B = i;
            result.Fraction = end > start ? static_cast<float>((aTime - static_cast<double>(start)) /
                static_cast<double>(end - start)) : 1.f;
            break;
        }
    return result;
}
}
namespace ReplayDoorVotePolicy
{
enum class Skip
{
    None, MissingReference, NotDoor, NotLoadDoor, NotPlayer, NotPlayerInput,
    Offline, NoParty, TooFewMembers, Locked, FreeDoor, Loading, MissingCell,
    MissingDestination, TooFar, UnmappedDoor, UnmappedCell, UnmappedDestination,
    UnmappedWorldSpace, SendFailed, Count
};

constexpr Skip Decide(bool aReference, bool aDoor, bool aTeleport, bool aPlayer,
    bool aInputCall, bool aOnline, bool aInParty, size_t aMembers) noexcept
{
    if (!aReference) return Skip::MissingReference;
    if (!aDoor) return Skip::NotDoor;
    if (!aTeleport) return Skip::NotLoadDoor;
    if (!aPlayer) return Skip::NotPlayer;
    if (!aInputCall) return Skip::NotPlayerInput;
    if (!aOnline) return Skip::Offline;
    if (!aInParty) return Skip::NoParty;
    if (aMembers < 2) return Skip::TooFewMembers;
    return Skip::None;
}

constexpr bool IsInputCall(uintptr_t aCaller, uintptr_t aPick, uintptr_t aChoice = 0) noexcept
{
    return (aPick != 0 && aCaller == aPick + 0x112) ||
        (aChoice != 0 && aCaller == aChoice + 0x73);
}

constexpr bool IsAutomaticEntry(uint32_t aDistanceBand, bool aEntering) noexcept
{
    return aDistanceBand == 1 && aEntering;
}

constexpr bool LostReadyVoter(bool aSameVote, uint32_t aPreviousReady, uint32_t aReady,
    bool aLocalReady, bool aLoading) noexcept
{
    return aSameVote && aLocalReady && !aLoading && aReady < aPreviousReady;
}

constexpr bool CanFollow(bool aPendingVote, uintptr_t aCell, uintptr_t aLeaderCell,
    uintptr_t aWorldSpace, uintptr_t aLeaderWorldSpace) noexcept
{
    return !aPendingVote && aCell && aLeaderCell &&
        (aCell == aLeaderCell || (aWorldSpace && aWorldSpace == aLeaderWorldSpace));
}
} // namespace ReplayDoorVotePolicy
