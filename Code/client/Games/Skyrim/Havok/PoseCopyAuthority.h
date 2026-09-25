#pragma once

#include <Structs/EvaluatedPoseSnapshot.h>

struct World;

// One writer for every networked actor's rendered skeleton: the owner's.
//
// Every native path that copies a generated pose onto an actor's bone nodes goes through
// ID 63856 (normal graph finalize ID 63589, the physics-step listener ID 63563, and others).
// It takes (hkQsTransform* pose, BSTArray<BoneNodeEntry>* boneNodes, count) and resolves
// direct and flattened-tree bone targets itself.
//  - On the owner, the hook records the exact array it is handed; the pose stream sends that.
//  - On the other PCs, the hook hands the original an interpolated owner pose instead of the
//    local graph's, so the follower keeps evaluating its behavior graph (events, variables,
//    gameplay) but its rendered bones are the owner's.
// Measured before: follower NPC bones up to 53 units / 115 degrees off at the same tick; the
// earlier post-graph writer alternated poses because later native copies overwrote it.
namespace PoseCopyAuthority
{
// Main thread: rebuild the boneNodes -> actor registry from the networked actors (every 250 ms).
void RefreshRegistry(World& aWorld) noexcept;
// Main thread, every frame: the shared-clock tick being presented (now minus presentation delay).
void SetPresentationTick(uint64_t aTick) noexcept;
// Main thread, every frame: the current shared-clock tick, stamped on captured poses.
void SetCurrentTick(uint64_t aTick) noexcept;
// Owner side: the last pose array the engine copied onto this actor's bones, if recent.
// Sets arPose.SourceTick to the shared-clock tick of the frame the pose was copied in.
bool GetCapturedPose(uint32_t aFormId, EvaluatedPoseSnapshot& arPose) noexcept;
// Other PCs: a received owner pose sample for this actor.
void PushOwnerSample(uint32_t aFormId, const EvaluatedPoseSnapshot& acPose, uint64_t aTick) noexcept;
void SetEnabled(bool aEnabled) noexcept;
[[nodiscard]] bool IsEnabled() noexcept;
// Counters for the test pipe (JSON object body without braces).
[[nodiscard]] std::string StatsJson() noexcept;
} // namespace PoseCopyAuthority
