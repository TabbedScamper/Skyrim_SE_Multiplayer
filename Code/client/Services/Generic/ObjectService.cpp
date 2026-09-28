#include <Services/SmoothClock.h>
#include <fstream>
#include <Games/ActorExtension.h>
#include <Services/ObjectService.h>
#include <Services/CorpseRagdollService.h>
#include <Services/Generic/SharedDropService.h>

#include <World.h>
#include <Events/DisconnectedEvent.h>
#include <Events/UpdateEvent.h>
#include <Events/CellChangeEvent.h>
#include <Events/ActivateEvent.h>
#include <Events/LockChangeEvent.h>
#include <Events/ScriptAnimationEvent.h>
#include <Messages/ServerTimeSettings.h>
#include <Messages/AssignObjectsRequest.h>
#include <Messages/AssignObjectsResponse.h>
#include <Messages/ActivateRequest.h>
#include <Messages/NotifyActivate.h>
#include <Messages/LockChangeRequest.h>
#include <Messages/NotifyLockChange.h>
#include <Messages/ScriptAnimationRequest.h>
#include <Messages/NotifyScriptAnimation.h>
#include <Messages/PhysicsReferencesMoveRequest.h>
#include <AI/AIProcess.h>
#include <Misc/MiddleProcess.h>
#include <Messages/NotifyPhysicsReferencesMove.h>

#include <PlayerCharacter.h>
#include <Forms/TESObjectCELL.h>
#include <Forms/TESWorldSpace.h>
#include <Forms/BGSEncounterZone.h>
#include <Forms/TESFaction.h>
#include <Games/TES.h>
#include <Havok/ActorPoseDiagnosticViews.h>

#include <inttypes.h>
#include <atomic>
#include <cmath>
#include <intrin.h>
#include <mutex>
#include <glm/gtc/quaternion.hpp>

namespace
{
// Only locally admitted SharedDrop packets may enter the reserved identity
// namespace. The legacy leader physics relay cannot authorize drop movement.
thread_local uint32_t s_sharedDropGeneration{};
std::unordered_map<uint32_t, uint32_t> s_sharedDropPoseGenerations;
std::atomic<uint64_t> s_renderDiagnosticsUntilMs{};
// Published only by OnUpdate while holding m_remotePhysicsLock. Frame probes
// resolve these IDs instead of iterating the update thread's ECS storage.
std::vector<uint32_t> s_renderActorIds;
uint64_t s_nextRenderActorsMs{};
// Follower copies remain dynamic. Main-thread publication and native-world locking
// separate scene graph access from solver writes; packet gaps retain the last target.
constexpr bool kHostDrivenMovingBodies = true;
constexpr int64_t kHostDrivenMaxExtrapolationMs = 150;
constexpr int64_t kHostDrivenHoldAfterMs = 300;
constexpr float kHavokToGameUnits = 70.f;
// Test switch: hand host-driven bodies to the exterior cell they move into.
std::atomic<bool> s_cellHandoffEnabled{true};
// Test switch: also write the root Havok body (a keyframed body otherwise follows its node).
std::atomic<bool> s_rootBodyWriteEnabled{true};
// Test switch: host-driven playback of moving bodies as a whole (off = local physics).
std::atomic<bool> s_hostDrivenPlaybackEnabled{true};
// Playback of host-driven bodies runs at the start of Main::Update on the main thread. The VM
// update (World::Update) is a job on a worker thread in parallel with the frame; writing a
// cart's scene nodes there raced the renderer and physics and showed as the cart flashing.
std::atomic<bool> s_mainFramePlaybackEnabled{true};
// The host's snapshot of those bodies is read there too: read on the update job, a cart's root and
// its wheels could come from two different physics steps, which played back as the cart jittering.
std::atomic<bool> s_mainFrameCaptureEnabled{true};

// Host sample probe: how well each sample's tick matches the motion it carries (the speed implied
// by position and tick against the body's own velocity). Timestamp noise plays back as jitter.
struct HostSampleProbe
{
    glm::vec3 Position{};
    uint64_t Tick{};
    bool Has{};
    uint32_t Samples{};
    float ErrorSum{};
    float ErrorMax{};
    float BodySpeedSum{};
    float GapErrorSum{};
    std::chrono::steady_clock::time_point NextLog{};
    glm::vec3 BodyPosition{};
    float BodyErrorSum{};
};
std::unordered_map<uint32_t, HostSampleProbe> s_hostSampleProbes;

// Host render probe: the same per-frame measure as the follower's jitter probe, on the host's own
// (physics-driven) node, so both screens are compared by one number.
struct HostRenderProbe
{
    uint64_t LastSeenMs{};
    glm::vec3 Last{};
    std::chrono::steady_clock::time_point LastAt{};
    bool Has{};
    float LastSpeed{};
    uint32_t Frames{};
    float SpeedSum{};
    float ChangeSum{};
    float ChangeMax{};
    glm::vec3 Direction{};
    std::chrono::steady_clock::time_point NextLog{};
};
std::unordered_map<uint32_t, HostRenderProbe> s_hostRenderProbes;

// How far the nearest mount (the horse pulling this cart) is ahead of the reference along its
// direction of travel. Compared between the PCs it measures the follower's playback lag.
float MountLead(const TESObjectREFR* apReference, const glm::vec3& acDirection, uint32_t& arMountId) noexcept
{
    arMountId = 0;
    if (!ObjectService::IsRenderDiagnosticsArmed())
        return 0.f;
    glm::vec2 direction{acDirection.x, acDirection.y};
    if (glm::length(direction) < 1.f)
        return 0.f;
    direction = glm::normalize(direction);
    const glm::vec2 origin{apReference->position.x, apReference->position.y};
    float best = 250.f;
    float lead = 0.f;
    for (const auto publishedActorId : s_renderActorIds)
    {
        auto* pActor = Cast<Actor>(TESForm::GetById(publishedActorId));
        // The nearest NPC riding it (the driver): not a player.
        if (!pActor || !pActor->GetExtension() || pActor->GetExtension()->IsPlayer())
            continue;
        const glm::vec2 offset = glm::vec2{pActor->position.x, pActor->position.y} - origin;
        const float distance = glm::length(offset);
        if (distance < best)
        {
            best = distance;
            lead = glm::dot(offset, direction);
            arMountId = pActor->formID;
        }
    }
    return lead;
}

// Drawn gap: the nearest NPC's rendered root node against the reference's rendered node, along
// the direction of travel, sampled at the end of a frame (after drawing). Logged every 5 s.
struct DrawnGapProbe
{
    uint64_t LastSeenMs{};
    uint32_t Samples{};
    float Sum{};
    float Min{1e9f};
    float Max{-1e9f};
    uint32_t ActorId{};
    uint32_t SitState{};
    uint32_t BoneSamples{};
    float BoneSum{};
    float BoneMin{1e9f};
    float BoneMax{-1e9f};
    std::chrono::steady_clock::time_point NextLog{};
};
std::unordered_map<uint32_t, DrawnGapProbe> s_drawnGapProbes;

void ProbeDrawnGap(const char* apSide, TESObjectREFR* apReference, const glm::vec3& acDirection) noexcept
{
    if (!ObjectService::IsRenderDiagnosticsArmed())
        return;
    const auto* pNode = apReference ? apReference->GetNiNode() : nullptr;
    glm::vec2 direction{acDirection.x, acDirection.y};
    if (!pNode || glm::length(direction) < 1.f)
        return;
    direction = glm::normalize(direction);
    const glm::vec2 origin{pNode->world.translate.x, pNode->world.translate.y};
    float best = 250.f;
    float gap = 0.f;
    uint32_t actorId = 0;
    Actor* pNearest = nullptr;
    for (const auto publishedActorId : s_renderActorIds)
    {
        auto* pActor = Cast<Actor>(TESForm::GetById(publishedActorId));
        if (!pActor || !pActor->GetExtension() || pActor->GetExtension()->IsPlayer())
            continue;
        const auto* pActorNode = pActor->GetNiNode();
        if (!pActorNode)
            continue;
        const glm::vec2 offset = glm::vec2{pActorNode->world.translate.x, pActorNode->world.translate.y} - origin;
        if (glm::length(offset) < best)
        {
            best = glm::length(offset);
            gap = glm::dot(offset, direction);
            actorId = pActor->formID;
            pNearest = pActor;
        }
    }
    if (!actorId)
        return;
    auto& probe = s_drawnGapProbes[apReference->formID];
    probe.LastSeenMs = GetTickCount64();
    ++probe.Samples;
    probe.Sum += gap;
    probe.Min = (std::min)(probe.Min, gap);
    probe.Max = (std::max)(probe.Max, gap);
    probe.ActorId = actorId;
    probe.SitState = pNearest ? (pNearest->actorState.flags1 >> 14) & 0xF : 0;
    // The drawn body: its pelvis bone, whose world transform the animation update computes.
    static BSFixedString s_pelvis("NPC Pelvis [Pelv]");
    if (auto* pRoot = pNearest ? pNearest->GetNiNode() : nullptr)
    {
        if (auto* pBone = pRoot->GetByName(s_pelvis))
        {
            const float boneGap = glm::dot(glm::vec2{pBone->world.translate.x, pBone->world.translate.y} - origin, direction);
            ++probe.BoneSamples;
            probe.BoneSum += boneGap;
            probe.BoneMin = (std::min)(probe.BoneMin, boneGap);
            probe.BoneMax = (std::max)(probe.BoneMax, boneGap);
        }
    }
    const auto now = std::chrono::steady_clock::now();
    if (now >= probe.NextLog)
    {
        spdlog::info("{} drawn gap {:X}: nearest NPC {:X} root node {:.1f} u along travel (min {:.1f} max {:.1f}), pelvis {:.1f} "
            "(min {:.1f} max {:.1f}), {} frames, sit state {}", apSide, apReference->formID, probe.ActorId, probe.Sum / probe.Samples, probe.Min,
            probe.Max, probe.BoneSamples ? probe.BoneSum / probe.BoneSamples : 0.f, probe.BoneMin, probe.BoneMax, probe.Samples, probe.SitState);
        probe = DrawnGapProbe{};
        probe.LastSeenMs = GetTickCount64();
        probe.NextLog = now + std::chrono::seconds(5);
    }
}

void ProbeHostRender() noexcept
{
    if (!ObjectService::IsRenderDiagnosticsArmed())
        return;
    const auto now = std::chrono::steady_clock::now();
    for (auto& [formId, probe] : s_hostRenderProbes)
    {
        auto* pReference = Cast<TESObjectREFR>(TESForm::GetById(formId));
        auto* pNode = pReference ? pReference->GetNiNode() : nullptr;
        if (!pNode)
            continue;
        const glm::vec3 shown{pNode->world.translate.x, pNode->world.translate.y, pNode->world.translate.z};
        if (probe.Has)
        {
            const float dtMs = std::chrono::duration<float, std::milli>(now - probe.LastAt).count();
            if (dtMs > 0.f)
            {
                const float speed = glm::length(shown - probe.Last) / dtMs * 1000.f;
                const float change = std::abs(speed - probe.LastSpeed);
                probe.SpeedSum += speed;
                probe.ChangeSum += change;
                probe.ChangeMax = (std::max)(probe.ChangeMax, change);
                probe.LastSpeed = speed;
                if (glm::length(shown - probe.Last) > 0.01f)
                    probe.Direction = shown - probe.Last;
                if (speed > 20.f)
                    ProbeDrawnGap("Host", pReference, probe.Direction * 1000.f);
                ++probe.Frames;
            }
        }
        probe.Last = shown;
        probe.LastAt = now;
        probe.Has = true;
        if (now >= probe.NextLog && probe.Frames)
        {
            uint32_t mountId = 0;
            const float lead = MountLead(pReference, probe.Direction * 1000.f, mountId);
            if (probe.SpeedSum / probe.Frames > 5.f)
                spdlog::info("Host body {:X} render: {} frames, speed {:.0f} u/s, speed change mean {:.0f} max {:.0f}, rider {:X} lead {:.1f} u",
                    formId, probe.Frames, probe.SpeedSum / probe.Frames, probe.ChangeSum / probe.Frames, probe.ChangeMax, mountId, lead);
            probe.Frames = 0;
            probe.SpeedSum = probe.ChangeSum = probe.ChangeMax = 0.f;
            probe.NextLog = now + std::chrono::seconds(5);
        }
    }
}
std::atomic<ObjectService*> s_objectService{};
std::atomic<bool> s_loggedMainFrameThread{};
std::atomic<bool> s_loggedUpdateThread{};
std::atomic<uint32_t> s_mainThreadId{};
std::atomic<uint64_t> s_updatesOnMain{}, s_updatesOffMain{};
std::chrono::steady_clock::time_point s_nextThreadReport{};
// Default-off, single-reference engine-boundary probe. The form ID is chosen
// at runtime by the test bridge; no quest/cart reference is hardcoded.
std::atomic<uint32_t> s_bodyPlaybackFormId{};
std::atomic<uint64_t> s_bodyPlaybackAttempts{};
std::atomic<uint64_t> s_bodyPlaybackSucceeded{};
std::atomic<uint64_t> s_bodyPlaybackStaleSkips{};
std::atomic<uint32_t> s_bodyPlaybackLastAgeMs{};
std::atomic<uint32_t> s_bodyPlaybackLastDurationUs{};
std::atomic<float> s_bodyPlaybackLastPreError{};
std::atomic<float> s_bodyPlaybackLastPostError{};
std::atomic<float> s_bodyPlaybackLastStep{};
std::atomic<uint32_t> s_referencePhaseFormId{};
std::atomic<void*> s_referencePhaseVtable{};
std::atomic<bool> s_referencePhaseInstalled{};
std::atomic<uint64_t> s_referenceUpdate3DCalls{};
std::atomic<uint64_t> s_referenceMoveHavokCalls{};
std::atomic<uint32_t> s_referencePhaseThreadId{};
std::atomic<uint32_t> s_referencePhaseLastMethod{};
std::atomic<uint32_t> s_referencePhaseDurationUs{};
std::atomic<float> s_referencePhaseRefDelta{};
std::atomic<float> s_referencePhaseNodeDelta{};
std::atomic<float> s_referencePhaseBodyDelta{};
std::atomic<float> s_referencePhaseRefBodyError{};
std::atomic<float> s_referencePhaseRefNodeError{};
std::atomic<uint64_t> s_referenceSetPositionCalls{};
std::atomic<uint64_t> s_referenceSetPositionRemoteSuppressedCalls{};
std::atomic<uint64_t> s_referenceSetPositionRemoteOverrideCalls{};
std::atomic<uint64_t> s_referenceSetPositionCallerRva{};
std::atomic<uint32_t> s_referenceSetPositionThreadId{};
std::atomic<uint32_t> s_referenceSetPositionFormType{};
std::atomic<bool> s_referenceSetPositionSourceIsNodeWorld{};
std::atomic<float> s_referenceSetPositionInputX{};
std::atomic<float> s_referenceSetPositionInputY{};
std::atomic<float> s_referenceSetPositionInputZ{};
std::atomic<float> s_referenceSetPositionPreReferenceError{};
std::atomic<uint64_t> s_collisionSyncSelectedCalls{};
std::atomic<uint64_t> s_collisionSyncLastCallerRva{};
std::atomic<uint32_t> s_collisionSyncLastThreadId{};
std::atomic<uint32_t> s_collisionSyncLastDurationUs{};
std::atomic<float> s_collisionSyncLastNodeDelta{};
std::atomic<float> s_collisionSyncLastReferenceDelta{};
std::atomic<float> s_collisionSyncLastBodyDelta{};
std::atomic<float> s_collisionSyncLastPreNodeReferenceError{};
std::atomic<float> s_collisionSyncLastPostNodeReferenceError{};
std::atomic<uint64_t> s_collisionWorldSelectedCalls{};
std::atomic<uint64_t> s_collisionWorldLastCallerRva{};
std::atomic<uint32_t> s_collisionWorldLastThreadId{};
std::atomic<uint32_t> s_collisionWorldLastDurationUs{};
std::atomic<float> s_collisionWorldLastInputX{};
std::atomic<float> s_collisionWorldLastInputY{};
std::atomic<float> s_collisionWorldLastInputZ{};
std::atomic<float> s_collisionWorldLastInputBodyError{};
std::atomic<float> s_collisionWorldLastInputNodeError{};
std::atomic<float> s_collisionWorldLastPostNodeInputError{};
std::atomic<float> s_collisionWorldLastPostReferenceInputError{};
std::atomic<uint64_t> s_worldUpdateCalls{};
std::atomic<uint32_t> s_worldUpdateLastThreadId{};
std::atomic<uint32_t> s_worldUpdateLastDurationUs{};
std::atomic<uint64_t> s_worldUpdateLastEndNs{};
// Start of the newest completed bhkWorld update, and whether one is running now. A host sample is
// stamped with the time its physics state was stepped, not the time it was read: the read comes a
// variable part of a frame later (measured up to 30 ms), which played back as the cart jittering.
std::atomic<uint64_t> s_worldUpdateStartNs{};
std::atomic<uint64_t> s_worldUpdateLastStartNs{};
std::atomic<int32_t> s_worldUpdatesRunning{};
// Off: stamping the cart's samples earlier than the actors' (read in the same frame) made the cart
// play back about 25 ms ahead of its horse, driver and passengers.
std::atomic<bool> s_physicsStampEnabled{false};
// Host-driven bodies follow a curve through the host samples that also matches the host body's
// velocity at each one. Straight lines between samples changed speed at every sample (the sampled
// positions carry up to half the speed in timing noise), which showed as the cart surging.
std::atomic<bool> s_hermitePlaybackEnabled{true};
// Host-driven bodies: the played-back transform (root and every child part) follows the host's
// path through a critically damped filter with this time constant. The host's samples carry timing
// noise that no interpolation removes; the filter does. Its lag is not compensated: the actors
// riding a cart play back that much later too (measured: reading the path 50 ms ahead put the
// cart 6 to 12 units ahead of its driver, who sits exactly at the cart's position on the host).
// Off: it also filtered out the host cart's real jolts (the bumps the host's camera rides). The
// smooth clock and the body velocity removed the jitter it was added for.
std::atomic<bool> s_cartSmoothingEnabled{false};
constexpr float kCartSmoothingMs = 50.f;
// The keyframed body of a host-driven reference carries the played-back motion as its velocity.
// Placed with zero velocity every frame, it was a platform that jumps instead of moving: what
// stood or sat on it (the player riding the intro cart) was pushed out and settled back every
// frame, and the whole cart vibrated against the view while it moved.
std::atomic<bool> s_bodyVelocityEnabled{true};
// The node of a host-driven reference shows the previous frame's pose; its body takes the current
// one. On the host the physics step moves a cart after the actors' animation update, so the cart,
// the bones of the riders seated from it and the rest of the frame show one moment. Written at the
// start of the frame here, the cart was drawn a frame ahead of its riders' bodies.
std::atomic<bool> s_visualLagFrameEnabled{true};
// Host-driven moving bodies stay simulated here (dynamic), steered every physics step to the host's
// pose: its velocity plus a correction. Keyframed playback moved the follower's cart like a puppet,
// without the host cart's jolts, and its riders and camera with it ("floaty").
std::atomic<bool> s_cartPhysicsEnabled{true};

// Remote actors riding a host-driven reference: on the host the engine seats a cart's driver and
// passengers at the cart's own position; here their position came from the actor stream, placed
// later in the frame on the update job, and the cart was drawn ahead of its driver. A rider is
// placed with the reference instead, right after it, at its host offset.
struct Rider
{
    uint32_t ReferenceId{};
    glm::vec3 Offset{};
    std::chrono::steady_clock::time_point SeenAt{};
    // Where the frame drew the rider against where it was placed (the engine's own actor update
    // still moves it by about one frame of travel afterwards: measured 3 units at cart speed, 0.0
    // on the host). Averaged and taken off the next placement.
    glm::vec3 Drift{};
    glm::vec3 Placed{};
    bool HasPlaced{};
    // The seat's heading against the reference's (a passenger faces sideways). Learned slowly from
    // the actor stream, whose latency otherwise turned riders after their cart.
    float HeadingOffset{};
    bool HasHeadingOffset{};
};
std::unordered_map<uint32_t, Rider> s_riders; // actor form id -> ride (under m_remotePhysicsLock)
// The match allows for the actor stream playing back later than the body stream (measured 4 to 5
// units at cart speed). The seat is the reference's own position: the engine seats a rider there
// (measured 0.0 units on the host); the offset seen here is only that latency gap.
constexpr float kRiderMatchUnits = 20.f;
std::atomic<uint64_t> s_nativeStepCalls{};
std::atomic<uint32_t> s_nativeStepLastThreadId{};
std::atomic<uint32_t> s_nativeStepLastDurationUs{};
std::atomic<uint64_t> s_nativeStepLastEndNs{};
std::atomic<void*> s_watchedHavokBody{};
std::atomic<void*> s_watchedHavokWorld{};
std::atomic<void*> s_watchedBodyWrapper{};
std::atomic<uint64_t> s_selectedBodySteps{};
std::atomic<uint64_t> s_selectedBodyChangedSteps{};
std::atomic<float> s_selectedBodyLastStepDelta{};
std::atomic<float> s_selectedBodyPeakStepDelta{};
std::atomic<uint64_t> s_selectedBodyStepsOver75Units{};
std::atomic<uint64_t> s_selectedBodyPeakStepTimeMs{};
std::atomic<float> s_selectedBodyPeakStepDt{};
std::atomic<float> s_selectedBodyPeakPreLinearSpeed{};
std::atomic<float> s_selectedBodyPeakPostLinearSpeed{};
std::atomic<float> s_selectedBodyPeakPreAngularSpeed{};
std::atomic<float> s_selectedBodyPeakPostAngularSpeed{};
std::atomic<uint32_t> s_selectedBodyPeakMotionType{};
std::atomic<bool> s_selectedBodyPeakTargetApplied{};
std::atomic<uint32_t> s_selectedBodyPeakTargetAgeMs{};
std::atomic<float> s_selectedBodyPeakVelocityAfterWrite{};
std::atomic<uint64_t> s_selectedBodyCacheRefreshes{};
std::atomic<uint64_t> s_selectedStepWorldMatches{};
std::atomic<uint64_t> s_selectedStepBodyReads{};
std::atomic<uint64_t> s_collisionWorldDuringWorldUpdate{};
std::atomic<uint64_t> s_collisionWorldOutsideWorldUpdate{};
std::atomic<uint32_t> s_collisionWorldAfterWorldUpdateUs{};
std::atomic<uint32_t> s_collisionWorldAfterNativeStepUs{};
thread_local uint32_t s_worldUpdateDepth{};
std::atomic<uint32_t> s_preStepPlaybackFormId{};
std::atomic<uint32_t> s_preStepPlaybackMode{};
// Retired probe state is kept zero for diagnostic API compatibility.
std::atomic<uint32_t> s_kinematicProbeFormId{};
std::atomic<uint32_t> s_kinematicOriginalMotionType{};
// One simulator per object: the follower replays the owner's cart assembly instead of simulating it.
std::atomic<bool> s_cartReplay{true};
// "Render them all" (owner 2026-09-28): owned actors and hitched carts carry NiAVObject kAlwaysDraw (1 << 11) and
// kForceUpdate (1 << 25) on their 3D root while in a co-op session, so the camera cull never marks them not
// visible and the engine never runs its off-screen shortcuts (stale skeleton placement, skipped controller
// writeback, reduced animation, far-away physics) for anything a player owns.
// Default off: kAlwaysDraw|kForceUpdate on owned roots did not reduce floats (paired A/B run 20260928-082058:
// 2.76 -> 2.41 floats/min, cart jumps 6.6 -> 15.1). Kept as a switch pending the real cull mechanism.
std::atomic<bool> s_renderAll{false}; // kWasInFrustrum latch: no effect (run 20260928-085006)
constexpr uint32_t kRenderAllFlags = (1u << 11) | (1u << 25);
// Host cart scene-node refresh every frame (unproven; paired A/B switch cart_node_refresh).
// Default off (review P1-4): unproven; the controller z writeback is the proven float fix.
std::atomic<bool> s_cartNodeRefresh{false};
std::atomic<uint64_t> s_preStepExpectedEpoch{};
std::mutex s_preStepTargetPublishMutex;
std::atomic<uint64_t> s_preStepTargetSequence{};
std::atomic<uint32_t> s_preStepTargetFormId{};
std::atomic<uint64_t> s_preStepTargetEpoch{};
std::atomic<uint64_t> s_preStepTargetTick{};
std::atomic<uint64_t> s_preStepTargetReceivedNs{};
std::array<std::atomic<float>, 3> s_preStepTargetPosition{};
std::array<std::atomic<float>, 3> s_preStepTargetVelocity{};
std::array<std::atomic<float>, 4> s_preStepTargetQuaternion{};
std::atomic<uint64_t> s_preStepPublishedTargets{};
std::atomic<uint64_t> s_preStepAttempts{};
std::atomic<uint64_t> s_preStepApplied{};
std::atomic<uint64_t> s_preStepStaleSkips{};
std::atomic<uint32_t> s_preStepLastSourceAgeMs{};
std::atomic<float> s_preStepLastPreError{};
std::atomic<float> s_preStepLastPostError{};
std::atomic<float> s_preStepLastVelocityCorrection{};
std::atomic<uint64_t> s_preStepPoseWrites{};
std::atomic<float> s_preStepLastPoseStep{};
std::atomic<uint64_t> s_physicsHostScans{};
std::atomic<uint64_t> s_physicsHostPacketsSent{};
std::atomic<uint64_t> s_physicsHostUpdatesQueued{};
std::atomic<uint64_t> s_physicsHostBodyOnlyUpdates{};
std::atomic<uint64_t> s_physicsHostSelectedObserved{};
std::atomic<uint64_t> s_physicsHostSelectedQueued{};
std::atomic<uint64_t> s_physicsFollowerPacketsReceived{};
std::atomic<uint64_t> s_physicsFollowerSelectedReceived{};
std::atomic<uint32_t> s_physicsLastSelectedTransitAgeMs{};
std::atomic<uint32_t> s_physicsLastHostScanDurationUs{};
std::atomic<uint32_t> s_physicsLastHostReferencesVisited{};
std::atomic<uint32_t> s_physicsLastLaneReferencesVisited{};
std::atomic<uint32_t> s_physicsLastHostUpdatesQueued{};
std::atomic<uint64_t> s_physicsHostScanTotalUs{};
std::atomic<uint32_t> s_physicsHostScanMaxUs{};
std::atomic<uint32_t> s_physicsLastHostCandidateCount{};
struct HostScanPhaseTiming
{
    std::atomic<uint32_t> LastUs{};
    std::atomic<uint32_t> MaxUs{};
    std::atomic<uint64_t> TotalUs{};

    void Record(uint32_t aDurationUs) noexcept
    {
        LastUs.store(aDurationUs, std::memory_order_relaxed);
        TotalUs.fetch_add(aDurationUs, std::memory_order_relaxed);
        auto previousMax = MaxUs.load(std::memory_order_relaxed);
        while (aDurationUs > previousMax &&
            !MaxUs.compare_exchange_weak(previousMax, aDurationUs,
                std::memory_order_relaxed)) {}
    }
};
HostScanPhaseTiming s_hostKnownRefreshTiming{};
HostScanPhaseTiming s_hostCurrentDiscoveryTiming{};
HostScanPhaseTiming s_hostGridDiscoveryTiming{};
HostScanPhaseTiming s_hostPruneTiming{};

uint32_t HostScanDurationUs(std::chrono::steady_clock::time_point aStart) noexcept
{
    return static_cast<uint32_t>((std::min)(int64_t{UINT32_MAX},
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - aStart).count()));
}
using Update3DPositionFn = void (*)(TESObjectREFR*, bool);
using MoveHavokFn = void (*)(TESObjectREFR*, bool);
Update3DPositionFn s_originalUpdate3DPosition{};
MoveHavokFn s_originalMoveHavok{};
std::atomic<void*> s_referenceNodeVtable{};
std::atomic<NiAVObject*> s_watchedReferenceNode{};
std::atomic<TESObjectREFR*> s_watchedReference{};
std::atomic<bool> s_referenceNodeHookInstalled{};
std::atomic<uint64_t> s_nodeDownwardCalls{};
std::atomic<uint64_t> s_nodeWorldDataCalls{};
std::atomic<uint64_t> s_nodeTransformBoundsCalls{};
std::atomic<uint32_t> s_nodeLastMethod{};
std::atomic<uint32_t> s_nodeLastThreadId{};
std::atomic<uint32_t> s_nodeLastDurationUs{};
std::atomic<float> s_nodeLastLocalDelta{};
std::atomic<float> s_nodeLastWorldDelta{};
std::atomic<float> s_nodeLastReferenceDelta{};
std::atomic<float> s_nodeLastRefNodeError{};
std::atomic<uint64_t> s_nodeWorldDataCallerRva{};
std::atomic<uint64_t> s_nodeTransformCallerRva{};
std::atomic<float> s_nodeWorldDataRefDelta{};
std::atomic<float> s_nodeTransformRefDelta{};
std::atomic<uint64_t> s_nodeWorldDataTargetRva{};
std::atomic<uint64_t> s_nodeTransformTargetRva{};
using NodeDownwardFn = void (*)(NiAVObject*, void*, uint32_t);
using NodeUpdateFn = void (*)(NiAVObject*, void*);
NodeDownwardFn s_originalNodeDownward{};
NodeUpdateFn s_originalNodeWorldData{};
NodeUpdateFn s_originalNodeTransformBounds{};

bool ReadNativeMemory(const void* apSource, void* apDestination, size_t aSize) noexcept
{
    if (!apSource || !apDestination || aSize == 0)
        return false;
    __try
    {
        std::memcpy(apDestination, apSource, aSize);
        return true;
    }
    __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ?
        EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH)
    {
        return false;
    }
}

template <class T> bool ReadNativeMemory(const void* apSource, T& arValue) noexcept
{
    return ReadNativeMemory(apSource, &arValue, sizeof(T));
}

// Capture already resolves a loaded reference and its current collision chain. Use a guarded
// copy here instead of three VirtualQuery syscalls per body (and per child). Do not cache raw
// Havok pointers across frames: Set3D/body replacement can occur without a cell transition.
bool ReadPhysicsMemory(const void* apSource, void* apDestination, size_t aSize) noexcept
{
    if (!apSource)
        return false;
    __try
    {
        std::memcpy(apDestination, apSource, aSize);
        return true;
    }
    __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ?
        EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH)
    {
        return false;
    }
}

template <class T> bool ReadPhysicsMemory(const void* apSource, T& arValue) noexcept
{
    return ReadPhysicsMemory(apSource, &arValue, sizeof(T));
}

// hkpEntity::Activate, ID 60849 / 0x140B4AFF0: island +0x32 bits 2..3 are activeMark.
// Unknown/replacing islands must take the sampling path, never silently lose a moving sample.
bool IsPhysicsBodyAwake(const void* apBody) noexcept
{
    const uint8_t* island{};
    uint8_t flags{};
    return !ReadPhysicsMemory(static_cast<const uint8_t*>(apBody) + 0x130, island) ||
        !island || !ReadPhysicsMemory(island + 0x32, flags) || (flags & 0x0C) != 0;
}

// 77851 and 78089: Skyrim's hkpWorld extension +0x430 owns bhkWorld,
// whose BSReadWriteLock is +0xC598. Lock order: native world, then targets.
struct ScopedPhysicsWorld
{
    void* Lock{};
    explicit ScopedPhysicsWorld(void* apWorld) noexcept
    {
        uint8_t* wrapper{};
        if (apWorld && ReadPhysicsMemory(static_cast<uint8_t*>(apWorld) + 0x430, wrapper) && wrapper)
        {
            Lock = wrapper + 0xC598;
            using LockFn = void(void*);
            POINTER_SKYRIMSE(LockFn, lock, 68234);
            lock.Get()(Lock);
        }
    }
    ~ScopedPhysicsWorld()
    {
        if (Lock)
        {
            using LockFn = void(void*);
            POINTER_SKYRIMSE(LockFn, unlock, 68240);
            unlock.Get()(Lock);
        }
    }
};

struct DynamicBody
{
    void* Wrapper{};
    void* HavokBody{};
    ActorPoseDiagnosticViews::RigidBody State{};
};

// A child body: its bhkRigidBody wrapper and the hkpRigidBody it owns.
struct ChildBody
{
    NiAVObject* Node{};
    void* Wrapper{};
    ActorPoseDiagnosticViews::RigidBody* Body{};
};

// NiMatrix3 (row-major entry[r][c]) <-> quaternion (x, y, z, w).
void NiMatrixToQuaternion(const NiMatrix3& m, float* q) noexcept
{
    const auto& e = m.entry;
    const float trace = e[0][0] + e[1][1] + e[2][2];
    if (trace > 0.f)
    {
        const float s = std::sqrt(trace + 1.f) * 2.f;
        q[3] = 0.25f * s; q[0] = (e[2][1] - e[1][2]) / s; q[1] = (e[0][2] - e[2][0]) / s; q[2] = (e[1][0] - e[0][1]) / s;
    }
    else if (e[0][0] > e[1][1] && e[0][0] > e[2][2])
    {
        const float s = std::sqrt(1.f + e[0][0] - e[1][1] - e[2][2]) * 2.f;
        q[3] = (e[2][1] - e[1][2]) / s; q[0] = 0.25f * s; q[1] = (e[0][1] + e[1][0]) / s; q[2] = (e[0][2] + e[2][0]) / s;
    }
    else if (e[1][1] > e[2][2])
    {
        const float s = std::sqrt(1.f + e[1][1] - e[0][0] - e[2][2]) * 2.f;
        q[3] = (e[0][2] - e[2][0]) / s; q[0] = (e[0][1] + e[1][0]) / s; q[1] = 0.25f * s; q[2] = (e[1][2] + e[2][1]) / s;
    }
    else
    {
        const float s = std::sqrt(1.f + e[2][2] - e[0][0] - e[1][1]) * 2.f;
        q[3] = (e[1][0] - e[0][1]) / s; q[0] = (e[0][2] + e[2][0]) / s; q[1] = (e[1][2] + e[2][1]) / s; q[2] = 0.25f * s;
    }
}

void QuaternionToNiMatrix(const float* q, NiMatrix3& m) noexcept
{
    const float x = q[0], y = q[1], z = q[2], w = q[3];
    auto& e = m.entry;
    e[0][0] = 1.f - 2.f * (y * y + z * z); e[0][1] = 2.f * (x * y - z * w); e[0][2] = 2.f * (x * z + y * w);
    e[1][0] = 2.f * (x * y + z * w); e[1][1] = 1.f - 2.f * (x * x + z * z); e[1][2] = 2.f * (y * z - x * w);
    e[2][0] = 2.f * (x * z - y * w); e[2][1] = 2.f * (y * z + x * w); e[2][2] = 1.f - 2.f * (x * x + y * y);
}

// The reference's Havok bodies other than its root, in 3D-tree order (cart wheels, yoke). Only
// real rigid bodies (valid motion type, in a world): a collision object can also hold a phantom.
template <class Container>
void CollectChildBodies(TESObjectREFR* apReference, Container& arBodies, bool aCapture = false) noexcept
{
    arBodies.clear();
    auto* pRoot = apReference ? apReference->GetNiNode() : nullptr;
    if (!pRoot)
        return;
    auto walk = [&](auto&& self, NiAVObject* apNode, int aDepth) -> void
    {
        if (!apNode || aDepth > 8 || arBodies.size() >= PhysicsReferenceUpdate::kMaxChildBodies)
            return;
        if (apNode != pRoot && apNode->collisionObject)
        {
            void* pWrapper = nullptr;
            void* pBody = nullptr;
            ActorPoseDiagnosticViews::RigidBody probe{};
            const auto read = [aCapture](const void* source, auto& value)
            {
                return aCapture ? ReadPhysicsMemory(source, value) : ReadNativeMemory(source, value);
            };
            if (read(reinterpret_cast<const uint8_t*>(apNode->collisionObject) + 0x20, pWrapper) && pWrapper &&
                read(reinterpret_cast<const uint8_t*>(pWrapper) + 0x10, pBody) && pBody &&
                read(pBody, probe) && probe.world && probe.motionType >= 1 && probe.motionType <= 7)
                arBodies.push_back({apNode, pWrapper, static_cast<ActorPoseDiagnosticViews::RigidBody*>(pBody)});
        }
        if (auto* pNode = apNode->AsNode())
        {
            for (uint16_t i = 0; i < pNode->children.length; ++i)
                self(self, pNode->children.data[i], aDepth + 1);
        }
    };
    walk(walk, pRoot, 0);
}

struct CaptureChildren
{
    std::array<ChildBody, PhysicsReferenceUpdate::kMaxChildBodies> Bodies;
    size_t Count{};
    void clear() noexcept { Count = 0; }
    size_t size() const noexcept { return Count; }
    void push_back(ChildBody aBody) noexcept { Bodies[Count++] = aBody; }
    auto begin() const noexcept { return Bodies.begin(); }
    auto end() const noexcept { return Bodies.begin() + Count; }
};

bool IsDynamicMotion(uint32_t aType) noexcept
{
    return (aType >= 1 && aType <= 3) || aType == 6;
}

// Membership is learned from native tether construction, never a world/actor scan.
// Slots are structural and never compacted by motion/world state. The existing wire
// format uses model-tree order; a missing slot rejects the entire assembly sample.
struct CartAssembly
{
    std::shared_ptr<NiAVObject> Root, HorseRoot;
    uint32_t HorseId{};
    std::array<std::shared_ptr<NiAVObject>, PhysicsReferenceUpdate::kMaxChildBodies> Nodes;
    std::array<std::shared_ptr<void>, PhysicsReferenceUpdate::kMaxChildBodies> Lifetimes;
    std::array<uint32_t, PhysicsReferenceUpdate::kMaxChildBodies> Uids{};
    std::array<bool, PhysicsReferenceUpdate::kMaxChildBodies> Simulated{};
    void* Tether{};
    size_t HelperSlot{PhysicsReferenceUpdate::kMaxChildBodies};
    size_t Count{};
    bool Complete{};
    uint64_t ActiveUntil{};
    uint64_t NextLog{};
    uint64_t NextSkipLog{};
    // Replay: last cell/world reconciliation of the cart reference (E19799).
    uint64_t NextReconcileMs{};
    glm::vec3 ReconciledAt{};
};
std::mutex s_assembliesLock;
std::unordered_map<uint32_t, CartAssembly> s_assemblies;
std::unordered_map<uint32_t, uint32_t> s_horseVehicles;
// Mirror of s_horseVehicles' keys readable without s_assembliesLock (the SetPosition hook runs on engine
// threads while our own code may hold that lock around position writes).
std::array<std::atomic<uint32_t>, 16> s_tetheredHorses{};
// Main thread publishes each tethered horse's expected reference z (controller z + learned offset); the native
// placement hook (engine job threads) reads it without touching engine systems. NaN = unknown.
std::array<std::atomic<float>, 16> s_tetheredHorseZ{};

void SetTetheredHorse(uint32_t aFormId, bool aTethered) noexcept
{
    for (auto& slot : s_tetheredHorses)
        if (slot.load(std::memory_order_relaxed) == aFormId)
        {
            if (!aTethered)
                slot.store(0, std::memory_order_release);
            return;
        }
    if (!aTethered)
        return;
    for (size_t i = 0; i < s_tetheredHorses.size(); ++i)
    {
        uint32_t expected = 0;
        s_tetheredHorseZ[i].store(std::numeric_limits<float>::quiet_NaN(), std::memory_order_relaxed);
        if (s_tetheredHorses[i].compare_exchange_strong(expected, aFormId, std::memory_order_acq_rel))
            return;
    }
    spdlog::warn("Tether: horse table full ({} slots); horse {:X} keeps the native path", s_tetheredHorses.size(), aFormId);
}
std::unordered_map<void*, uint32_t> s_tetherVehicles;
std::vector<uint32_t> s_newAssemblies, s_assembliesRefreshing;
std::atomic<uint64_t> s_assemblyTick{};
// Same presentation time as s_assemblyTick, unrounded (ms with the fraction). The cart replay reads it when the
// cart curve switch is on: a whole-millisecond clock moves the interpolation fraction in steps of about 6% of a
// 16 ms frame, which shows as small speed changes.
std::atomic<double> s_assemblyTimeMs{};
// Cart curve (follower replay): position between two owner samples follows a cubic that also matches the owner
// body's velocity at both samples (Hermite), on the unrounded clock. Straight lines change speed at every sample
// because each sample's tick carries timing noise (see s_hermitePlaybackEnabled, the older path's version).
std::atomic<bool> s_cartCurve{false};

// Motion trace (test bridge motion_trace): per main frame, the rendered world position and heading of chosen
// references (a named bone for actors when present, else the root node). Compared between the PCs to measure
// frame-level stutter and spins that 100 ms samples cannot show. Main thread only.
struct MotionTraceRecord
{
    double SteadyMs;
    double SharedMs;
    uint32_t FormId;
    float X, Y, Z, Heading, ReferenceZ;
    uint32_t PlayerId; // party player id when traced as a player ("players"), else 0
    float RefX, RefY, RefZ;    // reference (actor) position
    float RootX, RootY, RootZ; // 3D root node world position (differs from the reference in furniture/paired poses)
};
std::atomic<bool> s_motionTraceOn{false};
std::vector<uint32_t> s_motionTraceIds;
std::string s_motionTraceBone;
// "players" in the ids: also trace every player character (the local one and each remote copy), tagged with its party
// player id so one player can be compared across PCs (e.g. the follower walked to the chopping block).
bool s_motionTracePlayers{};
std::vector<MotionTraceRecord> s_motionTrace;
std::mutex s_motionTraceLock;
constexpr size_t kMotionTraceMax = 400000;

void RecordMotionTrace(World& aWorld) noexcept
{
    if (!s_motionTraceOn.load(std::memory_order_relaxed) || s_motionTrace.size() >= kMotionTraceMax)
        return;
    std::lock_guard lock(s_motionTraceLock);
    const double steady = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    const double shared = SmoothClock::NowMs();
    static BSFixedString s_bone("");
    static std::string s_boneName;
    if (s_boneName != s_motionTraceBone)
    {
        s_boneName = s_motionTraceBone;
        s_bone = BSFixedString(s_boneName.c_str());
    }
    std::vector<std::pair<uint32_t, uint32_t>> targets; // form id, player id
    for (const auto id : s_motionTraceIds)
        targets.emplace_back(id, 0);
    if (s_motionTracePlayers)
    {
        targets.emplace_back(0x14, aWorld.GetTransport().GetLocalPlayerId());
        auto players = aWorld.view<FormIdComponent, PlayerComponent>();
        for (auto entity : players)
            if (players.get<FormIdComponent>(entity).Id != 0x14)
                targets.emplace_back(players.get<FormIdComponent>(entity).Id, players.get<PlayerComponent>(entity).Id);
    }
    for (const auto& [id, playerId] : targets)
    {
        auto* pReference = Cast<TESObjectREFR>(TESForm::GetById(id));
        NiAVObject* pNode = pReference ? pReference->GetNiNode() : nullptr;
        if (!pNode)
            continue;
        const auto rootWorld = pNode->world.translate;
        if (!s_boneName.empty() && Cast<Actor>(pReference))
            if (auto* pBone = pNode->GetByName(s_bone))
                pNode = pBone;
        const auto& w = pNode->world;
        // Local +Y is forward: world forward is the rotation's second column.
        const float heading = std::atan2(w.rotate.entry[0][1], w.rotate.entry[1][1]) * 57.2957795f;
        s_motionTrace.push_back({steady, shared, id, w.translate.x, w.translate.y, w.translate.z, heading,
            pReference->rotation.z * 57.2957795f, playerId, pReference->position.x, pReference->position.y,
            pReference->position.z, rootWorld.x, rootWorld.y, rootWorld.z});
    }
}

std::atomic<bool> s_assemblyFollower{};
std::atomic<void*> s_tetherVtable{};
uint64_t s_assemblyEpoch{}; // main only
struct VehicleActorPose
{
    uint32_t FormId{}, VehicleId{};
    std::shared_ptr<NiAVObject> Root, VehicleRoot;
    uint64_t Tick{}, QueuedAt{}, PlacedAt{};
    NiPoint3 Position{}, Rotation{}, Velocity{}, Angular{};
};
std::unordered_map<uint32_t, VehicleActorPose> s_vehicleActorPoses;
std::vector<VehicleActorPose> s_vehicleActorsApplying; // main only, reused capacity
std::mutex s_retiredAssemblyNodesLock;
std::vector<NiAVObject*> s_retiredAssemblyNodes, s_assemblyNodesDraining;

std::shared_ptr<NiAVObject> HoldAssemblyNode(NiAVObject* apNode)
{
    apNode->IncRef();
    return {apNode, [](NiAVObject* node)
    {
        std::lock_guard lock(s_retiredAssemblyNodesLock);
        s_retiredAssemblyNodes.push_back(node);
    }};
}

void* AssemblyBody(NiAVObject* apNode) noexcept
{
    if (!apNode || !apNode->collisionObject)
        return nullptr;
    // E77883 checks AsBhkNiCollisionObject before E20014 checks wrapper RTTI.
    using AsCollision = void* (*)(void*);
    auto** vt = *reinterpret_cast<AsCollision**>(apNode->collisionObject);
    auto* collision = vt[0x12](apNode->collisionObject);
    if (!collision)
        return nullptr;
    using GetBody = void*(void*);
    POINTER_SKYRIMSE(GetBody, getBody, 20014);
    auto* wrapper = getBody.Get()(collision);
    void* body{};
    return wrapper && ReadPhysicsMemory(static_cast<uint8_t*>(wrapper) + 0x10, body) ? body : nullptr;
}

using TetherInitFn = void(void*, NiAVObject*, NiAVObject*);
TetherInitFn* s_originalTetherInit{};
void HookTetherInit(void* apTether, NiAVObject* apCart, NiAVObject* apHorse)
{
    s_originalTetherInit(apTether, apCart, apHorse);
    s_tetherVtable.store(*static_cast<void**>(apTether), std::memory_order_release);
    auto* cart = apCart ? static_cast<TESObjectREFR*>(apCart->userData) : nullptr;
    auto* horse = apHorse ? Cast<Actor>(static_cast<TESForm*>(apHorse->userData)) : nullptr;
    if (!cart || !horse || cart->GetNiNode() != apCart || horse->GetNiNode() != apHorse)
        return;
    CartAssembly assembly;
    assembly.Root = HoldAssemblyNode(apCart);
    assembly.HorseRoot = HoldAssemblyNode(apHorse);
    assembly.HorseId = horse->formID;
    assembly.Tether = apTether;
    assembly.Complete = true;
    size_t visited = 0;
    auto* rootBody = AssemblyBody(apCart);
    std::array<void*, PhysicsReferenceUpdate::kMaxChildBodies> bodies{};
    auto walk = [&](auto&& self, NiAVObject* node, unsigned depth) -> void
    {
        if (!node || !assembly.Complete)
            return;
        if (++visited > 256 || depth > 32)
        {
            assembly.Complete = false;
            return;
        }
        auto* body = AssemblyBody(node);
        if (body && body != rootBody &&
            std::find(bodies.begin(), bodies.begin() + assembly.Count, body) == bodies.begin() + assembly.Count)
        {
            if (assembly.Count == bodies.size())
            {
                assembly.Complete = false;
                return;
            }
            bodies[assembly.Count] = body;
            ActorPoseDiagnosticViews::RigidBody state{};
            ReadPhysicsMemory(body, state);
            assembly.Simulated[assembly.Count] = IsDynamicMotion(state.motionType);
            if (state.motionType == 4)
            {
                // E78162 queries the saved dynamic mass for a parked body. Native
                // massless helpers remain native; restorable parked wheels are ours.
                void* wrapper{};
                ReadPhysicsMemory(static_cast<uint8_t*>(node->collisionObject) + 0x20, wrapper);
                using GetMass = float(void*);
                POINTER_SKYRIMSE(GetMass, mass, 78162);
                assembly.Simulated[assembly.Count] = wrapper && mass.Get()(wrapper) > 0.f;
            }
            assembly.Nodes[assembly.Count++] = HoldAssemblyNode(node);
        }
        if (auto* branch = node->AsNode())
            for (uint16_t i = 0; i < branch->children.length && assembly.Complete; ++i)
                self(self, branch->children.data[i], depth + 1);
    };
    walk(walk, apCart, 0);
    // The tether helper is attached to HorseSpine2, outside the cart tree (E25740).
    // Stream it explicitly as the final structural slot. It retains its native
    // motion type; we never keyframe a simulated cart body or convert the massless helper.
    NiAVObject* helperNode{};
    ReadNativeMemory(static_cast<uint8_t*>(apTether) + 0x10, helperNode);
    if (assembly.Count == assembly.Nodes.size() || !helperNode || helperNode->collisionObject != apTether)
        assembly.Complete = false;
    else
    {
        assembly.HelperSlot = assembly.Count;
        assembly.Nodes[assembly.Count++] = HoldAssemblyNode(helperNode);
    }
    std::lock_guard lock(s_assembliesLock);
    if (auto it = s_assemblies.find(cart->formID); it != s_assemblies.end())
    {
        s_horseVehicles.erase(it->second.HorseId);
        SetTetheredHorse(it->second.HorseId, false);
        s_tetherVehicles.erase(it->second.Tether);
    }
    s_horseVehicles[horse->formID] = cart->formID;
    SetTetheredHorse(horse->formID, true);
    spdlog::info("Tether: horse {:X} hitched to cart {:X}", horse->formID, cart->formID);
    s_tetherVehicles[apTether] = cart->formID;
    s_newAssemblies.push_back(cart->formID);
    s_assemblies.insert_or_assign(cart->formID, std::move(assembly));
}

using TetherSyncFn = void(void*, void*);
TetherSyncFn* s_originalTetherSync{};
void HookTetherSync(void* apCollision, void* apUpdate)
{
    // Always run the native tether sync (E25737): it keeps the massless helper (E25740, attached to
    // HorseSpine2) on the horse. Skipping it on followers and steering the helper from the cart-relative
    // stream let every cart spike drag the helper, and through the tether the horse's front, far from the
    // horse (owner report 2026-09-27: "front of the horses stretched across the map").
    s_originalTetherSync(apCollision, apUpdate);
}

// E20318 writes the passenger node BEFORE the hooked reference setters. Its native
// QCanUpdateSync gate has no network ownership policy, so gate the entire update.
using PassengerUpdateFn = void(void*, float);
PassengerUpdateFn* s_originalPassengerUpdate{};
void HookPassengerUpdate(void* apController, float aTime)
{
    // Always run the native passenger controller (E20318): it seats each passenger on the LOCAL cart every
    // frame, as on the host. Gating it for remote passengers and placing them from the owner's world
    // positions separated them from the separately steered cart (owner report 2026-09-27: passengers "all
    // over the place" on the follower). Baseline a4efad6a behavior.
    s_originalPassengerUpdate(apController, aTime);
}

static TiltedPhoques::Initializer s_cartAssemblyHooks([]()
{
    POINTER_SKYRIMSE(TetherInitFn, tether, 25740);
    s_originalTetherInit = tether.Get();
    TP_HOOK(&s_originalTetherInit, HookTetherInit);
    POINTER_SKYRIMSE(PassengerUpdateFn, passenger, 20318);
    s_originalPassengerUpdate = passenger.Get();
    TP_HOOK(&s_originalPassengerUpdate, HookPassengerUpdate);
    POINTER_SKYRIMSE(TetherSyncFn, sync, 78179);
    s_originalTetherSync = sync.Get();
    TP_HOOK(&s_originalTetherSync, HookTetherSync);
});

void RestoreBodyMotion(NiAVObject* apNode) noexcept
{
    // E78182 / 0x14105D530: change only this collision body and its sync flag.
    // Request generic dynamic (1), retaining existing sphere/box/thin-box inertia.
    using ChangeMotion = void(void*, uint32_t, void*, bool);
    POINTER_SKYRIMSE(ChangeMotion, change, 78182);
    if (apNode && apNode->collisionObject)
        change.Get()(apNode->collisionObject, 1u, nullptr, false);
}

// Replay: keyframed (4) through the same E78182 path, so local gravity, joints, the tether and seated
// passengers cannot push the copy. RestoreBodyMotion returns it to dynamic when replay ends.
void KeyframeBodyMotion(NiAVObject* apNode) noexcept
{
    using ChangeMotion = void(void*, uint32_t, void*, bool);
    POINTER_SKYRIMSE(ChangeMotion, change, 78182);
    if (apNode && apNode->collisionObject)
        change.Get()(apNode->collisionObject, 4u, nullptr, false);
}

// Motion switches requested while ApplyRemotePhysics holds m_remotePhysicsLock, s_assembliesLock and a
// world lock. E78182 removes and re-adds the body, which can call back into our listeners on this thread
// and re-lock a held mutex: the follower froze at the first cart keyframe (run 223229, Application Hang).
// They run at the start of the next ApplyRemotePhysics, before any of our locks are taken.
std::vector<std::shared_ptr<NiAVObject>> s_pendingKeyframes; // main thread only
// Restore guarantee (Muse refute-cartreplay): every node replay keyframed, and the nodes replayed this pass.
// Any keyframed node not replayed in a pass goes back to dynamic at the start of the next, which covers every
// exit (toggle off, disconnect, epoch/leader change, assembly or pose erase, stream stall) in one place.
std::unordered_map<NiAVObject*, std::shared_ptr<NiAVObject>> s_replayKeyframed; // main thread only
std::unordered_set<NiAVObject*> s_replayedThisPass, s_replayedLastPass;           // main thread only

// Replay moves the keyframed cart through its scene nodes: the engine re-keys a keyframed body from its node
// every frame (run 224555: body placements undone each step, carts frozen 5800 u behind), and carries the
// body there with a velocity, so riders and contacts see a moving body. aPosition is Havok units, world space.
void PlaceNodeWorld(NiAVObject* apNode, const glm::vec3& aPosition, const glm::quat& aRotation) noexcept
{
    if (!apNode)
        return;
    const glm::mat3 world = glm::mat3_cast(glm::normalize(aRotation)); // world[col][row]
    const glm::vec3 translate = aPosition * kHavokToGameUnits;
    glm::mat3 parentRotate{1.f};
    glm::vec3 parentTranslate{};
    float parentScale = 1.f;
    if (const auto* parent = apNode->parent)
    {
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j)
                parentRotate[j][i] = parent->world.rotate.entry[i][j];
        parentTranslate = {parent->world.translate.x, parent->world.translate.y, parent->world.translate.z};
        parentScale = parent->world.scale != 0.f ? parent->world.scale : 1.f;
    }
    const glm::mat3 inverseParent = glm::transpose(parentRotate);
    const glm::mat3 local = inverseParent * world;
    const glm::vec3 localTranslate = inverseParent * (translate - parentTranslate) / parentScale;
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            apNode->local.rotate.entry[i][j] = local[j][i];
    apNode->local.translate.x = localTranslate.x;
    apNode->local.translate.y = localTranslate.y;
    apNode->local.translate.z = localTranslate.z;
    struct NiUpdateData { float Time{}; uint32_t Flags{}; } data{};
    using UpdateFn = void(NiAVObject*, NiUpdateData*);
    POINTER_SKYRIMSE(UpdateFn, update, 70251);
    update.Get()(apNode, &data);
}

// Replay keeps the cart REFERENCE with its replayed root, as the engine's Havok-moved path (E19826) does for the
// owner's dynamic cart. Moving only the nodes left the reference at the ride start: ~30 s in, the follower's
// own cart vanished and its passengers froze (run 225016, BB970 stopped being followed at 22:52:12).
// Angles invert the reference rotation used above: body = Rx(-x) * Ry(-y) * Rz(-z).
void SyncReplayReference(TESObjectREFR* apReference, const glm::vec3& aPosition, const glm::quat& aRotation,
    glm::vec3& arReconciledAt, uint64_t& arNextReconcileMs) noexcept
{
    const glm::vec3 translate = aPosition * kHavokToGameUnits;
    const glm::mat3 m = glm::mat3_cast(glm::normalize(aRotation)); // m[col][row]
    const float b = std::asin(std::clamp(m[2][0], -1.f, 1.f));
    const float a = std::atan2(-m[2][1], m[2][2]);
    const float c = std::atan2(-m[1][0], m[0][0]);
    if (!std::isfinite(translate.x) || !std::isfinite(translate.y) || !std::isfinite(translate.z) ||
        !std::isfinite(a) || !std::isfinite(b) || !std::isfinite(c))
        return;
    apReference->position.x = translate.x;
    apReference->position.y = translate.y;
    apReference->position.z = translate.z;
    apReference->rotation.x = -a;
    apReference->rotation.y = -b;
    apReference->rotation.z = -c;
    const auto now = GetTickCount64();
    if (now < arNextReconcileMs && glm::length(translate - arReconciledAt) < 256.f)
        return;
    arNextReconcileMs = now + 500;
    arReconciledAt = translate;
    auto* cell = apReference->GetParentCellEx();
    if (!cell)
        return;
    using Reconcile = void(TESObjectREFR*, TESObjectCELL*, TESWorldSpace*);
    POINTER_SKYRIMSE(Reconcile, reconcile, 19799);
    reconcile.Get()(apReference, cell->worldspace ? nullptr : cell, cell->worldspace);
}

void DrainPendingKeyframes() noexcept
{
    // Restore first: nodes keyframed by replay that the last pass did not replay.
    s_replayedLastPass.swap(s_replayedThisPass);
    s_replayedThisPass.clear();
    for (auto it = s_replayKeyframed.begin(); it != s_replayKeyframed.end();)
    {
        if (s_replayedLastPass.contains(it->first))
        {
            ++it;
            continue;
        }
        auto* node = it->second.get();
        // Review P0-3: a node whose 3D was detached (cart unloaded/reloaded) has no parent: just release it.
        if (!node || !node->parent)
        {
            it = s_replayKeyframed.erase(it);
            continue;
        }
        auto* body = AssemblyBody(node);
        ActorPoseDiagnosticViews::RigidBody state{};
        if (body && ReadPhysicsMemory(body, state) && state.world && state.motionType == 4)
        {
            ScopedPhysicsWorld worldLock(state.world);
            if (worldLock.Lock && ReadPhysicsMemory(body, state) && state.motionType == 4)
                RestoreBodyMotion(node);
        }
        spdlog::info("Cart replay: restored a part to dynamic ({} still keyframed)", s_replayKeyframed.size() - 1);
        it = s_replayKeyframed.erase(it);
    }
    auto pending = std::move(s_pendingKeyframes);
    s_pendingKeyframes.clear();
    for (const auto& node : pending)
    {
        auto* body = AssemblyBody(node.get());
        ActorPoseDiagnosticViews::RigidBody state{};
        if (!body || !ReadPhysicsMemory(body, state) || !state.world || !IsDynamicMotion(state.motionType))
            continue;
        ScopedPhysicsWorld worldLock(state.world);
        if (!worldLock.Lock || !ReadPhysicsMemory(body, state) || !IsDynamicMotion(state.motionType))
            continue;
        KeyframeBodyMotion(node.get());
        s_replayKeyframed.try_emplace(node.get(), node);
        // E78182 clears kSetLocal (collision flags +0x18, bit 8) for keyframed motion, so the engine then
        // copies the static node into the body every frame and our drive was undone (run 223811: 45 placements
        // in 45 steps, carts 8000 u behind at zero velocity). Set it back: the body stays keyframed (nothing
        // local can push it) and the node follows the driven body, as it follows a dynamic body on the owner.
        // RestoreBodyMotion (E78182 to dynamic) sets the bit again itself.
        if (auto* collision = static_cast<uint8_t*>(static_cast<void*>(node->collisionObject)))
            *reinterpret_cast<uint16_t*>(collision + 0x18) |= 8;
    }
}

// hkTransform rotation columns (transform[0..2], [4..6], [8..10]) to a quaternion (x, y, z, w).
void MatrixToQuaternion(const float* t, float* q) noexcept
{
    const float m00 = t[0], m10 = t[1], m20 = t[2], m01 = t[4], m11 = t[5], m21 = t[6], m02 = t[8], m12 = t[9], m22 = t[10];
    const float trace = m00 + m11 + m22;
    if (trace > 0.f)
    {
        const float s = std::sqrt(trace + 1.f) * 2.f;
        q[3] = 0.25f * s; q[0] = (m21 - m12) / s; q[1] = (m02 - m20) / s; q[2] = (m10 - m01) / s;
    }
    else if (m00 > m11 && m00 > m22)
    {
        const float s = std::sqrt(1.f + m00 - m11 - m22) * 2.f;
        q[3] = (m21 - m12) / s; q[0] = 0.25f * s; q[1] = (m01 + m10) / s; q[2] = (m02 + m20) / s;
    }
    else if (m11 > m22)
    {
        const float s = std::sqrt(1.f + m11 - m00 - m22) * 2.f;
        q[3] = (m02 - m20) / s; q[0] = (m01 + m10) / s; q[1] = 0.25f * s; q[2] = (m12 + m21) / s;
    }
    else
    {
        const float s = std::sqrt(1.f + m22 - m00 - m11) * 2.f;
        q[3] = (m10 - m01) / s; q[0] = (m02 + m20) / s; q[1] = (m12 + m21) / s; q[2] = 0.25f * s;
    }
}

bool GetDynamicBody(TESObjectREFR* apReference, DynamicBody& arBody,
    bool aAllowKeyframed = false, bool aCapture = false) noexcept
{
    const auto read = [aCapture](const void* source, auto& value)
    {
        return aCapture ? ReadPhysicsMemory(source, value) : ReadNativeMemory(source, value);
    };
    auto* pNode = apReference ? apReference->GetNiNode() : nullptr;
    if (!pNode || !pNode->collisionObject)
        return false;
    const auto collision = reinterpret_cast<uintptr_t>(pNode->collisionObject);
    void* pWrapper = nullptr;
    if (collision > std::numeric_limits<uintptr_t>::max() - 0x28 ||
        !read(reinterpret_cast<const void*>(collision + 0x20), pWrapper) || !pWrapper)
        return false;
    const auto wrapper = reinterpret_cast<uintptr_t>(pWrapper);
    void* pHavokBody = nullptr;
    if (wrapper > std::numeric_limits<uintptr_t>::max() - 0x18 ||
        !read(reinterpret_cast<const void*>(wrapper + 0x10), pHavokBody) ||
        !read(pHavokBody, arBody.State) ||
        (!IsDynamicMotion(arBody.State.motionType) &&
            !(aAllowKeyframed && arBody.State.motionType == 4)) ||
        !arBody.State.world)
        return false;
    for (size_t axis = 0; axis < 3; ++axis)
    {
        if (!std::isfinite(arBody.State.linearVelocity[axis]) ||
            std::abs(arBody.State.linearVelocity[axis]) > 100.f ||
            !std::isfinite(arBody.State.transform[12 + axis]) ||
            std::abs(arBody.State.transform[12 + axis]) > 100000.f)
            return false;
    }
    arBody.HavokBody = pHavokBody;
    arBody.Wrapper = pWrapper;
    return true;
}

void RestoreKinematicProbe() noexcept
{
    s_kinematicProbeFormId.store(0, std::memory_order_release);
    s_kinematicOriginalMotionType.store(0, std::memory_order_release);
}

struct ReferencePhaseSample
{
    glm::vec3 Reference{};
    glm::vec3 Node{};
    glm::vec3 Body{};
    bool NodePresent{};
    bool BodyPresent{};
};

ReferencePhaseSample SampleReferencePhase(TESObjectREFR* apReference) noexcept
{
    ReferencePhaseSample sample{};
    if (!apReference)
        return sample;
    sample.Reference = {apReference->position.x, apReference->position.y,
        apReference->position.z};
    if (const auto* pNode = apReference->GetNiNode())
    {
        NiTransform world{};
        if (ReadNativeMemory(&pNode->world, world))
        {
            sample.Node = {world.translate.x, world.translate.y,
                world.translate.z};
            sample.NodePresent = true;
        }
    }
    DynamicBody body{};
    if (GetDynamicBody(apReference, body))
    {
        sample.Body = {body.State.transform[12] * kHavokToGameUnits,
            body.State.transform[13] * kHavokToGameUnits,
            body.State.transform[14] * kHavokToGameUnits};
        sample.BodyPresent = true;
    }
    return sample;
}

void RecordReferencePhase(TESObjectREFR* apReference, uint32_t aMethod,
    const ReferencePhaseSample& acBefore,
    std::chrono::steady_clock::time_point aStarted) noexcept
{
    const auto after = SampleReferencePhase(apReference);
    s_referencePhaseLastMethod.store(aMethod, std::memory_order_relaxed);
    s_referencePhaseThreadId.store(GetCurrentThreadId(),
        std::memory_order_relaxed);
    s_referencePhaseDurationUs.store(static_cast<uint32_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - aStarted).count()),
        std::memory_order_relaxed);
    s_referencePhaseRefDelta.store(glm::length(after.Reference - acBefore.Reference),
        std::memory_order_relaxed);
    if (acBefore.NodePresent && after.NodePresent)
    {
        s_referencePhaseNodeDelta.store(glm::length(after.Node - acBefore.Node),
            std::memory_order_relaxed);
        s_referencePhaseRefNodeError.store(glm::length(after.Reference - after.Node),
            std::memory_order_relaxed);
    }
    if (acBefore.BodyPresent && after.BodyPresent)
    {
        s_referencePhaseBodyDelta.store(glm::length(after.Body - acBefore.Body),
            std::memory_order_relaxed);
        s_referencePhaseRefBodyError.store(glm::length(after.Reference - after.Body),
            std::memory_order_relaxed);
    }
}

void HookUpdate3DPosition(TESObjectREFR* apReference, bool aWarp)
{
    const bool watched = apReference && apReference->formID ==
        s_referencePhaseFormId.load(std::memory_order_relaxed);
    const auto started = std::chrono::steady_clock::now();
    const auto before = watched ? SampleReferencePhase(apReference) :
        ReferencePhaseSample{};
    if (s_originalUpdate3DPosition)
        s_originalUpdate3DPosition(apReference, aWarp);
    if (watched)
    {
        s_referenceUpdate3DCalls.fetch_add(1, std::memory_order_relaxed);
        RecordReferencePhase(apReference, 1, before, started);
    }
}

void HookMoveHavok(TESObjectREFR* apReference, bool aForceRec)
{
    const bool watched = apReference && apReference->formID ==
        s_referencePhaseFormId.load(std::memory_order_relaxed);
    const auto started = std::chrono::steady_clock::now();
    const auto before = watched ? SampleReferencePhase(apReference) :
        ReferencePhaseSample{};
    if (s_originalMoveHavok)
        s_originalMoveHavok(apReference, aForceRec);
    if (watched)
    {
        s_referenceMoveHavokCalls.fetch_add(1, std::memory_order_relaxed);
        RecordReferencePhase(apReference, 2, before, started);
    }
}

void MaybeInstallReferencePhaseHook(TESObjectREFR* apReference) noexcept
{
    if (!apReference || !apReference->loadedState ||
        apReference->formID != s_referencePhaseFormId.load(
            std::memory_order_acquire))
        return;
    void** pVtable{};
    if (!ReadNativeMemory(apReference, pVtable) || !pVtable)
        return;
    if (s_referencePhaseVtable.load(std::memory_order_acquire) == pVtable)
        return;
    // This diagnostic hooks one selected reference type per game process.
    // Never replace originals after a vtable has already been intercepted.
    if (s_referencePhaseVtable.load(std::memory_order_acquire))
        return;
    if (!ReadNativeMemory(pVtable + 0x3F, s_originalUpdate3DPosition) ||
        !ReadNativeMemory(pVtable + 0x85, s_originalMoveHavok) ||
        !s_originalUpdate3DPosition || !s_originalMoveHavok)
        return;
    s_referencePhaseVtable.store(pVtable, std::memory_order_release);
    const auto oldUpdate = TiltedPhoques::HookVTable(
        apReference, 0x3F, &HookUpdate3DPosition);
    const auto oldMove = TiltedPhoques::HookVTable(
        apReference, 0x85, &HookMoveHavok);
    s_referencePhaseInstalled.store(oldUpdate == s_originalUpdate3DPosition &&
        oldMove == s_originalMoveHavok, std::memory_order_release);
}

struct NodePhaseSample
{
    glm::vec3 Local{};
    glm::vec3 World{};
    glm::vec3 Reference{};
    bool Readable{};
    bool ReferenceReadable{};
};

NodePhaseSample SampleNodePhase(NiAVObject* apNode) noexcept
{
    NodePhaseSample sample{};
    std::array<NiTransform, 2> transforms{};
    if (!apNode || !ReadNativeMemory(&apNode->local, transforms))
        return sample;
    sample.Local = {transforms[0].translate.x, transforms[0].translate.y,
        transforms[0].translate.z};
    sample.World = {transforms[1].translate.x, transforms[1].translate.y,
        transforms[1].translate.z};
    sample.Readable = true;
    auto* pReference = s_watchedReference.load(std::memory_order_acquire);
    NiPoint3 position{};
    if (pReference &&
        s_watchedReferenceNode.load(std::memory_order_acquire) == apNode &&
        ReadNativeMemory(&pReference->position, position))
    {
        sample.Reference = {position.x, position.y, position.z};
        sample.ReferenceReadable = true;
    }
    return sample;
}

void RecordNodePhase(NiAVObject* apNode, uint32_t aMethod,
    const NodePhaseSample& acBefore,
    std::chrono::steady_clock::time_point aStarted,
    uintptr_t aCallerRva) noexcept
{
    const auto after = SampleNodePhase(apNode);
    s_nodeLastMethod.store(aMethod, std::memory_order_relaxed);
    s_nodeLastThreadId.store(GetCurrentThreadId(), std::memory_order_relaxed);
    s_nodeLastDurationUs.store(static_cast<uint32_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - aStarted).count()),
        std::memory_order_relaxed);
    if (acBefore.Readable && after.Readable)
    {
        s_nodeLastLocalDelta.store(glm::length(after.Local - acBefore.Local),
            std::memory_order_relaxed);
        s_nodeLastWorldDelta.store(glm::length(after.World - acBefore.World),
            std::memory_order_relaxed);
    }
    if (acBefore.ReferenceReadable && after.ReferenceReadable)
    {
        const float refDelta = glm::length(after.Reference - acBefore.Reference);
        s_nodeLastReferenceDelta.store(refDelta,
            std::memory_order_relaxed);
        s_nodeLastRefNodeError.store(
            glm::length(after.Reference - after.World),
            std::memory_order_relaxed);
        if (aMethod == 2)
        {
            s_nodeWorldDataRefDelta.store(refDelta, std::memory_order_relaxed);
            s_nodeWorldDataCallerRva.store(aCallerRva,
                std::memory_order_relaxed);
        }
        else if (aMethod == 3)
        {
            s_nodeTransformRefDelta.store(refDelta, std::memory_order_relaxed);
            s_nodeTransformCallerRva.store(aCallerRva,
                std::memory_order_relaxed);
        }
    }
}

uintptr_t SkyrimCallerRva(void* apReturnAddress) noexcept
{
    const auto base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    const auto caller = reinterpret_cast<uintptr_t>(apReturnAddress);
    return caller >= base && caller - base < 0x10000000 ?
        caller - base : 0;
}

void HookNodeDownward(NiAVObject* apNode, void* apUpdateData, uint32_t aFlags)
{
    const auto callerRva = SkyrimCallerRva(_ReturnAddress());
    const bool watched = apNode == s_watchedReferenceNode.load(
        std::memory_order_relaxed);
    const auto started = std::chrono::steady_clock::now();
    const auto before = watched ? SampleNodePhase(apNode) : NodePhaseSample{};
    if (s_originalNodeDownward)
        s_originalNodeDownward(apNode, apUpdateData, aFlags);
    if (watched)
    {
        s_nodeDownwardCalls.fetch_add(1, std::memory_order_relaxed);
        RecordNodePhase(apNode, 1, before, started, callerRva);
    }
}

void HookNodeWorldData(NiAVObject* apNode, void* apUpdateData)
{
    const auto callerRva = SkyrimCallerRva(_ReturnAddress());
    const bool watched = apNode == s_watchedReferenceNode.load(
        std::memory_order_relaxed);
    const auto started = std::chrono::steady_clock::now();
    const auto before = watched ? SampleNodePhase(apNode) : NodePhaseSample{};
    if (s_originalNodeWorldData)
        s_originalNodeWorldData(apNode, apUpdateData);
    if (watched)
    {
        s_nodeWorldDataCalls.fetch_add(1, std::memory_order_relaxed);
        RecordNodePhase(apNode, 2, before, started, callerRva);
    }
}

void HookNodeTransformBounds(NiAVObject* apNode, void* apUpdateData)
{
    const auto callerRva = SkyrimCallerRva(_ReturnAddress());
    const bool watched = apNode == s_watchedReferenceNode.load(
        std::memory_order_relaxed);
    const auto started = std::chrono::steady_clock::now();
    const auto before = watched ? SampleNodePhase(apNode) : NodePhaseSample{};
    if (s_originalNodeTransformBounds)
        s_originalNodeTransformBounds(apNode, apUpdateData);
    if (watched)
    {
        s_nodeTransformBoundsCalls.fetch_add(1, std::memory_order_relaxed);
        RecordNodePhase(apNode, 3, before, started, callerRva);
    }
}

std::mutex s_movingReferencesLock;
PhysicsScan::MovementQueue<4096> s_movingReferences;
std::atomic<bool> s_collectMovingReferences{};
std::atomic<uint64_t> s_movementOverflows{};

using CollisionSyncFn = void(void*, uint32_t);
CollisionSyncFn* s_originalCollisionSync{};

void HookCollisionSync(void* apCollision, uint32_t aFlags)
{
    const auto callerRva = SkyrimCallerRva(_ReturnAddress());
    NiAVObject* pNode{};
    auto* pReference = s_watchedReference.load(std::memory_order_relaxed);
    const bool watched = apCollision && pReference &&
        s_referencePhaseFormId.load(std::memory_order_relaxed) != 0 &&
        ReadNativeMemory(reinterpret_cast<const uint8_t*>(apCollision) + 0x10,
            pNode) &&
        pNode == s_watchedReferenceNode.load(std::memory_order_relaxed);
    const auto started = std::chrono::steady_clock::now();
    const auto before = watched ? SampleReferencePhase(pReference) :
        ReferencePhaseSample{};
    if (s_originalCollisionSync)
        s_originalCollisionSync(apCollision, aFlags);
    // 19826 itself resolves this node with 19750 in this same native sync phase.
    // Publish only identity; discovery/3D capture stays on the main thread.
    if (s_collectMovingReferences.load(std::memory_order_relaxed))
    {
        NiAVObject* node{};
        if (ReadPhysicsMemory(static_cast<uint8_t*>(apCollision) + 0x10, node) && node)
        {
            using FindReferenceFn = TESObjectREFR*(NiAVObject*);
            POINTER_SKYRIMSE(FindReferenceFn, findReference, 19750);
            auto* reference = findReference.Get()(node);
            if (reference && !Cast<Actor>(reference))
            {
                std::lock_guard movementLock(s_movingReferencesLock);
                if (!s_movingReferences.Insert(reference->formID))
                    s_movementOverflows.fetch_add(1, std::memory_order_relaxed);
            }
        }
    }
    if (!watched)
        return;
    const auto after = SampleReferencePhase(pReference);
    s_collisionSyncSelectedCalls.fetch_add(1, std::memory_order_relaxed);
    s_collisionSyncLastCallerRva.store(callerRva, std::memory_order_relaxed);
    s_collisionSyncLastThreadId.store(GetCurrentThreadId(),
        std::memory_order_relaxed);
    s_collisionSyncLastDurationUs.store(static_cast<uint32_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - started).count()),
        std::memory_order_relaxed);
    s_collisionSyncLastReferenceDelta.store(
        glm::length(after.Reference - before.Reference),
        std::memory_order_relaxed);
    if (before.NodePresent && after.NodePresent)
    {
        s_collisionSyncLastNodeDelta.store(
            glm::length(after.Node - before.Node), std::memory_order_relaxed);
        s_collisionSyncLastPreNodeReferenceError.store(
            glm::length(before.Node - before.Reference),
            std::memory_order_relaxed);
        s_collisionSyncLastPostNodeReferenceError.store(
            glm::length(after.Node - after.Reference),
            std::memory_order_relaxed);
    }
    if (before.BodyPresent && after.BodyPresent)
        s_collisionSyncLastBodyDelta.store(
            glm::length(after.Body - before.Body), std::memory_order_relaxed);
}

static TiltedPhoques::Initializer s_collisionSyncHook(
    []()
    {
        POINTER_SKYRIMSE(CollisionSyncFn, s_collisionSync, 19826);
        s_originalCollisionSync = s_collisionSync.Get();
        TP_HOOK(&s_originalCollisionSync, HookCollisionSync);
    });

using CollisionWorldFn = void(void*, const NiTransform*);
CollisionWorldFn* s_originalCollisionWorld{};

using WorldUpdateFn = bool(void*, uint32_t);
WorldUpdateFn* s_originalWorldUpdate{};

using NativeStepFn = int(void*, float);
NativeStepFn* s_originalNativeStep{};

// Publish targets on the main frame and steer dynamic copies on the native stepping thread.
struct StepTarget
{
    void* World{};
    void* Body{};
    uint32_t FormId{};
    float Position[3]{};  // Havok units, where the body must be after the step
    float Velocity[3]{};  // Havok units per second
    // Dynamic follow: steer a simulated body to this pose instead of placing it.
    bool Dynamic{};
    bool Assembly{};
    bool NativeHelper{};
    // Keyframed replay: drive exactly onto the owner pose (no caps), body is motion type 4.
    bool Replay{};
    float Rotation[4]{0.f, 0.f, 0.f, 1.f}; // x, y, z, w
    float Angular[3]{};   // radians per second
    uint32_t BodyUid{};
    uint64_t HostAgeMs{};
    std::chrono::steady_clock::time_point PublishedAt{};
    // Native reference keeps a removed/replaced body alive until the last target retires.
    std::shared_ptr<void> Lifetime;
};

// Constraint suppression was speculative. Keep all native joints enabled until paired
// solver/contact evidence identifies the competing driver; never detach a held item.
using HavokReferenceFn = void(void*);
POINTER_SKYRIMSE(HavokReferenceFn, s_holdBody, 57010);
POINTER_SKYRIMSE(HavokReferenceFn, s_releaseBody, 57011);

// A disconnect callback can destroy the final C++ owner on the VM worker. Defer
// the native release to main, including removed bodies whose world no longer steps.
std::mutex s_retiredBodiesLock;
std::vector<void*> s_retiredBodies;
std::vector<void*> s_retiredBodiesDraining; // main only, capacity reused
void RetireBody(void* apBody)
{
    std::lock_guard lock(s_retiredBodiesLock);
    s_retiredBodies.push_back(apBody);
}
void DrainRetiredBodies()
{
    {
        std::lock_guard lock(s_retiredBodiesLock);
        s_retiredBodies.swap(s_retiredBodiesDraining);
    }
    for (auto* body : s_retiredBodiesDraining)
    {
        ActorPoseDiagnosticViews::RigidBody state{};
        ReadPhysicsMemory(body, state); // native reference still held, cannot be recycled
        ScopedPhysicsWorld worldLock(state.world);
        s_releaseBody.Get()(body);
    }
    s_retiredBodiesDraining.clear();
}

// Dynamic follow: a simulated body reaches the host's pose within this time constant (seconds);
// farther than this (Havok units) it is placed there at once.
constexpr float kFollowTimeConstant = 0.1f;
constexpr float kFollowTeleport = 3.f;

// Solver writes numeric telemetry; the main frame formats and logs it.
struct FollowProbe
{
    uint32_t FormId{};
    float Gap{}, Speed{};
    uint64_t HostAgeMs{};
    uint32_t Steps{};
    uint32_t Teleports{};
    uint32_t KeyframedPlacements{};
    float MaxErrorUnits{};
    std::chrono::steady_clock::time_point NextLog{};
    std::chrono::steady_clock::time_point NextSteerLog{};
};
std::unordered_map<void*, FollowProbe> s_followProbes;
std::mutex s_stepTargetsLock;
std::vector<StepTarget> s_stepTargets;
std::vector<StepTarget> s_stepTargetsBuilding;
bool s_followProbesDirty{}; // Protected by s_stepTargetsLock.
std::chrono::steady_clock::time_point s_nextFollowReport{}; // Same lock; telemetry only.

// BEGIN FOLLOW PROBE PRUNE
template <class TProbes, class TTargets>
void PruneFollowProbes(TProbes& aProbes, const TTargets& acTargets, bool& aDirty)
{
    if (!aDirty)
        return;
    std::unordered_set<void*> bodies;
    bodies.reserve(acTargets.size());
    for (const auto& target : acTargets)
        bodies.insert(target.Body);
    std::erase_if(aProbes, [&](const auto& entry) { return !bodies.contains(entry.first); });
    aDirty = false;
}
// END FOLLOW PROBE PRUNE

struct PreStepTarget
{
    uint32_t FormId{};
    uint64_t Epoch{};
    uint64_t Tick{};
    uint64_t ReceivedNs{};
    glm::vec3 Position{};
    glm::vec3 Velocity{};
    glm::quat Quaternion{1.f, 0.f, 0.f, 0.f};
};

void PublishPreStepTarget(uint32_t aFormId, uint64_t aEpoch, uint64_t aTick,
    const PhysicsReferenceUpdate& acUpdate) noexcept
{
    if (aFormId != s_preStepPlaybackFormId.load(std::memory_order_acquire))
        return;
    const auto receivedNs = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
    std::lock_guard lock{s_preStepTargetPublishMutex};
    s_preStepTargetSequence.fetch_add(1, std::memory_order_acq_rel);
    s_preStepTargetFormId.store(aFormId, std::memory_order_relaxed);
    s_preStepTargetEpoch.store(aEpoch, std::memory_order_relaxed);
    s_preStepTargetTick.store(aTick, std::memory_order_relaxed);
    s_preStepTargetReceivedNs.store(receivedNs, std::memory_order_relaxed);
    const float velocity[3]{acUpdate.LinearVelocity.x,
        acUpdate.LinearVelocity.y, acUpdate.LinearVelocity.z};
    const auto& t = acUpdate.BodyTransform;
    const glm::mat3 rotation{glm::vec3{t[0], t[1], t[2]},
        glm::vec3{t[4], t[5], t[6]},
        glm::vec3{t[8], t[9], t[10]}};
    const glm::quat quaternion = glm::normalize(glm::quat_cast(rotation));
    for (size_t axis = 0; axis < 3; ++axis)
    {
        s_preStepTargetPosition[axis].store(acUpdate.BodyTransform[12 + axis],
            std::memory_order_relaxed);
        s_preStepTargetVelocity[axis].store(velocity[axis],
            std::memory_order_relaxed);
    }
    s_preStepTargetQuaternion[0].store(quaternion.x, std::memory_order_relaxed);
    s_preStepTargetQuaternion[1].store(quaternion.y, std::memory_order_relaxed);
    s_preStepTargetQuaternion[2].store(quaternion.z, std::memory_order_relaxed);
    s_preStepTargetQuaternion[3].store(quaternion.w, std::memory_order_relaxed);
    s_preStepTargetSequence.fetch_add(1, std::memory_order_release);
    s_preStepPublishedTargets.fetch_add(1, std::memory_order_relaxed);
}

bool ReadPreStepTarget(PreStepTarget& arTarget) noexcept
{
    for (int attempt = 0; attempt < 2; ++attempt)
    {
        const auto before = s_preStepTargetSequence.load(
            std::memory_order_acquire);
        if (before & 1)
            continue;
        arTarget.FormId = s_preStepTargetFormId.load(std::memory_order_relaxed);
        arTarget.Epoch = s_preStepTargetEpoch.load(std::memory_order_relaxed);
        arTarget.Tick = s_preStepTargetTick.load(std::memory_order_relaxed);
        arTarget.ReceivedNs = s_preStepTargetReceivedNs.load(
            std::memory_order_relaxed);
        arTarget.Position = {s_preStepTargetPosition[0].load(
                std::memory_order_relaxed),
            s_preStepTargetPosition[1].load(std::memory_order_relaxed),
            s_preStepTargetPosition[2].load(std::memory_order_relaxed)};
        arTarget.Velocity = {s_preStepTargetVelocity[0].load(
                std::memory_order_relaxed),
            s_preStepTargetVelocity[1].load(std::memory_order_relaxed),
            s_preStepTargetVelocity[2].load(std::memory_order_relaxed)};
        arTarget.Quaternion = glm::quat{
            s_preStepTargetQuaternion[3].load(std::memory_order_relaxed),
            s_preStepTargetQuaternion[0].load(std::memory_order_relaxed),
            s_preStepTargetQuaternion[1].load(std::memory_order_relaxed),
            s_preStepTargetQuaternion[2].load(std::memory_order_relaxed)};
        if (s_preStepTargetSequence.load(std::memory_order_acquire) == before)
            return arTarget.ReceivedNs != 0;
    }
    return false;
}

template <class NativeStep>
int RunNativeStep(void* apWorld, float aDeltaTime, NativeStep&& aNativeStep, bool aAssemblyOnly = false)
{
    const auto started = std::chrono::steady_clock::now();
    ScopedPhysicsWorld worldLock(s_worldUpdateDepth ? apWorld : nullptr);
    glm::vec3 predictedTarget{};
    bool targetSampled = false;
    bool targetApplied = false;
    float velocityAfterWrite = -1.f;
    {
        std::lock_guard lock(s_stepTargetsLock);
        for (const auto& target : s_stepTargets)
        {
            if (aAssemblyOnly && !target.Assembly)
                continue;
            if (!worldLock.Lock || target.World != apWorld || !target.Body)
                continue;
            ActorPoseDiagnosticViews::RigidBody state{};
            if (!ReadNativeMemory(target.Body, state) || state.world != apWorld)
                continue;
            auto* pBody = static_cast<ActorPoseDiagnosticViews::RigidBody*>(target.Body);
            if (target.Dynamic)
            {
                if (pBody->uid != target.BodyUid || (!IsDynamicMotion(pBody->motionType) &&
                    !((target.NativeHelper || target.Replay) && pBody->motionType == 4)) ||
                    !std::isfinite(aDeltaTime) || aDeltaTime <= 0.f)
                    continue;

                // The host's motion plus a correction that closes the gap within the time constant.
                const glm::vec3 current{pBody->transform[12], pBody->transform[13], pBody->transform[14]};
                const glm::vec3 wanted{target.Position[0], target.Position[1], target.Position[2]};
                const glm::vec3 error = wanted - current;
                using TSetPositionAndRotation = void(__fastcall*)(void*, const float*, const float*);
                POINTER_SKYRIMSE(std::remove_pointer_t<TSetPositionAndRotation>, s_placeBody, 60898);
                const auto probeIt = s_followProbes.find(target.Body);
                if (probeIt == s_followProbes.end())
                    continue;
                auto& probe = probeIt->second;
                const float gap = glm::length(error) * kHavokToGameUnits;
                probe.FormId = target.FormId;
                probe.Gap = gap;
                probe.Speed = glm::length(glm::vec3{pBody->linearVelocity[0], pBody->linearVelocity[1],
                    pBody->linearVelocity[2]}) * kHavokToGameUnits;
                const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(started - target.PublishedAt).count();
                probe.HostAgeMs = target.HostAgeMs + static_cast<uint64_t>((std::max)(int64_t{0}, elapsed));
                ++probe.Steps;
                probe.MaxErrorUnits = (std::max)(probe.MaxErrorUnits, gap);
                if (gap > 10.f)
                    s_nextFollowReport = (std::min)(s_nextFollowReport, probe.NextSteerLog);
                if (target.Assembly && target.Replay && pBody->motionType == 4)
                    continue; // keyframed: the engine moves it from the node PlaceNodeWorld set this frame
                if (target.Assembly && target.Replay)
                {
                    // Replay: reach the owner pose exactly at the end of this step. Keyframed bodies take the
                    // drive's velocities as-is, so the engine moves the node from the body as on the owner and
                    // seated riders ride a genuinely moving body. Place only after a gap far beyond one step.
                    if (glm::length(error) * kHavokToGameUnits > 150.f)
                    {
                        alignas(16) float position[4]{wanted.x, wanted.y, wanted.z, 0.f};
                        alignas(16) float quaternion[4]{target.Rotation[0], target.Rotation[1], target.Rotation[2], target.Rotation[3]};
                        s_placeBody.Get()(target.Body, position, quaternion);
                        ++probe.Teleports;
                    }
                    const glm::vec3 feed{target.Velocity[0], target.Velocity[1], target.Velocity[2]};
                    const auto goal = wanted + feed * aDeltaTime;
                    alignas(16) float position[4]{goal.x, goal.y, goal.z, 0.f};
                    glm::quat want{target.Rotation[3], target.Rotation[0], target.Rotation[1], target.Rotation[2]};
                    const glm::vec3 angular{target.Angular[0], target.Angular[1], target.Angular[2]};
                    const float speed = glm::length(angular);
                    if (speed > 0.0001f)
                        want = glm::normalize(glm::angleAxis(speed * aDeltaTime, angular / speed) * want);
                    alignas(16) float rotation[4]{want.x, want.y, want.z, want.w};
                    using Drive = void(const float*, const float*, float, void*);
                    POINTER_SKYRIMSE(Drive, drive, 62478);
                    drive.Get()(position, rotation, 1.f / aDeltaTime, target.Body);
                    if (target.Body == s_watchedHavokBody.load(std::memory_order_acquire))
                    {
                        targetApplied = targetSampled = true;
                        predictedTarget = wanted;
                        velocityAfterWrite = glm::length(glm::vec3{pBody->linearVelocity[0],
                            pBody->linearVelocity[1], pBody->linearVelocity[2]});
                    }
                    continue;
                }
                if (target.Assembly)
                {
                    // E62478 is a COM-aware velocity drive, NOT a motion-type change.
                    // All assembly members use the same horizon. Never teleport one
                    // constrained member across its neighbours after a packet/capture stall.
                    const glm::vec3 feed{target.Velocity[0], target.Velocity[1], target.Velocity[2]};
                    glm::vec3 correction = error;
                    const float length = glm::length(correction);
                    if (length > 10.f / kHavokToGameUnits)
                        correction *= (10.f / kHavokToGameUnits) / length;
                    const auto goal = current + correction + feed * aDeltaTime;
                    alignas(16) float position[4]{goal.x, goal.y, goal.z, 0.f};
                    glm::quat want{target.Rotation[3], target.Rotation[0], target.Rotation[1], target.Rotation[2]};
                    const glm::vec3 angular{target.Angular[0], target.Angular[1], target.Angular[2]};
                    const float speed = glm::length(angular);
                    if (speed > 0.0001f)
                        want = glm::angleAxis(speed * aDeltaTime, angular / speed) * want;
                    alignas(16) float rotation[4]{want.x, want.y, want.z, want.w};
                    using Drive = void(const float*, const float*, float, void*);
                    POINTER_SKYRIMSE(Drive, drive, 62478);
                    drive.Get()(position, rotation, 1.f / aDeltaTime, target.Body);
                    // Bound commanded movement, including COM rotation. Contacts remain
                    // authoritative to Havok; this is not a claim about measured max step.
                    auto* motion = static_cast<uint8_t*>(target.Body) + 0x150;
                    using VelocityFn = void(*)(void*, const float*);
                    auto** vt = *reinterpret_cast<VelocityFn**>(motion);
                    for (unsigned axis = 0; axis < 2; ++axis)
                    {
                        auto* values = axis ? pBody->angularVelocity : pBody->linearVelocity;
                        glm::vec3 velocity{values[0], values[1], values[2]};
                        const float limit = axis ? 12.f : 25.f / kHavokToGameUnits / aDeltaTime;
                        const float magnitude = glm::length(velocity);
                        if (magnitude > limit)
                            velocity *= limit / magnitude;
                        alignas(16) float bounded[4]{velocity.x, velocity.y, velocity.z, 0.f};
                        vt[axis ? 0x11 : 0x10](motion, bounded);
                    }
                    if (target.Body == s_watchedHavokBody.load(std::memory_order_acquire))
                    {
                        targetApplied = targetSampled = true;
                        predictedTarget = wanted;
                        velocityAfterWrite = glm::length(glm::vec3{pBody->linearVelocity[0],
                            pBody->linearVelocity[1], pBody->linearVelocity[2]});
                    }
                    continue;
                }
                if (glm::length(error) > kFollowTeleport)
                    ++probe.Teleports;
                bool placed = false;
                if (glm::length(error) > kFollowTeleport)
                {
                    alignas(16) float position[4]{wanted.x, wanted.y, wanted.z, 0.f};
                    alignas(16) float quaternion[4]{target.Rotation[0], target.Rotation[1], target.Rotation[2], target.Rotation[3]};
                    s_placeBody.Get()(target.Body, position, quaternion);
                    placed = true;
                }
                // Placed: the gap is closed, so only the host's motion (the old gap's correction on top
                // flung the body on after a teleport).
                const glm::vec3 linear = glm::vec3{target.Velocity[0], target.Velocity[1], target.Velocity[2]} +
                    (placed ? glm::vec3{} : error / kFollowTimeConstant);
                float currentRotation[4];
                MatrixToQuaternion(pBody->transform, currentRotation);
                const glm::quat have{currentRotation[3], currentRotation[0], currentRotation[1], currentRotation[2]};
                const glm::quat want{target.Rotation[3], target.Rotation[0], target.Rotation[1], target.Rotation[2]};
                glm::quat delta = want * glm::conjugate(have);
                if (delta.w < 0.f)
                    delta = -delta;
                const glm::vec3 turn = glm::vec3{delta.x, delta.y, delta.z} * 2.f / kFollowTimeConstant;
                alignas(16) float linearVelocity[4]{linear.x, linear.y, linear.z, 0.f};
                alignas(16) float angularVelocity[4]{target.Angular[0] + turn.x,
                    target.Angular[1] + turn.y, target.Angular[2] + turn.z, 0.f};
                using ActivateFn = void(void*);
                POINTER_SKYRIMSE(ActivateFn, activate, 60849);
                activate.Get()(target.Body);
                auto* motion = static_cast<uint8_t*>(target.Body) + 0x150;
                using VelocityFn = void(void*, const float*);
                auto** vtable = *reinterpret_cast<VelocityFn***>(motion);
                vtable[0x10](motion, linearVelocity);
                vtable[0x11](motion, angularVelocity);
                if (target.Body == s_watchedHavokBody.load(std::memory_order_acquire))
                {
                    targetApplied = targetSampled = true;
                    predictedTarget = wanted;
                    velocityAfterWrite = glm::length(linear);
                    s_preStepAttempts.fetch_add(1, std::memory_order_relaxed);
                    s_preStepApplied.fetch_add(1, std::memory_order_relaxed);
                    s_preStepLastSourceAgeMs.store(static_cast<uint32_t>((std::min)(uint64_t{UINT32_MAX}, probe.HostAgeMs)),
                        std::memory_order_relaxed);
                    s_preStepLastPreError.store(gap, std::memory_order_relaxed);
                }
            }
        }
    }

    // bhkWorld::GetWorld1 (vtable slot 0x27) returns hkpWorld directly into
    // r13 before this wrapper is called. Its argument is not a bhkWorldM.
    const bool selectedWorld = apWorld &&
        apWorld == s_watchedHavokWorld.load(std::memory_order_acquire) &&
        (s_referencePhaseFormId.load(std::memory_order_acquire) != 0 ||
            s_preStepPlaybackFormId.load(std::memory_order_acquire) != 0);
    if (selectedWorld)
        s_selectedStepWorldMatches.fetch_add(1, std::memory_order_relaxed);
    const auto* pSelectedBody = selectedWorld ?
        s_watchedHavokBody.load(std::memory_order_acquire) : nullptr;
    ActorPoseDiagnosticViews::RigidBody before{};
    const bool beforeReadable = pSelectedBody &&
        ReadNativeMemory(pSelectedBody, before) && before.world == apWorld;
    if (beforeReadable)
        s_selectedStepBodyReads.fetch_add(1, std::memory_order_relaxed);
    CorpseRagdollService::OnHavokStep(apWorld, aDeltaTime, false);
    const int result = aNativeStep();
    CorpseRagdollService::OnHavokStep(apWorld, aDeltaTime, true);
    if (beforeReadable && pSelectedBody ==
        s_watchedHavokBody.load(std::memory_order_acquire))
    {
        ActorPoseDiagnosticViews::RigidBody after{};
        if (ReadNativeMemory(pSelectedBody, after) && after.world == apWorld)
        {
            if (targetSampled)
            {
                const glm::vec3 observed{after.transform[12],
                    after.transform[13], after.transform[14]};
                s_preStepLastPostError.store(glm::length(
                    predictedTarget - observed) * kHavokToGameUnits,
                    std::memory_order_relaxed);
            }
            const glm::vec3 delta{
                after.transform[12] - before.transform[12],
                after.transform[13] - before.transform[13],
                after.transform[14] - before.transform[14]};
            const float distance = glm::length(delta) * kHavokToGameUnits;
            s_selectedBodySteps.fetch_add(1, std::memory_order_relaxed);
            if (distance > 0.001f)
                s_selectedBodyChangedSteps.fetch_add(1,
                    std::memory_order_relaxed);
            s_selectedBodyLastStepDelta.store(distance,
                std::memory_order_relaxed);
            auto peak = s_selectedBodyPeakStepDelta.load(std::memory_order_relaxed);
            while (distance > peak)
            {
                if (s_selectedBodyPeakStepDelta.compare_exchange_weak(peak,
                        distance, std::memory_order_relaxed))
                {
                    const auto speed = [](const float (&v)[4]) noexcept {
                        return std::sqrt(v[0] * v[0] + v[1] * v[1] +
                            v[2] * v[2]);
                    };
                    s_selectedBodyPeakStepTimeMs.store(GetTickCount64(),
                        std::memory_order_relaxed);
                    s_selectedBodyPeakStepDt.store(aDeltaTime,
                        std::memory_order_relaxed);
                    s_selectedBodyPeakPreLinearSpeed.store(
                        speed(before.linearVelocity), std::memory_order_relaxed);
                    s_selectedBodyPeakPostLinearSpeed.store(
                        speed(after.linearVelocity), std::memory_order_relaxed);
                    s_selectedBodyPeakPreAngularSpeed.store(
                        speed(before.angularVelocity), std::memory_order_relaxed);
                    s_selectedBodyPeakPostAngularSpeed.store(
                        speed(after.angularVelocity), std::memory_order_relaxed);
                    s_selectedBodyPeakMotionType.store(before.motionType,
                        std::memory_order_relaxed);
                    s_selectedBodyPeakTargetApplied.store(targetApplied,
                        std::memory_order_relaxed);
                    s_selectedBodyPeakTargetAgeMs.store(targetApplied ?
                        s_preStepLastSourceAgeMs.load(std::memory_order_relaxed) :
                        UINT32_MAX, std::memory_order_relaxed);
                    s_selectedBodyPeakVelocityAfterWrite.store(
                        velocityAfterWrite, std::memory_order_relaxed);
                    break;
                }
            }
            if (distance >= 75.f)
                s_selectedBodyStepsOver75Units.fetch_add(1,
                    std::memory_order_relaxed);
        }
    }
    const auto ended = std::chrono::steady_clock::now();
    s_nativeStepCalls.fetch_add(1, std::memory_order_relaxed);
    s_nativeStepLastThreadId.store(GetCurrentThreadId(),
        std::memory_order_relaxed);
    s_nativeStepLastDurationUs.store(static_cast<uint32_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            ended - started).count()), std::memory_order_relaxed);
    s_nativeStepLastEndNs.store(static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            ended.time_since_epoch()).count()), std::memory_order_relaxed);
    return result;
}

int HookNativeStep(void* apWorld, float aDeltaTime)
{
    return RunNativeStep(apWorld, aDeltaTime, [&]() { return s_originalNativeStep(apWorld, aDeltaTime); });
}

// E77851 also dispatches simulationType 3 through E61417 (VA 0x140B5B250).
// Its float is the FOURTH argument, after the world, job queue and thread pool.
using MultithreadedStepFn = void(void*, void*, void*, float);
MultithreadedStepFn* s_originalMultithreadedStep{};
void HookMultithreadedStep(void* apWorld, void* apQueue, void* apPool, float aDeltaTime)
{
    RunNativeStep(apWorld, aDeltaTime, [&]()
    {
        s_originalMultithreadedStep(apWorld, apQueue, apPool, aDeltaTime);
        return 0;
    }, true);
}

static TiltedPhoques::Initializer s_nativeStepHook(
    []()
    {
        // On 1.7.104, bhkWorld::Update calls this native wrapper at RVA
        // 0xB5B0B0 with the hkpWorld returned by bhkWorld::GetWorld1.
        // The selected opt-in trial can apply a bounded host body target here;
        // normal play only forwards the native solver call unchanged.
        POINTER_SKYRIMSE(NativeStepFn, s_nativeStep, 61410);
        s_originalNativeStep = s_nativeStep.Get();
        TP_HOOK(&s_originalNativeStep, HookNativeStep);
        POINTER_SKYRIMSE(MultithreadedStepFn, multithreaded, 61417);
        s_originalMultithreadedStep = multithreaded.Get();
        TP_HOOK(&s_originalMultithreadedStep, HookMultithreadedStep);
    });

bool HookWorldUpdate(void* apWorld, uint32_t aFlags)
{
    const auto started = std::chrono::steady_clock::now();
    s_worldUpdatesRunning.fetch_add(1, std::memory_order_acq_rel);
    s_worldUpdateStartNs.store(static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        started.time_since_epoch()).count()), std::memory_order_relaxed);
    ++s_worldUpdateDepth;
    const bool result = s_originalWorldUpdate ?
        s_originalWorldUpdate(apWorld, aFlags) : false;
    --s_worldUpdateDepth;
    s_worldUpdateLastStartNs.store(s_worldUpdateStartNs.load(std::memory_order_relaxed), std::memory_order_relaxed);
    s_worldUpdatesRunning.fetch_sub(1, std::memory_order_acq_rel);
    const auto ended = std::chrono::steady_clock::now();
    s_worldUpdateCalls.fetch_add(1, std::memory_order_relaxed);
    s_worldUpdateLastThreadId.store(GetCurrentThreadId(),
        std::memory_order_relaxed);
    s_worldUpdateLastDurationUs.store(static_cast<uint32_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            ended - started).count()), std::memory_order_relaxed);
    s_worldUpdateLastEndNs.store(static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            ended.time_since_epoch()).count()), std::memory_order_relaxed);
    return result;
}

static TiltedPhoques::Initializer s_worldUpdateHook(
    []()
    {
        // Precision uses the same AE relocation for a pre-physics callback.
        // Hook the full native update transparently first to validate cadence
        // and ordering on this 1.7.104 runtime, without touching Havok state.
        POINTER_SKYRIMSE(WorldUpdateFn, s_worldUpdate, 77851);
        s_originalWorldUpdate = s_worldUpdate.Get();
        TP_HOOK(&s_originalWorldUpdate, HookWorldUpdate);
    });

void HookCollisionWorld(void* apCollision, const NiTransform* apWorld)
{
    NiAVObject* pNode{};
    auto* pReference = s_watchedReference.load(std::memory_order_relaxed);
    const bool watched = apCollision && apWorld && pReference &&
        s_referencePhaseFormId.load(std::memory_order_relaxed) != 0 &&
        ReadNativeMemory(reinterpret_cast<const uint8_t*>(apCollision) + 0x10,
            pNode) &&
        pNode == s_watchedReferenceNode.load(std::memory_order_relaxed);
    const auto callerRva = watched ? SkyrimCallerRva(_ReturnAddress()) : 0;
    const auto started = std::chrono::steady_clock::now();
    const auto before = watched ? SampleReferencePhase(pReference) :
        ReferencePhaseSample{};
    NiTransform input{};
    const bool inputReadable = watched && ReadNativeMemory(apWorld, input) &&
        std::isfinite(input.translate.x) &&
        std::isfinite(input.translate.y) &&
        std::isfinite(input.translate.z);
    if (s_originalCollisionWorld)
        s_originalCollisionWorld(apCollision, apWorld);
    if (!watched || !inputReadable)
        return;
    if (s_worldUpdateDepth)
        s_collisionWorldDuringWorldUpdate.fetch_add(1,
            std::memory_order_relaxed);
    else
    {
        s_collisionWorldOutsideWorldUpdate.fetch_add(1,
            std::memory_order_relaxed);
        const auto lastEnd = s_worldUpdateLastEndNs.load(
            std::memory_order_relaxed);
        const auto nowNs = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count());
        if (lastEnd && nowNs >= lastEnd)
            s_collisionWorldAfterWorldUpdateUs.store(static_cast<uint32_t>(
                (std::min)(uint64_t{UINT32_MAX}, (nowNs - lastEnd) / 1000)),
                std::memory_order_relaxed);
        const auto stepEnd = s_nativeStepLastEndNs.load(
            std::memory_order_relaxed);
        if (stepEnd && nowNs >= stepEnd)
            s_collisionWorldAfterNativeStepUs.store(static_cast<uint32_t>(
                (std::min)(uint64_t{UINT32_MAX}, (nowNs - stepEnd) / 1000)),
                std::memory_order_relaxed);
    }
    const auto after = SampleReferencePhase(pReference);
    const glm::vec3 target{input.translate.x, input.translate.y,
        input.translate.z};
    s_collisionWorldSelectedCalls.fetch_add(1, std::memory_order_relaxed);
    s_collisionWorldLastCallerRva.store(callerRva, std::memory_order_relaxed);
    s_collisionWorldLastThreadId.store(GetCurrentThreadId(),
        std::memory_order_relaxed);
    s_collisionWorldLastDurationUs.store(static_cast<uint32_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - started).count()),
        std::memory_order_relaxed);
    s_collisionWorldLastInputX.store(target.x, std::memory_order_relaxed);
    s_collisionWorldLastInputY.store(target.y, std::memory_order_relaxed);
    s_collisionWorldLastInputZ.store(target.z, std::memory_order_relaxed);
    if (before.BodyPresent)
        s_collisionWorldLastInputBodyError.store(
            glm::length(target - before.Body), std::memory_order_relaxed);
    if (before.NodePresent)
        s_collisionWorldLastInputNodeError.store(
            glm::length(target - before.Node), std::memory_order_relaxed);
    if (after.NodePresent)
        s_collisionWorldLastPostNodeInputError.store(
            glm::length(target - after.Node), std::memory_order_relaxed);
    s_collisionWorldLastPostReferenceInputError.store(
        glm::length(target - after.Reference), std::memory_order_relaxed);
}

static TiltedPhoques::Initializer s_collisionWorldHook(
    []()
    {
        POINTER_SKYRIMSE(CollisionWorldFn, s_collisionWorld, 78382);
        s_originalCollisionWorld = s_collisionWorld.Get();
        TP_HOOK(&s_originalCollisionWorld, HookCollisionWorld);
    });

void MaybeInstallRenderNodePhaseHook(TESObjectREFR* apReference) noexcept
{
    if (!apReference || !apReference->loadedState ||
        apReference->formID != s_referencePhaseFormId.load(
            std::memory_order_acquire))
        return;
    auto* pNode = apReference->GetNiNode();
    if (!pNode)
        return;
    s_watchedReferenceNode.store(pNode, std::memory_order_release);
    s_watchedReference.store(apReference, std::memory_order_release);
    void** pVtable{};
    if (!ReadNativeMemory(pNode, pVtable) || !pVtable)
        return;
    if (s_referenceNodeVtable.load(std::memory_order_acquire) == pVtable ||
        s_referenceNodeVtable.load(std::memory_order_acquire))
        return;
    if (!ReadNativeMemory(pVtable + 0x2C, s_originalNodeDownward) ||
        !ReadNativeMemory(pVtable + 0x30, s_originalNodeWorldData) ||
        !ReadNativeMemory(pVtable + 0x31, s_originalNodeTransformBounds) ||
        !s_originalNodeDownward || !s_originalNodeWorldData ||
        !s_originalNodeTransformBounds)
        return;
    s_nodeWorldDataTargetRva.store(SkyrimCallerRva(
        reinterpret_cast<void*>(s_originalNodeWorldData)),
        std::memory_order_relaxed);
    s_nodeTransformTargetRva.store(SkyrimCallerRva(
        reinterpret_cast<void*>(s_originalNodeTransformBounds)),
        std::memory_order_relaxed);
    s_referenceNodeVtable.store(pVtable, std::memory_order_release);
    const auto oldDownward = TiltedPhoques::HookVTable(
        pNode, 0x2C, &HookNodeDownward);
    const auto oldWorldData = TiltedPhoques::HookVTable(
        pNode, 0x30, &HookNodeWorldData);
    const auto oldTransformBounds = TiltedPhoques::HookVTable(
        pNode, 0x31, &HookNodeTransformBounds);
    s_referenceNodeHookInstalled.store(
        oldDownward == s_originalNodeDownward &&
        oldWorldData == s_originalNodeWorldData &&
        oldTransformBounds == s_originalNodeTransformBounds,
        std::memory_order_release);
}

// This stream is for loose, freely simulated clutter. Activators, animated
// objects, movable statics, doors, and other placed scene machinery may move
// under Papyrus, packages, constraints, or authored animations. Changing
// their motion type to keyframed can stop those native drivers altogether.
bool IsPassivePhysicsReference(const TESObjectREFR* apReference) noexcept
{
    if (!apReference || !apReference->baseForm)
        return false;

    switch (apReference->baseForm->formType)
    {
    case FormType::Armor:
    case FormType::Book:
    case FormType::Ingredient:
    case FormType::Light:
    case FormType::Misc:
    case FormType::Weapon:
    case FormType::Ammo:
    case FormType::KeyMaster:
    case FormType::Alchemy:
    case FormType::SoulGem:
        return true;
    default:
        return false;
    }
}
}

void ObjectService::SetBodyPlaybackProbe(uint32_t aFormId) noexcept
{
    if (aFormId)
        s_preStepPlaybackFormId.store(0, std::memory_order_release);
    s_bodyPlaybackFormId.store(aFormId, std::memory_order_release);
}

void ObjectService::SetPreStepBodyPlaybackProbe(uint32_t aFormId,
    uint32_t aMode) noexcept
{
    // The old post-step position trial must never run alongside this trial.
    s_bodyPlaybackFormId.store(0, std::memory_order_release);
    s_preStepExpectedEpoch.store(0, std::memory_order_release);
    s_watchedBodyWrapper.store(nullptr, std::memory_order_release);
    s_watchedHavokBody.store(nullptr, std::memory_order_release);
    s_watchedHavokWorld.store(nullptr, std::memory_order_release);
    s_preStepPlaybackMode.store(0,
        std::memory_order_release);
    s_preStepPlaybackFormId.store(aFormId, std::memory_order_release);
}

ObjectService::PreStepPlaybackDiagnostic
ObjectService::GetPreStepPlaybackDiagnostic() noexcept
{
    return {s_preStepPlaybackFormId.load(std::memory_order_acquire),
        s_preStepPlaybackMode.load(std::memory_order_acquire),
        s_preStepPublishedTargets.load(std::memory_order_relaxed),
        s_preStepAttempts.load(std::memory_order_relaxed),
        s_preStepApplied.load(std::memory_order_relaxed),
        s_preStepStaleSkips.load(std::memory_order_relaxed),
        s_preStepLastSourceAgeMs.load(std::memory_order_relaxed),
        s_preStepLastPreError.load(std::memory_order_relaxed),
        s_preStepLastPostError.load(std::memory_order_relaxed),
        s_preStepLastVelocityCorrection.load(std::memory_order_relaxed),
        s_preStepPoseWrites.load(std::memory_order_relaxed),
        s_preStepLastPoseStep.load(std::memory_order_relaxed),
        s_physicsHostScans.load(std::memory_order_relaxed),
        s_physicsHostPacketsSent.load(std::memory_order_relaxed),
        s_physicsHostUpdatesQueued.load(std::memory_order_relaxed),
        s_physicsHostBodyOnlyUpdates.load(std::memory_order_relaxed),
        s_physicsHostSelectedObserved.load(std::memory_order_relaxed),
        s_physicsHostSelectedQueued.load(std::memory_order_relaxed),
        s_physicsFollowerPacketsReceived.load(std::memory_order_relaxed),
        s_physicsFollowerSelectedReceived.load(std::memory_order_relaxed),
        s_physicsLastSelectedTransitAgeMs.load(std::memory_order_relaxed),
        s_physicsLastHostScanDurationUs.load(std::memory_order_relaxed),
        s_physicsLastLaneReferencesVisited.load(std::memory_order_relaxed),
        s_physicsLastHostUpdatesQueued.load(std::memory_order_relaxed),
        s_physicsHostScanTotalUs.load(std::memory_order_relaxed),
        s_physicsHostScanMaxUs.load(std::memory_order_relaxed),
        s_physicsLastHostReferencesVisited.load(std::memory_order_relaxed),
        s_physicsLastHostCandidateCount.load(std::memory_order_relaxed),
        s_hostKnownRefreshTiming.LastUs.load(std::memory_order_relaxed),
        s_hostKnownRefreshTiming.MaxUs.load(std::memory_order_relaxed),
        s_hostKnownRefreshTiming.TotalUs.load(std::memory_order_relaxed),
        s_hostCurrentDiscoveryTiming.LastUs.load(std::memory_order_relaxed),
        s_hostCurrentDiscoveryTiming.MaxUs.load(std::memory_order_relaxed),
        s_hostCurrentDiscoveryTiming.TotalUs.load(std::memory_order_relaxed),
        s_hostGridDiscoveryTiming.LastUs.load(std::memory_order_relaxed),
        s_hostGridDiscoveryTiming.MaxUs.load(std::memory_order_relaxed),
        s_hostGridDiscoveryTiming.TotalUs.load(std::memory_order_relaxed),
        s_hostPruneTiming.LastUs.load(std::memory_order_relaxed),
        s_hostPruneTiming.MaxUs.load(std::memory_order_relaxed),
        s_hostPruneTiming.TotalUs.load(std::memory_order_relaxed)};
}

ObjectService::BodyPlaybackDiagnostic ObjectService::GetBodyPlaybackDiagnostic() noexcept
{
    return {s_bodyPlaybackFormId.load(std::memory_order_acquire),
        s_bodyPlaybackAttempts.load(std::memory_order_relaxed),
        s_bodyPlaybackSucceeded.load(std::memory_order_relaxed),
        s_bodyPlaybackStaleSkips.load(std::memory_order_relaxed),
        s_bodyPlaybackLastAgeMs.load(std::memory_order_relaxed),
        s_bodyPlaybackLastDurationUs.load(std::memory_order_relaxed),
        s_bodyPlaybackLastPreError.load(std::memory_order_relaxed),
        s_bodyPlaybackLastPostError.load(std::memory_order_relaxed),
        s_bodyPlaybackLastStep.load(std::memory_order_relaxed)};
}

void ObjectService::SetReferencePhaseProbe(uint32_t aFormId) noexcept
{
    if (s_referencePhaseFormId.load(std::memory_order_acquire) != aFormId)
    {
        s_selectedBodyPeakStepDelta.store(0.f, std::memory_order_relaxed);
        s_selectedBodyStepsOver75Units.store(0, std::memory_order_relaxed);
        s_selectedBodyPeakStepTimeMs.store(0, std::memory_order_relaxed);
        s_selectedBodyPeakStepDt.store(0.f, std::memory_order_relaxed);
        s_selectedBodyPeakPreLinearSpeed.store(0.f, std::memory_order_relaxed);
        s_selectedBodyPeakPostLinearSpeed.store(0.f, std::memory_order_relaxed);
        s_selectedBodyPeakPreAngularSpeed.store(0.f, std::memory_order_relaxed);
        s_selectedBodyPeakPostAngularSpeed.store(0.f, std::memory_order_relaxed);
        s_selectedBodyPeakMotionType.store(0, std::memory_order_relaxed);
        s_selectedBodyPeakTargetApplied.store(false, std::memory_order_relaxed);
        s_selectedBodyPeakTargetAgeMs.store(0, std::memory_order_relaxed);
        s_selectedBodyPeakVelocityAfterWrite.store(-1.f,
            std::memory_order_relaxed);
    }
    if (!aFormId)
    {
        s_watchedReference.store(nullptr, std::memory_order_release);
        s_watchedReferenceNode.store(nullptr, std::memory_order_release);
        s_watchedHavokBody.store(nullptr, std::memory_order_release);
        s_watchedHavokWorld.store(nullptr, std::memory_order_release);
    }
    s_referencePhaseFormId.store(aFormId, std::memory_order_release);
}

void ObjectService::RecordNativeSetPosition(const TESObjectREFR* apReference,
    uint64_t aCallerRva, const NiPoint3* apInput,
    bool aRemote, bool aScopedOverride) noexcept
{
    if (!apReference || !apInput || apReference->formID !=
        s_referencePhaseFormId.load(std::memory_order_relaxed))
        return;
    if (!std::isfinite(apInput->x) || !std::isfinite(apInput->y) ||
        !std::isfinite(apInput->z) ||
        !std::isfinite(apReference->position.x) ||
        !std::isfinite(apReference->position.y) ||
        !std::isfinite(apReference->position.z))
        return;
    const auto* pNode = s_watchedReferenceNode.load(std::memory_order_relaxed);
    const auto* pWatched = s_watchedReference.load(std::memory_order_relaxed);
    const bool sourceIsNodeWorld = pWatched == apReference && pNode &&
        apInput == &pNode->world.translate;
    const glm::vec3 delta{apInput->x - apReference->position.x,
        apInput->y - apReference->position.y,
        apInput->z - apReference->position.z};
    s_referenceSetPositionCalls.fetch_add(1, std::memory_order_relaxed);
    if (aRemote)
    {
        (aScopedOverride ? s_referenceSetPositionRemoteOverrideCalls :
            s_referenceSetPositionRemoteSuppressedCalls).fetch_add(
                1, std::memory_order_relaxed);
    }
    s_referenceSetPositionCallerRva.store(aCallerRva, std::memory_order_relaxed);
    s_referenceSetPositionThreadId.store(GetCurrentThreadId(), std::memory_order_relaxed);
    s_referenceSetPositionFormType.store(static_cast<uint32_t>(apReference->formType),
        std::memory_order_relaxed);
    s_referenceSetPositionSourceIsNodeWorld.store(sourceIsNodeWorld,
        std::memory_order_relaxed);
    s_referenceSetPositionInputX.store(apInput->x, std::memory_order_relaxed);
    s_referenceSetPositionInputY.store(apInput->y, std::memory_order_relaxed);
    s_referenceSetPositionInputZ.store(apInput->z, std::memory_order_relaxed);
    s_referenceSetPositionPreReferenceError.store(glm::length(delta),
        std::memory_order_relaxed);
}

ObjectService::ReferencePhaseDiagnostic
ObjectService::GetReferencePhaseDiagnostic() noexcept
{
    return {s_referencePhaseFormId.load(std::memory_order_acquire),
        s_referencePhaseInstalled.load(std::memory_order_acquire),
        s_referenceUpdate3DCalls.load(std::memory_order_relaxed),
        s_referenceMoveHavokCalls.load(std::memory_order_relaxed),
        s_referencePhaseThreadId.load(std::memory_order_relaxed),
        s_referencePhaseLastMethod.load(std::memory_order_relaxed),
        s_referencePhaseDurationUs.load(std::memory_order_relaxed),
        s_referencePhaseRefDelta.load(std::memory_order_relaxed),
        s_referencePhaseNodeDelta.load(std::memory_order_relaxed),
        s_referencePhaseBodyDelta.load(std::memory_order_relaxed),
        s_referencePhaseRefBodyError.load(std::memory_order_relaxed),
        s_referencePhaseRefNodeError.load(std::memory_order_relaxed),
        s_referenceSetPositionCalls.load(std::memory_order_relaxed),
        s_referenceSetPositionRemoteSuppressedCalls.load(std::memory_order_relaxed),
        s_referenceSetPositionRemoteOverrideCalls.load(std::memory_order_relaxed),
        s_referenceSetPositionCallerRva.load(std::memory_order_relaxed),
        s_referenceSetPositionThreadId.load(std::memory_order_relaxed),
        s_referenceSetPositionFormType.load(std::memory_order_relaxed),
        s_referenceSetPositionSourceIsNodeWorld.load(std::memory_order_relaxed),
        s_referenceSetPositionInputX.load(std::memory_order_relaxed),
        s_referenceSetPositionInputY.load(std::memory_order_relaxed),
        s_referenceSetPositionInputZ.load(std::memory_order_relaxed),
        s_referenceSetPositionPreReferenceError.load(std::memory_order_relaxed)};
}

ObjectService::RenderNodePhaseDiagnostic
ObjectService::GetRenderNodePhaseDiagnostic() noexcept
{
    return {s_referenceNodeHookInstalled.load(std::memory_order_acquire),
        s_nodeDownwardCalls.load(std::memory_order_relaxed),
        s_nodeWorldDataCalls.load(std::memory_order_relaxed),
        s_nodeTransformBoundsCalls.load(std::memory_order_relaxed),
        s_nodeLastMethod.load(std::memory_order_relaxed),
        s_nodeLastThreadId.load(std::memory_order_relaxed),
        s_nodeLastDurationUs.load(std::memory_order_relaxed),
        s_nodeLastLocalDelta.load(std::memory_order_relaxed),
        s_nodeLastWorldDelta.load(std::memory_order_relaxed),
        s_nodeLastReferenceDelta.load(std::memory_order_relaxed),
        s_nodeLastRefNodeError.load(std::memory_order_relaxed),
        s_nodeWorldDataCallerRva.load(std::memory_order_relaxed),
        s_nodeTransformCallerRva.load(std::memory_order_relaxed),
        s_nodeWorldDataRefDelta.load(std::memory_order_relaxed),
        s_nodeTransformRefDelta.load(std::memory_order_relaxed),
        s_nodeWorldDataTargetRva.load(std::memory_order_relaxed),
        s_nodeTransformTargetRva.load(std::memory_order_relaxed)};
}

ObjectService::CollisionSyncDiagnostic
ObjectService::GetCollisionSyncDiagnostic() noexcept
{
    return {s_collisionSyncSelectedCalls.load(std::memory_order_relaxed),
        s_collisionSyncLastCallerRva.load(std::memory_order_relaxed),
        s_collisionSyncLastThreadId.load(std::memory_order_relaxed),
        s_collisionSyncLastDurationUs.load(std::memory_order_relaxed),
        s_collisionSyncLastNodeDelta.load(std::memory_order_relaxed),
        s_collisionSyncLastReferenceDelta.load(std::memory_order_relaxed),
        s_collisionSyncLastBodyDelta.load(std::memory_order_relaxed),
        s_collisionSyncLastPreNodeReferenceError.load(std::memory_order_relaxed),
        s_collisionSyncLastPostNodeReferenceError.load(std::memory_order_relaxed)};
}

ObjectService::CollisionWorldDiagnostic
ObjectService::GetCollisionWorldDiagnostic() noexcept
{
    return {s_collisionWorldSelectedCalls.load(std::memory_order_relaxed),
        s_collisionWorldLastCallerRva.load(std::memory_order_relaxed),
        s_collisionWorldLastThreadId.load(std::memory_order_relaxed),
        s_collisionWorldLastDurationUs.load(std::memory_order_relaxed),
        s_collisionWorldLastInputX.load(std::memory_order_relaxed),
        s_collisionWorldLastInputY.load(std::memory_order_relaxed),
        s_collisionWorldLastInputZ.load(std::memory_order_relaxed),
        s_collisionWorldLastInputBodyError.load(std::memory_order_relaxed),
        s_collisionWorldLastInputNodeError.load(std::memory_order_relaxed),
        s_collisionWorldLastPostNodeInputError.load(std::memory_order_relaxed),
        s_collisionWorldLastPostReferenceInputError.load(std::memory_order_relaxed)};
}

ObjectService::WorldUpdateDiagnostic
ObjectService::GetWorldUpdateDiagnostic() noexcept
{
    return {s_worldUpdateCalls.load(std::memory_order_relaxed),
        s_worldUpdateLastThreadId.load(std::memory_order_relaxed),
        s_worldUpdateLastDurationUs.load(std::memory_order_relaxed),
        s_nativeStepCalls.load(std::memory_order_relaxed),
        s_nativeStepLastThreadId.load(std::memory_order_relaxed),
        s_nativeStepLastDurationUs.load(std::memory_order_relaxed),
        s_selectedBodySteps.load(std::memory_order_relaxed),
        s_selectedBodyChangedSteps.load(std::memory_order_relaxed),
        s_selectedBodyLastStepDelta.load(std::memory_order_relaxed),
        s_selectedBodyPeakStepDelta.load(std::memory_order_relaxed),
        s_selectedBodyStepsOver75Units.load(std::memory_order_relaxed),
        s_selectedBodyPeakStepTimeMs.load(std::memory_order_relaxed),
        s_selectedBodyPeakStepDt.load(std::memory_order_relaxed),
        s_selectedBodyPeakPreLinearSpeed.load(std::memory_order_relaxed),
        s_selectedBodyPeakPostLinearSpeed.load(std::memory_order_relaxed),
        s_selectedBodyPeakPreAngularSpeed.load(std::memory_order_relaxed),
        s_selectedBodyPeakPostAngularSpeed.load(std::memory_order_relaxed),
        s_selectedBodyPeakMotionType.load(std::memory_order_relaxed),
        s_selectedBodyPeakTargetApplied.load(std::memory_order_relaxed),
        s_selectedBodyPeakTargetAgeMs.load(std::memory_order_relaxed),
        s_selectedBodyPeakVelocityAfterWrite.load(std::memory_order_relaxed),
        s_selectedBodyCacheRefreshes.load(std::memory_order_relaxed),
        s_selectedStepWorldMatches.load(std::memory_order_relaxed),
        s_selectedStepBodyReads.load(std::memory_order_relaxed),
        s_collisionWorldDuringWorldUpdate.load(std::memory_order_relaxed),
        s_collisionWorldOutsideWorldUpdate.load(std::memory_order_relaxed),
        s_collisionWorldAfterWorldUpdateUs.load(std::memory_order_relaxed),
        s_collisionWorldAfterNativeStepUs.load(std::memory_order_relaxed)};
}

ObjectService::ObjectService(World& aWorld, entt::dispatcher& aDispatcher, TransportService& aTransport)
    : m_world(aWorld)
    , m_transport(aTransport)
{
    s_objectService.store(this, std::memory_order_release);
    m_disconnectedConnection = aDispatcher.sink<DisconnectedEvent>().connect<&ObjectService::OnDisconnected>(this);
    m_cellChangeConnection = aDispatcher.sink<CellChangeEvent>().connect<&ObjectService::OnCellChange>(this);
    m_onActivateConnection = aDispatcher.sink<ActivateEvent>().connect<&ObjectService::OnActivate>(this);
    m_activateConnection = aDispatcher.sink<NotifyActivate>().connect<&ObjectService::OnActivateNotify>(this);
    m_lockChangeConnection = aDispatcher.sink<LockChangeEvent>().connect<&ObjectService::OnLockChange>(this);
    m_lockChangeNotifyConnection = aDispatcher.sink<NotifyLockChange>().connect<&ObjectService::OnLockChangeNotify>(this);
    m_assignObjectConnection = aDispatcher.sink<AssignObjectsResponse>().connect<&ObjectService::OnAssignObjectsResponse>(this);
    m_scriptAnimationConnection = aDispatcher.sink<ScriptAnimationEvent>().connect<&ObjectService::OnScriptAnimationEvent>(this);
    m_scriptAnimationNotifyConnection = aDispatcher.sink<NotifyScriptAnimation>().connect<&ObjectService::OnNotifyScriptAnimation>(this);
    m_updateConnection = aDispatcher.sink<UpdateEvent>().connect<&ObjectService::OnUpdate>(this);
    m_physicsMoveConnection = aDispatcher.sink<NotifyPhysicsReferencesMove>().connect<&ObjectService::OnPhysicsReferencesMove>(this);

    EventDispatcherManager::Get()->activateEvent.RegisterSink(this);
    EventDispatcherManager::Get()->objectLoadedEvent.RegisterSink(this);
    EventDispatcherManager::Get()->cellAttachDetachEvent.RegisterSink(this);
    EventDispatcherManager::Get()->moveAttachDetachEvent.RegisterSink(this);
}

// These events are opaque in this fork's EventDispatcher.h. Payloads are the SKSE/CommonLib
// TESObjectLoadedEvent and TES{Cell,Move}AttachDetachEvent layouts, not new engine offsets.
BSTEventResult ObjectService::OnEvent(const TESObjectLoadedEvent* apEvent,
    const EventDispatcher<TESObjectLoadedEvent>*)
{
    if (apEvent)
    {
        uint32_t formId{};
        std::memcpy(&formId, apEvent, sizeof(formId));
        QueuePhysicsRefresh(formId);
    }
    return BSTEventResult::kOk;
}

BSTEventResult ObjectService::OnEvent(const TESCellAttachDetachEvent* apEvent,
    const EventDispatcher<TESCellAttachDetachEvent>*)
{
    TESObjectREFR* reference{};
    if (apEvent)
        std::memcpy(&reference, apEvent, sizeof(reference));
    if (reference)
        QueuePhysicsRefresh(reference->formID);
    return BSTEventResult::kOk;
}

BSTEventResult ObjectService::OnEvent(const TESMoveAttachDetachEvent* apEvent,
    const EventDispatcher<TESMoveAttachDetachEvent>*)
{
    TESObjectREFR* reference{};
    if (apEvent)
        std::memcpy(&reference, apEvent, sizeof(reference));
    if (reference)
        QueuePhysicsRefresh(reference->formID);
    return BSTEventResult::kOk;
}

void ObjectService::QueuePhysicsRefresh(uint32_t aFormId) noexcept
{
    if (!aFormId)
        return;
    std::lock_guard lock(m_physicsEventsLock);
    m_physicsDirty.Insert(aFormId);
}

void ObjectService::RefreshPhysicsReference(uint32_t aFormId) noexcept
{
    auto* reference = Cast<TESObjectREFR>(TESForm::GetById(aFormId));
    auto& drops = m_world.GetSharedDropService();
    const bool shared = reference && drops.IsShared(aFormId);
    const bool admitted = reference && reference->loadedState && reference->parentCell &&
        reference->parentCell->IsAttached() && !Cast<Actor>(reference) &&
        (shared ? drops.IsOwner(aFormId) : (m_physicsLeader && !reference->IsTemporary()));
    // Body replacement needs no retained native pointer: admission classifies once, and
    // capture resolves the current root/collision/body chain each time it samples.
    bool passive = admitted && !shared && IsPassivePhysicsReference(reference);
    // Sleeping bodies stay in bounded repair; native motion notifications promote them.
    bool wasBody{}, referenceMoved{};
    {
        std::lock_guard lock(m_hostPhysicsLock);
        if (const auto it = m_referencePoses.find(aFormId); it != m_referencePoses.end())
            wasBody = it->second.Body;
        if (passive)
            if (const auto it = m_referencePoses.find(aFormId); it != m_referencePoses.end())
            {
                const auto delta = reference->position - it->second.Position;
                const auto turn = reference->rotation - it->second.Rotation;
                referenceMoved = it->second.Promote || glm::dot(delta, delta) >= 0.01f || glm::dot(turn, turn) >= 0.000001f;
            }
    }
    DynamicBody body{};
    const bool hasBody = admitted && reference->baseForm &&
        reference->baseForm->formType != FormType::Door &&
        GetDynamicBody(reference, body, true, true);
    if (wasBody || (hasBody && referenceMoved))
        passive = false;
    const auto generation = shared ? drops.PhysicsGeneration(aFormId) : 0;
    std::lock_guard lock(m_hostPhysicsLock);
    auto it = m_referencePoses.find(aFormId);
    if (!admitted)
    {
        m_physicsBodies.Erase(aFormId);
        m_physicsUpdateRefs.Erase(aFormId);
        m_referencePoses.erase(aFormId);
        return;
    }
    if (!passive && !hasBody)
    {
        m_physicsBodies.Erase(aFormId);
        if (it == m_referencePoses.end())
        {
            m_physicsUpdateRefs.Erase(aFormId);
            return;
        }
        m_physicsUpdateRefs.Insert(aFormId);
        if (++it->second.MissingChecks >= 3)
        {
            m_physicsUpdateRefs.Erase(aFormId);
            m_referencePoses.erase(it);
        }
        return;
    }
    if (hasBody)
        passive = false; // even sleeping clutter gets a body-space authoritative pose
    if (it != m_referencePoses.end() && (it->second.Shared != shared ||
        it->second.Generation != generation))
    {
        m_referencePoses.erase(it);
        it = m_referencePoses.end();
    }
    if (it == m_referencePoses.end())
    {
        auto& pose = m_referencePoses[aFormId];
        pose.Shared = shared;
        pose.Generation = generation;
        pose.Update.ChildBodies.reserve(PhysicsReferenceUpdate::kMaxChildBodies);
        // Grow storage only when membership grows, not when a snapshot is captured.
        const auto capacity = (m_referencePoses.size() + 63) / 64 * 64;
        m_physicsPromotions.reserve(capacity);
        for (auto& snapshot : m_physicsSnapshots)
            if (snapshot.Entries.size() < capacity)
                snapshot.Entries.resize(capacity);
    }
    auto& pose = m_referencePoses.at(aFormId);
    {
        std::lock_guard lock(s_assembliesLock);
        const auto assembly = s_assemblies.find(aFormId);
        pose.Assembly = assembly != s_assemblies.end() && assembly->second.Complete &&
            assembly->second.Root.get() == reference->GetNiNode();
    }
    pose.MissingChecks = 0;
    pose.Body = !passive;
    if (!passive)
        pose.Promote = false;
    // Moving/recently changed bodies use the active set. Everything else uses bounded
    // repair on the same main thread; there is no full passive-set capture pass.
    const bool mainFrame = !passive &&
        (pose.Assembly || std::chrono::steady_clock::now() - pose.LastActive < 500ms ||
            (body.State.motionType != 4 && IsPhysicsBodyAwake(body.HavokBody)));
    if (!mainFrame)
    {
        m_physicsBodies.Erase(aFormId);
        m_physicsUpdateRefs.Insert(aFormId);
    }
    else
    {
        m_physicsUpdateRefs.Erase(aFormId);
        m_physicsBodies.Insert(aFormId);
    }
}

void ObjectService::RefreshPhysicsCandidates() noexcept
{
    const auto now = std::chrono::steady_clock::now();
    auto* player = PlayerCharacter::Get();
    if (!player || !player->parentCell)
        return;
    const bool leader = m_world.GetPartyService().IsLeader();
    const auto epoch = m_world.GetPartyService().GetStartEpoch();
    if (leader != m_physicsLeader || epoch != m_physicsEpoch)
    {
        std::lock_guard lock(m_hostPhysicsLock);
        m_physicsLeader = leader;
        m_physicsEpoch = epoch;
        m_referencePoses.clear();
        m_ownedPhysicsGenerations.clear();
        m_physicsBodies.Clear();
        m_physicsUpdateRefs.Clear();
        m_physicsPromotions.clear();
        m_physicsCells.clear();
        m_physicsSnapshotRead = m_physicsSnapshotCount = 0;
        m_nextPhysicsSnapshot = {};
        m_nextPhysicsCells = m_nextOwnedPhysics = m_nextPhysicsMaintenance = {};
    }
    m_physicsMovingScratch.clear();
    {
        std::lock_guard movementLock(s_movingReferencesLock);
        m_physicsMovingScratch.reserve(s_movingReferences.Count + s_movingReferences.Overflow.size());
        s_movingReferences.Drain([&](uint32_t id) { m_physicsMovingScratch.push_back(id); });
    }
    for (const auto id : m_physicsMovingScratch)
    {
        RefreshPhysicsReference(id);
        if (auto it = m_referencePoses.find(id); it != m_referencePoses.end())
        {
            it->second.LastActive = now;
            it->second.Promote = true;
            m_physicsUpdateRefs.Erase(id);
            m_physicsBodies.Insert(id);
        }
    }
    const auto pruneStarted = std::chrono::steady_clock::now();
    // Bounded repair of missed detach/ownership notifications. Recheck before the passive
    // snapshot updates its comparison pose, so a thrown item can move to the main lane.
    if (now >= m_nextPhysicsMaintenance)
    {
        for (size_t i = 0; i < 64; ++i)
        {
            uint32_t id{};
            {
                std::lock_guard lock(m_hostPhysicsLock);
                const auto count = m_physicsBodies.Ids.size() + m_physicsUpdateRefs.Ids.size();
                if (!count || i >= count)
                    break;
                const auto index = m_physicsMaintenanceCursor++ % count;
                id = index < m_physicsBodies.Ids.size() ? m_physicsBodies.Ids[index] :
                    m_physicsUpdateRefs.Ids[index - m_physicsBodies.Ids.size()];
            }
            RefreshPhysicsReference(id);
        }
        m_nextPhysicsMaintenance = now + 50ms;
    }
    s_hostPruneTiming.Record(HostScanDurationUs(pruneStarted));
    {
        std::lock_guard lock(m_physicsEventsLock);
        if (m_physicsRefresh.Ids.empty())
            std::swap(m_physicsRefresh, m_physicsDirty);
    }
    // Process load/detach/body replacement notifications incrementally. Coalescing resolves
    // current state, so unload/reload pairs cannot remove a newly attached copy out of order.
    for (size_t i = 0; i < 128 && !m_physicsRefresh.Ids.empty(); ++i)
    {
        const auto id = m_physicsRefresh.Ids.back();
        m_physicsRefresh.Erase(id);
        RefreshPhysicsReference(id);
    }
    if (!leader)
        return;
    const auto gridStarted = std::chrono::steady_clock::now();
    if (now >= m_nextPhysicsCells)
    {
        std::array<uint32_t, 226> cells{};
        size_t count{};
        cells[count++] = player->parentCell->formID;
        auto* tes = TES::Get();
        auto* grid = tes ? tes->cells : nullptr;
        if (player->parentCell->worldspace && grid && grid->arr &&
            grid->dimension > 0 && grid->dimension <= 15)
            for (uint32_t i = 0; i < grid->dimension * grid->dimension; ++i)
                if (auto* cell = grid->arr[i]; cell && cell != player->parentCell &&
                    cell->IsAttached() && cell->worldspace == player->parentCell->worldspace)
                    cells[count++] = cell->formID;
        std::erase_if(m_physicsCells, [&](const auto& cell)
        {
            return std::find(cells.begin(), cells.begin() + count, cell.FormId) == cells.begin() + count;
        });
        for (size_t i = 0; i < count; ++i)
            if (std::none_of(m_physicsCells.begin(), m_physicsCells.end(), [&](const auto& cell)
                { return cell.FormId == cells[i]; }))
                m_physicsCells.push_back({cells[i]});
        m_nextPhysicsCells = now + 250ms;
    }
    s_hostGridDiscoveryTiming.Record(HostScanDurationUs(gridStarted));
    // Bootstrap/join-in-progress plus a slow repair sweep for mods that replace bodies
    // without load events. At most 16 cell slots per frame, never a whole cell per scan.
    const auto started = std::chrono::steady_clock::now();
    uint32_t visited{};
    for (size_t i = 0; i < m_physicsCells.size() && visited < 16; ++i)
    {
        auto& scan = m_physicsCells[m_physicsCellCursor++ % m_physicsCells.size()];
        if (now < scan.NextSweep)
            continue;
        auto* cell = Cast<TESObjectCELL>(TESForm::GetById(scan.FormId));
        if (!cell || !cell->IsAttached() || !cell->refData.refArray || cell->refData.capacity > 50000)
            continue;
        while (scan.Cursor < cell->refData.capacity && visited < 16)
        {
            auto* reference = cell->refData.refArray[scan.Cursor++].Get();
            ++visited;
            if (reference)
                RefreshPhysicsReference(reference->formID);
        }
        if (scan.Cursor >= cell->refData.capacity)
        {
            scan.Cursor = 0;
            scan.NextSweep = now + 5s;
        }
    }
    s_hostCurrentDiscoveryTiming.Record(HostScanDurationUs(started));
}

void ObjectService::FlushPhysicsSnapshots() noexcept
{
    for (;;)
    {
        {
            std::lock_guard lock(m_hostPhysicsLock);
            if (!m_physicsSnapshotCount)
                return;
            std::swap(m_physicsSending, m_physicsSnapshots[m_physicsSnapshotRead]);
            m_physicsSnapshotRead = (m_physicsSnapshotRead + 1) % m_physicsSnapshots.size();
            --m_physicsSnapshotCount;
        }
        if (m_physicsSending.Epoch != m_world.GetPartyService().GetStartEpoch())
            continue;
        PhysicsReferencesMoveRequest request;
        request.Tick = m_physicsSending.Tick;
        request.Updates.reserve((std::min)(m_physicsSending.Count, PhysicsReferenceUpdate::MaxUpdates));
        auto& drops = m_world.GetSharedDropService();
        for (size_t i = 0; i < m_physicsSending.Count; ++i)
        {
            const auto& captured = m_physicsSending.Entries[i];
            PhysicsReferenceUpdate update;
            update.Id = captured.Id;
            update.Position = captured.Position;
            update.Rotation = captured.Rotation;
            update.MotionType = captured.MotionType;
            update.LinearVelocity = captured.LinearVelocity;
            update.BodyTransform = captured.BodyTransform;
            update.ChildBodies.assign(captured.Children.begin(), captured.Children.begin() + captured.ChildCount);
            if (captured.Shared)
            {
                if (drops.IsOwner(captured.FormId) && drops.PhysicsGeneration(captured.FormId) == captured.Generation)
                    drops.SendPhysics(captured.FormId, update, request.Tick);
            }
            else if (m_world.GetPartyService().IsLeader())
                request.Updates.push_back(std::move(update));
            if (request.Updates.size() == PhysicsReferenceUpdate::MaxUpdates)
            {
                m_transport.Send(request);
                s_physicsHostPacketsSent.fetch_add(1, std::memory_order_relaxed);
                request.Updates.clear();
            }
        }
        if (!request.Updates.empty())
        {
            m_transport.Send(request);
            s_physicsHostPacketsSent.fetch_add(1, std::memory_order_relaxed);
        }
    }
}

bool ObjectService::GetRemotePhysicsDiagnostic(uint32_t aFormId,
    RemotePhysicsDiagnostic& arDiagnostic) const noexcept
{
    std::lock_guard lock(m_remotePhysicsLock);
    const auto it = m_remoteReferencePoses.find(aFormId);
    if (it == m_remoteReferencePoses.end())
        return false;
    const auto& pose = it->second;
    arDiagnostic.Position = pose.Position;
    arDiagnostic.Tick = pose.Tick;
    arDiagnostic.AuthorityEpoch = pose.AuthorityEpoch;
    arDiagnostic.BodyDriven = pose.BodyDriven;
    arDiagnostic.AgeMs = static_cast<uint64_t>((std::max)(int64_t{0},
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - pose.LastReceived).count()));
    return true;
}

bool IsPlayerHome(const TESObjectCELL* pCell) noexcept
{
    if (pCell && pCell->loadedCellData && pCell->loadedCellData->encounterZone)
    {
        // Only return true if cell has the NoResetZone encounter zone
        if (pCell->loadedCellData->encounterZone->formID == 0xf90b1)
        {
            switch (pCell->formID)
            {
            case 0xeec55: // one known exception: Sinderion's Field Lab
                return false;
            default: return true;
            }
        }
    }

    return false;
}

// Find each loaded faction's containers for a jailed player's belongings and stolen items.
// Do not sync them: each player's items must stay separate (#700).
// Only compare the container pointers; never read through them.
Set<const TESObjectREFR*> GetPlayerStashContainers() noexcept
{
    Set<const TESObjectREFR*> containers{};

    ModManager* pModManager = ModManager::Get();
    if (!pModManager)
        return containers;

    for (const TESFaction* pFaction : pModManager->factions)
    {
        if (!pFaction)
            continue;

        if (pFaction->crimeData.playerInventoryContainer)
            containers.insert(pFaction->crimeData.playerInventoryContainer);

        if (pFaction->crimeData.stolenGoodsContainer)
            containers.insert(pFaction->crimeData.stolenGoodsContainer);
    }

    return containers;
}

bool ShouldSyncObject(const TESObjectREFR* apObject, const Set<const TESObjectREFR*>& acPlayerStashContainers) noexcept
{
    if (!apObject)
        return false;

    if (acPlayerStashContainers.contains(apObject))
        return false;

    // Quest chests that take the player's whole inventory without going through faction crime data.
    switch (apObject->formID)
    {
    case 0x39CF1: // Don't sync the chest in the "Diplomatic Immunity" quest
        return false;
    case 0x3EF03: // ...as well as in the "No One Escapes Cidhna Mine" quest
        return false;
    default:
        return true;
    }
}

void ObjectService::OnDisconnected(const DisconnectedEvent&) noexcept
{
    s_assemblyFollower.store(false, std::memory_order_release);
    RestoreKinematicProbe();
    std::lock_guard remoteLock(m_remotePhysicsLock);
    {
        std::lock_guard stepLock(s_stepTargetsLock);
        s_stepTargets.clear();
        s_stepTargetsBuilding.clear();
        s_followProbes.clear();
    }
    s_preStepExpectedEpoch.store(0, std::memory_order_release);
    s_watchedBodyWrapper.store(nullptr, std::memory_order_release);
    s_watchedHavokBody.store(nullptr, std::memory_order_release);
    s_watchedHavokWorld.store(nullptr, std::memory_order_release);
    {
        std::lock_guard lock(m_remotePhysicsLock);
        m_remoteReferencePoses.clear();
        s_sharedDropPoseGenerations.clear();
        s_renderDiagnosticsUntilMs.store(0, std::memory_order_relaxed);
        s_renderActorIds.clear();
        s_hostRenderProbes.clear();
        s_hostSampleProbes.clear();
        s_drawnGapProbes.clear();
        s_nextRenderActorsMs = 0;
    }
    {
        std::lock_guard lock(m_hostPhysicsLock);
        m_captureOnMainFrame.store(false, std::memory_order_relaxed);
        m_referencePoses.clear();
        m_ownedPhysicsGenerations.clear();
        m_physicsBodies.Clear();
        m_physicsUpdateRefs.Clear();
        m_physicsPromotions.clear();
        m_physicsSnapshotRead = m_physicsSnapshotCount = 0;
        m_nextPhysicsSnapshot = {};
        m_physicsEpoch = 0;
    }
    {
        std::lock_guard lock(m_hostPhysicsLock);
        m_physicsCells.clear();
        m_nextPhysicsCells = m_nextOwnedPhysics = m_nextPhysicsMaintenance = {};
    }
}

void ObjectService::OnUpdate(const UpdateEvent&) noexcept
{
    if (!s_loggedUpdateThread.exchange(true))
        spdlog::info("Object update runs on thread {}", GetCurrentThreadId());
    {
        const auto mainThread = s_mainThreadId.load(std::memory_order_relaxed);
        const auto thread = static_cast<uint32_t>(GetCurrentThreadId());
        (thread == mainThread ? s_updatesOnMain : s_updatesOffMain).fetch_add(1, std::memory_order_relaxed);
        if (const auto now = std::chrono::steady_clock::now(); now >= s_nextThreadReport)
        {
            s_nextThreadReport = now + std::chrono::seconds(20);
            spdlog::info("Object update threads: {} on main {}, {} elsewhere (this one {})", s_updatesOnMain.load(),
                mainThread, s_updatesOffMain.load(), thread);
        }
    }
    const auto diagnosticNow = GetTickCount64();
    if (m_transport.IsConnected() && m_world.GetPartyService().IsInParty() &&
        IsRenderDiagnosticsArmed())
    {
        if (diagnosticNow >= s_nextRenderActorsMs)
        {
            std::vector<uint32_t> actorIds;
            auto view = m_world.view<FormIdComponent>();
            for (auto entity : view)
            {
                const auto formId = view.get<FormIdComponent>(entity).Id;
                auto* pActor = Cast<Actor>(TESForm::GetById(formId));
                if (pActor && pActor->loadedState && pActor->GetExtension() &&
                    !pActor->GetExtension()->IsPlayer())
                    actorIds.push_back(formId);
            }
            std::lock_guard lock(m_remotePhysicsLock);
            s_renderActorIds.swap(actorIds);
            const auto expired = [diagnosticNow](const auto& entry)
            {
                auto* pReference = Cast<TESObjectREFR>(TESForm::GetById(entry.first));
                return !pReference || !pReference->loadedState ||
                    (diagnosticNow >= entry.second.LastSeenMs &&
                        diagnosticNow - entry.second.LastSeenMs > 10000);
            };
            std::erase_if(s_hostRenderProbes, expired);
            std::erase_if(s_drawnGapProbes, expired);
            std::erase_if(s_hostSampleProbes, [](const auto& entry)
            {
                return !s_hostRenderProbes.contains(entry.first);
            });
            s_nextRenderActorsMs = diagnosticNow + 1000;
        }
    }
    else if (s_nextRenderActorsMs)
    {
        std::lock_guard lock(m_remotePhysicsLock);
        s_renderActorIds.clear();
        s_hostRenderProbes.clear();
        s_hostSampleProbes.clear();
        s_drawnGapProbes.clear();
        s_nextRenderActorsMs = 0;
    }

    if (!m_transport.IsConnected() || !m_world.GetPartyService().IsInParty())
    {
        m_applyOnMainFrame.store(false, std::memory_order_relaxed);
        m_captureOnMainFrame.store(false, std::memory_order_relaxed);
        {
            std::lock_guard lock(m_hostPhysicsLock);
            if (m_physicsEpoch || !m_referencePoses.empty())
            {
                m_physicsEpoch = 0;
                m_referencePoses.clear();
                m_physicsBodies.Clear();
                m_physicsUpdateRefs.Clear();
                m_physicsPromotions.clear();
                m_physicsSnapshotRead = m_physicsSnapshotCount = 0;
                m_nextPhysicsSnapshot = {};
                m_physicsCells.clear();
                m_nextPhysicsCells = m_nextOwnedPhysics = m_nextPhysicsMaintenance = {};
            }
        }
        RestoreKinematicProbe();
        s_preStepExpectedEpoch.store(0, std::memory_order_release);
        s_watchedBodyWrapper.store(nullptr, std::memory_order_release);
        s_watchedHavokBody.store(nullptr, std::memory_order_release);
        s_watchedHavokWorld.store(nullptr, std::memory_order_release);
        return;
    }

    s_preStepExpectedEpoch.store(m_world.GetPartyService().IsLeader() ? 0 :
        m_world.GetPartyService().GetStartEpoch(),
        std::memory_order_release);
    for (const auto& drop : m_world.GetSharedDropService().TakePhysics())
    {
        NotifyPhysicsReferencesMove message;
        message.AuthorityEpoch = drop.Epoch;
        message.Tick = drop.Tick;
        message.Updates.push_back(drop.Physics);
        s_sharedDropGeneration = drop.Generation;
        OnPhysicsReferencesMove(message);
        s_sharedDropGeneration = 0;
    }
    if (m_world.GetPartyService().IsLeader() && !m_world.GetSharedDropService().HasRemoteReferences())
    {
        // Promoted to leader: no host-driven bodies here any more; the native step must not keep
        // steering the last published ones.
        std::lock_guard stepLock(s_stepTargetsLock);
        s_followProbesDirty |= !s_stepTargets.empty();
        s_stepTargets.clear();
    }
    // Ownership is service data, not scene data. Compare it on the worker and
    // enqueue only changed identities; never walk every owned drop on the main frame.
    const auto ownedNow = std::chrono::steady_clock::now();
    bool pollOwners{};
    {
        std::lock_guard hostLock(m_hostPhysicsLock);
        pollOwners = ownedNow >= m_nextOwnedPhysics;
        if (pollOwners)
            m_nextOwnedPhysics = ownedNow + 50ms;
    }
    if (pollOwners)
    {
        const auto owned = m_world.GetSharedDropService().OwnedReferences();
        std::unordered_map<uint32_t, uint32_t> generations;
        generations.reserve(owned.size());
        for (const auto id : owned)
            generations.emplace(id, m_world.GetSharedDropService().PhysicsGeneration(id));
        std::lock_guard hostLock(m_hostPhysicsLock);
        for (const auto& [id, generation] : generations)
        {
            const auto previous = m_ownedPhysicsGenerations.find(id);
            if (previous == m_ownedPhysicsGenerations.end() || previous->second != generation)
                QueuePhysicsRefresh(id);
        }
        for (const auto& [id, generation] : m_ownedPhysicsGenerations)
            if (!generations.contains(id))
                QueuePhysicsRefresh(id);
        m_ownedPhysicsGenerations.swap(generations);
    }
    m_captureOnMainFrame.store(true, std::memory_order_relaxed);
    m_applyOnMainFrame.store(true, std::memory_order_relaxed);
    FlushPhysicsSnapshots();
}

void ObjectService::RefreshPhysicsDiagnostics() noexcept
{
    const auto phaseFormId = s_referencePhaseFormId.load(
        std::memory_order_acquire);
    const auto playbackFormId = s_preStepPlaybackFormId.load(
        std::memory_order_acquire);
    const auto playbackMode = s_preStepPlaybackMode.load(
        std::memory_order_acquire);
    const auto kinematicSelected = !m_world.GetPartyService().IsLeader() &&
        playbackFormId && (playbackMode == 3 || playbackMode == 4);
    if (s_kinematicProbeFormId.load(std::memory_order_acquire) !=
        (kinematicSelected ? playbackFormId : 0))
        RestoreKinematicProbe();
    const auto watchedFormId = playbackFormId ? playbackFormId : phaseFormId;
    if (watchedFormId)
    {
        auto* pReference = Cast<TESObjectREFR>(TESForm::GetById(watchedFormId));
        if (pReference && !Cast<Actor>(pReference))
        {
            MaybeInstallReferencePhaseHook(pReference);
            MaybeInstallRenderNodePhaseHook(pReference);
            DynamicBody selectedBody{};
            if (GetDynamicBody(pReference, selectedBody, kinematicSelected))
            {
                s_selectedBodyCacheRefreshes.fetch_add(1,
                    std::memory_order_relaxed);
                s_watchedHavokWorld.store(selectedBody.State.world,
                    std::memory_order_release);
                s_watchedHavokBody.store(selectedBody.HavokBody,
                    std::memory_order_release);
                s_watchedBodyWrapper.store(selectedBody.Wrapper,
                    std::memory_order_release);
            }
            else
            {
                s_watchedHavokBody.store(nullptr, std::memory_order_release);
                s_watchedHavokWorld.store(nullptr, std::memory_order_release);
                s_watchedBodyWrapper.store(nullptr, std::memory_order_release);
            }
        }
        else
        {
            s_watchedHavokBody.store(nullptr, std::memory_order_release);
            s_watchedHavokWorld.store(nullptr, std::memory_order_release);
            s_watchedBodyWrapper.store(nullptr, std::memory_order_release);
        }
    }

}

void ObjectService::CaptureHostPhysics(const bool aUpdateThread) noexcept
{
    const auto started = std::chrono::steady_clock::now();
    // The optional render probes share maps with playback diagnostics. Ordinary capture
    // has its own lock and never waits for the follower's playback/receive traversal.
    std::unique_lock diagnosticLock(m_remotePhysicsLock, std::defer_lock);
    const bool captureDiagnostics = IsRenderDiagnosticsArmed();
    if (captureDiagnostics)
        diagnosticLock.lock();
    std::unique_lock physicsLock(m_hostPhysicsLock);
    const auto watchedFormId = s_preStepPlaybackFormId.load(std::memory_order_acquire) ?
        s_preStepPlaybackFormId.load(std::memory_order_acquire) : s_referencePhaseFormId.load(std::memory_order_acquire);
    const auto now = std::chrono::steady_clock::now();
    const size_t lane = aUpdateThread ? 1 : 0;
    auto& nextSnapshot = m_nextPhysicsSnapshot[lane];
    if (now < nextSnapshot)
        return;
    if (PhysicsScan::MakeSnapshotRoom(m_physicsSnapshotRead, m_physicsSnapshotCount, m_physicsSnapshots.size()))
    {
        // Latest-state transport: capture this tick and advance LastSent normally.
        // Tick timestamps and the eviction counter expose discarded queued history.
        ++m_physicsSnapshotEvictions;
    }
    auto& snapshot = m_physicsSnapshots[(m_physicsSnapshotRead + m_physicsSnapshotCount) % m_physicsSnapshots.size()];
    snapshot.Count = 0;
    snapshot.Epoch = m_physicsEpoch;
    // A buffer returned by the sender can predate recent admissions.
    if (snapshot.Entries.size() < m_referencePoses.size())
        snapshot.Entries.resize((m_referencePoses.size() + 63) / 64 * 64);

    auto* pPlayer = PlayerCharacter::Get();
    if (!pPlayer || !pPlayer->parentCell)
        return;

    snapshot.Tick = SmoothClock::NowTick() ? SmoothClock::NowTick() : m_transport.GetClock().GetCurrentTick();
    if (!aUpdateThread && s_physicsStampEnabled.load(std::memory_order_relaxed))
    {
        const auto nowNs = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
            now.time_since_epoch()).count());
        const auto stepNs = s_worldUpdateLastStartNs.load(std::memory_order_relaxed);
        if (stepNs && nowNs >= stepNs && nowNs - stepNs < 200'000'000ull)
            snapshot.Tick -= (nowNs - stepNs) / 1'000'000ull;
    }
    // The main lane captures every frame: moving bodies (carts, debris) reach the follower at the owner's
    // frame rate instead of 20 Hz, so fast motion and jolts are not smoothed away between samples. Resting
    // bodies already go quiet per reference (500 ms / 5 s). The sleeping/repair lane keeps its 20 Hz cadence.
    if (!aUpdateThread)
        nextSnapshot = now;
    else
    {
        // Preserve the 20 Hz cadence through frame rounding; do not emit catch-up bursts.
        if (nextSnapshot.time_since_epoch().count() == 0)
            nextSnapshot = now;
        nextSnapshot += 50ms * ((now - nextSnapshot) / 50ms + 1);
    }
    const bool reportLane = !aUpdateThread;
    if (reportLane)
        s_physicsHostScans.fetch_add(1, std::memory_order_relaxed);
    uint32_t sampled{};
    uint32_t referencesVisited{};
    auto processReference = [&](TESObjectREFR* pReference)
    {
        if (!pReference || pReference == pPlayer || !pReference->loadedState ||
            !pReference->parentCell || !pReference->parentCell->IsAttached() || Cast<Actor>(pReference))
            return;

        const auto poseIt = m_referencePoses.find(pReference->formID);
        if (poseIt == m_referencePoses.end())
            return;
        auto& previous = poseIt->second;
        auto& sharedDrops = m_world.GetSharedDropService();
        const bool shared = previous.Shared;
        if (shared ? !sharedDrops.IsOwner(pReference->formID) :
            (!m_world.GetPartyService().IsLeader() || pReference->IsTemporary()))
            return;

        // Reject distant loaded-grid references before walking their native
        // collision/Havok graph. The old order performed several guarded
        // memory reads for every loaded ref, even outside stream range.
        const auto deltaFromPlayer = pReference->position - pPlayer->position;
        if (glm::dot(deltaFromPlayer, deltaFromPlayer) > 30000.f * 30000.f)
            return;

        const bool passive = !previous.Body;
        DynamicBody body{};
        if (!passive && !GetDynamicBody(pReference, body, true, true))
            return;
        ScopedPhysicsWorld worldLock(passive ? nullptr : body.State.world);
        if (!passive && (!worldLock.Lock || !GetDynamicBody(pReference, body, true, true)))
            return;
        // Host scene-driven bodies stream too. Followers stay dynamic. Doors are owned
        // by the door protocol and are deliberately excluded from this physics stream.
        const bool keyframed = !passive && body.State.motionType == 4;
        if (keyframed && (!pReference->baseForm || pReference->baseForm->formType == FormType::Door))
            return;
        if (!passive && !std::all_of(std::begin(body.State.transform),
                std::end(body.State.transform), [](float value)
                { return std::isfinite(value); }))
            return;

        const bool inserted = previous.LastSent.time_since_epoch().count() == 0;
        const bool assemblyStream = previous.Assembly;
        const bool awake = !passive && !keyframed && IsPhysicsBodyAwake(body.HavokBody);
        if (awake || assemblyStream)
            previous.LastActive = now;
        // Sleep alone is not enough to suppress a final changed pose or a keyframe. Keep
        // previously dynamic carts subscribed through their scripted keyframed arrival.
        if (!assemblyStream && !passive && !inserted && !keyframed && !awake &&
            pReference->position == previous.Position && pReference->rotation == previous.Rotation &&
            !PhysicsBodyMotionChanged(previous.LastSentBodyTransform, previous.LastSentBodyVelocity,
                std::to_array(body.State.transform),
                {body.State.linearVelocity[0], body.State.linearVelocity[1], body.State.linearVelocity[2]}) &&
            now - previous.LastSent < (previous.HasMoved ? 500ms : 5000ms))
            return;
        ++sampled;
        if (pReference->formID == watchedFormId)
            s_physicsHostSelectedObserved.fetch_add(1, std::memory_order_relaxed);
        if (inserted)
        {
            previous.Position = pReference->position;
            previous.Rotation = pReference->rotation;
            if (!passive)
            {
                std::copy(std::begin(body.State.transform), std::end(body.State.transform),
                    previous.LastSentBodyTransform.begin());
                previous.LastSentBodyVelocity = {body.State.linearVelocity[0],
                    body.State.linearVelocity[1], body.State.linearVelocity[2]};
                previous.HasBodyState = true;
            }
            // Fall through: send the resting pose once on discovery. Each PC loads its own
            // save, so a body that never moves on the host (measured: the parked Helgen carts
            // after Continue, 34 units and 0.75 rad apart) otherwise never converges.
        }

        const auto positionDelta = pReference->position - previous.Position;
        const auto rotationDelta = pReference->rotation - previous.Rotation;
        bool bodyMoved = passive ? previous.HasBodyState :
            !previous.HasBodyState;
        if (!passive && previous.HasBodyState)
        {
            const glm::vec3 velocity{body.State.linearVelocity[0],
                body.State.linearVelocity[1], body.State.linearVelocity[2]};
            std::array<float, 16> currentTransform{};
            std::copy(std::begin(body.State.transform),
                std::end(body.State.transform), currentTransform.begin());
            bodyMoved = PhysicsBodyMotionChanged(
                previous.LastSentBodyTransform, previous.LastSentBodyVelocity,
                currentTransform, velocity);
        }
        const bool referenceMoved =
            glm::dot(positionDelta, positionDelta) >= 0.01f ||
            glm::dot(rotationDelta, rotationDelta) >= 0.000001f;
        const bool moved = bodyMoved || referenceMoved;
        if (moved)
            previous.LastActive = now;
        if (passive && referenceMoved && !previous.Promote)
        {
            previous.Promote = true;
            m_physicsPromotions.push_back(pReference->formID);
        }
        previous.Position = pReference->position;
        previous.Rotation = pReference->rotation;
        if (moved)
            previous.HasMoved = true;
        // Periodic keyframes recover from a dropped delta or a follower that
        // enters an already-active cell. Resting bodies refresh slowly so a
        // follower that loaded later still converges; moving ones every 500 ms.
        if (!assemblyStream && !inserted && !awake && !moved && now - previous.LastSent < (previous.HasMoved ? 500ms : 5000ms))
            return;

        auto& update = previous.Update;
        update.ChildBodies.clear();
        update.MotionType = 0;
        update.LinearVelocity = {};
        update.BodyTransform = {};
        if (shared)
            update.Id = sharedDrops.PhysicsId(pReference->formID);
        else if (!m_world.GetModSystem().GetServerModId(pReference->formID, update.Id))
            return;
        update.Position = {previous.Position.x, previous.Position.y, previous.Position.z};
        update.Rotation = {previous.Rotation.x, previous.Rotation.y, previous.Rotation.z};
        if (!passive)
        {
            update.MotionType = 3;
            if (awake || (!keyframed && moved))
                previous.StreamedDynamic = true;
            update.LinearVelocity = {body.State.linearVelocity[0],
                body.State.linearVelocity[1], body.State.linearVelocity[2]};
            std::copy(std::begin(body.State.transform),
                std::end(body.State.transform), update.BodyTransform.begin());
            if (shared)
                update.Position = {body.State.transform[12] * kHavokToGameUnits,
                    body.State.transform[13] * kHavokToGameUnits, body.State.transform[14] * kHavokToGameUnits};
            // Only registered tether assemblies pay O(B) child capture cost. Membership
            // was built once at tether setup, and fixed slots survive motion transitions.
            if (assemblyStream)
            {
                std::lock_guard assemblyLock(s_assembliesLock);
                const auto assemblyIt = s_assemblies.find(pReference->formID);
                if (assemblyIt == s_assemblies.end())
                    return;
                const auto& assembly = assemblyIt->second;
                if (!assembly.Complete || assembly.Root.get() != pReference->GetNiNode())
                    return;
                for (size_t i = 0; i < assembly.Count; ++i)
                {
                    ActorPoseDiagnosticViews::RigidBody child{};
                    if (!ReadPhysicsMemory(AssemblyBody(assembly.Nodes[i].get()), child) ||
                        child.world != body.State.world)
                        return; // no partial/reindexed assembly packets
                    std::array<float, 7> record{};
                    for (size_t k = 0; k < 3; ++k)
                        record[k] = child.transform[12 + k] - body.State.transform[12 + k];
                    MatrixToQuaternion(child.transform, record.data() + 3);
                    update.ChildBodies.push_back(record);
                }
            }
        }
        if (!passive && captureDiagnostics)
        {
            s_hostRenderProbes[pReference->formID].LastSeenMs = GetTickCount64();
            auto& probe = s_hostSampleProbes[pReference->formID];
            const glm::vec3 position{update.Position.x, update.Position.y, update.Position.z};
            const float bodySpeed = glm::length(glm::vec3{update.LinearVelocity.x, update.LinearVelocity.y,
                update.LinearVelocity.z}) * kHavokToGameUnits;
            if (probe.Has && snapshot.Tick > probe.Tick && snapshot.Tick - probe.Tick < 200)
            {
                const float dtMs = static_cast<float>(snapshot.Tick - probe.Tick);
                const float implied = glm::length(position - probe.Position) / dtMs * 1000.f;
                const float error = std::abs(implied - bodySpeed);
                probe.ErrorSum += error;
                probe.ErrorMax = (std::max)(probe.ErrorMax, error);
                probe.BodySpeedSum += bodySpeed;
                probe.GapErrorSum += std::abs(dtMs - 50.f);
                const glm::vec3 bodyPosition{update.BodyTransform[12] * kHavokToGameUnits,
                    update.BodyTransform[13] * kHavokToGameUnits, update.BodyTransform[14] * kHavokToGameUnits};
                probe.BodyErrorSum += std::abs(glm::length(bodyPosition - probe.BodyPosition) / dtMs * 1000.f - bodySpeed);
                ++probe.Samples;
            }
            probe.Position = position;
            probe.BodyPosition = {update.BodyTransform[12] * kHavokToGameUnits, update.BodyTransform[13] * kHavokToGameUnits,
                update.BodyTransform[14] * kHavokToGameUnits};
            probe.Tick = snapshot.Tick;
            probe.Has = true;

        }
        auto& captured = snapshot.Entries[snapshot.Count++];
        captured.FormId = pReference->formID;
        captured.Generation = previous.Generation;
        captured.Shared = shared;
        captured.Id = update.Id;
        captured.Position = update.Position;
        captured.Rotation = update.Rotation;
        captured.MotionType = update.MotionType;
        captured.LinearVelocity = update.LinearVelocity;
        captured.BodyTransform = update.BodyTransform;
        captured.ChildCount = update.ChildBodies.size();
        std::copy(update.ChildBodies.begin(), update.ChildBodies.end(), captured.Children.begin());
        s_physicsHostUpdatesQueued.fetch_add(1, std::memory_order_relaxed);
        if (!passive && bodyMoved && !referenceMoved)
            s_physicsHostBodyOnlyUpdates.fetch_add(1,
                std::memory_order_relaxed);
        if (pReference->formID == watchedFormId)
            s_physicsHostSelectedQueued.fetch_add(1,
                std::memory_order_relaxed);
        previous.LastSent = now;
        previous.HasBodyState = !passive;
        if (!passive)
        {
            previous.LastSentBodyTransform = update.BodyTransform;
            previous.LastSentBodyVelocity = update.LinearVelocity;
        }
    };

    const auto knownRefreshStarted = std::chrono::steady_clock::now();
    if (aUpdateThread)
    {
        PhysicsScan::VisitRepair(m_physicsUpdateRefs, m_physicsPassiveCursor, 16, [&](uint32_t id)
        {
            ++referencesVisited;
            processReference(Cast<TESObjectREFR>(TESForm::GetById(id)));
        });
    }
    else
    {
        for (size_t i = 0; i < m_physicsBodies.Ids.size();)
        {
            const auto id = m_physicsBodies.Ids[i];
            ++referencesVisited;
            processReference(Cast<TESObjectREFR>(TESForm::GetById(id)));
            const auto pose = m_referencePoses.find(id);
            if (pose == m_referencePoses.end() || now - pose->second.LastActive >= 500ms)
            {
                m_physicsBodies.Erase(id);
                if (pose != m_referencePoses.end())
                    m_physicsUpdateRefs.Insert(id);
            }
            else
                ++i;
        }
    }
    s_hostKnownRefreshTiming.Record(HostScanDurationUs(knownRefreshStarted));
    const auto scanDurationUs = HostScanDurationUs(started);
    s_physicsLastLaneReferencesVisited.store(referencesVisited, std::memory_order_relaxed);
    if (aUpdateThread)
        m_physicsRepairReportUs += scanDurationUs;
    if (reportLane)
    {
        s_physicsLastHostScanDurationUs.store(scanDurationUs,
            std::memory_order_relaxed);
        s_physicsHostScanTotalUs.fetch_add(scanDurationUs,
            std::memory_order_relaxed);
        auto previousMax = s_physicsHostScanMaxUs.load(std::memory_order_relaxed);
        while (scanDurationUs > previousMax &&
            !s_physicsHostScanMaxUs.compare_exchange_weak(previousMax,
                scanDurationUs, std::memory_order_relaxed)) {}
        s_physicsLastHostReferencesVisited.store(referencesVisited,
            std::memory_order_relaxed);
        s_physicsLastHostCandidateCount.store(static_cast<uint32_t>((std::min)(
            size_t{UINT32_MAX}, m_physicsBodies.Ids.size() + m_physicsUpdateRefs.Ids.size())),
            std::memory_order_relaxed);
        s_physicsLastHostUpdatesQueued.store(static_cast<uint32_t>(snapshot.Count), std::memory_order_relaxed);
    }
    if (snapshot.Count)
        ++m_physicsSnapshotCount;
    // Report the main-frame lane when enabled; the legacy switch reports the update lane.
    if (reportLane)
    {
        m_physicsScanReportTotalUs += scanDurationUs;
        ++m_physicsScanReportCount;
        m_physicsScanReportMaxUs = (std::max)(m_physicsScanReportMaxUs, scanDurationUs);
        if (m_nextPhysicsScanReport.time_since_epoch().count() == 0)
            m_nextPhysicsScanReport = now + 5s;
        if (now >= m_nextPhysicsScanReport)
        {
            const auto bodies = m_physicsBodies.Ids.size();
            physicsLock.unlock();
            spdlog::info("Physics scan: {} bodies {} sampled {:.2f} ms avg {:.2f} ms max (5 s)",
                bodies, sampled,
                static_cast<double>(m_physicsScanReportTotalUs) / m_physicsScanReportCount / 1000.,
                m_physicsScanReportMaxUs / 1000.);
            spdlog::info("Physics maintenance: {:.2f} ms total, repair capture {:.2f} ms total (5 s); {} snapshot evictions; {} movement queue overflows",
                m_physicsMaintenanceReportUs / 1000., m_physicsRepairReportUs / 1000., m_physicsSnapshotEvictions,
                s_movementOverflows.load(std::memory_order_relaxed));
            m_physicsMaintenanceReportUs = m_physicsRepairReportUs = 0;
            m_physicsScanReportTotalUs = m_physicsScanReportCount = m_physicsScanReportMaxUs = 0;
            m_nextPhysicsScanReport = now + 5s;
        }
    }
}

void ObjectService::OnPhysicsReferencesMove(const NotifyPhysicsReferencesMove& acMessage) noexcept
{
    std::lock_guard lock(m_remotePhysicsLock);
    const auto& party = m_world.GetPartyService();
    if (!party.IsInParty() ||
        acMessage.AuthorityEpoch != party.GetStartEpoch())
        return;
    s_physicsFollowerPacketsReceived.fetch_add(1,
        std::memory_order_relaxed);

    for (const auto& update : acMessage.Updates)
    {
        const bool shared = update.Id.ModId == SharedDropData::PhysicsModId;
        if (shared && !s_sharedDropGeneration)
            continue;
        if (!shared && party.IsLeader())
            continue;
        if (!std::isfinite(update.Position.x) || !std::isfinite(update.Position.y) ||
            !std::isfinite(update.Position.z) || !std::isfinite(update.Rotation.x) ||
            !std::isfinite(update.Rotation.y) || !std::isfinite(update.Rotation.z))
            continue;
        const uint32_t formId = shared ? m_world.GetSharedDropService().ResolvePhysics(update.Id) :
            m_world.GetModSystem().GetGameId(update.Id);
        auto* pReference = Cast<TESObjectREFR>(TESForm::GetById(formId));
        if (!pReference || Cast<Actor>(pReference) || !pReference->loadedState)
            continue;

        if (shared)
        {
            if (m_world.GetSharedDropService().PhysicsGeneration(formId) != s_sharedDropGeneration)
                continue;
            if (s_sharedDropPoseGenerations[formId] != s_sharedDropGeneration)
                m_remoteReferencePoses.erase(formId);
            s_sharedDropPoseGenerations[formId] = s_sharedDropGeneration;
        }

        // Items promoted from the passive lane when thrown carry the same authoritative
        // body stream as carts. Keep accepting that stream after they come to rest.
        if (update.MotionType == 3)
        {
            if (!std::isfinite(update.LinearVelocity.x) ||
                !std::isfinite(update.LinearVelocity.y) ||
                !std::isfinite(update.LinearVelocity.z))
                continue;
            if (!std::all_of(update.BodyTransform.begin(),
                    update.BodyTransform.end(), [](float value)
                    { return std::isfinite(value); }))
                continue;
            if (update.ChildBodies.size() > PhysicsReferenceUpdate::kMaxChildBodies ||
                !std::all_of(update.ChildBodies.begin(), update.ChildBodies.end(), [](const auto& child)
                {
                    const float norm = child[3] * child[3] + child[4] * child[4] + child[5] * child[5] + child[6] * child[6];
                    return std::all_of(child.begin(), child.end(), [](float v) { return std::isfinite(v); }) &&
                        norm > 0.5f && norm < 1.5f;
                }))
                continue;
            auto& pose = m_remoteReferencePoses[formId];
            if (pose.AuthorityEpoch == acMessage.AuthorityEpoch && acMessage.Tick <= pose.Tick)
                continue;
            pose.PriorPosition = pose.Position;
            pose.PriorTick = pose.Tick;
            pose.Position.x = update.Position.x;
            pose.Position.y = update.Position.y;
            pose.Position.z = update.Position.z;
            pose.Rotation.x = update.Rotation.x;
            pose.Rotation.y = update.Rotation.y;
            pose.Rotation.z = update.Rotation.z;
            // A periodic resend of the same resting pose must not re-arm its log.
            // Reuse the stream's motion thresholds, including rotation-only motion.
            if (glm::length(update.LinearVelocity) * kHavokToGameUnits > 2.f ||
                PhysicsBodyMotionChanged(pose.BodyTransform, {}, update.BodyTransform, {}))
                pose.LoggedFinalPose = false;
            pose.LinearVelocity = update.LinearVelocity;
            pose.BodyTransform = update.BodyTransform;
            pose.Tick = acMessage.Tick;
            pose.AuthorityEpoch = acMessage.AuthorityEpoch;
            pose.LastReceived = std::chrono::steady_clock::now();
            pose.BodyDriven = true;
            pose.HostMotionType = update.MotionType;
            pose.Samples[pose.SampleNext] = {acMessage.Tick, pose.Position, pose.Rotation, update.ChildBodies,
                {}};
            {
                auto& stored = pose.Samples[pose.SampleNext];
                const auto& t = update.BodyTransform;
                stored.BodyPosition = {t[12], t[13], t[14]};
                const glm::quat bodyRotation = glm::normalize(glm::quat_cast(glm::mat3{glm::vec3{t[0], t[1], t[2]},
                    glm::vec3{t[4], t[5], t[6]}, glm::vec3{t[8], t[9], t[10]}}));
                stored.BodyRotation = {bodyRotation.x, bodyRotation.y, bodyRotation.z, bodyRotation.w};
            }
            {
                auto& velocity = pose.Samples[pose.SampleNext].Velocity;
                velocity.x = update.LinearVelocity.x * kHavokToGameUnits;
                velocity.y = update.LinearVelocity.y * kHavokToGameUnits;
                velocity.z = update.LinearVelocity.z * kHavokToGameUnits;
            }
            pose.SampleNext = (pose.SampleNext + 1) % pose.Samples.size();
            pose.SampleCount = (std::min)(pose.SampleCount + 1, static_cast<uint32_t>(pose.Samples.size()));
            if (formId == s_preStepPlaybackFormId.load(
                    std::memory_order_relaxed) ||
                formId == s_referencePhaseFormId.load(
                    std::memory_order_relaxed))
            {
                s_physicsFollowerSelectedReceived.fetch_add(1,
                    std::memory_order_relaxed);
                const auto currentTick = m_transport.GetClock().GetCurrentTick();
                s_physicsLastSelectedTransitAgeMs.store(static_cast<uint32_t>(
                    (std::min)(uint64_t{UINT32_MAX},
                        currentTick >= acMessage.Tick ?
                            currentTick - acMessage.Tick : 0)),
                    std::memory_order_relaxed);
            }
            PublishPreStepTarget(formId, acMessage.AuthorityEpoch,
                acMessage.Tick, update);
            continue;
        }

        if (update.MotionType != 0 || !IsPassivePhysicsReference(pReference))
            continue;

        auto& pose = m_remoteReferencePoses[formId];
        if (pose.AuthorityEpoch == acMessage.AuthorityEpoch && acMessage.Tick <= pose.Tick)
            continue;
        pose.Position = update.Position;
        pose.Rotation = update.Rotation;
        pose.Tick = acMessage.Tick;
        pose.AuthorityEpoch = acMessage.AuthorityEpoch;
        pose.LastReceived = std::chrono::steady_clock::now();
        pose.BodyDriven = false;
        continue;
    }
}

void DrainActorSceneUpdates() noexcept;
void PublishTetheredHorseZ(uint32_t aFormId, float aZ) noexcept;

void ObjectService::OnMainFrame() noexcept
{
    s_mainThreadId.store(static_cast<uint32_t>(GetCurrentThreadId()), std::memory_order_relaxed);
    auto* pService = s_objectService.load(std::memory_order_acquire);
    if (!pService)
        return;
    DrainRetiredBodies();
    DrainActorSceneUpdates();
    {
        std::lock_guard lock(s_retiredAssemblyNodesLock);
        s_retiredAssemblyNodes.swap(s_assemblyNodesDraining);
    }
    for (auto* node : s_assemblyNodesDraining)
        node->DecRef();
    s_assemblyNodesDraining.clear();
    pService->m_world.GetSharedDropService().OnMainFrame();
    RecordMotionTrace(pService->m_world);
    if (IsRenderDiagnosticsArmed())
    {
        std::lock_guard lock(pService->m_remotePhysicsLock);
        ProbeHostRender();
    }
    const bool active = pService->m_transport.IsConnected() && pService->m_world.GetPartyService().IsInParty();
    s_assemblyFollower.store(active && !pService->m_world.GetPartyService().IsLeader(), std::memory_order_release);
    {
        // Render all owned (main thread): set or clear kAlwaysDraw|kForceUpdate on owned actor roots and hitched
        // carts. Tracks what it marked so switching off (or leaving the session) restores the flags exactly.
        static std::unordered_map<NiAVObject*, uint32_t> s_marked; // root -> bits we added
        static uint64_t s_renderAllRuns{};
        const bool want = active && s_renderAll.load(std::memory_order_relaxed);
        // Latch "in the camera view this frame" on every owned NPC: Actor boolBits (flags1, +0xE8) kWasInFrustrum
        // (1 << 21) is the per-frame cull RESULT the engine's off-screen shortcuts read (skeleton world update used
        // by E37320 placement, controller -> reference writeback). Setting cull inputs (NiAVObject kNotVisible /
        // kAlwaysDraw / kForceUpdate, kFarAway) did not move the float rate (Muse diag-cull, run 20260928-082058).
        // Plain bit write on the main thread; the cull pass re-evaluates it for drawing every frame.
        size_t latched = 0;
        if (want)
        {
            auto owned = pService->m_world.view<LocalComponent, FormIdComponent>();
            for (auto entity : owned)
            {
                auto* actor = Cast<Actor>(TESForm::GetById(owned.get<FormIdComponent>(entity).Id));
                if (actor && actor != PlayerCharacter::Get() && !actor->IsDeleted() && !actor->IsDisabled() && actor->GetNiNode())
                {
                    actor->flags1 |= (1u << 21);
                    ++latched;
                }
            }
        }
        if (want && (++s_renderAllRuns == 1 || s_renderAllRuns % 3600 == 0))
            spdlog::info("Render all owned: {} NPCs latched in view", latched);
    }
    // Owner side: hitched horses follow their own character controller vertically. Measured (run 20260928-011041):
    // off camera the controller keeps stepping down the slope (12474 -> 12330) while the actor reference stays at
    // 12479.6 and xy still advances; the engine skips the controller->reference z writeback, then catches up in one
    // ~150 u step that yanks the tether and flips the cart. Normal reference z = controller z - 6.4 (both horses).
    // Write only the reference/3D (SetPosition without Havok sync); the controller already is where it should be.
    if (active && pService->m_world.GetPartyService().IsLeader() && ObjectService::IsHorseWriteback())
    {
        static std::unordered_map<uint32_t, float> s_refMinusController;
        struct FrozenZ { float Z{}; uint64_t SinceMs{}; float ControllerZ{}; };
        static std::unordered_map<uint32_t, FrozenZ> s_frozen;
        static uint64_t s_writebacks{};
        std::vector<uint32_t> horses;
        {
            std::lock_guard lock(s_assembliesLock);
            for (const auto& [cartId, assembly] : s_assemblies)
                horses.push_back(assembly.HorseId);
        }
        for (const auto horseId : horses)
        {
            auto* horse = Cast<Actor>(TESForm::GetById(horseId));
            if (!horse || horse->GetExtension()->IsRemote() || !horse->currentProcess || !horse->currentProcess->middleProcess)
                continue;
            auto* controller = *reinterpret_cast<uint8_t**>(reinterpret_cast<uint8_t*>(horse->currentProcess->middleProcess) + 0x250);
            if (!controller)
                continue;
            alignas(16) float pos[4]{};
            using GetPositionFn = void(const void*, float*, bool);
            (*reinterpret_cast<GetPositionFn***>(controller))[2](controller, pos, false);
            const float controllerZ = pos[2] * kHavokToGameUnits;
            if (!std::isfinite(controllerZ))
                continue;
            auto& offset = s_refMinusController.try_emplace(horseId, -6.4f).first->second;
            const float wanted = controllerZ + offset;
            PublishTetheredHorseZ(horseId, wanted);
            const float error = horse->position.z - wanted;
            if (std::abs(error) < 3.f)
            {
                offset += 0.1f * ((horse->position.z - controllerZ) - offset); // learn while in sync
                continue;
            }
            // Only the float case: the reference sits ABOVE its controller and has not moved vertically for 150 ms
            // (the skipped writeback). Correcting every small lag moved the horse node each frame, and the tether
            // helper on its spine tugged the cart (run 20260928-012104: 3000 writebacks, cart 0 jumps 59/min).
            auto& frozen = s_frozen[horseId];
            const auto nowMs = GetTickCount64();
            if (std::abs(horse->position.z - frozen.Z) > 0.05f)
            {
                frozen.Z = horse->position.z;
                frozen.SinceMs = nowMs;
                frozen.ControllerZ = controllerZ;
            }
            // Offset-independent trigger: frozen for 150 ms while the controller descended > 8 u since the freeze
            // began (the learned offset can drift during a ride; run 20260928-073658 missed a 21 u hold).
            const bool controllerLeft = frozen.ControllerZ - controllerZ > 8.f;
            if ((error < 12.f && !controllerLeft) || error > 400.f || nowMs - frozen.SinceMs < 150)
                continue;
            NiPoint3 fixed = horse->position;
            fixed.z = wanted;
            horse->SetPosition(fixed, false);
            if (++s_writebacks <= 10 || s_writebacks % 200 == 0)
                spdlog::info("Horse {:X}: reference z {:.1f} lagged its controller by {:.1f} u; written back ({} total)",
                    horseId, horse->position.z, error, s_writebacks);
        }
    }

    // (A horse ground-snap was tried here and removed: setting the horse down jerked the tether and threw the
    // cart around, 66 cart z jumps in 48 s, run 20260928-004116.)
    // Owner side: keep every hitched cart's scene graph current while it is off camera. E37320 places a cart
    // horse at a cart node's world translate (NiAVObject +0xA0) every frame; the engine refreshes those node
    // transforms only in the visible update, so with the host looking away the horse kept a frozen height and
    // dropped ~185 u when the cart came back into view (owner repro; visibility flags on the horse alone did
    // not help, runs 232814..234803). Clear the culled bit on the cart tree and update it every frame.
    if (active && pService->m_world.GetPartyService().IsLeader() && s_cartNodeRefresh.load(std::memory_order_relaxed))
    {
        std::vector<std::shared_ptr<NiAVObject>> roots;
        {
            std::lock_guard lock(s_assembliesLock);
            for (const auto& [cartId, assembly] : s_assemblies)
                if (assembly.Root)
                    roots.push_back(assembly.Root);
        }
        struct NiUpdateData { float Time{}; uint32_t Flags{}; } data{};
        using UpdateFn = void(NiAVObject*, NiUpdateData*);
        POINTER_SKYRIMSE(UpdateFn, update, 70251);
        for (const auto& root : roots)
        {
            uint32_t visited = 0;
            auto walk = [&](auto&& self, NiAVObject* apNode, uint32_t aDepth) -> void
            {
                if (!apNode || aDepth > 32 || ++visited > 512)
                    return;
                *reinterpret_cast<uint32_t*>(reinterpret_cast<uint8_t*>(apNode) + 0xF4) &= ~0x100000u;
                if (auto* node = apNode->AsNode())
                    for (uint16_t i = 0; i < node->children.length; ++i)
                        self(self, node->children.data[i], aDepth + 1);
            };
            walk(walk, root.get(), 0);
            update.Get()(root.get(), &data);
        }
    }
    // One presentation clock for the whole assembly: cart bodies, the horse (root via VehiclePresentationTick
    // and bones via PoseCopyAuthority) and the riders all play the owner's timeline presentationDelay behind now.
    // A separate "now" clock for the cart put it ~300 ms (about 48 u) ahead of the horse's bones, and the tether
    // then stretched the horse between the two.
    const uint64_t frameTick = SmoothClock::NowTick() ? SmoothClock::NowTick() : pService->m_transport.GetClock().GetCurrentTick();
    const uint64_t presentationDelay = pService->m_world.GetCharacterService().GetPresentationDelayMs();
    const uint64_t assemblyTick = frameTick > presentationDelay ? frameTick - presentationDelay : 0;
    s_assemblyTick.store(assemblyTick, std::memory_order_release);
    {
        const double nowMs = SmoothClock::NowMs() > 0.0 ? SmoothClock::NowMs() : static_cast<double>(frameTick);
        s_assemblyTimeMs.store(nowMs - static_cast<double>(presentationDelay), std::memory_order_release);
    }
    {
        std::lock_guard assemblyLock(s_assembliesLock);
        const auto epoch = active ? pService->m_world.GetPartyService().GetStartEpoch() : 0;
        const bool epochChanged = s_assemblyEpoch != epoch;
        s_assemblyEpoch = epoch;
        if (epochChanged)
            s_vehicleActorPoses.clear();
        s_newAssemblies.swap(s_assembliesRefreshing);
        std::erase_if(s_assemblies, [&](auto& entry)
        {
            auto* cart = Cast<TESObjectREFR>(TESForm::GetById(entry.first));
            auto* horse = Cast<Actor>(TESForm::GetById(entry.second.HorseId));
            if (!cart || !horse || cart->GetNiNode() != entry.second.Root.get() ||
                horse->GetNiNode() != entry.second.HorseRoot.get() ||
                entry.second.HelperSlot >= entry.second.Count ||
                entry.second.Nodes[entry.second.HelperSlot]->collisionObject != entry.second.Tether)
            {
                s_horseVehicles.erase(entry.second.HorseId);
                SetTetheredHorse(entry.second.HorseId, false);
                s_tetherVehicles.erase(entry.second.Tether);
                return true;
            }
            if (epochChanged || !active || pService->m_world.GetPartyService().IsLeader())
            {
                entry.second.ActiveUntil = 0;
                for (auto& body : entry.second.Lifetimes)
                    body.reset();
            }
            return false;
        });
        s_vehicleActorsApplying.clear();
        std::erase_if(s_vehicleActorPoses, [&](auto& entry)
        {
            auto& pose = entry.second;
            auto* actor = Cast<Actor>(TESForm::GetById(entry.first));
            const auto vehicle = s_assemblies.find(pose.VehicleId);
            uint32_t handle{};
            if (actor)
                ReadNativeMemory(reinterpret_cast<uint8_t*>(actor) + 0x1F0, handle);
            auto* attached = handle ? TESObjectREFR::GetByHandle(handle) : nullptr;
            const bool belongs = vehicle != s_assemblies.end() &&
                (vehicle->second.HorseId == entry.first || (attached && attached->formID == pose.VehicleId));
            if (!s_assemblyFollower.load(std::memory_order_relaxed) || !actor || actor->actorState.IsDeadState() ||
                actor->GetNiNode() != pose.Root.get() || !belongs || pose.VehicleRoot != vehicle->second.Root ||
                GetTickCount64() - pose.QueuedAt > 500)
                return true;
            if (GetTickCount64() < vehicle->second.ActiveUntil)
                s_vehicleActorsApplying.push_back(pose);
            return false;
        });
    }
    for (auto id : s_assembliesRefreshing)
        pService->QueuePhysicsRefresh(id);
    s_assembliesRefreshing.clear();
    // No registry/native-world lock across actor scene updates. These run before
    // native Main::Update and its tether/controller jobs, on the same cart clock.
    for (const auto& pose : s_vehicleActorsApplying)
    {
        auto* actor = Cast<Actor>(TESForm::GetById(pose.FormId));
        if (!actor || actor->GetNiNode() != pose.Root.get())
            continue;
        const float seconds = assemblyTick > pose.Tick ?
            static_cast<float>((std::min)(uint64_t{150}, assemblyTick - pose.Tick)) / 1000.f : 0.f;
        actor->ForcePosition(pose.Position + pose.Velocity * seconds);
        const auto rotation = pose.Rotation + pose.Angular * seconds;
        actor->SetRotation(rotation.x, rotation.y, rotation.z);
        std::lock_guard lock(s_assembliesLock);
        if (auto it = s_vehicleActorPoses.find(pose.FormId); it != s_vehicleActorPoses.end() && it->second.Root == pose.Root)
            it->second.PlacedAt = GetTickCount64();
    }
    s_collectMovingReferences.store(active, std::memory_order_relaxed);
    if (!active)
    {
        // Not following anything: nothing is replayed this pass, so every keyframed cart part is restored.
        // Outside our locks, like the per-pass drain in ApplyRemotePhysics.
        DrainPendingKeyframes();
        ObjectService::ResetTetheredHorseState();
        std::lock_guard remoteLock(pService->m_remotePhysicsLock);
        pService->m_remoteReferencePoses.clear();
        s_sharedDropPoseGenerations.clear();
        std::lock_guard stepLock(s_stepTargetsLock);
        s_stepTargets.clear();
        s_stepTargetsBuilding.clear();
        s_followProbes.clear();
        return;
    }
    pService->RefreshPhysicsDiagnostics();
    const auto maintenanceStarted = std::chrono::steady_clock::now();
    {
        std::lock_guard hostLock(pService->m_hostPhysicsLock);
        pService->RefreshPhysicsCandidates();
    }
    const auto maintenanceUs = HostScanDurationUs(maintenanceStarted);
    pService->CaptureHostPhysics(false);
    pService->CaptureHostPhysics(true); // bounded sleeping/repair lane, still on main
    {
        std::lock_guard hostLock(pService->m_hostPhysicsLock);
        for (const auto id : pService->m_physicsPromotions)
            pService->RefreshPhysicsReference(id);
        pService->m_physicsPromotions.clear();
    }
    // Logger runs after releasing both native-world and target locks.
    std::vector<FollowProbe> reports, summaries;
    const auto now = std::chrono::steady_clock::now();
    {
        std::lock_guard stepLock(s_stepTargetsLock);
        if (s_followProbesDirty || now >= s_nextFollowReport)
        {
            s_nextFollowReport = std::chrono::steady_clock::time_point::max();
            for (auto& [body, probe] : s_followProbes)
            {
                if (probe.Steps && now >= probe.NextSteerLog && probe.Gap > 10.f)
                {
                    reports.push_back(probe);
                    probe.NextSteerLog = now + 2s;
                }
                if (probe.NextLog.time_since_epoch().count() == 0)
                    probe.NextLog = now + 5s;
                if (now >= probe.NextLog)
                {
                    summaries.push_back(probe);
                    probe.Steps = probe.Teleports = 0;
                    probe.MaxErrorUnits = 0.f;
                    probe.NextLog = now + 5s;
                }
                s_nextFollowReport = (std::min)(s_nextFollowReport, probe.NextLog);
                if (probe.Steps && probe.Gap > 10.f)
                    s_nextFollowReport = (std::min)(s_nextFollowReport, probe.NextSteerLog);
            }
        }
        PruneFollowProbes(s_followProbes, s_stepTargets, s_followProbesDirty);
    }
    for (const auto& probe : reports)
        spdlog::info("Cart steer {:X}: gap {:.1f} u vel {:.1f} host-age {} ms fighter unresolved; {} steps {} teleports",
            probe.FormId, probe.Gap, probe.Speed, probe.HostAgeMs, probe.Steps, probe.Teleports);
    for (const auto& probe : summaries)
        spdlog::info("Follow body {:X}: {} steps, {} teleports, 0 keyframed placements, largest gap {:.1f} u",
            probe.FormId, probe.Steps, probe.Teleports, probe.MaxErrorUnits);
    pService->m_physicsMaintenanceReportUs += maintenanceUs;
    pService->ApplyRemotePhysics();
}

bool ObjectService::AttachRider(Actor* apActor, const NiPoint3& acHostPosition, const float aHostHeading) noexcept
{
    auto* pService = s_objectService.load(std::memory_order_acquire);
    if (!pService || !apActor || !pService->m_applyOnMainFrame.load(std::memory_order_relaxed))
        return false;
    std::lock_guard lock(pService->m_remotePhysicsLock);
    const glm::vec3 host{acHostPosition.x, acHostPosition.y, acHostPosition.z};
    for (const auto& [formId, pose] : pService->m_remoteReferencePoses)
    {
        if (!pose.HostDriven || !pose.HasPlaybackTarget)
            continue;
        const glm::vec3 offset = host - pose.PlaybackTarget;
        if (glm::length(offset) > kRiderMatchUnits)
            continue;
        auto& rider = s_riders[apActor->formID];
        if (rider.ReferenceId != formId)
            spdlog::info("Actor {:X} rides host-driven body {:X} (offset {:.1f} u)", apActor->formID, formId, glm::length(offset));
        rider.ReferenceId = formId;
        rider.Offset = {};
        const float headingOffset = std::remainder(aHostHeading - pose.PlaybackHeading, static_cast<float>(TiltedPhoques::Pi * 2));
        if (!rider.HasHeadingOffset)
        {
            rider.HeadingOffset = headingOffset;
            rider.HasHeadingOffset = true;
        }
        else
            rider.HeadingOffset += std::remainder(headingOffset - rider.HeadingOffset, static_cast<float>(TiltedPhoques::Pi * 2)) * 0.01f;
        rider.SeenAt = std::chrono::steady_clock::now();
        return true;
    }
    s_riders.erase(apActor->formID);
    return false;
}

bool ObjectService::VehiclePresentationTick(Actor* apActor, uint64_t& aTick) noexcept
{
    if (!apActor || !s_assemblyFollower.load(std::memory_order_acquire))
        return false;
    // Only the HORSE shares the assembly clock (the tether consumes its pose). Passengers are seated on the
    // local cart by the native passenger controller and keep normal actor syncing (baseline).
    std::lock_guard lock(s_assembliesLock);
    const auto horse = s_horseVehicles.find(apActor->formID);
    if (horse == s_horseVehicles.end())
        return false;
    const auto it = s_assemblies.find(horse->second);
    if (it == s_assemblies.end() || GetTickCount64() >= it->second.ActiveUntil)
        return false;
    const auto tick = s_assemblyTick.load(std::memory_order_acquire);
    if (tick)
        aTick = tick;
    return true;
}

void ObjectService::QueueVehiclePose(Actor* apActor, uint64_t aTick, const NiPoint3& aPosition,
    const NiPoint3& aRotation, const NiPoint3& aVelocity, const NiPoint3& aAngular) noexcept
{
    uint32_t handle{};
    ReadNativeMemory(reinterpret_cast<uint8_t*>(apActor) + 0x1F0, handle);
    auto* vehicle = handle ? TESObjectREFR::GetByHandle(handle) : nullptr;
    std::lock_guard lock(s_assembliesLock);
    uint32_t id = vehicle ? vehicle->formID : 0;
    if (auto horse = s_horseVehicles.find(apActor->formID); horse != s_horseVehicles.end())
        id = horse->second;
    const auto assembly = s_assemblies.find(id);
    auto* root = apActor->GetNiNode();
    if (!root || assembly == s_assemblies.end() || !s_assemblyFollower.load(std::memory_order_acquire))
        return;
    auto& pose = s_vehicleActorPoses[apActor->formID];
    if (pose.Root.get() != root)
    {
        pose.Root = HoldAssemblyNode(root);
        pose.PlacedAt = 0;
    }
    pose.FormId = apActor->formID;
    pose.VehicleId = id;
    pose.VehicleRoot = assembly->second.Root;
    pose.Tick = aTick;
    pose.QueuedAt = GetTickCount64();
    pose.Position = aPosition;
    pose.Rotation = aRotation;
    pose.Velocity = aVelocity;
    pose.Angular = aAngular;
}

void ObjectService::ArmRenderDiagnostics() noexcept
{
    // Ride tests request a snapshot through coop-start-test before waiting up
    // to five minutes and reading the cart/sit logs.
    const auto until = GetTickCount64() + 360000;
    auto previous = s_renderDiagnosticsUntilMs.load(std::memory_order_relaxed);
    while (previous < until && !s_renderDiagnosticsUntilMs.compare_exchange_weak(
        previous, until, std::memory_order_relaxed))
    {
    }
}

bool ObjectService::IsRenderDiagnosticsArmed() noexcept
{
    const auto until = s_renderDiagnosticsUntilMs.load(std::memory_order_relaxed);
    return until && GetTickCount64() < until;
}

void PublishTetheredHorseZ(uint32_t aFormId, float aZ) noexcept
{
    for (size_t i = 0; i < s_tetheredHorses.size(); ++i)
        if (s_tetheredHorses[i].load(std::memory_order_relaxed) == aFormId)
        {
            s_tetheredHorseZ[i].store(aZ, std::memory_order_release);
            return;
        }
}

void ObjectService::ResetTetheredHorseState() noexcept
{
    for (auto& z : s_tetheredHorseZ)
        z.store(std::numeric_limits<float>::quiet_NaN(), std::memory_order_release);
}

float ObjectService::TetheredHorseExpectedZ(uint32_t aFormId) noexcept
{
    if (!aFormId)
        return std::numeric_limits<float>::quiet_NaN();
    for (size_t i = 0; i < s_tetheredHorses.size(); ++i)
        if (s_tetheredHorses[i].load(std::memory_order_acquire) == aFormId)
            return s_tetheredHorseZ[i].load(std::memory_order_acquire);
    return std::numeric_limits<float>::quiet_NaN();
}

bool ObjectService::IsTetheredHorse(uint32_t aFormId) noexcept
{
    if (!aFormId)
        return false;
    for (const auto& slot : s_tetheredHorses)
        if (slot.load(std::memory_order_acquire) == aFormId)
            return true;
    return false;
}

namespace
{
std::mutex s_sceneUpdateLock;
std::atomic<uint32_t> s_sceneUpdateMode{0};
std::atomic<bool> s_horseWriteback{false}; // superseded by AnimationSystem force_seen (cause, not symptom)
std::vector<uint32_t> s_sceneUpdateQueue, s_sceneUpdateDraining;
}

void ObjectService::SetHorseWriteback(bool aEnabled) noexcept
{
    s_horseWriteback.store(aEnabled, std::memory_order_relaxed);
    spdlog::info("Horse writeback: {}", aEnabled);
}

void ObjectService::SetCartNodeRefresh(bool aEnabled) noexcept
{
    s_cartNodeRefresh.store(aEnabled, std::memory_order_relaxed);
    spdlog::info("Cart node refresh: {}", aEnabled);
}

bool ObjectService::IsCartNodeRefresh() noexcept
{
    return s_cartNodeRefresh.load(std::memory_order_relaxed);
}

void ObjectService::SetRenderAll(bool aEnabled) noexcept
{
    s_renderAll.store(aEnabled, std::memory_order_relaxed);
    spdlog::info("Render all owned: {}", aEnabled);
}

bool ObjectService::IsRenderAll() noexcept
{
    return s_renderAll.load(std::memory_order_relaxed);
}

bool ObjectService::IsHorseWriteback() noexcept
{
    return s_horseWriteback.load(std::memory_order_relaxed);
}

void ObjectService::SetSceneUpdateMode(uint32_t aMode) noexcept
{
    s_sceneUpdateMode.store(aMode, std::memory_order_relaxed);
    spdlog::info("Scene update mode: {}", aMode);
}

uint32_t ObjectService::GetSceneUpdateMode() noexcept
{
    return s_sceneUpdateMode.load(std::memory_order_relaxed);
}

void ObjectService::QueueActorSceneUpdate(uint32_t aFormId) noexcept
{
    const auto mode = s_sceneUpdateMode.load(std::memory_order_relaxed);
    if (mode != 1 && mode != 2)
        return;
    std::lock_guard lock(s_sceneUpdateLock);
    if (s_sceneUpdateQueue.size() < 4096)
        s_sceneUpdateQueue.push_back(aFormId);
}

// Main thread, no engine job running: refresh owned NPC scene trees so off-camera bones are current (E37320 places
// an actor from its own skeleton node). Every NPC that is not seated, plus riders (cart drivers are seated and
// mounted; the hitched horse follows them). Plain seated passengers are skipped: E20318 places them and a refresh
// made them flash.
void DrainActorSceneUpdates() noexcept
{
    {
        std::lock_guard lock(s_sceneUpdateLock);
        s_sceneUpdateDraining.swap(s_sceneUpdateQueue);
    }
    std::sort(s_sceneUpdateDraining.begin(), s_sceneUpdateDraining.end());
    s_sceneUpdateDraining.erase(std::unique(s_sceneUpdateDraining.begin(), s_sceneUpdateDraining.end()),
        s_sceneUpdateDraining.end());
    struct NiUpdateData { float Time{}; uint32_t Flags{}; } data{};
    using UpdateFn = void(NiAVObject*, NiUpdateData*);
    POINTER_SKYRIMSE(UpdateFn, update, 70251);
    for (const auto formId : s_sceneUpdateDraining)
    {
        auto* actor = Cast<Actor>(TESForm::GetById(formId));
        auto* extension = actor ? actor->GetExtension() : nullptr;
        if (!actor || !extension || extension->IsRemote() || actor->IsDeleted() || actor->IsDisabled())
            continue;
        auto* root = actor->GetNiNode();
        if (!root)
            continue;
        const uint32_t sitSleepState = (actor->actorState.flags1 >> 14) & 0xF;
        if (sitSleepState != 0 && s_sceneUpdateMode.load(std::memory_order_relaxed) == 1)
        {
            const auto mount = actor->GetNativeMountState();
            if (!(mount.HorseExtra && mount.HorseHandle))
                continue;
        }
        update.Get()(root, &data);
    }
    s_sceneUpdateDraining.clear();
}

void ObjectService::SetCartCurve(bool aEnabled) noexcept
{
    s_cartCurve.store(aEnabled, std::memory_order_relaxed);
    spdlog::info("Cart curve: {}", aEnabled);
}

bool ObjectService::IsCartCurve() noexcept
{
    return s_cartCurve.load(std::memory_order_relaxed);
}

std::string ObjectService::MotionTrace(const std::string& aIds, const std::string& aBone, const std::string& aDump) noexcept
{
    // The bridge runs on the window thread; the recorder on the main frame. One lock covers both.
    std::lock_guard lock(s_motionTraceLock);
    if (!aDump.empty())
    {
        s_motionTraceOn.store(false, std::memory_order_relaxed);
        std::ofstream out(aDump, std::ios::trunc);
        for (const auto& r : s_motionTrace)
            out << fmt::format("{{\"t\":{:.3f},\"s\":{:.3f},\"id\":{},\"p\":[{:.2f},{:.2f},{:.2f}],\"h\":{:.2f},\"rz\":{:.2f},\"pl\":{},\"ref\":[{:.1f},{:.1f},{:.1f}],\"root\":[{:.1f},{:.1f},{:.1f}]}}\n",
                r.SteadyMs, r.SharedMs, r.FormId, r.X, r.Y, r.Z, r.Heading, r.ReferenceZ, r.PlayerId, r.RefX, r.RefY, r.RefZ,
                r.RootX, r.RootY, r.RootZ);
        const auto count = s_motionTrace.size();
        s_motionTrace.clear();
        s_motionTrace.shrink_to_fit();
        return fmt::format("\"dumped\":{},\"ok\":{}", count, static_cast<bool>(out));
    }
    if (!aIds.empty())
    {
        s_motionTraceIds.clear();
        s_motionTracePlayers = false;
        size_t start = 0;
        while (start < aIds.size())
        {
            const auto end = aIds.find(',', start);
            const auto part = aIds.substr(start, end == std::string::npos ? std::string::npos : end - start);
            if (part == "players")
                s_motionTracePlayers = true;
            else if (!part.empty())
                s_motionTraceIds.push_back(static_cast<uint32_t>(std::stoul(part, nullptr, 0)));
            if (end == std::string::npos)
                break;
            start = end + 1;
        }
        s_motionTraceBone = aBone;
        s_motionTrace.clear();
        s_motionTrace.reserve(kMotionTraceMax);
        s_motionTraceOn.store(true, std::memory_order_relaxed);
    }
    return fmt::format("\"on\":{},\"ids\":{},\"records\":{}", s_motionTraceOn.load(), s_motionTraceIds.size(),
        s_motionTrace.size());
}

void ObjectService::SetCartReplayEnabled(bool aEnabled) noexcept
{
    s_cartReplay.store(aEnabled, std::memory_order_relaxed);
    spdlog::info("Cart replay: {}", aEnabled ? "on (follower assemblies keyframed onto the owner pose)" :
        "off (dynamic steer)");
}

bool ObjectService::IsCartReplayEnabled() noexcept
{
    return s_cartReplay.load(std::memory_order_relaxed);
}

void ObjectService::SetCartPhysicsEnabled(bool aEnabled) noexcept
{
    // Compatibility entry point: the retired false mode keyframed follower bodies.
    // Keep the owner's dynamic-only policy and report rejected requests explicitly.
    s_cartPhysicsEnabled.store(true, std::memory_order_relaxed);
    spdlog::info("Cart physics: requested {} effective true; follower bodies remain dynamic and steered{}",
        aEnabled, aEnabled ? "" : "; disable request ignored because keyframed playback is retired");
}

void ObjectService::SetVisualLagFrameEnabled(bool aEnabled) noexcept
{
    s_visualLagFrameEnabled.store(aEnabled, std::memory_order_relaxed);
    spdlog::info("Host-driven body node one frame behind its body {}", aEnabled ? "on" : "off");
}

void ObjectService::SetBodyVelocityEnabled(bool aEnabled) noexcept
{
    s_bodyVelocityEnabled.store(aEnabled, std::memory_order_relaxed);
    spdlog::info("Host-driven body velocity {}", aEnabled ? "on" : "off");
}

void ObjectService::SetCartSmoothingEnabled(bool aEnabled) noexcept
{
    s_cartSmoothingEnabled.store(aEnabled, std::memory_order_relaxed);
    spdlog::info("Host-driven body smoothing {}", aEnabled ? "on" : "off");
}

void ObjectService::SetHermitePlaybackEnabled(bool aEnabled) noexcept
{
    s_hermitePlaybackEnabled.store(aEnabled, std::memory_order_relaxed);
    spdlog::info("Host-driven body playback {}", aEnabled ? "on velocity curves" : "on straight lines");
}

void ObjectService::SetPhysicsStampEnabled(bool aEnabled) noexcept
{
    s_physicsStampEnabled.store(aEnabled, std::memory_order_relaxed);
    spdlog::info("Host samples stamped with the {} time", aEnabled ? "physics step" : "read");
}

namespace
{
// Angle (degrees) between two rotations.
float RotationAngleDegrees(const NiMatrix3& a, const NiMatrix3& b) noexcept
{
    float trace = 0.f;
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c)
            trace += a.entry[r][c] * b.entry[r][c];
    const float cosine = std::clamp((trace - 1.f) * 0.5f, -1.f, 1.f);
    return std::acos(cosine) * 57.2958f;
}
} // namespace

void ObjectService::OnMainFrameEnd() noexcept
{
    auto* pService = s_objectService.load(std::memory_order_acquire);
    if (!pService || !pService->m_applyOnMainFrame.load(std::memory_order_relaxed))
        return;
    std::lock_guard lock(pService->m_remotePhysicsLock);
    for (auto& [formId, pose] : pService->m_remoteReferencePoses)
    {
        if (!pose.ProbeEndArmed)
            continue;
        pose.ProbeEndArmed = false;
        auto* pReference = Cast<TESObjectREFR>(TESForm::GetById(formId));
        const auto* pNode = pReference ? pReference->GetNiNode() : nullptr;
        if (!pNode)
            continue;
        ++pose.ProbeEndFrames;
        // Rider drift: drawn root node against the seat (see Rider::Drift).
        for (auto& [riderId, rider] : s_riders)
        {
            if (rider.ReferenceId != formId || !rider.HasPlaced)
                continue;
            auto* pRider = Cast<Actor>(TESForm::GetById(riderId));
            const auto* pRiderNode = pRider ? pRider->GetNiNode() : nullptr;
            if (!pRiderNode)
                continue;
            const glm::vec3 drawn{pRiderNode->world.translate.x, pRiderNode->world.translate.y, pRiderNode->world.translate.z};
            const glm::vec3 error = drawn - rider.Placed;
            if (glm::length(error) < 30.f)
                rider.Drift += error * 0.2f;
        }
        if (glm::length(pose.RenderVelocity) > 20.f)
            ProbeDrawnGap("Follower", pReference, pose.RenderVelocity);
        const float move = glm::length(glm::vec3{pNode->world.translate.x, pNode->world.translate.y, pNode->world.translate.z} -
            pose.ProbeWritten);
        const float turn = RotationAngleDegrees(pNode->world.rotate, pose.ProbeWrittenRotate);
        std::vector<ChildBody> children;
        CollectChildBodies(pReference, children);
        float childMove = 0.f, childTurn = 0.f;
        if (!children.empty())
        {
            const auto& w = children[0].Node->world;
            childMove = glm::length(glm::vec3{w.translate.x, w.translate.y, w.translate.z} - pose.ProbeChild0Written);
            childTurn = RotationAngleDegrees(w.rotate, pose.ProbeChild0Rotate);
        }
        if (move > 0.1f || turn > 0.1f || childMove > 0.1f || childTurn > 0.1f)
            ++pose.ProbeEndMoved;
        pose.ProbeEndMoveMax = (std::max)(pose.ProbeEndMoveMax, move);
        pose.ProbeEndTurnMax = (std::max)(pose.ProbeEndTurnMax, turn);
        pose.ProbeEndChildMoveMax = (std::max)(pose.ProbeEndChildMoveMax, childMove);
        pose.ProbeEndChildTurnMax = (std::max)(pose.ProbeEndChildTurnMax, childTurn);
    }
}

void ObjectService::SetMainFrameCaptureEnabled(bool aEnabled) noexcept
{
    // The retired worker lane is not a supported alternative to main-frame capture.
    s_mainFrameCaptureEnabled.store(true, std::memory_order_relaxed);
    spdlog::info("Main-frame capture: requested {} effective true{}",
        aEnabled, aEnabled ? "" : "; disable request ignored because worker capture is retired");
}

void ObjectService::SetMainFramePlaybackEnabled(bool aEnabled) noexcept
{
    // Main publishes retained body targets; the solver owns the actual body writes.
    s_mainFramePlaybackEnabled.store(true, std::memory_order_relaxed);
    spdlog::info("Main-frame playback: requested {} effective true{}",
        aEnabled, aEnabled ? "" : "; disable request ignored because worker playback is retired");
}

void ObjectService::ApplyRemotePhysics() noexcept
{
    DrainPendingKeyframes();
    std::lock_guard lock(m_remotePhysicsLock);
    s_stepTargetsBuilding.clear();
    struct PublishStepTargets
    {
        ~PublishStepTargets()
        {
            std::lock_guard stepLock(s_stepTargetsLock);
            s_followProbesDirty |= s_stepTargets.size() != s_stepTargetsBuilding.size();
            s_stepTargets.swap(s_stepTargetsBuilding);
        }
    } publishStepTargets;
    const auto now = std::chrono::steady_clock::now();
    for (auto it = m_remoteReferencePoses.begin(); it != m_remoteReferencePoses.end();)
    {
        auto* pReference = Cast<TESObjectREFR>(TESForm::GetById(it->first));
        // Only admitted drop packets create this entry. Ordinary host-driven bodies must
        // not acquire the shared-drop mutex on every playback frame.
        const auto generation = s_sharedDropPoseGenerations.find(it->first);
        const bool shared = generation != s_sharedDropPoseGenerations.end();
        if (!m_transport.IsConnected() || !m_world.GetPartyService().IsInParty() ||
            !pReference || Cast<Actor>(pReference) || !pReference->loadedState ||
            !pReference->parentCell || !pReference->parentCell->IsAttached() ||
            (shared && generation->second != m_world.GetSharedDropService().PhysicsGeneration(it->first)) ||
            (shared ? m_world.GetSharedDropService().IsOwner(it->first) : m_world.GetPartyService().IsLeader()) ||
            it->second.AuthorityEpoch != m_world.GetPartyService().GetStartEpoch())
        {
            it = m_remoteReferencePoses.erase(it);
            continue;
        }

        auto& pose = it->second;
        if (!pose.BodyDriven)
        {
            DynamicBody body{};
            if (GetDynamicBody(pReference, body, true))
            {
                ScopedPhysicsWorld worldLock(body.State.world);
                if (worldLock.Lock && GetDynamicBody(pReference, body, true))
                {
                    // 19517 -> 0x140EDE280 builds Rx(-x)*Ry(-y)*Rz(-z).
                    // Rotate the reference-to-body offset as well as the body orientation.
                    const auto referenceRotation = [](const NiPoint3& angle)
                    {
                        return glm::angleAxis(-angle.x, glm::vec3{1.f, 0.f, 0.f}) *
                            glm::angleAxis(-angle.y, glm::vec3{0.f, 1.f, 0.f}) *
                            glm::angleAxis(-angle.z, glm::vec3{0.f, 0.f, 1.f});
                    };
                    const auto turn = referenceRotation(pose.Rotation) * glm::conjugate(referenceRotation(pReference->rotation));
                    float bodyQ[4];
                    MatrixToQuaternion(body.State.transform, bodyQ);
                    const auto rotation = glm::mat3_cast(turn * glm::quat{bodyQ[3], bodyQ[0], bodyQ[1], bodyQ[2]});
                    const auto offset = glm::vec3{body.State.transform[12], body.State.transform[13], body.State.transform[14]} -
                        glm::vec3{pReference->position.x, pReference->position.y, pReference->position.z} / kHavokToGameUnits;
                    const auto position = glm::vec3{pose.Position.x, pose.Position.y, pose.Position.z} / kHavokToGameUnits + turn * offset;
                    pose.BodyTransform = {};
                    for (int axis = 0; axis < 3; ++axis)
                    {
                        pose.BodyTransform[12 + axis] = position[axis];
                        for (int row = 0; row < 3; ++row)
                            pose.BodyTransform[axis * 4 + row] = rotation[axis][row];
                    }
                    pose.BodyDriven = true;
                    pose.SampleCount = 0;
                }
            }
        }

        if (pose.BodyDriven)
        {
            if (!pose.SampleCount)
            {
                auto& sample = pose.Samples[0];
                sample.Tick = pose.Tick;
                sample.BodyPosition = {pose.BodyTransform[12], pose.BodyTransform[13], pose.BodyTransform[14]};
                float q[4];
                MatrixToQuaternion(pose.BodyTransform.data(), q);
                sample.BodyRotation = {q[0], q[1], q[2], q[3]};
                pose.SampleCount = pose.SampleNext = 1;
            }
            {
                if (!pose.DynamicFollow)
                {
                    pose.DynamicFollow = true;
                    pose.HostDriven = false;
                    spdlog::info("Host-driven body {:X}: simulated here, steered to the host's pose", it->first);
                }
                const uint32_t count = pose.SampleCount;
                const uint32_t size = static_cast<uint32_t>(pose.Samples.size());
                const auto sample = [&](uint32_t aIndex) -> const RemoteReferencePose::Sample&
                { return pose.Samples[(pose.SampleNext + size - count + aIndex) % size]; };
                bool cartAssembly = false;
                {
                    std::lock_guard assemblyLock(s_assembliesLock);
                    const auto found = s_assemblies.find(it->first);
                    cartAssembly = found != s_assemblies.end() && found->second.Complete &&
                        found->second.Root.get() == pReference->GetNiNode();
                }
                const bool curve = cartAssembly && s_cartCurve.load(std::memory_order_relaxed);
                const double renderTime = cartAssembly ? (curve ? s_assemblyTimeMs.load(std::memory_order_acquire) :
                    static_cast<double>(s_assemblyTick.load(std::memory_order_acquire))) :
                    (SmoothClock::NowMs() > 0.0 ? SmoothClock::NowMs() :
                    static_cast<double>(m_transport.GetClock().GetCurrentTick())) -
                    static_cast<double>(m_world.GetCharacterService().GetPresentationDelayMs());
                const auto window = PhysicsScan::SelectPlaybackWindow(count, renderTime,
                    [&](uint32_t index) { return sample(index).Tick; });
                const RemoteReferencePose::Sample* pA = &sample(window.A);
                const RemoteReferencePose::Sample* pB = &sample(window.B);
                float t = window.Fraction;
                float prediction = 0.f;
                // No extrapolation past the newest owner sample: with the assembly on the delayed
                // presentation clock the window is normally bracketed, and a late packet holds the
                // newest target (like every other steered body) instead of predicting up to 150 ms ahead.
                // Missing packets keep the newest target subscribed, with zero feed-forward.
                const bool atFinalPose = window.Hold;
                if (atFinalPose && !cartAssembly)
                {
                    pA = pB = &sample(count - 1);
                    t = 0.f;
                }
                else
                    pose.SettledAtFinalPose = false;
                // Local scripts may park a copy. Restore simulation; follower copies
                // must keep being steered even when the owner's last target is at rest.
                DynamicBody body{};
                if (GetDynamicBody(pReference, body, true) && body.HavokBody && body.State.world)
                {
                    ScopedPhysicsWorld worldLock(body.State.world);
                    if (!worldLock.Lock || !GetDynamicBody(pReference, body, true))
                    {
                        ++it;
                        continue;
                    }
                    const bool replay = cartAssembly && s_cartReplay.load(std::memory_order_relaxed);
                    if (replay && IsDynamicMotion(body.State.motionType))
                    {
                        // Deferred (see s_pendingKeyframes); the exact drive also works on the dynamic body meanwhile.
                        s_pendingKeyframes.push_back(HoldAssemblyNode(pReference->GetNiNode()));
                        spdlog::info("Cart replay {:X}: root keyframe queued (motion {})", it->first, body.State.motionType);
                    }
                    else if (body.State.motionType == 4 && !replay)
                    {
                        if (cartAssembly)
                            RestoreBodyMotion(pReference->GetNiNode());
                        else
                            pReference->SetMotionType(static_cast<TESObjectREFR::MotionType>(
                                pose.HostMotionType >= 1 && pose.HostMotionType <= 3 ? pose.HostMotionType : 3), false);
                        if (!GetDynamicBody(pReference, body) || !body.HavokBody || !body.State.world)
                        {
                            ++it;
                            continue;
                        }
                    }
                    std::unique_lock assemblyLock(s_assembliesLock);
                    const auto assemblyIt = s_assemblies.find(it->first);
                    CartAssembly* assembly = cartAssembly && assemblyIt != s_assemblies.end() &&
                        assemblyIt->second.Root.get() == pReference->GetNiNode() ? &assemblyIt->second : nullptr;
                    std::array<void*, PhysicsReferenceUpdate::kMaxChildBodies> childBodies{};
                    // Why a followed cart was skipped this frame (logged at most once a second per cart): a skipped
                    // cart is neither steered nor replayed, and the skip used to be silent.
                    const char* skipReason = nullptr;
                    size_t skipIndex = 0;
                    uint32_t skipMotion = 0;
                    bool childrenValid = !assembly || (assembly->Count == pA->Children.size() &&
                        assembly->Count == pB->Children.size() && assembly->HelperSlot < assembly->Count &&
                        assembly->Nodes[assembly->HelperSlot]->collisionObject == assembly->Tether);
                    if (assembly && childrenValid)
                    {
                        for (size_t i = 0; i < assembly->Count; ++i)
                        {
                            auto* node = assembly->Nodes[i].get();
                            auto* ancestor = node;
                            auto* expectedRoot = i == assembly->HelperSlot ? assembly->HorseRoot.get() : assembly->Root.get();
                            for (unsigned depth = 0; ancestor && ancestor != expectedRoot && depth < 32; ++depth)
                                ancestor = ancestor->parent;
                            ActorPoseDiagnosticViews::RigidBody child{};
                            childBodies[i] = AssemblyBody(node);
                            if (ancestor != expectedRoot || childBodies[i] == body.HavokBody ||
                                !ReadPhysicsMemory(childBodies[i], child) || child.world != body.State.world)
                            {
                                skipReason = ancestor != expectedRoot ? "part left the cart tree" :
                                    childBodies[i] == body.HavokBody ? "part body is the root body" :
                                    !childBodies[i] ? "part has no body" : "part body unreadable or in another world";
                                skipIndex = i;
                                skipMotion = child.motionType;
                                childrenValid = false;
                                break;
                            }
                            if (i != assembly->HelperSlot && assembly->Simulated[i])
                            {
                                if (replay && IsDynamicMotion(child.motionType))
                                {
                                    s_pendingKeyframes.push_back(assembly->Nodes[i]);
                                    spdlog::info("Cart replay {:X}: part {} keyframe queued (motion {})", it->first, i,
                                        child.motionType);
                                }
                                else if (!replay && child.motionType == 4)
                                    RestoreBodyMotion(node);
                            }
                            if (!ReadPhysicsMemory(childBodies[i], child) ||
                                (assembly->Simulated[i] && !(replay ? child.motionType == 4 || IsDynamicMotion(child.motionType) :
                                    IsDynamicMotion(child.motionType))) ||
                                (i == assembly->HelperSlot && child.motionType != 4))
                            {
                                skipReason = i == assembly->HelperSlot ? "tether helper not keyframed" :
                                    "part motion type not allowed";
                                skipIndex = i;
                                skipMotion = child.motionType;
                                childrenValid = false;
                                break;
                            }
                        }
                    }
                    if (!childrenValid)
                    {
                        if (assembly)
                        {
                            assembly->ActiveUntil = 0;
                            const auto nowMs = GetTickCount64();
                            if (nowMs >= assembly->NextSkipLog)
                            {
                                assembly->NextSkipLog = nowMs + 1000;
                                spdlog::warn("Cart assembly {:X}: skipped ({}), part {} of {} motion {}, samples {} / {} parts, replay {}",
                                    it->first, skipReason ? skipReason : "sample part count differs", skipIndex,
                                    assembly->Count, skipMotion, pA->Children.size(), pB->Children.size(), replay);
                            }
                        }
                        ++it;
                        continue;
                    }
                    StepTarget target{body.State.world, body.HavokBody, it->first};
                    target.Dynamic = true;
                    target.Assembly = assembly != nullptr;
                    target.Replay = assembly != nullptr && replay;
                    target.BodyUid = body.State.uid;
                    target.PublishedAt = now;
                    const auto hostNow = m_transport.GetClock().GetCurrentTick();
                    target.HostAgeMs = hostNow >= pose.Tick ? hostNow - pose.Tick : 0;
                    if (pose.BodyLifetime.get() != body.HavokBody || pose.BodyUid != body.State.uid)
                    {
                        s_holdBody.Get()(body.HavokBody);
                        pose.BodyLifetime = std::shared_ptr<void>(body.HavokBody,
                            RetireBody);
                        pose.BodyUid = body.State.uid;
                    }
                    target.Lifetime = pose.BodyLifetime;
                    {
                        std::lock_guard stepLock(s_stepTargetsLock);
                        const auto [probe, inserted] = s_followProbes.try_emplace(body.HavokBody);
                        const auto index = s_stepTargetsBuilding.size();
                        s_followProbesDirty |= inserted || index >= s_stepTargets.size() ||
                            s_stepTargets[index].Body != target.Body || s_stepTargets[index].BodyUid != target.BodyUid;
                    }
                    glm::vec3 position = pA->BodyPosition + (pB->BodyPosition - pA->BodyPosition) * t;
                    if (curve && !atFinalPose && pB->Tick > pA->Tick && pB->Tick - pA->Tick <= 250)
                    {
                        // Tangents: each sample's owner body velocity (game u/s -> Havok u/s) over the interval.
                        const float span = static_cast<float>(pB->Tick - pA->Tick) / 1000.f;
                        const glm::vec3 mA = glm::vec3{pA->Velocity.x, pA->Velocity.y, pA->Velocity.z} / kHavokToGameUnits * span;
                        const glm::vec3 mB = glm::vec3{pB->Velocity.x, pB->Velocity.y, pB->Velocity.z} / kHavokToGameUnits * span;
                        const float t2 = t * t, t3 = t2 * t;
                        position = (2.f * t3 - 3.f * t2 + 1.f) * pA->BodyPosition + (t3 - 2.f * t2 + t) * mA +
                            (-2.f * t3 + 3.f * t2) * pB->BodyPosition + (t3 - t2) * mB;
                    }
                    const glm::quat qa{pA->BodyRotation.w, pA->BodyRotation.x, pA->BodyRotation.y, pA->BodyRotation.z};
                    const glm::quat qb{pB->BodyRotation.w, pB->BodyRotation.x, pB->BodyRotation.y, pB->BodyRotation.z};
                    glm::quat rotation = glm::normalize(glm::slerp(qa, qb, t));
                    const glm::vec3 velocity = (glm::vec3{pA->Velocity.x, pA->Velocity.y, pA->Velocity.z} +
                        (glm::vec3{pB->Velocity.x, pB->Velocity.y, pB->Velocity.z} - glm::vec3{pA->Velocity.x, pA->Velocity.y,
                            pA->Velocity.z}) * t) / kHavokToGameUnits;
                    glm::vec3 angular{};
                    if (atFinalPose)
                    {
                        const glm::vec3 have{body.State.transform[12], body.State.transform[13], body.State.transform[14]};
                        target.Velocity[0] = target.Velocity[1] = target.Velocity[2] = 0.f;
                        if (glm::length(have - position) < 1.f / kHavokToGameUnits)
                        {
                            // Record convergence, but keep maintaining the resting target.
                            pose.SettledAtFinalPose = true;
                            if (!pose.LoggedFinalPose)
                            {
                                pose.LoggedFinalPose = true;
                                spdlog::info("Host-driven body {:X}: at the host's final pose", it->first);
                            }
                        }
                    }
                    if (pB->Tick > pA->Tick)
                    {
                        glm::quat step = qb * glm::conjugate(qa);
                        if (step.w < 0.f)
                            step = -step;
                        angular = glm::vec3{step.x, step.y, step.z} * 2.f / (static_cast<float>(pB->Tick - pA->Tick) / 1000.f);
                    }
                    if (assembly)
                    {
                        position += velocity * prediction;
                        const float speed = glm::length(angular);
                        if (speed > 0.0001f)
                            rotation = glm::angleAxis(speed * prediction, angular / speed) * rotation;
                    }
                    for (int k = 0; k < 3; ++k)
                    {
                        target.Position[k] = position[k];
                        target.Velocity[k] = atFinalPose ? 0.f : velocity[k];
                        target.Angular[k] = atFinalPose ? 0.f : angular[k];
                    }
                    target.Rotation[0] = rotation.x;
                    target.Rotation[1] = rotation.y;
                    target.Rotation[2] = rotation.z;
                    target.Rotation[3] = rotation.w;
                    s_stepTargetsBuilding.push_back(target);
                    if (target.Replay)
                    {
                        PlaceNodeWorld(pReference->GetNiNode(), position, rotation);
                        s_replayedThisPass.insert(pReference->GetNiNode());
                        SyncReplayReference(pReference, position, rotation, assembly->ReconciledAt,
                            assembly->NextReconcileMs);
                    }
                    if (assembly)
                    {
                        float maxChildGap = 0.f;
                        const float interval = pB->Tick > pA->Tick ? static_cast<float>(pB->Tick - pA->Tick) / 1000.f : 0.f;
                        for (size_t i = 0; i < assembly->Count; ++i)
                        {
                            const auto& ca = pA->Children[i];
                            const auto& cb = pB->Children[i];
                            const glm::vec3 offsetA{ca[0], ca[1], ca[2]}, offsetB{cb[0], cb[1], cb[2]};
                            const auto relativeVelocity = interval > 0.f ? (offsetB - offsetA) / interval : glm::vec3{};
                            const auto childPosition = position + glm::mix(offsetA, offsetB, t) + relativeVelocity * prediction;
                            const glm::quat childA{ca[6], ca[3], ca[4], ca[5]}, childB{cb[6], cb[3], cb[4], cb[5]};
                            auto childRotation = glm::normalize(glm::slerp(childA, childB,
                                t + (interval > 0.f ? prediction / interval : 0.f)));
                            auto childTurn = childB * glm::conjugate(childA);
                            if (childTurn.w < 0.f)
                                childTurn = -childTurn;
                            const auto childAngular = !atFinalPose && interval > 0.f ?
                                glm::vec3{childTurn.x, childTurn.y, childTurn.z} * (2.f / interval) : glm::vec3{};
                            StepTarget childTarget = target;
                            childTarget.NativeHelper = i == assembly->HelperSlot;
                            childTarget.Body = childBodies[i];
                            ActorPoseDiagnosticViews::RigidBody state{};
                            ReadPhysicsMemory(childBodies[i], state);
                            childTarget.BodyUid = state.uid;
                            if (assembly->Lifetimes[i].get() != childBodies[i] || assembly->Uids[i] != state.uid)
                            {
                                s_holdBody.Get()(childBodies[i]);
                                assembly->Lifetimes[i] = std::shared_ptr<void>(childBodies[i], RetireBody);
                                assembly->Uids[i] = state.uid;
                            }
                            childTarget.Lifetime = assembly->Lifetimes[i];
                            for (int k = 0; k < 3; ++k)
                            {
                                childTarget.Position[k] = childPosition[k];
                                childTarget.Velocity[k] = atFinalPose ? 0.f : velocity[k] + relativeVelocity[k];
                                childTarget.Angular[k] = childAngular[k];
                            }
                            childTarget.Rotation[0] = childRotation.x;
                            childTarget.Rotation[1] = childRotation.y;
                            childTarget.Rotation[2] = childRotation.z;
                            childTarget.Rotation[3] = childRotation.w;
                            maxChildGap = (std::max)(maxChildGap, glm::length(childPosition -
                                glm::vec3{state.transform[12], state.transform[13], state.transform[14]}) * kHavokToGameUnits);
                            // Steer only the assembly ROOT. Wheels and other parts are joined to the root by the
                            // cart's own constraints; steering each part separately fought those joints (follower
                            // log 2026-09-27: 15-22 u part gaps corrected at 450-810 u/s, visible jitter). The
                            // constraints carry the parts with the steered root, as on the host. Part gaps are still
                            // measured above for the two-second log.
                            if (!replay || i == assembly->HelperSlot || !assembly->Simulated[i])
                                continue;
                            // Replay drives every part from the same owner sample: rigid parts cannot fight joints
                            // that no longer simulate here. The helper stays with the native tether sync.
                            childTarget.Replay = true;
                            PlaceNodeWorld(assembly->Nodes[i].get(), childPosition, childRotation);
                            s_replayedThisPass.insert(assembly->Nodes[i].get());
                            {
                                std::lock_guard stepLock(s_stepTargetsLock);
                                const auto [probe, inserted] = s_followProbes.try_emplace(childBodies[i]);
                                const auto index = s_stepTargetsBuilding.size();
                                s_followProbesDirty |= inserted || index >= s_stepTargets.size() ||
                                    s_stepTargets[index].Body != childTarget.Body || s_stepTargets[index].BodyUid != childTarget.BodyUid;
                            }
                            s_stepTargetsBuilding.push_back(std::move(childTarget));
                        }
                        assembly->ActiveUntil = GetTickCount64() + 250;
                        if (GetTickCount64() >= assembly->NextLog)
                        {
                            const float rootGap = glm::length(position - glm::vec3{body.State.transform[12],
                                body.State.transform[13], body.State.transform[14]}) * kHavokToGameUnits;
                            // These are local playback-target errors, not paired harness gaps.
                            // The passenger hook still checks readiness for each controller call.
                            spdlog::info("Cart assembly {:X}: root gap {:.1f} max child gap {:.1f} controller {}",
                                it->first, rootGap, maxChildGap,
                                "gate armed for ready remote passengers; helper streamed; gaps vs playback target");
                            assembly->NextLog = GetTickCount64() + 2000;
                        }
                    }
                }
                ++it;
                continue;
            }
        }
        ++it;
    }
}

void ObjectService::OnCellChange(const CellChangeEvent& acEvent) noexcept
{
    if (!m_transport.IsConnected())
        return;

    PlayerCharacter* pPlayer = PlayerCharacter::Get();
    TESObjectCELL* pCell = pPlayer->parentCell;

    // Player homes should not be synced, so that chest contents,
    // which are often used as storage, are never accidentally wiped.
    if (!World::Get().GetServerSettings().SyncPlayerHomes && IsPlayerHome(pCell))
        return;

    GameId cellId{};
    if (!m_world.GetModSystem().GetServerModId(pCell->formID, cellId))
    {
        spdlog::error("Server cell id not found for cell form id {:X}", pCell->formID);
        return;
    }

    GameId worldSpaceId{};
    if (TESWorldSpace* pWorldSpace = pPlayer->GetWorldSpace())
    {
        if (!m_world.GetModSystem().GetServerModId(pWorldSpace->formID, worldSpaceId))
        {
            spdlog::error("Server world space id not found for world space form id {:X}", pWorldSpace->formID);
            return;
        }
    }

    Vector<FormType> formTypes = {FormType::Container, FormType::Door};
    // Door seemed to be at the wrong form id (29, now 32), verify this.
    Vector<TESObjectREFR*> objects = pCell->GetRefsByFormTypes(formTypes);

    AssignObjectsRequest request{};

    const Set<const TESObjectREFR*> playerStashContainers = GetPlayerStashContainers();

    for (TESObjectREFR* pObject : objects)
    {
        if (!ShouldSyncObject(pObject, playerStashContainers))
        {
            spdlog::warn("Excluding sync for {:X}", pObject->formID);
            continue;
        }

        ObjectData objectData{};
        objectData.CellId = cellId;
        objectData.WorldSpaceId = worldSpaceId;
        objectData.CurrentCoords = GridCellCoords::CalculateGridCellCoords(pObject->position.x, pObject->position.y);

        if (!m_world.GetModSystem().GetServerModId(pObject->formID, objectData.Id))
        {
            spdlog::error("Server form id not found for object with form id {:X}", pObject->formID);
            continue;
        }

        if (Lock* pLock = pObject->GetLock())
        {
            objectData.CurrentLockData.IsLocked = pLock->IsLocked();
            objectData.CurrentLockData.LockLevel = pLock->lockLevel;
        }

        if (pObject->baseForm->formType == FormType::Container)
            objectData.CurrentInventory = pObject->GetInventory();

        request.Objects.push_back(objectData);
    }

    m_transport.Send(request);
}

void ObjectService::OnAssignObjectsResponse(const AssignObjectsResponse& acMessage) noexcept
{
    for (const ObjectData& objectData : acMessage.Objects)
    {
        const uint32_t cObjectId = World::Get().GetModSystem().GetGameId(objectData.Id);
        TESObjectREFR* pObject = Cast<TESObjectREFR>(TESForm::GetById(cObjectId));
        if (!pObject)
        {
            spdlog::error("Object not found for form id {:X}", objectData.Id);
            continue;
        }

        CreateObjectEntity(pObject->formID, objectData.ServerId);

        if (objectData.IsSenderFirst)
            continue;

        if (objectData.CurrentLockData != LockData{})
        {
            Lock* pLock = pObject->GetLock();

            if (!pLock)
            {
                pLock = pObject->CreateLock();
                if (!pLock)
                    continue;
            }

            pLock->lockLevel = objectData.CurrentLockData.LockLevel;
            pLock->SetLock(objectData.CurrentLockData.IsLocked);
            pObject->LockChange();
        }

        if (pObject->baseForm->formType == FormType::Container)
        {
            Inventory currentInventory = pObject->GetInventory();

            if (currentInventory.ContainsQuestItems())
                pObject->SetInventoryRetainingQuestItems(currentInventory, objectData.CurrentInventory);
            else
                pObject->SetInventory(objectData.CurrentInventory);
        }
    }
}

entt::entity ObjectService::CreateObjectEntity(const uint32_t acFormId, const uint32_t acServerId) noexcept
{
    const auto view = m_world.view<FormIdComponent, ObjectComponent>();

    auto it = std::find_if(view.begin(), view.end(), [acServerId, view](entt::entity entity) { return view.get<ObjectComponent>(entity).Id == acServerId; });

    if (it != view.end())
        return *it;

    entt::entity entity = m_world.create();
    spdlog::info("Created object entity, server id: {:X}, form id {:X}", acServerId, acFormId);

    m_world.emplace<FormIdComponent>(entity, acFormId);
    m_world.emplace<ObjectComponent>(entity, acServerId);

    return entity;
}

void ObjectService::OnActivate(const ActivateEvent& acEvent) noexcept
{
    if (acEvent.ActivateFlag)
    {
        acEvent.pObject->Activate(acEvent.pActivator, acEvent.Unk1, acEvent.pObjectToGet, acEvent.Count, acEvent.DefaultProcessing);
    }

    if (!m_transport.IsConnected())
        return;

    if (Lock* pLock = acEvent.pObject->GetLock())
    {
        if (pLock->flags & 0xFF)
            return;
    }

    ActivateRequest request;

    if (!m_world.GetModSystem().GetServerModId(acEvent.pObject->formID, request.Id))
    {
        spdlog::error("Server form id not found for object form id {:X}", acEvent.pObject->formID);
        return;
    }

    TESObjectCELL* pCell = acEvent.pObject->GetParentCellEx();
    if (!pCell)
    {
        spdlog::error("Activated object has no parent cell: {:X}", acEvent.pObject->formID);
        return;
    }

    if (!m_world.GetModSystem().GetServerModId(pCell->formID, request.CellId))
    {
        spdlog::error("Server cell id not found for cell form id {:X}", pCell->formID);
        return;
    }

    auto view = m_world.view<FormIdComponent>();
    const auto pEntity = std::find_if(std::begin(view), std::end(view), [id = acEvent.pActivator->formID, view](entt::entity entity) { return view.get<FormIdComponent>(entity).Id == id; });

    if (pEntity == std::end(view))
    {
        // spdlog::error("Activator entity not found for form id {:X}", acEvent.pActivator->formID);
        return;
    }

    std::optional<uint32_t> serverIdRes = Utils::GetServerId(*pEntity);
    if (!serverIdRes.has_value())
        return;

    request.ActivatorId = serverIdRes.value();
    request.PreActivationOpenState = acEvent.PreActivationOpenState;

    m_transport.Send(request);
}

void ObjectService::OnActivateNotify(const NotifyActivate& acMessage) noexcept
{
    Actor* pActor = Utils::GetByServerId<Actor>(acMessage.ActivatorId);
    if (!pActor)
    {
        spdlog::error("{}: could not find actor server id {:X}", __FUNCTION__, acMessage.ActivatorId);
        return;
    }

    const uint32_t cObjectId = World::Get().GetModSystem().GetGameId(acMessage.Id);
    TESObjectREFR* pObject = Cast<TESObjectREFR>(TESForm::GetById(cObjectId));
    if (!pObject)
    {
        spdlog::error("Failed to retrieve object to activate.");
        return;
    }

    if (pObject->baseForm->formType == FormType::Door)
    {
        auto remotePreActivationState = static_cast<TESObjectREFR::OpenState>(acMessage.PreActivationOpenState);
        TESObjectREFR::OpenState localState = pObject->GetOpenState();

        if (remotePreActivationState != localState)
        {
            // The doors are unsynced at this point. If we'll Activate the one on our side
            // it'll just continue to be unsynced (open remotely, closed locally and vice versa)
            return;
        }
    }

    // unsure if these flags are the best, but these are passed with the papyrus Activate fn
    // might be an idea to have the client send the flags through NotifyActivate
    pObject->Activate(pActor, 0, nullptr, 1, 0);
}

void ObjectService::OnLockChange(const LockChangeEvent& acEvent) noexcept
{
    if (!m_transport.IsConnected())
        return;

    LockChangeRequest request;

    if (!m_world.GetModSystem().GetServerModId(acEvent.FormId, request.Id))
    {
        spdlog::error("Server form id for lock object not found, form id: {:X}", acEvent.FormId);
        return;
    }

    const auto* const pObject = Cast<TESObjectREFR>(TESForm::GetById(acEvent.FormId));

    TESObjectCELL* pCell = pObject->GetParentCellEx();
    if (!pCell)
    {
        spdlog::error("Activated object has no parent cell: {:X}", pObject->formID);
        return;
    }

    if (!m_world.GetModSystem().GetServerModId(pCell->formID, request.CellId))
    {
        spdlog::error("Server cell id for cell not found, cell form id: {:X}", pCell->formID);
        return;
    }

    request.IsLocked = acEvent.IsLocked;
    request.LockLevel = acEvent.LockLevel;

    m_transport.Send(request);
}

void ObjectService::OnLockChangeNotify(const NotifyLockChange& acMessage) noexcept
{
    const auto cObjectId = World::Get().GetModSystem().GetGameId(acMessage.Id);
    if (cObjectId == 0)
    {
        spdlog::error("Failed to retrieve object id to (un)lock.");
        return;
    }

    auto* pObject = Cast<TESObjectREFR>(TESForm::GetById(cObjectId));
    if (!pObject)
    {
        spdlog::error("Failed to retrieve object to (un)lock.");
        return;
    }

    auto* pLock = pObject->GetLock();

    if(!acMessage.IsLocked)
    {
        if (!pLock || !pLock->IsLocked())
            return;
    }

    if (!pLock && acMessage.IsLocked)
    {
        pLock = pObject->CreateLock();
        if (!pLock)
        {
            spdlog::error("Failed to create lock for object form id {:X}", pObject->formID);
            return;
        }
    }

    pLock->lockLevel = acMessage.LockLevel;
    pLock->SetLock(acMessage.IsLocked);
    pObject->LockChange();
}

void ObjectService::OnScriptAnimationEvent(const ScriptAnimationEvent& acEvent) noexcept
{
    ScriptAnimationRequest request{};
    request.FormID = acEvent.FormID;
    request.Animation = acEvent.Animation;
    request.EventName = acEvent.EventName;

    m_transport.Send(request);
}

void ObjectService::OnNotifyScriptAnimation(const NotifyScriptAnimation& acMessage) noexcept
{
    if (acMessage.FormID == 0)
        return;

    auto* pForm = TESForm::GetById(acMessage.FormID);
    auto* pObject = Cast<TESObjectREFR>(pForm);

    if (!pObject)
    {
        spdlog::error("Failed to fetch notify script animation object, form id: {:X}", acMessage.FormID);
        return;
    }

    BSFixedString eventName(acMessage.EventName.c_str());
    if (acMessage.Animation == String{})
    {
        pObject->PlayAnimation(&eventName);
    }
    else
    {
        BSFixedString animation(acMessage.Animation.c_str());
        pObject->PlayAnimationAndWait(&animation, &eventName);
    }
}

BSTEventResult ObjectService::OnEvent(const TESActivateEvent* acEvent, const EventDispatcher<TESActivateEvent>* aDispatcher)
{
#if ENVIRONMENT_DEBUG
    auto view = m_world.view<ObjectComponent>();

    const auto itor = std::find_if(std::begin(view), std::end(view), [id = acEvent->object->formID, view](entt::entity entity) { return view.get<ObjectComponent>(entity).Id == id; });

    if (itor == std::end(view))
    {
        AddObjectComponent(acEvent->object);
    }
#endif

    return BSTEventResult::kOk;
}

void ObjectService::SetHostDrivenPlaybackEnabled(bool aEnabled) noexcept
{
    s_hostDrivenPlaybackEnabled.store(aEnabled, std::memory_order_relaxed);
    spdlog::info("Host-driven body playback {}", aEnabled ? "on" : "off");
}

void ObjectService::SetRootBodyWriteEnabled(bool aEnabled) noexcept
{
    s_rootBodyWriteEnabled.store(aEnabled, std::memory_order_relaxed);
    spdlog::info("Host-driven root body write {}", aEnabled ? "on" : "off");
}

void ObjectService::SetCellHandoffEnabled(bool aEnabled) noexcept
{
    s_cellHandoffEnabled.store(aEnabled, std::memory_order_relaxed);
    spdlog::info("Host-driven body cell handoff {}", aEnabled ? "on" : "off");
}
