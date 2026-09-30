#pragma once

#include <cstdint>

struct World;

// Cutscene follow: while the party leader has no free control of its character (an intro, a
// scripted scene), the scene plays once, the leader's way, and every player sees its own
// character play the leader's part. On each PC the other players' characters are hidden; on a
// follower's PC its own character takes the leader's position, heading and streamed pose (movement
// off). When the leader is free again, everything is restored (the follower is then placed around
// the leader by PartyService).
namespace CutsceneFollow
{
// Safe to read from animation worker threads.
bool IsActive() noexcept;
// Per frame, from PartyService. aActive: in a party session and the leader has no free control.
void Update(World& aWorld, bool aActive, bool aIsLeader, uint32_t aLeaderPlayerId) noexcept;
// The leader's character, as this follower's copy (0 when not following). Animation threads.
uint32_t LeaderFormId() noexcept;
// Scene idles on a follower's own character. The first-person camera follows only idles this
// character plays itself (the pose copy moves the body, not the camera), so a leader idle the
// follower's own scene never plays (the paired IdleExecutionerChop_Player) is mirrored onto it.
// Each idle plays once: a mirror and the follower's own later play of it within 5 s cancel.
bool ClaimLeaderIdleMirror(uint32_t aIdleFormId) noexcept; // true: play it now
bool ClaimLocalIdle(uint32_t aIdleFormId) noexcept;        // false: already mirrored, skip it
} // namespace CutsceneFollow
