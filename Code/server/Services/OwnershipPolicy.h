#pragma once

namespace OwnershipPolicy
{
// Preserve the baseline leader authority inside its load range. A follower
// keeps simulation outside that range. Epochs remain diagnostic until native
// checkpoint rebinding is validated; they are not a reason to delete natives.
constexpr bool ShouldClaim(bool aPlayerActor, bool aLeaderConnected, bool aLeaderInRange,
    bool aSameParty, bool aSessionStartedOrOwnerGone,
    bool aReleased, bool aReleaseReady, bool aDeclined) noexcept
{
    return !aPlayerActor && aLeaderConnected && aLeaderInRange && aSameParty &&
        aSessionStartedOrOwnerGone && !aDeclined && (!aReleased || aReleaseReady);
}

constexpr bool ReleaseReady(unsigned long long aNow, unsigned long long aReleasedAt) noexcept
{
    return aNow >= aReleasedAt && aNow - aReleasedAt >= 1000;
}

constexpr bool EligibleTarget(bool aConnected, bool aSameParty,
    bool aKnownCell, bool aInRange, bool aLeader, bool aDeclined) noexcept
{
    return aConnected && aSameParty && (aKnownCell ? aInRange : aLeader) && !aDeclined;
}

constexpr bool CanReplicate(bool aOwnerConnected, bool aOwnerIsRecipient, bool aPlayerActor,
    bool aSameParty, bool aLeader, bool aOwnerInRange, bool aReleased) noexcept
{
    return aOwnerConnected && !aOwnerIsRecipient && (aPlayerActor ||
        (aSameParty && (!aLeader || (aOwnerInRange && !aReleased))));
}

constexpr bool AcceptOwnershipEpoch(unsigned aExpected, unsigned aCurrent) noexcept
{
    return aExpected != 0 && aExpected == aCurrent;
}

constexpr unsigned char AfterMemberLeft(unsigned char aState, bool aHasMembers,
    bool aAllLoaded, bool aAllGameplayReady) noexcept
{
    if (aHasMembers && aState == 1 && aAllLoaded)
        return 2; // Clients must observe world-ready before gameplay-ready.
    if (aHasMembers && aState == 2 && aAllGameplayReady)
        aState = 3;
    return aState;
}
} // namespace OwnershipPolicy
