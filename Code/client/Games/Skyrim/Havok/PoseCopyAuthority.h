#pragma once

#include <Structs/EvaluatedPoseSnapshot.h>

struct Actor;

struct World;

// Fresh owner poses drive networked skeletons. Living actors without a recent sample
// use their local graph (driven by replayed actions/variables), then blend back.
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
[[nodiscard]] uint64_t GetPresentationTick() noexcept;
// The presentation time now (SmoothClock minus the presentation delay), fractional milliseconds.
[[nodiscard]] double GetPresentationTimeMs() noexcept;
void SetPresentationDelayMs(uint32_t aDelayMs) noexcept;
// Cutscene follow: this PC's own player takes the pose of this actor (the leader's character
// here); 0 turns it off immediately. A new source starts at the next registry refresh.
void SetLocalMirror(uint32_t aSourceFormId) noexcept;
// Whether this actor's ragdoll bodies are simulating here (CorpseRagdollService, per frame). A
// dying actor whose ragdoll is not simulating yet (a death animation) keeps taking its owner's pose.
void SetRagdollSimulating(uint32_t aFormId, bool aSimulating) noexcept;
// A received ragdoll must keep the owner's rendered pose while body binding retries.
void SetRagdollPending(uint32_t aFormId, bool aPending) noexcept;
// These drivers are controlled by streamed rigid-body targets. Their local animation drive is skipped.
void SetControlledRagdollDrivers(const Vector<void*>& acDrivers) noexcept;
void SetControlledRagdollDriver(void* apDriver, bool aControlled) noexcept;
void ClearRagdollAuthority() noexcept;
[[nodiscard]] uint64_t GetCurrentTick() noexcept;
// Whether the presentation timeline needs the living actor's local graph. Physics
// transitions are excluded by the caller; pending streamed ragdolls never fall back.
[[nodiscard]] bool NeedsLocalGraph(uint32_t aFormId) noexcept;
// Diagnostic: whether the owner's pose currently overrides this actor's skeleton, and the newest sample's age.
[[nodiscard]] std::string DescribeOverride(uint32_t aFormId) noexcept;
// Diagnostic, main thread: per graph, the pose slots of nodes whose name contains acNeedle (JSON members).
[[nodiscard]] std::string DescribeBoneSlots(Actor* apActor, const std::string& acNeedle) noexcept;
// Diagnostic, main thread: seated players' pelvis/COM (sent or sampled vs drawn), logged per call.
void ProbeSeatedPlayers(World& aWorld) noexcept;
// The graph animating the drawn third-person skeleton (the player has two); caller holds the manager lock.
uint32_t DrawnGraphIndex(Actor* apActor, BSAnimationGraphManager* apManager, uint32_t aFallback) noexcept;
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
