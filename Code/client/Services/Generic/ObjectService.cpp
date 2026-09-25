#include <Services/SmoothClock.h>
#include <Games/ActorExtension.h>
#include <Services/ObjectService.h>

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
// The paired 2026-09-22 intro trial still teleported carts. Keep the
// experimental easing implementation for a body-level follow-up, but never
// enable it in a distributed build until Havok/script ownership is validated.
constexpr bool kEnablePerFrameReferenceCorrection = false;
// The 2026-09-23 paired trial oversped the follower cart and increased
// divergence. Keep the source-backed setter path for further body-timing
// research, but ship observation only until body/scene ownership is resolved.
constexpr bool kEnableDynamicBodyServo = false;
// Follower playback of the host's moving dynamic bodies (the Helgen carts first).
// Two independent Havok simulations of a tethered cart cannot agree: the
// follower's own step moved cart 0xBB970 100-117 game units in single 16-ms
// frames while the host's moved under 10 (REFERENCE_RESEARCH.md). A keyframed
// body follows its scene node, so writing Havok body poses into one is
// overwritten by the node every step (the earlier kinematic trial's ~1,578-unit
// drift). Instead the follower makes the body keyframed and drives the
// *reference* transform from the host's samples, rendered ~100 ms behind the
// newest sample so it always interpolates between two known poses. The host
// stops sending once a body is at rest; the follower then holds the last pose.
// It never hands the body back to local physics while loaded: the first trial
// did after 1.5 s and the Helgen cart snapped ~14,700 units back to where its
// stale Havok body still was. The Havok body is moved along with the reference
// so no invisible collider is left behind.
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
std::atomic<bool> s_mainFrameCaptureEnabled{false};

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
    glm::vec2 direction{acDirection.x, acDirection.y};
    if (glm::length(direction) < 1.f)
        return 0.f;
    direction = glm::normalize(direction);
    const glm::vec2 origin{apReference->position.x, apReference->position.y};
    float best = 250.f;
    float lead = 0.f;
    auto view = World::Get().view<FormIdComponent>();
    for (auto entity : view)
    {
        auto* pActor = Cast<Actor>(TESForm::GetById(view.get<FormIdComponent>(entity).Id));
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
    auto view = World::Get().view<FormIdComponent>();
    for (auto entity : view)
    {
        auto* pActor = Cast<Actor>(TESForm::GetById(view.get<FormIdComponent>(entity).Id));
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
        probe.NextLog = now + std::chrono::seconds(5);
    }
}

void ProbeHostRender() noexcept
{
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
// Only the explicitly selected follower probe may change a constrained body
// to keyframed. Keep the original type for restoration on disable/disconnect.
std::atomic<uint32_t> s_kinematicProbeFormId{};
std::atomic<uint32_t> s_kinematicOriginalMotionType{};
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
    const auto address = reinterpret_cast<uintptr_t>(apSource);
    if (address > std::numeric_limits<uintptr_t>::max() - aSize)
        return false;
    MEMORY_BASIC_INFORMATION info{};
    if (!VirtualQuery(apSource, &info, sizeof(info)) || info.State != MEM_COMMIT ||
        (info.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0)
        return false;
    const auto regionEnd = reinterpret_cast<uintptr_t>(info.BaseAddress) + info.RegionSize;
    if (regionEnd < address || regionEnd - address < aSize)
        return false;
    std::memcpy(apDestination, apSource, aSize);
    return true;
}

template <class T> bool ReadNativeMemory(const void* apSource, T& arValue) noexcept
{
    return ReadNativeMemory(apSource, &arValue, sizeof(T));
}

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
void CollectChildBodies(TESObjectREFR* apReference, std::vector<ChildBody>& arBodies) noexcept
{
    arBodies.clear();
    auto* pRoot = apReference ? apReference->GetNiNode() : nullptr;
    if (!pRoot)
        return;
    std::function<void(NiAVObject*, int)> walk = [&](NiAVObject* apNode, int aDepth)
    {
        if (!apNode || aDepth > 8 || arBodies.size() >= PhysicsReferenceUpdate::kMaxChildBodies)
            return;
        if (apNode != pRoot && apNode->collisionObject)
        {
            void* pWrapper = nullptr;
            void* pBody = nullptr;
            ActorPoseDiagnosticViews::RigidBody probe{};
            if (ReadNativeMemory(reinterpret_cast<const uint8_t*>(apNode->collisionObject) + 0x20, pWrapper) && pWrapper &&
                ReadNativeMemory(reinterpret_cast<const uint8_t*>(pWrapper) + 0x10, pBody) && pBody &&
                ReadNativeMemory(pBody, probe) && probe.world && probe.motionType >= 1 && probe.motionType <= 7)
                arBodies.push_back({apNode, pWrapper, static_cast<ActorPoseDiagnosticViews::RigidBody*>(pBody)});
        }
        if (auto* pNode = apNode->AsNode())
        {
            for (uint16_t i = 0; i < pNode->children.length; ++i)
                walk(pNode->children.data[i], aDepth + 1);
        }
    };
    walk(pRoot, 0);
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

// bhkRigidBody virtual slot 0x37: SetPositionAndRotation(hkVector4, hkQuaternion). It takes the
// bhkWorld write lock around hkpRigidBody::setPositionAndRotation (ID 60898); calling the raw
// Havok function from our update crashed (0x140B4CF03) when a physics step was running.
bool SetWrappedBodyPose(void* apWrapper, const float* apPosition, const float* apRotation) noexcept
{
    void** pVtable = nullptr;
    void* pMethod = nullptr;
    if (!ReadNativeMemory(apWrapper, pVtable) || !pVtable || !ReadNativeMemory(pVtable + 0x37, pMethod) || !pMethod)
        return false;
    MEMORY_BASIC_INFORMATION info{};
    if (!VirtualQuery(pMethod, &info, sizeof(info)) || info.State != MEM_COMMIT ||
        ((info.Protect & 0xFF) != PAGE_EXECUTE && (info.Protect & 0xFF) != PAGE_EXECUTE_READ &&
         (info.Protect & 0xFF) != PAGE_EXECUTE_READWRITE && (info.Protect & 0xFF) != PAGE_EXECUTE_WRITECOPY))
        return false;
    alignas(16) float position[4]{apPosition[0], apPosition[1], apPosition[2], 0.f};
    alignas(16) float rotation[4]{apRotation[0], apRotation[1], apRotation[2], apRotation[3]};
    using TSetPositionAndRotation = void(__fastcall*)(void*, const float*, const float*);
    reinterpret_cast<TSetPositionAndRotation>(pMethod)(apWrapper, position, rotation);
    return true;
}

bool GetDynamicBody(TESObjectREFR* apReference, DynamicBody& arBody,
    bool aAllowKeyframed = false) noexcept
{
    auto* pNode = apReference ? apReference->GetNiNode() : nullptr;
    if (!pNode || !pNode->collisionObject)
        return false;
    const auto collision = reinterpret_cast<uintptr_t>(pNode->collisionObject);
    void* pWrapper = nullptr;
    if (collision > std::numeric_limits<uintptr_t>::max() - 0x28 ||
        !ReadNativeMemory(reinterpret_cast<const void*>(collision + 0x20), pWrapper) || !pWrapper)
        return false;
    const auto wrapper = reinterpret_cast<uintptr_t>(pWrapper);
    void* pHavokBody = nullptr;
    if (wrapper > std::numeric_limits<uintptr_t>::max() - 0x18 ||
        !ReadNativeMemory(reinterpret_cast<const void*>(wrapper + 0x10), pHavokBody) ||
        !ReadNativeMemory(pHavokBody, arBody.State) ||
        (arBody.State.motionType != 3 &&
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
    const auto formId = s_kinematicProbeFormId.exchange(0,
        std::memory_order_acq_rel);
    if (!formId)
        return;
    const auto originalType = s_kinematicOriginalMotionType.exchange(0,
        std::memory_order_acq_rel);
    if (originalType < 1 || originalType > 7)
        return;
    if (auto* pReference = Cast<TESObjectREFR>(TESForm::GetById(formId));
        pReference && pReference->loadedState)
        pReference->SetMotionType(
            static_cast<TESObjectREFR::MotionType>(originalType), true);
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

// Host-driven bodies moving with a velocity: placed inside the physics step (its real dt known) so
// the step lands each exactly where its reference is drawn. The engine seats a rider from the body;
// placing it from our update with the previous frame's duration left the driver shaking by +-3 units.
struct StepTarget
{
    void* World{};
    void* Body{};
    float Position[3]{};  // Havok units, where the body must be after the step
    float Velocity[3]{};  // Havok units per second
    // Dynamic follow: steer a simulated body to this pose instead of placing it.
    bool Dynamic{};
    float Rotation[4]{0.f, 0.f, 0.f, 1.f}; // x, y, z, w
    float Angular[3]{};   // radians per second
};

// Dynamic follow: a simulated body reaches the host's pose within this time constant (seconds);
// farther than this (Havok units) it is placed there at once.
constexpr float kFollowTimeConstant = 0.1f;
constexpr float kFollowTeleport = 3.f;
std::mutex s_stepTargetsLock;
std::vector<StepTarget> s_stepTargets;
std::vector<StepTarget> s_stepTargetsBuilding;

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

bool SetNativeBodyPose(void* apWrapper, const glm::vec3& acPosition,
    const glm::quat& acRotation) noexcept
{
    void** pVtable{};
    if (!ReadNativeMemory(apWrapper, pVtable) || !pVtable)
        return false;
    void* pMethod{};
    if (!ReadNativeMemory(pVtable + 0x37, pMethod) || !pMethod)
        return false;
    MEMORY_BASIC_INFORMATION info{};
    if (!VirtualQuery(pMethod, &info, sizeof(info)) ||
        info.State != MEM_COMMIT ||
        ((info.Protect & 0xFF) != PAGE_EXECUTE &&
            (info.Protect & 0xFF) != PAGE_EXECUTE_READ &&
            (info.Protect & 0xFF) != PAGE_EXECUTE_READWRITE &&
            (info.Protect & 0xFF) != PAGE_EXECUTE_WRITECOPY))
        return false;
    float position[4]{acPosition.x, acPosition.y, acPosition.z, 0.f};
    float rotation[4]{acRotation.x, acRotation.y, acRotation.z,
        acRotation.w};
    using SetPoseFn = void(__fastcall*)(void*, float*, float*);
    reinterpret_cast<SetPoseFn>(pMethod)(apWrapper, position, rotation);
    return true;
}

int HookNativeStep(void* apWorld, float aDeltaTime)
{
    const auto started = std::chrono::steady_clock::now();
    {
        std::lock_guard lock(s_stepTargetsLock);
        for (const auto& target : s_stepTargets)
        {
            if (target.World != apWorld || !target.Body)
                continue;
            auto* pBody = static_cast<ActorPoseDiagnosticViews::RigidBody*>(target.Body);
            if (pBody->world != apWorld)
                continue;
            if (target.Dynamic)
            {
                // The host's motion plus a correction that closes the gap within the time constant.
                const glm::vec3 current{pBody->transform[12], pBody->transform[13], pBody->transform[14]};
                const glm::vec3 wanted{target.Position[0], target.Position[1], target.Position[2]};
                const glm::vec3 error = wanted - current;
                using TSetPositionAndRotation = void(__fastcall*)(void*, const float*, const float*);
                POINTER_SKYRIMSE(std::remove_pointer_t<TSetPositionAndRotation>, s_placeBody, 60898);
                if (glm::length(error) > kFollowTeleport)
                {
                    alignas(16) float position[4]{wanted.x, wanted.y, wanted.z, 0.f};
                    alignas(16) float quaternion[4]{target.Rotation[0], target.Rotation[1], target.Rotation[2], target.Rotation[3]};
                    s_placeBody.Get()(target.Body, position, quaternion);
                }
                const glm::vec3 linear = glm::vec3{target.Velocity[0], target.Velocity[1], target.Velocity[2]} +
                    error / kFollowTimeConstant;
                float currentRotation[4];
                MatrixToQuaternion(pBody->transform, currentRotation);
                const glm::quat have{currentRotation[3], currentRotation[0], currentRotation[1], currentRotation[2]};
                const glm::quat want{target.Rotation[3], target.Rotation[0], target.Rotation[1], target.Rotation[2]};
                glm::quat delta = want * glm::conjugate(have);
                if (delta.w < 0.f)
                    delta = -delta;
                const glm::vec3 turn = glm::vec3{delta.x, delta.y, delta.z} * 2.f / kFollowTimeConstant;
                for (int k = 0; k < 3; ++k)
                {
                    pBody->linearVelocity[k] = linear[k];
                    pBody->angularVelocity[k] = target.Angular[k] + turn[k];
                }
                continue;
            }
            float rotation[4];
            MatrixToQuaternion(pBody->transform, rotation);
            alignas(16) float position[4]{target.Position[0] - target.Velocity[0] * aDeltaTime,
                target.Position[1] - target.Velocity[1] * aDeltaTime, target.Position[2] - target.Velocity[2] * aDeltaTime, 0.f};
            alignas(16) float quaternion[4]{rotation[0], rotation[1], rotation[2], rotation[3]};
            // hkpRigidBody::setPositionAndRotation (60898), unlocked: this is the stepping thread,
            // before the step starts.
            using TSetPositionAndRotation = void(__fastcall*)(void*, const float*, const float*);
            POINTER_SKYRIMSE(std::remove_pointer_t<TSetPositionAndRotation>, s_setPositionAndRotation, 60898);
            s_setPositionAndRotation.Get()(target.Body, position, quaternion);
            for (int k = 0; k < 3; ++k)
                pBody->linearVelocity[k] = target.Velocity[k];
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
    glm::vec3 predictedTarget{};
    bool targetSampled = false;
    bool targetApplied = false;
    float velocityAfterWrite = -1.f;
    const auto playbackFormId = s_preStepPlaybackFormId.load(
        std::memory_order_acquire);
    const auto expectedEpoch = s_preStepExpectedEpoch.load(
        std::memory_order_acquire);
    if (beforeReadable && playbackFormId && expectedEpoch)
    {
        PreStepTarget target{};
        if (ReadPreStepTarget(target) && target.FormId == playbackFormId &&
            target.Epoch == expectedEpoch)
        {
            const auto nowNs = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    started.time_since_epoch()).count());
            const auto ageNs = nowNs >= target.ReceivedNs ?
                nowNs - target.ReceivedNs : UINT64_MAX;
            s_preStepLastSourceAgeMs.store(static_cast<uint32_t>(
                (std::min)(uint64_t{UINT32_MAX}, ageNs / 1000000)),
                std::memory_order_relaxed);
            if (ageNs <= 250000000 && std::isfinite(aDeltaTime) &&
                aDeltaTime > 0.f && aDeltaTime <= 0.05f &&
                std::isfinite(target.Position.x) &&
                std::isfinite(target.Position.y) &&
                std::isfinite(target.Position.z) &&
                std::isfinite(target.Velocity.x) &&
                std::isfinite(target.Velocity.y) &&
                std::isfinite(target.Velocity.z))
            {
                const float ageSeconds = static_cast<float>(ageNs) / 1e9f;
                predictedTarget = target.Position +
                    target.Velocity * (std::min)(ageSeconds, 0.1f);
                targetSampled = std::isfinite(predictedTarget.x) &&
                    std::isfinite(predictedTarget.y) &&
                    std::isfinite(predictedTarget.z);
                const glm::vec3 current{before.transform[12],
                    before.transform[13], before.transform[14]};
                const glm::vec3 error = predictedTarget - current;
                s_preStepLastPreError.store(glm::length(error) *
                    kHavokToGameUnits, std::memory_order_relaxed);
                glm::vec3 correction = error / 0.20f;
                const float correctionSpeed = glm::length(correction);
                if (correctionSpeed > 4.f)
                    correction *= 4.f / correctionSpeed;
                const glm::vec3 currentVelocity{before.linearVelocity[0],
                    before.linearVelocity[1], before.linearVelocity[2]};
                glm::vec3 velocity = currentVelocity +
                    (target.Velocity + correction - currentVelocity) * 0.5f;
                const float speed = glm::length(velocity);
                if (speed > 25.f)
                    velocity *= 25.f / speed;
                s_preStepLastVelocityCorrection.store(
                    glm::length(velocity - currentVelocity),
                    std::memory_order_relaxed);
                void* pWrapper = s_watchedBodyWrapper.load(
                    std::memory_order_acquire);
                if (pWrapper && pSelectedBody == s_watchedHavokBody.load(
                        std::memory_order_acquire))
                {
                    const auto playbackMode = s_preStepPlaybackMode.load(
                        std::memory_order_acquire);
                    if ((playbackMode == 3 || playbackMode == 4) &&
                        before.motionType == 4 &&
                        playbackFormId == s_kinematicProbeFormId.load(
                            std::memory_order_acquire) &&
                        std::isfinite(target.Quaternion.x) &&
                        std::isfinite(target.Quaternion.y) &&
                        std::isfinite(target.Quaternion.z) &&
                        std::isfinite(target.Quaternion.w) &&
                        std::isfinite(predictedTarget.x) &&
                        std::isfinite(predictedTarget.y) &&
                        std::isfinite(predictedTarget.z) &&
                        std::all_of(std::begin(before.transform),
                            std::begin(before.transform) + 12,
                            [](float value) { return std::isfinite(value); }))
                    {
                        const glm::mat3 currentMatrix{
                            glm::vec3{before.transform[0], before.transform[1],
                                before.transform[2]},
                            glm::vec3{before.transform[4], before.transform[5],
                                before.transform[6]},
                            glm::vec3{before.transform[8], before.transform[9],
                                before.transform[10]}};
                        const glm::quat currentRotation = glm::normalize(
                            glm::quat_cast(currentMatrix));
                        const float targetLength = glm::length(target.Quaternion);
                        if (std::isfinite(currentRotation.x) &&
                            std::isfinite(currentRotation.y) &&
                            std::isfinite(currentRotation.z) &&
                            std::isfinite(currentRotation.w) &&
                            std::isfinite(targetLength) && targetLength > 0.5f &&
                            targetLength < 1.5f)
                        {
                            const glm::quat targetRotation =
                                glm::normalize(target.Quaternion);
                            if (playbackMode == 4)
                            {
                                // Drive the keyframed body to the target over
                                // this Havok substep. Do not teleport it: a
                                // velocity-driven keyframe retains contact
                                // motion for other bodies it pushes.
                                glm::vec3 linear = error / aDeltaTime;
                                const float speed = glm::length(linear);
                                if (speed > 25.f)
                                    linear *= 25.f / speed;
                                glm::quat delta = glm::normalize(
                                    targetRotation * glm::inverse(currentRotation));
                                if (delta.w < 0.f)
                                    delta = -delta;
                                const float angle = 2.f * std::acos(
                                    std::clamp(delta.w, -1.f, 1.f));
                                const float sinHalf = std::sqrt((std::max)(
                                    0.f, 1.f - delta.w * delta.w));
                                glm::vec3 angular = sinHalf > 0.0001f ?
                                    glm::vec3{delta.x, delta.y, delta.z} *
                                        (angle / (sinHalf * aDeltaTime)) :
                                    glm::vec3{0.f};
                                const float angularSpeed = glm::length(angular);
                                if (angularSpeed > 20.f)
                                    angular *= 20.f / angularSpeed;
                                const float currentSpeed = glm::length(
                                    glm::vec3{before.linearVelocity[0],
                                        before.linearVelocity[1],
                                        before.linearVelocity[2]});
                                const float currentAngularSpeed = glm::length(
                                    glm::vec3{before.angularVelocity[0],
                                        before.angularVelocity[1],
                                        before.angularVelocity[2]});
                                const bool needsWrite =
                                    glm::length(error) * kHavokToGameUnits >
                                        0.25f || angle > glm::radians(0.25f) ||
                                    currentSpeed > 0.001f ||
                                    currentAngularSpeed > 0.001f;
                                if (needsWrite && std::isfinite(speed) &&
                                    std::isfinite(angularSpeed) &&
                                    std::isfinite(currentAngularSpeed))
                                {
                                    TP_THIS_FUNCTION(TSetVelocity, void, void,
                                        const float*);
                                    POINTER_SKYRIMSE(TSetVelocity,
                                        setLinearVelocity, 78089);
                                    POINTER_SKYRIMSE(TSetVelocity,
                                        setAngularVelocity, 78090);
                                    const float nativeLinear[4]{linear.x,
                                        linear.y, linear.z, 0.f};
                                    const float nativeAngular[4]{angular.x,
                                        angular.y, angular.z, 0.f};
                                    s_preStepAttempts.fetch_add(1,
                                        std::memory_order_relaxed);
                                    TiltedPhoques::ThisCall(setLinearVelocity,
                                        pWrapper, nativeLinear);
                                    TiltedPhoques::ThisCall(setAngularVelocity,
                                        pWrapper, nativeAngular);
                                    s_preStepApplied.fetch_add(1,
                                        std::memory_order_relaxed);
                                    targetApplied = true;
                                }
                            }
                            else
                            {
                                const float alpha = std::clamp(
                                    1.f - std::exp(-aDeltaTime / 0.05f),
                                    0.f, 1.f);
                                const glm::quat nextRotation = glm::normalize(
                                    glm::slerp(currentRotation, targetRotation,
                                        alpha));
                                glm::vec3 positionStep = error * alpha;
                                const float stepLength = glm::length(positionStep);
                                const float maxStep = 20.f / kHavokToGameUnits;
                                if (stepLength > maxStep)
                                    positionStep *= maxStep / stepLength;
                                s_preStepAttempts.fetch_add(1,
                                    std::memory_order_relaxed);
                                if (std::isfinite(nextRotation.x) &&
                                    std::isfinite(nextRotation.y) &&
                                    std::isfinite(nextRotation.z) &&
                                    std::isfinite(nextRotation.w) &&
                                    SetNativeBodyPose(pWrapper,
                                        current + positionStep, nextRotation))
                                {
                                    s_preStepPoseWrites.fetch_add(1,
                                        std::memory_order_relaxed);
                                    s_preStepApplied.fetch_add(1,
                                        std::memory_order_relaxed);
                                    s_preStepLastPoseStep.store(glm::length(
                                        positionStep) * kHavokToGameUnits,
                                        std::memory_order_relaxed);
                                    targetApplied = true;
                                }
                            }
                        }
                    }
                    else if (playbackMode == 2 &&
                        std::isfinite(target.Quaternion.x) &&
                        std::isfinite(target.Quaternion.y) &&
                        std::isfinite(target.Quaternion.z) &&
                        std::isfinite(target.Quaternion.w) &&
                        std::all_of(std::begin(before.transform),
                            std::begin(before.transform) + 12,
                            [](float value) { return std::isfinite(value); }))
                    {
                        const glm::mat3 currentMatrix{
                            glm::vec3{before.transform[0],
                                before.transform[1], before.transform[2]},
                            glm::vec3{before.transform[4],
                                before.transform[5], before.transform[6]},
                            glm::vec3{before.transform[8],
                                before.transform[9], before.transform[10]}};
                        const glm::quat currentRotation = glm::normalize(
                            glm::quat_cast(currentMatrix));
                        const float quaternionDot = std::clamp(std::abs(
                            glm::dot(currentRotation, target.Quaternion)),
                            0.f, 1.f);
                        const float angle = 2.f * std::acos(quaternionDot);
                        const float rotationAlpha = angle > 0.001f ?
                            (std::min)(0.25f, glm::radians(10.f) / angle) : 1.f;
                        const glm::quat nextRotation = glm::normalize(
                            glm::slerp(currentRotation, target.Quaternion,
                                rotationAlpha));
                        glm::vec3 positionStep = error * 0.25f;
                        const float stepLength = glm::length(positionStep);
                        const float maxStep = 8.f / kHavokToGameUnits;
                        if (stepLength > maxStep)
                            positionStep *= maxStep / stepLength;
                        if (std::isfinite(nextRotation.x) &&
                            std::isfinite(nextRotation.y) &&
                            std::isfinite(nextRotation.z) &&
                            std::isfinite(nextRotation.w) &&
                            SetNativeBodyPose(pWrapper, current + positionStep,
                                nextRotation))
                        {
                            s_preStepPoseWrites.fetch_add(1,
                                std::memory_order_relaxed);
                            s_preStepLastPoseStep.store(glm::length(
                                positionStep) * kHavokToGameUnits,
                                std::memory_order_relaxed);
                        }
                    }
                    if (playbackMode != 3 && playbackMode != 4)
                    {
                        TP_THIS_FUNCTION(TSetLinearVelocity, void, void,
                            const float*);
                        POINTER_SKYRIMSE(TSetLinearVelocity, setLinearVelocity,
                            78089);
                        const float nativeVelocity[4]{velocity.x, velocity.y,
                            velocity.z, 0.f};
                        s_preStepAttempts.fetch_add(1,
                            std::memory_order_relaxed);
                        TiltedPhoques::ThisCall(setLinearVelocity, pWrapper,
                            nativeVelocity);
                        s_preStepApplied.fetch_add(1,
                            std::memory_order_relaxed);
                        targetApplied = true;
                    }
                }
            }
            else
                s_preStepStaleSkips.fetch_add(1,
                    std::memory_order_relaxed);
        }
    }
    if (targetApplied && pSelectedBody)
    {
        ActorPoseDiagnosticViews::RigidBody afterWrite{};
        if (ReadNativeMemory(pSelectedBody, afterWrite) &&
            afterWrite.world == apWorld)
        {
            const auto& v = afterWrite.linearVelocity;
            velocityAfterWrite = std::sqrt(v[0] * v[0] + v[1] * v[1] +
                v[2] * v[2]);
        }
    }
    const int result = s_originalNativeStep ?
        s_originalNativeStep(apWorld, aDeltaTime) : 0;
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

bool SetDynamicBodyPosition(const DynamicBody& acBody, const glm::vec3& acGamePosition) noexcept
{
    void** pVtable = nullptr;
    if (!ReadNativeMemory(acBody.Wrapper, pVtable) || !pVtable)
        return false;
    void* pMethod = nullptr;
    if (!ReadNativeMemory(pVtable + 0x35, pMethod) || !pMethod)
        return false;
    MEMORY_BASIC_INFORMATION info{};
    if (!VirtualQuery(pMethod, &info, sizeof(info)) || info.State != MEM_COMMIT ||
        ((info.Protect & 0xFF) != PAGE_EXECUTE &&
         (info.Protect & 0xFF) != PAGE_EXECUTE_READ &&
         (info.Protect & 0xFF) != PAGE_EXECUTE_READWRITE &&
         (info.Protect & 0xFF) != PAGE_EXECUTE_WRITECOPY))
        return false;

    // hkVector4 is four floats. Live cart samples verified 70 game units per
    // Havok world unit on this runtime; the fourth lane is not a position.
    float nativePosition[4]{acGamePosition.x / kHavokToGameUnits,
        acGamePosition.y / kHavokToGameUnits,
        acGamePosition.z / kHavokToGameUnits, 0.f};
    using SetPositionFn = void(__fastcall*)(void*, float*);
    reinterpret_cast<SetPositionFn>(pMethod)(acBody.Wrapper, nativePosition);
    return true;
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
    s_preStepPlaybackMode.store(aFormId ? aMode : 0,
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
        s_physicsLastHostReferencesVisited.load(std::memory_order_relaxed),
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
    RestoreKinematicProbe();
    s_preStepExpectedEpoch.store(0, std::memory_order_release);
    s_watchedBodyWrapper.store(nullptr, std::memory_order_release);
    s_watchedHavokBody.store(nullptr, std::memory_order_release);
    s_watchedHavokWorld.store(nullptr, std::memory_order_release);
    m_referencePoses.clear();
    {
        std::lock_guard lock(m_remotePhysicsLock);
        m_remoteReferencePoses.clear();
    }
    m_physicsStreamCandidates.clear();
    m_gridDiscoveryCursor = 0;
    m_nextCurrentCellDiscovery = {};
    m_nextPhysicsPosePrune = {};
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
    if (!m_transport.IsConnected() || !m_world.GetPartyService().IsInParty())
    {
        m_applyOnMainFrame.store(false, std::memory_order_relaxed);
        m_captureOnMainFrame.store(false, std::memory_order_relaxed);
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
            if (kinematicSelected && watchedFormId == playbackFormId &&
                !s_kinematicProbeFormId.load(std::memory_order_acquire) &&
                GetDynamicBody(pReference, selectedBody))
            {
                const auto originalType = selectedBody.State.motionType;
                if (pReference->SetMotionType(
                        TESObjectREFR::MotionType::Keyframed, false))
                {
                    DynamicBody changed{};
                    if (GetDynamicBody(pReference, changed, true) &&
                        changed.State.motionType == 4)
                    {
                        s_kinematicOriginalMotionType.store(originalType,
                            std::memory_order_release);
                        s_kinematicProbeFormId.store(playbackFormId,
                            std::memory_order_release);
                    }
                    else
                        pReference->SetMotionType(
                            static_cast<TESObjectREFR::MotionType>(originalType), true);
                }
            }
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

    if (!m_world.GetPartyService().IsLeader())
    {
        const bool mainFrame = s_mainFramePlaybackEnabled.load(std::memory_order_relaxed);
        m_applyOnMainFrame.store(mainFrame, std::memory_order_relaxed);
        m_captureOnMainFrame.store(false, std::memory_order_relaxed);
        if (!mainFrame)
            ApplyRemotePhysics();
        return;
    }
    m_applyOnMainFrame.store(false, std::memory_order_relaxed);

    if (s_mainFrameCaptureEnabled.load(std::memory_order_relaxed))
    {
        m_captureOnMainFrame.store(true, std::memory_order_relaxed);
        std::vector<PhysicsReferencesMoveRequest> pending;
        {
            std::lock_guard lock(m_remotePhysicsLock);
            pending.swap(m_pendingPhysicsRequests);
        }
        for (const auto& request : pending)
        {
            s_physicsHostPacketsSent.fetch_add(1, std::memory_order_relaxed);
            m_transport.Send(request);
        }
        return;
    }
    m_captureOnMainFrame.store(false, std::memory_order_relaxed);
    CaptureHostPhysics(true);
}

void ObjectService::CaptureHostPhysics(const bool aSendNow) noexcept
{
    std::lock_guard physicsLock(m_remotePhysicsLock);
    const auto watchedFormId = s_preStepPlaybackFormId.load(std::memory_order_acquire) ?
        s_preStepPlaybackFormId.load(std::memory_order_acquire) : s_referencePhaseFormId.load(std::memory_order_acquire);
    const auto now = std::chrono::steady_clock::now();
    if (now < m_nextPhysicsSnapshot)
        return;
    m_nextPhysicsSnapshot = now + 50ms;
    s_physicsHostScans.fetch_add(1, std::memory_order_relaxed);

    auto* pPlayer = PlayerCharacter::Get();
    if (!pPlayer || !pPlayer->parentCell || !pPlayer->parentCell->refData.refArray)
        return;

    PhysicsReferencesMoveRequest request{};
    request.Tick = SmoothClock::NowTick() ? SmoothClock::NowTick() : m_transport.GetClock().GetCurrentTick();
    if (s_physicsStampEnabled.load(std::memory_order_relaxed))
    {
        // Physics mid-step: its state is half old, half new. Read it next frame instead.
        if (s_worldUpdatesRunning.load(std::memory_order_acquire) > 0)
        {
            m_nextPhysicsSnapshot = now;
            return;
        }
        const auto nowNs = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
            now.time_since_epoch()).count());
        const auto stepNs = s_worldUpdateLastStartNs.load(std::memory_order_relaxed);
        if (stepNs && nowNs >= stepNs && nowNs - stepNs < 200'000'000ull)
            request.Tick -= (nowNs - stepNs) / 1'000'000ull;
    }
    Set<uint32_t> observed;
    uint32_t referencesVisited{};
    auto processReference = [&](TESObjectREFR* pReference)
    {
        if (!pReference || pReference == pPlayer || !pReference->loadedState ||
            Cast<Actor>(pReference))
            return;

        if (observed.contains(pReference->formID))
            return;

        // Reject distant loaded-grid references before walking their native
        // collision/Havok graph. The old order performed several guarded
        // memory reads for every loaded ref, even outside stream range.
        const auto deltaFromPlayer = pReference->position - pPlayer->position;
        if (glm::dot(deltaFromPlayer, deltaFromPlayer) > 30000.f * 30000.f)
            return;

        const bool passive = IsPassivePhysicsReference(pReference);
        DynamicBody body{};
        if (!passive && !GetDynamicBody(pReference, body, true))
            return;
        // Keyframed bodies are driven by their node (scene, script, animation) on each PC, so
        // they are only sent at rest: measured, the Helgen carts are keyframed after the intro
        // scene and each PC's save parked them up to 34 units and 0.75 rad apart. Doors animate
        // their body while the reference stays put; leave them to the door sync.
        const bool keyframed = !passive && body.State.motionType == 4;
        if (keyframed && (!pReference->baseForm || pReference->baseForm->formType == FormType::Door))
            return;
        if (!passive && !std::all_of(std::begin(body.State.transform),
                std::end(body.State.transform), [](float value)
                { return std::isfinite(value); }))
            return;

        observed.insert(pReference->formID);
        // Retain discovered refs through transient native-body failures, and
        // keep passive items on the fast path after their first discovery.
        m_physicsStreamCandidates.insert(pReference->formID);
        if (pReference->formID == watchedFormId)
            s_physicsHostSelectedObserved.fetch_add(1,
                std::memory_order_relaxed);
        auto [poseIt, inserted] = m_referencePoses.try_emplace(pReference->formID);
        auto& previous = poseIt->second;
        if (inserted)
        {
            previous.Position = pReference->position;
            previous.Rotation = pReference->rotation;
            if (!passive)
            {
                std::copy(std::begin(body.State.transform),
                    std::end(body.State.transform),
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
        previous.Position = pReference->position;
        previous.Rotation = pReference->rotation;
        if (keyframed && moved)
        {
            // Animating on its own: track it, send nothing until it has rested for 5 s.
            std::copy(std::begin(body.State.transform), std::end(body.State.transform),
                previous.LastSentBodyTransform.begin());
            previous.LastSentBodyVelocity = {body.State.linearVelocity[0], body.State.linearVelocity[1],
                body.State.linearVelocity[2]};
            previous.LastSent = now;
            return;
        }
        if (moved)
            previous.HasMoved = true;
        // Periodic keyframes recover from a dropped delta or a follower that
        // enters an already-active cell. Resting bodies refresh slowly so a
        // follower that loaded later still converges; moving ones every 500 ms.
        if (!moved && now - previous.LastSent < (previous.HasMoved ? 500ms : 5000ms))
            return;

        PhysicsReferenceUpdate update{};
        if (!m_world.GetModSystem().GetServerModId(pReference->formID, update.Id))
            return;
        update.Position = {previous.Position.x, previous.Position.y, previous.Position.z};
        update.Rotation = {previous.Rotation.x, previous.Rotation.y, previous.Rotation.z};
        if (!passive)
        {
            update.MotionType = 3;
            update.LinearVelocity = {body.State.linearVelocity[0],
                body.State.linearVelocity[1], body.State.linearVelocity[2]};
            std::copy(std::begin(body.State.transform),
                std::end(body.State.transform), update.BodyTransform.begin());
            std::vector<ChildBody> children;
            CollectChildBodies(pReference, children);
            // Each child node local transform: on the owner its dynamic body turns the node (the
            // wheel spins); on a follower the bodies are keyframed and follow their nodes.
            // Physics writes a dynamic child's WORLD transform and leaves its local at the authored
            // value (measured: the host wheel local rotation stayed identity while its world turned),
            // so send the effective local: parent world^-1 * child world.
            for (const auto& childBody : children)
            {
                const NiAVObject* pParent = childBody.Node->parent;
                if (!pParent)
                    continue;
                const auto& pw = pParent->world;
                const auto& cw = childBody.Node->world;
                NiMatrix3 localRotate{};
                for (int r = 0; r < 3; ++r)
                    for (int c = 0; c < 3; ++c)
                    {
                        float sum = 0.f;
                        for (int k = 0; k < 3; ++k)
                            sum += pw.rotate.entry[k][r] * cw.rotate.entry[k][c];
                        localRotate.entry[r][c] = sum;
                    }
                const float d[3]{cw.translate.x - pw.translate.x, cw.translate.y - pw.translate.y, cw.translate.z - pw.translate.z};
                const float inverseScale = pw.scale != 0.f ? 1.f / pw.scale : 1.f;
                std::array<float, 7> entry{};
                for (int r = 0; r < 3; ++r)
                    entry[r] = (pw.rotate.entry[0][r] * d[0] + pw.rotate.entry[1][r] * d[1] + pw.rotate.entry[2][r] * d[2]) * inverseScale;
                NiMatrixToQuaternion(localRotate, entry.data() + 3);
                update.ChildBodies.push_back(entry);
            }
        }
        if (!passive)
        {
            s_hostRenderProbes.try_emplace(pReference->formID);
            auto& probe = s_hostSampleProbes[pReference->formID];
            const glm::vec3 position{update.Position.x, update.Position.y, update.Position.z};
            const float bodySpeed = glm::length(glm::vec3{update.LinearVelocity.x, update.LinearVelocity.y,
                update.LinearVelocity.z}) * kHavokToGameUnits;
            if (probe.Has && request.Tick > probe.Tick && request.Tick - probe.Tick < 200)
            {
                const float dtMs = static_cast<float>(request.Tick - probe.Tick);
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
            probe.Tick = request.Tick;
            probe.Has = true;
            if (now >= probe.NextLog && probe.Samples)
            {
                if (probe.BodySpeedSum / probe.Samples > 5.f)
                    spdlog::info("Host sample {:X}: {} samples, body speed {:.0f} u/s, implied-speed error mean {:.1f} max {:.1f} u/s, "
                        "body-position error mean {:.1f} u/s, tick gap off 50 ms by {:.1f} ms ({} thread)", pReference->formID, probe.Samples,
                        probe.BodySpeedSum / probe.Samples, probe.ErrorSum / probe.Samples, probe.ErrorMax,
                        probe.BodyErrorSum / probe.Samples,
                        probe.GapErrorSum / probe.Samples, aSendNow ? "update" : "main");
                probe = HostSampleProbe{probe.Position, probe.Tick, true};
                probe.BodyPosition = {update.BodyTransform[12] * kHavokToGameUnits,
                    update.BodyTransform[13] * kHavokToGameUnits, update.BodyTransform[14] * kHavokToGameUnits};
                probe.NextLog = now + std::chrono::seconds(5);
            }
        }
        request.Updates.push_back(update);
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

    auto scanCell = [&](TESObjectCELL* pCell)
    {
        if (!pCell || !pCell->refData.refArray ||
            pCell->refData.capacity > 50000)
            return;
        const auto& references = pCell->refData;
        for (uint32_t i = 0; i < references.capacity; ++i)
        {
            ++referencesVisited;
            processReference(references.refArray[i].Get());
        }
    };

    // Refresh known stream objects every snapshot. Resolve by form ID rather
    // than retaining cell/reference pointers across unloads.
    const auto knownRefreshStarted = std::chrono::steady_clock::now();
    for (const auto formId : m_physicsStreamCandidates)
        processReference(Cast<TESObjectREFR>(TESForm::GetById(formId)));
    s_hostKnownRefreshTiming.Record(HostScanDurationUs(knownRefreshStarted));

    // Discover new bodies without a 12k-reference full-grid walk on every
    // frame. The current cell refreshes twice per second; one additional
    // attached exterior cell is swept per snapshot.
    if (now >= m_nextCurrentCellDiscovery)
    {
        const auto currentDiscoveryStarted = std::chrono::steady_clock::now();
        scanCell(pPlayer->parentCell);
        m_nextCurrentCellDiscovery = now + 500ms;
        s_hostCurrentDiscoveryTiming.Record(
            HostScanDurationUs(currentDiscoveryStarted));
    }
    else
        s_hostCurrentDiscoveryTiming.Record(0);
    auto* pTes = TES::Get();
    auto* pGrid = pTes ? pTes->cells : nullptr;
    if (pPlayer->parentCell->worldspace && pGrid && pGrid->arr &&
        pGrid->dimension > 0 && pGrid->dimension <= 15)
    {
        const uint32_t count = pGrid->dimension * pGrid->dimension;
        auto* pCell = pGrid->arr[m_gridDiscoveryCursor++ % count];
        if (pCell && pCell != pPlayer->parentCell &&
            pCell->IsAttached() &&
            pCell->worldspace == pPlayer->parentCell->worldspace)
        {
            const auto gridDiscoveryStarted = std::chrono::steady_clock::now();
            scanCell(pCell);
            s_hostGridDiscoveryTiming.Record(
                HostScanDurationUs(gridDiscoveryStarted));
        }
        else
            s_hostGridDiscoveryTiming.Record(0);
    }
    else
        s_hostGridDiscoveryTiming.Record(0);
    if (now >= m_nextPhysicsPosePrune)
    {
        const auto pruneStarted = std::chrono::steady_clock::now();
        for (auto it = m_physicsStreamCandidates.begin();
            it != m_physicsStreamCandidates.end();)
        {
            auto* pReference = Cast<TESObjectREFR>(TESForm::GetById(*it));
            if (!pReference || !pReference->loadedState)
                it = m_physicsStreamCandidates.erase(it);
            else
                ++it;
        }
        for (auto it = m_referencePoses.begin(); it != m_referencePoses.end();)
        {
            auto* pReference = Cast<TESObjectREFR>(TESForm::GetById(it->first));
            if (!pReference || !pReference->loadedState)
                it = m_referencePoses.erase(it);
            else
                ++it;
        }
        m_nextPhysicsPosePrune = now + 5s;
        s_hostPruneTiming.Record(HostScanDurationUs(pruneStarted));
    }
    else
        s_hostPruneTiming.Record(0);
    const auto scanDurationUs = HostScanDurationUs(now);
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
        size_t{UINT32_MAX}, m_physicsStreamCandidates.size())),
        std::memory_order_relaxed);
    s_physicsLastHostUpdatesQueued.store(static_cast<uint32_t>(
        (std::min)(size_t{UINT32_MAX}, request.Updates.size())),
        std::memory_order_relaxed);
    if (!request.Updates.empty())
    {
        if (!aSendNow)
        {
            // Two snapshots can wait at most (the update job runs every frame).
            if (m_pendingPhysicsRequests.size() < 4)
                m_pendingPhysicsRequests.push_back(std::move(request));
            return;
        }
        s_physicsHostPacketsSent.fetch_add(1, std::memory_order_relaxed);
        m_transport.Send(request);
    }
}

void ObjectService::OnPhysicsReferencesMove(const NotifyPhysicsReferencesMove& acMessage) noexcept
{
    std::lock_guard lock(m_remotePhysicsLock);
    const auto& party = m_world.GetPartyService();
    if (!party.IsInParty() || party.IsLeader() ||
        acMessage.AuthorityEpoch != party.GetStartEpoch())
        return;
    s_physicsFollowerPacketsReceived.fetch_add(1,
        std::memory_order_relaxed);

    for (const auto& update : acMessage.Updates)
    {
        if (!std::isfinite(update.Position.x) || !std::isfinite(update.Position.y) ||
            !std::isfinite(update.Position.z) || !std::isfinite(update.Rotation.x) ||
            !std::isfinite(update.Rotation.y) || !std::isfinite(update.Rotation.z))
            continue;
        const uint32_t formId = m_world.GetModSystem().GetGameId(update.Id);
        auto* pReference = Cast<TESObjectREFR>(TESForm::GetById(formId));
        if (!pReference || Cast<Actor>(pReference) || !pReference->loadedState)
            continue;

        if (update.MotionType == 3 && !IsPassivePhysicsReference(pReference))
        {
            if (!std::isfinite(update.LinearVelocity.x) ||
                !std::isfinite(update.LinearVelocity.y) ||
                !std::isfinite(update.LinearVelocity.z))
                continue;
            if (!std::all_of(update.BodyTransform.begin(),
                    update.BodyTransform.end(), [](float value)
                    { return std::isfinite(value); }))
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

        if constexpr (!kEnablePerFrameReferenceCorrection)
        {
            pReference->SetMotionType(TESObjectREFR::MotionType::Keyframed, false);
            pReference->position.x = update.Position.x;
            pReference->position.y = update.Position.y;
            pReference->position.z = update.Position.z;
            pReference->SetRotation(update.Rotation.x, update.Rotation.y, update.Rotation.z);
            pReference->Update3DPosition(true);
            continue;
        }

        auto& pose = m_remoteReferencePoses[formId];
        if (pose.AuthorityEpoch == acMessage.AuthorityEpoch && acMessage.Tick <= pose.Tick)
            continue;
        pose.Position.x = update.Position.x;
        pose.Position.y = update.Position.y;
        pose.Position.z = update.Position.z;
        pose.Rotation.x = update.Rotation.x;
        pose.Rotation.y = update.Rotation.y;
        pose.Rotation.z = update.Rotation.z;
        pose.Tick = acMessage.Tick;
        pose.AuthorityEpoch = acMessage.AuthorityEpoch;
    }
}

void ObjectService::OnMainFrame() noexcept
{
    s_mainThreadId.store(static_cast<uint32_t>(GetCurrentThreadId()), std::memory_order_relaxed);
    auto* pService = s_objectService.load(std::memory_order_acquire);
    if (!pService)
        return;
    {
        std::lock_guard lock(pService->m_remotePhysicsLock);
        ProbeHostRender();
    }
    if (pService->m_captureOnMainFrame.load(std::memory_order_relaxed) &&
        s_mainFrameCaptureEnabled.load(std::memory_order_relaxed))
    {
        pService->CaptureHostPhysics(false);
        return;
    }
    if (!pService->m_applyOnMainFrame.load(std::memory_order_relaxed) ||
        !s_mainFramePlaybackEnabled.load(std::memory_order_relaxed))
        return;
    if (!s_loggedMainFrameThread.exchange(true))
        spdlog::info("Host-driven body playback runs on the main thread {}", GetCurrentThreadId());
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

void ObjectService::SetCartPhysicsEnabled(bool aEnabled) noexcept
{
    s_cartPhysicsEnabled.store(aEnabled, std::memory_order_relaxed);
    spdlog::info("Host-driven bodies {}", aEnabled ? "simulated and steered to the host" : "keyframed to the host");
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
    s_mainFrameCaptureEnabled.store(aEnabled, std::memory_order_relaxed);
    spdlog::info("Host body snapshot on the {} thread", aEnabled ? "main" : "update");
}

void ObjectService::SetMainFramePlaybackEnabled(bool aEnabled) noexcept
{
    s_mainFramePlaybackEnabled.store(aEnabled, std::memory_order_relaxed);
    spdlog::info("Host-driven body playback on the {} thread", aEnabled ? "main" : "update");
}

void ObjectService::ApplyRemotePhysics() noexcept
{
    std::lock_guard lock(m_remotePhysicsLock);
    s_stepTargetsBuilding.clear();
    struct PublishStepTargets
    {
        ~PublishStepTargets()
        {
            std::lock_guard stepLock(s_stepTargetsLock);
            s_stepTargets.swap(s_stepTargetsBuilding);
        }
    } publishStepTargets;
    const auto now = std::chrono::steady_clock::now();
    for (auto it = m_remoteReferencePoses.begin(); it != m_remoteReferencePoses.end();)
    {
        auto* pReference = Cast<TESObjectREFR>(TESForm::GetById(it->first));
        if (!pReference || Cast<Actor>(pReference) || !pReference->loadedState ||
            it->second.AuthorityEpoch != m_world.GetPartyService().GetStartEpoch())
        {
            it = m_remoteReferencePoses.erase(it);
            continue;
        }

        auto& pose = it->second;
        if (pose.BodyDriven && kHostDrivenMovingBodies && s_hostDrivenPlaybackEnabled.load(std::memory_order_relaxed) &&
            s_bodyPlaybackFormId.load(std::memory_order_acquire) != it->first)
        {
            if (!pose.SampleCount)
            {
                ++it;
                continue;
            }
            if (s_cartPhysicsEnabled.load(std::memory_order_relaxed))
            {
                if (!pose.DynamicFollow)
                {
                    pose.DynamicFollow = true;
                    // The host's own motion type (box inertia for the intro carts).
                    if (pose.HostDriven)
                        pReference->SetMotionType(static_cast<TESObjectREFR::MotionType>(
                            pose.HostMotionType >= 1 && pose.HostMotionType <= 3 ? pose.HostMotionType : 3), false);
                    pose.HostDriven = false;
                    spdlog::info("Host-driven body {:X}: simulated here, steered to the host's pose", it->first);
                }
                const uint32_t count = pose.SampleCount;
                const uint32_t size = static_cast<uint32_t>(pose.Samples.size());
                const auto sample = [&](uint32_t aIndex) -> const RemoteReferencePose::Sample&
                { return pose.Samples[(pose.SampleNext + size - count + aIndex) % size]; };
                const double renderTime = (SmoothClock::NowMs() > 0.0 ? SmoothClock::NowMs() :
                    static_cast<double>(m_transport.GetClock().GetCurrentTick())) -
                    static_cast<double>(m_world.GetCharacterService().GetPresentationDelayMs());
                const RemoteReferencePose::Sample* pA = &sample(count - 1);
                const RemoteReferencePose::Sample* pB = pA;
                float t = 0.f;
                if (renderTime <= static_cast<double>(sample(0).Tick))
                    pA = pB = &sample(0);
                else
                {
                    for (uint32_t i = 1; i < count; ++i)
                    {
                        const auto& a = sample(i - 1);
                        const auto& b = sample(i);
                        if (renderTime > static_cast<double>(b.Tick))
                            continue;
                        pA = &a;
                        pB = &b;
                        t = b.Tick > a.Tick ? static_cast<float>((renderTime - static_cast<double>(a.Tick)) /
                            static_cast<double>(b.Tick - a.Tick)) : 1.f;
                        break;
                    }
                }
                // Past the newest sample, the host stopped sending (at rest): keep steering to that final
                // pose, with no motion of its own, until it sits there, then place it exactly and stop.
                // Left to settle on its own, the cart came to rest off the host's, and the driver and
                // passengers climbed out of a different cart.
                const bool atFinalPose = renderTime - static_cast<double>(sample(count - 1).Tick) >
                    static_cast<double>(kHostDrivenHoldAfterMs);
                if (atFinalPose)
                {
                    pA = pB = &sample(count - 1);
                    t = 0.f;
                    if (pose.SettledAtFinalPose)
                    {
                        ++it;
                        continue;
                    }
                }
                else
                    pose.SettledAtFinalPose = false;
                DynamicBody body{};
                if (GetDynamicBody(pReference, body, false) && body.HavokBody && body.State.world)
                {
                    StepTarget target{body.State.world, body.HavokBody};
                    target.Dynamic = true;
                    const glm::vec3 position = pA->BodyPosition + (pB->BodyPosition - pA->BodyPosition) * t;
                    const glm::quat qa{pA->BodyRotation.w, pA->BodyRotation.x, pA->BodyRotation.y, pA->BodyRotation.z};
                    const glm::quat qb{pB->BodyRotation.w, pB->BodyRotation.x, pB->BodyRotation.y, pB->BodyRotation.z};
                    const glm::quat rotation = glm::normalize(glm::slerp(qa, qb, t));
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
                            // There (within a unit): this frame's step finishes it; then leave it at rest.
                            pose.SettledAtFinalPose = true;
                            spdlog::info("Host-driven body {:X}: at the host's final pose", it->first);
                        }
                    }
                    else if (pB->Tick > pA->Tick)
                    {
                        glm::quat step = qb * glm::conjugate(qa);
                        if (step.w < 0.f)
                            step = -step;
                        angular = glm::vec3{step.x, step.y, step.z} * 2.f / (static_cast<float>(pB->Tick - pA->Tick) / 1000.f);
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
                }
                ++it;
                continue;
            }
            if (!pose.HostDriven)
            {
                pose.DynamicFollow = false;
                pose.HostDriven = pReference->SetMotionType(TESObjectREFR::MotionType::Keyframed, false);
                spdlog::info("Host-driven body {:X}: following the host's transform (keyframed={})", it->first,
                    pose.HostDriven);
            }

            // Oldest-to-newest view of the sample ring.
            const uint32_t count = pose.SampleCount;
            const uint32_t size = static_cast<uint32_t>(pose.Samples.size());
            const auto sample = [&](uint32_t aIndex) -> const RemoteReferencePose::Sample&
            { return pose.Samples[(pose.SampleNext + size - count + aIndex) % size]; };
            // Smooth presentation time, read now (see SmoothClock); renderTick is its whole milliseconds.
            const bool smoothing = s_cartSmoothingEnabled.load(std::memory_order_relaxed);
            const double renderTime = (SmoothClock::NowMs() > 0.0 ? SmoothClock::NowMs() :
                static_cast<double>(m_transport.GetClock().GetCurrentTick())) -
                static_cast<double>(m_world.GetCharacterService().GetPresentationDelayMs());
            const int64_t renderTick = static_cast<int64_t>(std::floor(renderTime));

            const auto lerpAngle = [](float aFrom, float aTo, float aT)
            {
                const float delta = std::remainder(aTo - aFrom, static_cast<float>(TiltedPhoques::Pi * 2));
                return aFrom + delta * aT;
            };
            NiPoint3 position = sample(count - 1).Position;
            NiPoint3 rotation = sample(count - 1).Rotation;
            // Child bodies interpolate between the same two samples (never extrapolated).
            const RemoteReferencePose::Sample* pChildA = &sample(count - 1);
            const RemoteReferencePose::Sample* pChildB = pChildA;
            float childT = 0.f;
            if (renderTick <= static_cast<int64_t>(sample(0).Tick))
            {
                position = sample(0).Position;
                rotation = sample(0).Rotation;
            }
            else if (renderTick >= static_cast<int64_t>(sample(count - 1).Tick))
            {
                // Past the newest sample: a late packet continues the last motion briefly;
                // a body the host stopped sending (at rest) holds its last pose exactly.
                // A body that was moving and then holds is a visible freeze: log it.
                const int64_t starvedMs = renderTick - static_cast<int64_t>(sample(count - 1).Tick);
                if (count >= 2 && starvedMs > kHostDrivenHoldAfterMs && starvedMs < kHostDrivenHoldAfterMs + 40 &&
                    glm::length(glm::vec3{sample(count - 1).Position.x - sample(count - 2).Position.x,
                        sample(count - 1).Position.y - sample(count - 2).Position.y,
                        sample(count - 1).Position.z - sample(count - 2).Position.z}) > 1.f)
                    spdlog::info("Host-driven body {:X} starved: no host sample for {} ms while moving", it->first, starvedMs);
                if (count >= 2 && renderTick - static_cast<int64_t>(sample(count - 1).Tick) <= kHostDrivenHoldAfterMs)
                {
                    const auto& a = sample(count - 2);
                    const auto& b = sample(count - 1);
                    const int64_t span = static_cast<int64_t>(b.Tick) - static_cast<int64_t>(a.Tick);
                    const int64_t ahead = (std::min)(renderTick - static_cast<int64_t>(b.Tick), kHostDrivenMaxExtrapolationMs);
                    if (span > 0)
                    {
                        const float t = 1.f + static_cast<float>(ahead) / static_cast<float>(span);
                        position = a.Position + (b.Position - a.Position) * t;
                        rotation = glm::vec3{lerpAngle(a.Rotation.x, b.Rotation.x, t), lerpAngle(a.Rotation.y, b.Rotation.y, t),
                            lerpAngle(a.Rotation.z, b.Rotation.z, t)};
                    }
                }
            }
            else
            {
                for (uint32_t i = 1; i < count; ++i)
                {
                    const auto& a = sample(i - 1);
                    const auto& b = sample(i);
                    if (renderTick > static_cast<int64_t>(b.Tick))
                        continue;
                    const int64_t span = static_cast<int64_t>(b.Tick) - static_cast<int64_t>(a.Tick);
                    const float t = span > 0 ? static_cast<float>((renderTime - static_cast<double>(a.Tick)) /
                        static_cast<double>(span)) : 1.f;
                    position = a.Position + (b.Position - a.Position) * t;
                    if (s_hermitePlaybackEnabled.load(std::memory_order_relaxed) && span > 0 && span < 250)
                    {
                        const float spanSeconds = static_cast<float>(span) / 1000.f;
                        const float t2 = t * t;
                        const float t3 = t2 * t;
                        const float h00 = 2.f * t3 - 3.f * t2 + 1.f;
                        const float h10 = t3 - 2.f * t2 + t;
                        const float h01 = -2.f * t3 + 3.f * t2;
                        const float h11 = t3 - t2;
                        position = a.Position * h00 + a.Velocity * (h10 * spanSeconds) + b.Position * h01 +
                            b.Velocity * (h11 * spanSeconds);
                    }
                    rotation = glm::vec3{lerpAngle(a.Rotation.x, b.Rotation.x, t), lerpAngle(a.Rotation.y, b.Rotation.y, t),
                        lerpAngle(a.Rotation.z, b.Rotation.z, t)};
                    pChildA = &a;
                    pChildB = &b;
                    childT = t;
                    break;
                }
            }
            // A body at rest (no newer sample past the hold window) keeps the pose already
            // written; resting bodies are all host-driven, so rewriting them every frame adds up.
            const uint64_t newestTick = sample(count - 1).Tick;
            const bool atRest = renderTick - static_cast<int64_t>(newestTick) > kHostDrivenHoldAfterMs;
            if (atRest && pose.AppliedRestTick == newestTick)
            {
                ++it;
                continue;
            }
            pose.AppliedRestTick = atRest ? newestTick : 0;
            pose.PlaybackTarget = {position.x, position.y, position.z};
            pose.PlaybackHeading = rotation.z;
            pose.HasPlaybackTarget = true;
            // Critically damped smoothing of the root (snap on a real jump, or when switched off).
            float smoothAlpha = 1.f;
            if (smoothing && pose.SmoothHas)
            {
                const float dtMs = std::chrono::duration<float, std::milli>(now - pose.SmoothAt).count();
                smoothAlpha = dtMs > 0.f ? 1.f - std::exp(-dtMs / kCartSmoothingMs) : 0.f;
                const glm::vec3 target{position.x, position.y, position.z};
                if (glm::length(target - pose.SmoothPosition) > 50.f)
                    smoothAlpha = 1.f;
            }
            if (smoothing && !atRest)
            {
                const glm::vec3 target{position.x, position.y, position.z};
                pose.SmoothPosition = pose.SmoothHas ? pose.SmoothPosition + (target - pose.SmoothPosition) * smoothAlpha : target;
                for (int k = 0; k < 3; ++k)
                {
                    const float targetAngle = (&rotation.x)[k];
                    pose.SmoothRotation[k] = pose.SmoothHas ? pose.SmoothRotation[k] +
                        std::remainder(targetAngle - pose.SmoothRotation[k], static_cast<float>(TiltedPhoques::Pi * 2)) * smoothAlpha :
                        targetAngle;
                }
                position.x = pose.SmoothPosition.x;
                position.y = pose.SmoothPosition.y;
                position.z = pose.SmoothPosition.z;
                rotation.x = pose.SmoothRotation.x;
                rotation.y = pose.SmoothRotation.y;
                rotation.z = pose.SmoothRotation.z;
                pose.SmoothHas = true;
            }
            else
            {
                pose.SmoothPosition = {position.x, position.y, position.z};
                pose.SmoothRotation = {rotation.x, rotation.y, rotation.z};
                pose.SmoothHas = true;
                smoothAlpha = 1.f;
            }
            pose.SmoothAt = now;
            // The motion drawn this frame, for the body's velocity below.
            {
                const glm::vec3 drawn{position.x, position.y, position.z};
                const glm::vec3 drawnRotation{rotation.x, rotation.y, rotation.z};
                const float dtSeconds = std::chrono::duration<float>(now - pose.LastRenderedAt).count();
                if (pose.HasLastRendered && dtSeconds > 0.f && dtSeconds < 0.2f && glm::length(drawn - pose.LastRendered) < 50.f)
                {
                    pose.RenderVelocity = (drawn - pose.LastRendered) / dtSeconds;
                    pose.LastFrameSeconds = dtSeconds;
                    for (int k = 0; k < 3; ++k)
                        pose.RenderAngular[k] = std::remainder(drawnRotation[k] - pose.LastRenderedRotation[k],
                            static_cast<float>(TiltedPhoques::Pi * 2)) / dtSeconds;
                }
                else
                {
                    pose.RenderVelocity = {};
                    pose.RenderAngular = {};
                }
                pose.LastRendered = drawn;
                pose.LastRenderedRotation = drawnRotation;
                pose.LastRenderedAt = now;
                pose.HasLastRendered = true;
            }
            // Jitter probe: did anything move the node since our last write, and how even is our
            // own step (speed change between frames)?
            if (auto* pProbeNode = pReference->GetNiNode(); pProbeNode && pose.ProbeHas)
            {
                const glm::vec3 shown{pProbeNode->world.translate.x, pProbeNode->world.translate.y,
                    pProbeNode->world.translate.z};
                const float drift = glm::length(shown - pose.ProbeWritten);
                if (drift > 0.5f)
                    ++pose.ProbeMoved;
                // The child parts (wheels, yoke): does something else move them between our writes?
                {
                    std::vector<ChildBody> probeChildren;
                    CollectChildBodies(pReference, probeChildren);
                    pose.ProbeChildMotion.clear();
                    for (size_t i = 0; i < probeChildren.size(); ++i)
                    {
                        pose.ProbeChildMotion += std::to_string(probeChildren[i].Body->motionType);
                        if (i >= pose.ProbeChildWritten.size())
                            continue;
                        const auto& w = probeChildren[i].Node->world.translate;
                        const float childDrift = glm::length(glm::vec3{w.x, w.y, w.z} - pose.ProbeChildWritten[i]);
                        if (childDrift > 0.5f)
                            ++pose.ProbeChildMoved;
                        pose.ProbeChildDriftMax = (std::max)(pose.ProbeChildDriftMax, childDrift);
                    }
                }
                pose.ProbeDriftMax = (std::max)(pose.ProbeDriftMax, drift);
                const float dtMs = std::chrono::duration<float, std::milli>(now - pose.ProbeLastWrite).count();
                if (dtMs > 0.f)
                {
                    const float speed = glm::length(glm::vec3{position.x, position.y, position.z} - pose.ProbeWritten) /
                        dtMs * 1000.f;
                    const float change = std::abs(speed - pose.ProbeLastSpeed);
                    pose.ProbeSpeedSum += speed;
                    pose.ProbeSpeedChangeSum += change;
                    pose.ProbeSpeedChangeMax = (std::max)(pose.ProbeSpeedChangeMax, change);
                    pose.ProbeLastSpeed = speed;
                    // Angular speed (deg/s) per axis and its frame-to-frame change.
                    const glm::vec3 rot{rotation.x, rotation.y, rotation.z};
                    glm::vec3 angular{};
                    for (int k = 0; k < 3; ++k)
                        angular[k] = std::remainder(rot[k] - pose.ProbeLastRotation[k], static_cast<float>(TiltedPhoques::Pi * 2)) /
                            dtMs * 1000.f * 57.2958f;
                    const float angularChange = glm::length(angular - pose.ProbeLastAngularSpeed);
                    pose.ProbeAngularChangeSum += angularChange;
                    pose.ProbeAngularChangeMax = (std::max)(pose.ProbeAngularChangeMax, angularChange);
                    pose.ProbeLastAngularSpeed = angular;
                    pose.ProbeDtMaxMs = (std::max)(pose.ProbeDtMaxMs, dtMs);
                    ++pose.ProbeFrames;
                }
                if (now >= pose.ProbeNextLog && pose.ProbeFrames)
                {
                    const float meanSpeed = pose.ProbeSpeedSum / pose.ProbeFrames;
                    uint32_t mountId = 0;
                    const float mountLead = MountLead(pReference, pose.RenderVelocity, mountId);
                    if (meanSpeed > 5.f)
                        spdlog::info("Host-driven body {:X} rider {:X} lead {:.1f} u", it->first, mountId, mountLead);
                    if (meanSpeed > 5.f)
                        spdlog::info("Host-driven body {:X} jitter: {} frames, speed {:.0f} u/s, speed change mean {:.0f} max {:.0f}, "
                            "turn-rate change mean {:.1f} max {:.1f} deg/s, moved by others {} (max {:.1f} u), children [{}] moved by "
                            "others {} (max {:.1f} u), longest frame {:.0f} ms; drawn: {}/{} frames moved after the write, "
                            "root max {:.2f} u {:.2f} deg, child max {:.2f} u {:.2f} deg", it->first, pose.ProbeFrames, meanSpeed,
                            pose.ProbeSpeedChangeSum / pose.ProbeFrames, pose.ProbeSpeedChangeMax,
                            pose.ProbeAngularChangeSum / pose.ProbeFrames, pose.ProbeAngularChangeMax, pose.ProbeMoved,
                            pose.ProbeDriftMax, pose.ProbeChildMotion, pose.ProbeChildMoved, pose.ProbeChildDriftMax, pose.ProbeDtMaxMs, pose.ProbeEndMoved,
                            pose.ProbeEndFrames, pose.ProbeEndMoveMax, pose.ProbeEndTurnMax, pose.ProbeEndChildMoveMax,
                            pose.ProbeEndChildTurnMax);
                    pose.ProbeNextLog = now + std::chrono::seconds(5);
                    pose.ProbeFrames = pose.ProbeMoved = 0;
                    pose.ProbeDriftMax = pose.ProbeSpeedSum = pose.ProbeSpeedChangeSum = pose.ProbeSpeedChangeMax = 0.f;
                    pose.ProbeDtMaxMs = 0.f;
                    pose.ProbeAngularChangeSum = pose.ProbeAngularChangeMax = pose.ProbeChildDriftMax = 0.f;
                    pose.ProbeChildMoved = 0;
                    pose.ProbeEndFrames = pose.ProbeEndMoved = 0;
                    pose.ProbeEndMoveMax = pose.ProbeEndTurnMax = pose.ProbeEndChildMoveMax = pose.ProbeEndChildTurnMax = 0.f;
                }
            }
            const glm::vec3 jump{position.x - pReference->position.x, position.y - pReference->position.y, position.z - pReference->position.z};
            // The node shows the previous frame's pose (see s_visualLagFrameEnabled).
            NiPoint3 drawnPosition = position;
            NiPoint3 drawnRotation = rotation;
            if (s_visualLagFrameEnabled.load(std::memory_order_relaxed) && pose.HasPreviousDrawn &&
                glm::length(glm::vec3{position.x, position.y, position.z} - pose.PreviousDrawnPosition) < 50.f)
            {
                drawnPosition.x = pose.PreviousDrawnPosition.x;
                drawnPosition.y = pose.PreviousDrawnPosition.y;
                drawnPosition.z = pose.PreviousDrawnPosition.z;
                drawnRotation.x = pose.PreviousDrawnRotation.x;
                drawnRotation.y = pose.PreviousDrawnRotation.y;
                drawnRotation.z = pose.PreviousDrawnRotation.z;
            }
            pose.PreviousDrawnPosition = {position.x, position.y, position.z};
            pose.PreviousDrawnRotation = {rotation.x, rotation.y, rotation.z};
            pose.HasPreviousDrawn = true;
            pReference->position = drawnPosition;
            pReference->SetRotation(drawnRotation.x, drawnRotation.y, drawnRotation.z);
            // The other bodies (cart wheels, yoke): a keyframed body follows its node, so turn the
            // nodes to the host pose; carried rigidly by the root they never turned.
            if (!pChildA->Children.empty() && pChildA->Children.size() == pChildB->Children.size())
            {
                std::vector<ChildBody> childBodies;
                CollectChildBodies(pReference, childBodies);
                if (childBodies.size() == pChildA->Children.size())
                {
                    for (size_t i = 0; i < childBodies.size(); ++i)
                    {
                        const auto& x = pChildA->Children[i];
                        const auto& y = pChildB->Children[i];
                        auto& local = childBodies[i].Node->local;
                        local.translate.x = x[0] + (y[0] - x[0]) * childT;
                        local.translate.y = x[1] + (y[1] - x[1]) * childT;
                        local.translate.z = x[2] + (y[2] - x[2]) * childT;
                        float dot = 0.f;
                        for (int k = 0; k < 4; ++k)
                            dot += x[3 + k] * y[3 + k];
                        const float sign = dot < 0.f ? -1.f : 1.f;
                        float q[4];
                        float norm = 0.f;
                        for (int k = 0; k < 4; ++k)
                        {
                            q[k] = x[3 + k] + (sign * y[3 + k] - x[3 + k]) * childT;
                            norm += q[k] * q[k];
                        }
                        norm = norm > 0.f ? 1.f / std::sqrt(norm) : 1.f;
                        for (float& value : q)
                            value *= norm;
                        // The same smoothing for the part (translation and rotation).
                        if (pose.SmoothChildren.size() != childBodies.size())
                            pose.SmoothChildren.assign(childBodies.size(), std::array<float, 7>{0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f});
                        auto& smooth = pose.SmoothChildren[i];
                        const bool seeded = smooth[3] != 0.f || smooth[4] != 0.f || smooth[5] != 0.f || smooth[6] != 0.f;
                        const float a = seeded ? smoothAlpha : 1.f;
                        smooth[0] += (local.translate.x - smooth[0]) * a;
                        smooth[1] += (local.translate.y - smooth[1]) * a;
                        smooth[2] += (local.translate.z - smooth[2]) * a;
                        float qdot = 0.f;
                        for (int k = 0; k < 4; ++k)
                            qdot += smooth[3 + k] * q[k];
                        const float qsign = qdot < 0.f ? -1.f : 1.f;
                        float qnorm = 0.f;
                        for (int k = 0; k < 4; ++k)
                        {
                            smooth[3 + k] += (qsign * q[k] - smooth[3 + k]) * a;
                            qnorm += smooth[3 + k] * smooth[3 + k];
                        }
                        qnorm = qnorm > 0.f ? 1.f / std::sqrt(qnorm) : 1.f;
                        for (int k = 0; k < 4; ++k)
                        {
                            smooth[3 + k] *= qnorm;
                            q[k] = smooth[3 + k];
                        }
                        local.translate.x = smooth[0];
                        local.translate.y = smooth[1];
                        local.translate.z = smooth[2];
                        QuaternionToNiMatrix(q, local.rotate);
                    }
                }
            }
            // Warp (reset render motion history) only on a real jump: warping every frame of a
            // smoothly rolling cart made it flash under temporal anti-aliasing.
            pReference->Update3DPosition(glm::dot(jump, jump) > 50.f * 50.f);
            if (const auto* pProbeNode = pReference->GetNiNode())
            {
                pose.ProbeWrittenRotate = pProbeNode->world.rotate;
                pose.ProbeEndArmed = true;
                pose.ProbeWritten = {pProbeNode->world.translate.x, pProbeNode->world.translate.y, pProbeNode->world.translate.z};
                pose.ProbeHas = true;
                pose.ProbeLastWrite = now;
                pose.ProbeLastRotation = {rotation.x, rotation.y, rotation.z};
                std::vector<ChildBody> probeChildren;
                CollectChildBodies(pReference, probeChildren);
                pose.ProbeChildWritten.clear();
                for (const auto& child : probeChildren)
                    pose.ProbeChildWritten.push_back({child.Node->world.translate.x, child.Node->world.translate.y,
                        child.Node->world.translate.z});
                if (!probeChildren.empty())
                {
                    pose.ProbeChild0Written = pose.ProbeChildWritten[0];
                    pose.ProbeChild0Rotate = probeChildren[0].Node->world.rotate;
                }
            }
            // Its riders, at their host offsets from it.
            for (auto riderIt = s_riders.begin(); riderIt != s_riders.end();)
            {
                if (now - riderIt->second.SeenAt > std::chrono::milliseconds(500))
                {
                    riderIt = s_riders.erase(riderIt);
                    continue;
                }
                if (riderIt->second.ReferenceId == it->first)
                {
                    if (auto* pRider = Cast<Actor>(TESForm::GetById(riderIt->first)); pRider && !pRider->actorState.IsDeadState())
                    {
                        const glm::vec3 seat = glm::vec3{position.x, position.y, position.z} + riderIt->second.Offset -
                            riderIt->second.Drift;
                        riderIt->second.Placed = glm::vec3{position.x, position.y, position.z} + riderIt->second.Offset;
                        riderIt->second.HasPlaced = true;
                        NiPoint3 seatPosition{};
                        seatPosition.x = seat.x;
                        seatPosition.y = seat.y;
                        seatPosition.z = seat.z;
                        pRider->ForcePosition(seatPosition);
                        // Seated riders turn with their reference, as the engine seats them on the host.
                        if (false && riderIt->second.HasHeadingOffset)
                            pRider->SetRotation(pRider->rotation.x, pRider->rotation.y,
                                std::remainder(rotation.z + riderIt->second.HeadingOffset, static_cast<float>(TiltedPhoques::Pi * 2)));
                    }
                }
                ++riderIt;
            }
            // Writing the position does not move a reference into the exterior cell it
            // now stands in. Measured: the follower's cart kept its start cell, and when
            // that cell detached behind the players the cart (and the player riding it)
            // unloaded mid-road. Hand it to the new cell the way the engine does for a
            // Havok-moved reference (ID 19826 calls 19799 with the worldspace). MoveTo is
            // not usable here: it disables and re-enables the reference, reloading its 3D
            // and body, which measured as 7,000+ unit jumps on every crossing.
            if (auto* pCell = pReference->parentCell; s_cellHandoffEnabled.load(std::memory_order_relaxed) && pCell && !(pCell->cellFlags & 1))
            {
                if (auto* pWorldSpace = pReference->GetWorldSpace())
                {
                    const auto gridX = static_cast<int32_t>(std::floor(position.x / 4096.f));
                    const auto gridY = static_cast<int32_t>(std::floor(position.y / 4096.f));
                    auto* pTarget = ModManager::Get()->GetCellFromCoordinates(gridX, gridY, pWorldSpace, false);
                    if (pTarget && pTarget != pCell && pTarget->IsAttached())
                    {
                        spdlog::info("Host-driven body {:X} crossed from cell {:X} to {:X}", it->first, pCell->formID,
                            pTarget->formID);
                        using TUpdateParentCell = void(TESObjectREFR*, TESObjectCELL*, TESWorldSpace*);
                        POINTER_SKYRIMSE(TUpdateParentCell, s_updateParentCell, 19799);
                        s_updateParentCell.Get()(pReference, nullptr, pWorldSpace);
                    }
                }
            }
            // Keep the (keyframed) Havok body with the reference, at the host's offset between
            // its body origin and reference position (the centre of mass is not the origin).
            DynamicBody body{};
            if (GetDynamicBody(pReference, body, true))
            {
                const glm::vec3 hostBody{pose.BodyTransform[12] * kHavokToGameUnits,
                    pose.BodyTransform[13] * kHavokToGameUnits, pose.BodyTransform[14] * kHavokToGameUnits};
                const glm::vec3 offset = hostBody - glm::vec3{pose.Position.x, pose.Position.y, pose.Position.z};
                if (glm::dot(offset, offset) < 1000.f * 1000.f)
                {
                    const glm::vec3 rootTarget = glm::vec3{position.x, position.y, position.z} + offset;
                    if (s_rootBodyWriteEnabled.load(std::memory_order_relaxed))
                    {
                        // With the body carrying the drawn velocity, this frame's physics step moves it
                        // one frame further; place it that far back so it lands where the cart is drawn.
                        // The engine seats a rider from the body: one frame ahead put the driver 3 units
                        // in front of his cart (0.0 on the host).
                        SetDynamicBodyPosition(body, rootTarget);
                        if (s_bodyVelocityEnabled.load(std::memory_order_relaxed) && body.HavokBody && body.State.world)
                        {
                            StepTarget target{body.State.world, body.HavokBody};
                            for (int k = 0; k < 3; ++k)
                            {
                                target.Position[k] = rootTarget[k] / kHavokToGameUnits;
                                target.Velocity[k] = pose.RenderVelocity[k] / kHavokToGameUnits;
                            }
                            s_stepTargetsBuilding.push_back(target);
                        }
                        if (s_bodyVelocityEnabled.load(std::memory_order_relaxed) && body.HavokBody)
                        {
                            // Linear from the drawn motion; angular about world z from the heading
                            // (Skyrim's heading turns clockwise, Havok's positive z counterclockwise).
                            auto* pRigid = static_cast<ActorPoseDiagnosticViews::RigidBody*>(body.HavokBody);
                            for (int k = 0; k < 3; ++k)
                                pRigid->linearVelocity[k] = pose.RenderVelocity[k] / kHavokToGameUnits;
                            pRigid->linearVelocity[3] = 0.f;
                            pRigid->angularVelocity[0] = 0.f;
                            pRigid->angularVelocity[1] = 0.f;
                            pRigid->angularVelocity[2] = -pose.RenderAngular.z;
                            pRigid->angularVelocity[3] = 0.f;
                        }
                    }
                }
            }
            ++it;
            continue;
        }
        if (pose.BodyDriven)
        {
            if (s_bodyPlaybackFormId.load(std::memory_order_acquire) == it->first)
            {
                const auto sourceAge = std::chrono::duration_cast<std::chrono::milliseconds>(
                    now - pose.LastReceived).count();
                s_bodyPlaybackLastAgeMs.store(static_cast<uint32_t>((std::max)(
                    int64_t{}, sourceAge)), std::memory_order_relaxed);
                if (sourceAge <= 350)
                {
                    DynamicBody body{};
                    if (GetDynamicBody(pReference, body))
                    {
                        const auto started = std::chrono::steady_clock::now();
                        const auto currentTick = m_transport.GetClock().GetCurrentTick();
                        float fraction = 1.f;
                        if (pose.PriorTick && pose.Tick > pose.PriorTick &&
                            currentTick >= pose.PriorTick)
                        {
                            fraction = std::clamp(
                                static_cast<float>(currentTick - pose.PriorTick) /
                                    static_cast<float>(pose.Tick - pose.PriorTick),
                                0.f, 1.5f);
                        }
                        const glm::vec3 prior{pose.PriorPosition.x, pose.PriorPosition.y,
                            pose.PriorPosition.z};
                        const glm::vec3 latest{pose.Position.x, pose.Position.y,
                            pose.Position.z};
                        const glm::vec3 target = pose.PriorTick ?
                            prior + (latest - prior) * fraction : latest;
                        const glm::vec3 current{
                            body.State.transform[12] * kHavokToGameUnits,
                            body.State.transform[13] * kHavokToGameUnits,
                            body.State.transform[14] * kHavokToGameUnits};
                        s_bodyPlaybackLastPreError.store(glm::length(target - current),
                            std::memory_order_relaxed);
                        const float elapsed = pose.LastApplied == std::chrono::steady_clock::time_point{}
                            ? 1.f / 60.f
                            : std::clamp(std::chrono::duration<float>(now - pose.LastApplied).count(),
                                0.f, 0.05f);
                        pose.LastApplied = now;
                        const float alpha = 1.f - std::exp(-elapsed / 0.035f);
                        glm::vec3 step = (target - current) * alpha;
                        const float distance = glm::length(step);
                        const float maxStep = 3500.f * elapsed;
                        if (distance > maxStep && maxStep > 0.f)
                            step *= maxStep / distance;
                        if (glm::dot(step, step) > 0.0001f)
                        {
                            s_bodyPlaybackAttempts.fetch_add(1,
                                std::memory_order_relaxed);
                            s_bodyPlaybackLastStep.store(glm::length(step),
                                std::memory_order_relaxed);
                            if (SetDynamicBodyPosition(body, current + step))
                            {
                                s_bodyPlaybackSucceeded.fetch_add(1,
                                    std::memory_order_relaxed);
                                DynamicBody after{};
                                if (GetDynamicBody(pReference, after))
                                {
                                    const glm::vec3 observed{
                                        after.State.transform[12] * kHavokToGameUnits,
                                        after.State.transform[13] * kHavokToGameUnits,
                                        after.State.transform[14] * kHavokToGameUnits};
                                    s_bodyPlaybackLastPostError.store(
                                        glm::length(target - observed),
                                        std::memory_order_relaxed);
                                }
                            }
                        }
                        s_bodyPlaybackLastDurationUs.store(static_cast<uint32_t>(
                            std::chrono::duration_cast<std::chrono::microseconds>(
                                std::chrono::steady_clock::now() - started).count()),
                            std::memory_order_relaxed);
                    }
                }
                else
                    s_bodyPlaybackStaleSkips.fetch_add(1,
                        std::memory_order_relaxed);
                ++it;
                continue;
            }
            if constexpr (!kEnableDynamicBodyServo)
            {
                ++it;
                continue;
            }
            // Set the native dynamic body's velocity rather than warping its
            // reference/mesh or changing its motion type. Havok and the cart
            // script retain their normal integration and constraints.
            if (now - pose.LastReceived > 350ms)
            {
                ++it;
                continue;
            }
            DynamicBody body{};
            if (!GetDynamicBody(pReference, body))
            {
                ++it;
                continue;
            }
            const glm::vec3 current{body.State.linearVelocity[0],
                body.State.linearVelocity[1], body.State.linearVelocity[2]};
            const auto positionError = pose.Position - pReference->position;
            const glm::vec3 error{positionError.x / kHavokToGameUnits,
                positionError.y / kHavokToGameUnits,
                positionError.z / kHavokToGameUnits};
            glm::vec3 correction = error / 0.25f;
            const float correctionSpeed = glm::length(correction);
            if (correctionSpeed > 4.f)
                correction *= 4.f / correctionSpeed;
            const glm::vec3 target = pose.LinearVelocity + correction;
            const float elapsed = pose.LastApplied == std::chrono::steady_clock::time_point{}
                ? 1.f / 60.f
                : std::clamp(std::chrono::duration<float>(now - pose.LastApplied).count(), 0.f, 0.05f);
            pose.LastApplied = now;
            const float alpha = 1.f - std::exp(-elapsed / 0.08f);
            const glm::vec3 velocity = current + (target - current) * alpha;
            const glm::vec3 delta = velocity - current;
            if (glm::dot(delta, delta) > 0.0001f)
            {
                TP_THIS_FUNCTION(TSetLinearVelocity, void, void, const float*);
                POINTER_SKYRIMSE(TSetLinearVelocity, setLinearVelocity, 78089);
                const float nativeVelocity[4]{velocity.x, velocity.y, velocity.z, 0.f};
                TiltedPhoques::ThisCall(setLinearVelocity, body.Wrapper, nativeVelocity);
            }
            ++it;
            continue;
        }
        if (!IsPassivePhysicsReference(pReference))
        {
            it = m_remoteReferencePoses.erase(it);
            continue;
        }
        // A render-frame correction keeps Havok's follower proxy kinematic,
        // but never blocks the game waiting for another network packet.
        if (!pose.Kinematic)
            pose.Kinematic = pReference->SetMotionType(TESObjectREFR::MotionType::Keyframed, false);
        const float elapsed = pose.LastApplied == std::chrono::steady_clock::time_point{}
            ? 1.f / 60.f
            : std::clamp(std::chrono::duration<float>(now - pose.LastApplied).count(), 0.f, 0.05f);
        pose.LastApplied = now;
        const auto difference = pose.Position - pReference->position;
        const float distanceSquared = glm::dot(difference, difference);
        const float responseSeconds = distanceSquared > 2500.f ? 0.035f : 0.08f;
        const float alpha = 1.f - std::exp(-elapsed / responseSeconds);
        if (distanceSquared > 0.0001f)
            pReference->position += difference * alpha;

        const auto rotationDifference = pose.Rotation - pReference->rotation;
        const float dx = std::remainder(rotationDifference.x, static_cast<float>(TiltedPhoques::Pi * 2));
        const float dy = std::remainder(rotationDifference.y, static_cast<float>(TiltedPhoques::Pi * 2));
        const float dz = std::remainder(rotationDifference.z, static_cast<float>(TiltedPhoques::Pi * 2));
        if (distanceSquared > 0.0001f || dx * dx + dy * dy + dz * dz > 0.000001f)
        {
            pReference->SetRotation(pReference->rotation.x + dx * alpha,
                pReference->rotation.y + dy * alpha, pReference->rotation.z + dz * alpha);
            pReference->Update3DPosition(true);
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
