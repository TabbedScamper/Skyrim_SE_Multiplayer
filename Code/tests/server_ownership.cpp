#include "../server/Services/OwnershipPolicy.h"

// Baseline leader authority applies before a failed follower's timeout, within
// leader range. Explicit declines and release grace periods still apply.
static_assert(OwnershipPolicy::ShouldClaim(false, true, true, true, true, false, true, false));
static_assert(!OwnershipPolicy::ShouldClaim(false, true, false, true, true, false, true, false));
static_assert(!OwnershipPolicy::ShouldClaim(false, false, true, true, true, false, true, false));
static_assert(!OwnershipPolicy::ShouldClaim(false, true, true, false, true, false, true, false));
static_assert(!OwnershipPolicy::ShouldClaim(true, true, true, true, true, false, true, false));
static_assert(!OwnershipPolicy::ShouldClaim(false, true, true, true, false, false, true, false));
static_assert(!OwnershipPolicy::ShouldClaim(false, true, true, true, true, true, false, false));
static_assert(OwnershipPolicy::ShouldClaim(false, true, true, true, true, true, true, false));
static_assert(!OwnershipPolicy::ShouldClaim(false, true, true, true, true, true, true, true));
static_assert(!OwnershipPolicy::ReleaseReady(1999, 1000));
static_assert(OwnershipPolicy::ReleaseReady(2000, 1000));
static_assert(!OwnershipPolicy::ReleaseReady(999, 1000));
static_assert(OwnershipPolicy::AcceptOwnershipEpoch(7, 7));
static_assert(!OwnershipPolicy::AcceptOwnershipEpoch(6, 7));
static_assert(!OwnershipPolicy::AcceptOwnershipEpoch(0, 0));
// Known cells always need range. Keep the baseline unknown-cell leader fallback.
static_assert(OwnershipPolicy::EligibleTarget(true, true, true, true, false, false));
static_assert(!OwnershipPolicy::EligibleTarget(false, true, true, true, true, false));
static_assert(!OwnershipPolicy::EligibleTarget(true, false, true, true, true, false));
static_assert(!OwnershipPolicy::EligibleTarget(true, true, true, false, true, false));
static_assert(OwnershipPolicy::EligibleTarget(true, true, false, false, true, false));
static_assert(!OwnershipPolicy::EligibleTarget(true, true, false, true, false, false));
static_assert(!OwnershipPolicy::EligibleTarget(true, true, true, true, true, true));
static_assert(OwnershipPolicy::CanReplicate(true, false, false, true, true, true, false));
static_assert(!OwnershipPolicy::CanReplicate(false, false, false, true, true, true, false));
static_assert(!OwnershipPolicy::CanReplicate(true, true, false, true, true, true, false));
static_assert(!OwnershipPolicy::CanReplicate(true, false, false, false, true, true, false));
static_assert(!OwnershipPolicy::CanReplicate(true, false, false, true, true, false, false));
static_assert(!OwnershipPolicy::CanReplicate(true, false, false, true, true, true, true));
static_assert(OwnershipPolicy::CanReplicate(true, false, true, true, true, true, false));
static_assert(OwnershipPolicy::AfterMemberLeft(3, true, false, false) == 3);
static_assert(OwnershipPolicy::AfterMemberLeft(1, true, false, false) == 1);
static_assert(OwnershipPolicy::AfterMemberLeft(1, true, true, false) == 2);
static_assert(OwnershipPolicy::AfterMemberLeft(1, true, true, true) == 2);
static_assert(OwnershipPolicy::AfterMemberLeft(2, true, true, true) == 3);
static_assert(OwnershipPolicy::AfterMemberLeft(0, true, true, true) == 0);
static_assert(OwnershipPolicy::AfterMemberLeft(1, false, true, true) == 1);
