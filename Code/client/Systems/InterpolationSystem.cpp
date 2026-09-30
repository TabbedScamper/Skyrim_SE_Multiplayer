#include <TiltedOnlinePCH.h>

#include <Systems/InterpolationSystem.h>
#include <Services/CorpseRagdollService.h>
#include <Games/Skyrim/Havok/PoseCopyAuthority.h>
#include <Components.h>

#include <AI/AIProcess.h>
#include <Misc/MiddleProcess.h>

#include <Games/References.h>
#include <World.h>
#include <PlayerCharacter.h>
#include <Services/ObjectService.h>
#include <Services/CharacterService.h>
#include <Services/CreatorTogether.h>
#include <Services/PlayerCollision.h>
#include <Services/Generic/HeadTrackService.h>
#include <Services/SmoothClock.h>

namespace
{
// Another player's copy on static furniture (not riding a host-driven vehicle) is left unseated: its owner's stream
// places and poses it. Seated locally, the copy took this PC's own furniture animation at the Helgen chopping block
// (pelvis 85 u off the owner's, run 20260928-120033) and stayed seated after its owner got up (4964 u off).
std::atomic<bool> s_unseatRemotePlayers{false}; // no effect at the block (run 20260928-124808): the copy re-seats
std::mutex s_unseatLock;
std::vector<uint32_t> s_unseat;

// Remote actor placement on the main frame (owner, 2026-09-30: NPCs jitter slightly on the follower, Lokir most).
// Measured: the world update, and with it this system, runs inside the Papyrus VM update hook, on varying worker
// threads at a varying point of the frame; the follower drew Lokir 3.7 u (mean) off the interpolation of the samples
// it had received and 2.4x as jerky. Update publishes each actor's timeline here; the main frame places every remote
// actor for one presentation time per frame (fractional ms), just before the engine's frame.
std::atomic<bool> s_mainFramePlacement{true};
std::atomic<uint32_t> s_presentationDelayMs{100};
struct Placement
{
    std::vector<InterpolationComponent::TimePoint> Points; // positions and rotations only
    uint64_t PublishedMs{};
};
std::mutex s_placementLock;
std::unordered_map<uint32_t, Placement> s_placements;

void QueueUnseat(const uint32_t aFormId) noexcept
{
    std::lock_guard lock(s_unseatLock);
    if (std::find(s_unseat.begin(), s_unseat.end(), aFormId) == s_unseat.end())
        s_unseat.push_back(aFormId);
}
} // namespace

void InterpolationSystem::Update(Actor* apActor, InterpolationComponent& aInterpolationComponent, const uint64_t aDefaultTick) noexcept
{
    uint64_t aTick = aDefaultTick;
    const bool vehicleTimeline = ObjectService::VehiclePresentationTick(apActor, aTick);
    World::Get().GetHeadTrackService().UpdateRemote(apActor, aTick);
    auto& movements = aInterpolationComponent.TimePoints;

    if (movements.size() < 2)
        return;

    while (movements.size() > 2)
    {
        const auto second = *(++movements.begin());
        if (aTick > second.Tick)
            movements.pop_front();
        else
            break;
    }

    const auto& first = *(movements.begin());
    const auto& second = *(++movements.begin());

    // Clamp before subtracting unsigned network ticks. A presentation tick
    // before the first point must not wrap into a huge positive delta and
    // jump the remote actor to its future sample.
    float delta = 0.f;
    if (aTick >= second.Tick)
        delta = 1.f;
    else if (aTick > first.Tick && second.Tick > first.Tick)
        delta = static_cast<float>(aTick - first.Tick) /
            static_cast<float>(second.Tick - first.Tick);

    NiPoint3 position{TiltedPhoques::Lerp(first.Position, second.Position, delta)};
    // The native horse tether consumes this actor pose. Predict the active assembly
    // on the same frame clock as its cart; leave every unrelated actor's delay intact.
    if (vehicleTimeline && aTick > second.Tick && second.Tick > first.Tick)
    {
        const float prediction = static_cast<float>((std::min)(uint64_t{150}, aTick - second.Tick)) /
            static_cast<float>(second.Tick - first.Tick);
        position += (second.Position - first.Position) * prediction;
        delta += prediction;
    }
    float creatorHeading = 0.f;
    // In the character creator every player's character stands on this player's spot (the one
    // being viewed is visible; see CreatorTogether).
    const bool creatorPreview = apActor && CreatorTogether::GetDisplay(apActor->formID, position, creatorHeading);

    aInterpolationComponent.Position = position;

    // Don't try to move a null actor
    if (!apActor)
        return;

    // Once Skyrim has made a remote actor a corpse, Havok owns its physical
    // pose. Keep consuming the network timeline above for identity/spawn
    // bookkeeping, but do not teleport the dead reference or reload its
    // movement graph every presentation frame.
    // A dying copy that follows the owner's ragdoll takes its heading from that stream (CorpseRagdollService);
    // a second writer here raced it (Muse refute-corpse).
    if (apActor->actorState.IsDeadState() || CorpseRagdollService::IsFollowingOwner(apActor->formID))
        return;

    // Seated (ActorState1 sitSleepState, bits 14-17: 2 sitting down, 3 sitting): this PC's engine
    // attaches the actor to its seat every frame, as the host's does, on a chair or on a moving cart
    // alike. Placing it from the actor stream instead fought the seat and left the cart's driver
    // and passengers trailing their cart. The owner's pose still drives the body.
    const uint32_t sitSleepState = (apActor->actorState.flags1 >> 14) & 0xF;
    // A remote dragon never keeps a local perch: its local AI picked its own perch (Alduin drawn 210-265 u from the host's
    // on the Helgen tower, Muse diag-alduin); unseated, the owner's stream places it (ForcePosition below).
    // Off: measured, the perched copy already matches the owner within 1 u (run 20260928-143935); the audit's 200 u was
    // two snapshots of a dragon flying at over 1000 u/s taken at different moments, and the unseat never stuck.
    constexpr bool kUnseatRemoteDragons = false;
    const bool remoteDragon = kUnseatRemoteDragons && (sitSleepState == 2 || sitSleepState == 3) && !vehicleTimeline &&
        apActor->GetExtension() && apActor->GetExtension()->IsRemote() && !apActor->GetExtension()->IsPlayer() && apActor->IsDragon();
    if (remoteDragon || ((sitSleepState == 2 || sitSleepState == 3) && !vehicleTimeline &&
        s_unseatRemotePlayers.load(std::memory_order_relaxed) && apActor->GetExtension() && apActor->GetExtension()->IsRemotePlayer()))
    {
        static std::unordered_map<uint32_t, uint64_t> s_lastUnseat;
        auto& last = s_lastUnseat[apActor->formID];
        if (aTick >= last + 500)
        {
            last = aTick;
            QueueUnseat(apActor->formID);
            spdlog::info("Remote {} {:X}: left unseated on static furniture (sit state {}); the owner's stream places it",
                remoteDragon ? "dragon" : "player", apActor->formID, sitSleepState);
        }
    }
    {
        // Diagnostic: a remote actor's model appearing or disappearing (no 3D, or the root hidden:
        // NiAVObject flags bit 0), with its sit state (the cart driver vanished at the stop).
        static std::unordered_map<uint32_t, int> s_lastVisibility;
        const auto* pRoot = apActor->GetNiNode();
        const int visibility = !pRoot ? 0 : ((pRoot->flags & 1) ? 1 : 2);
        auto [visibilityIt, inserted] = s_lastVisibility.try_emplace(apActor->formID, visibility);
        if (!inserted && visibilityIt->second != visibility)
        {
            static const char* const s_names[] = {"no 3D", "hidden", "visible"};
            spdlog::info("Remote actor {:X} model {} -> {} (sit state {}, at {:.0f}, {:.0f}, {:.0f})", apActor->formID,
                s_names[visibilityIt->second], s_names[visibility], sitSleepState, apActor->position.x, apActor->position.y,
                apActor->position.z);
            visibilityIt->second = visibility;
        }
        // Diagnostic: a seated actor's body drawn away from its reference (the cart driver vanished
        // before the stop with his 3D present and not hidden). Skeleton root vs actor position, 4 Hz,
        // logged when it leaves or comes back within 80 units.
        struct SeatedDrift
        {
            uint64_t NextCheck{};
            bool Away{};
            uint64_t AwaySince{};
        };
        static std::unordered_map<uint32_t, SeatedDrift> s_seatedDrift;
        if (pRoot && (sitSleepState == 2 || sitSleepState == 3 || s_seatedDrift.contains(apActor->formID)))
        {
            auto& seated = s_seatedDrift[apActor->formID];
            auto& nextCheck = seated.NextCheck;
            auto& away = seated.Away;
            if (aTick >= nextCheck)
            {
                nextCheck = aTick + 250;
                static BSFixedString s_skeletonRoot("NPC Root [Root]");
                if (auto* pSkeleton = const_cast<NiNode*>(pRoot)->GetByName(s_skeletonRoot))
                {
                    const auto& w = pSkeleton->world.translate;
                    const float dx = w.x - apActor->position.x, dy = w.y - apActor->position.y, dz = w.z - apActor->position.z;
                    const float drift = std::sqrt(dx * dx + dy * dy + dz * dz);
                    if ((drift > 80.f) != away)
                    {
                        away = drift > 80.f;
                        spdlog::info("Seated actor {:X}: body {} its reference ({:.0f} units; sit state {}, body at {:.0f}, {:.0f}, {:.0f}, "
                            "reference at {:.0f}, {:.0f}, {:.0f}; owner pose: {})", apActor->formID, away ? "drawn away from" : "back at", drift,
                            sitSleepState, w.x, w.y, w.z, apActor->position.x, apActor->position.y, apActor->position.z,
                            PoseCopyAuthority::DescribeOverride(apActor->formID));
                    }
                    // A remote player still seated here after the intro while its owner walks away: the seat
                    // state only travels with actions, and leaving the intro cart sends none, so the copy
                    // stayed glued to the old cart seat (body 80-90 units from where the player is, looking
                    // half sunk and unanimated). Unseat it once the leader is free and the gap has lasted 1 s.
                    const bool remotePlayer = apActor->GetExtension() && apActor->GetExtension()->IsRemotePlayer();
                    if (remotePlayer && drift > 60.f && (sitSleepState == 2 || sitSleepState == 3) &&
                        PlayerCollision::LeaderHasFreeControl())
                    {
                        if (!seated.AwaySince)
                            seated.AwaySince = aTick;
                        else if (aTick - seated.AwaySince > 1000)
                        {
                            seated.AwaySince = 0;
                            QueueUnseat(apActor->formID);
                            spdlog::info("Seated actor {:X}: a remote player left on an old seat ({:.0f} units); unseated", apActor->formID, drift);
                        }
                    }
                    else
                        seated.AwaySince = 0;
                }
                if (sitSleepState != 2 && sitSleepState != 3 && !away)
                    s_seatedDrift.erase(apActor->formID);
            }
        }
    }
    static std::unordered_map<uint32_t, uint64_t> s_nextSitLog;
    if (ObjectService::IsRenderDiagnosticsArmed())
    {
        // Diagnostic: each remote actor's sit state, every 10 s.
        const auto now = GetTickCount64();
        std::erase_if(s_nextSitLog, [now](const auto& entry) { return now >= entry.second; });
        auto& next = s_nextSitLog[apActor->formID];
        if (now >= next)
        {
            next = now + 10000;
            spdlog::info("Remote actor {:X}: sitSleepState {} flags1 {:08X} furniture-seated={}", apActor->formID, sitSleepState,
                apActor->actorState.flags1, sitSleepState == 2 || sitSleepState == 3);
        }
    }
    else if (!s_nextSitLog.empty())
        s_nextSitLog.clear();
    {
        // Diagnostic: a jump of more than 150 units in one placement.
        const float jumpX = position.x - apActor->position.x, jumpY = position.y - apActor->position.y,
                    jumpZ = position.z - apActor->position.z;
        const float jump = std::sqrt(jumpX * jumpX + jumpY * jumpY + jumpZ * jumpZ);
        if (jump > 150.f)
            spdlog::info("Remote actor {:X} placed {:.0f} u away from where it was ({:.0f}, {:.0f}, {:.0f}) -> ({:.0f}, {:.0f}, {:.0f}), "
                "sit state {}", apActor->formID, jump, apActor->position.x, apActor->position.y, apActor->position.z, position.x,
                position.y, position.z, sitSleepState);
    }
    const bool mainPlacement = s_mainFramePlacement.load(std::memory_order_relaxed) && !vehicleTimeline && !creatorPreview;
    if (mainPlacement)
    {
        Placement placement;
        placement.PublishedMs = GetTickCount64();
        for (const auto& point : movements)
        {
            if (placement.Points.size() >= 6)
                break;
            InterpolationComponent::TimePoint slim;
            slim.Tick = point.Tick;
            slim.Position = point.Position;
            slim.Rotation = point.Rotation;
            placement.Points.push_back(slim);
        }
        std::lock_guard lock(s_placementLock);
        s_placements[apActor->formID] = std::move(placement);
    }
    else if (!vehicleTimeline || creatorPreview)
        apActor->ForcePosition(position);
    const auto& discrete = aTick >= second.Tick ? second : first;
    apActor->LoadAnimationVariables(discrete.Variables);

    if (apActor->currentProcess && apActor->currentProcess->middleProcess)
    {
        apActor->currentProcess->middleProcess->direction = discrete.Direction;
    }

    auto rotA = first.Rotation;
    auto rotB = second.Rotation;

    const auto deltaX = TiltedPhoques::DeltaAngle(rotA.x, rotB.x, true) * delta;
    const auto deltaY = TiltedPhoques::DeltaAngle(rotA.y, rotB.y, true) * delta;
    const auto deltaZ = TiltedPhoques::DeltaAngle(rotA.z, rotB.z, true) * delta;

    auto finalX = TiltedPhoques::Mod(rotA.x + deltaX, float(TiltedPhoques::Pi * 2));
    if (finalX > 0.f && finalX > float(TiltedPhoques::Pi / 2))
        finalX -= TiltedPhoques::Pi * 2;

    const auto finalY = TiltedPhoques::Mod(rotA.y + deltaY, float(TiltedPhoques::Pi * 2));
    const auto finalZ = TiltedPhoques::Mod(rotA.z + deltaZ, float(TiltedPhoques::Pi * 2));

    if (vehicleTimeline && !creatorPreview)
    {
        const float interval = second.Tick > first.Tick ? static_cast<float>(second.Tick - first.Tick) / 1000.f : 0.f;
        NiPoint3 velocity{}, angular{};
        if (interval > 0.f)
        {
            velocity = (second.Position - first.Position) / interval;
            angular = glm::vec3{TiltedPhoques::DeltaAngle(rotA.x, rotB.x, true) / interval,
                TiltedPhoques::DeltaAngle(rotA.y, rotB.y, true) / interval,
                TiltedPhoques::DeltaAngle(rotA.z, rotB.z, true) / interval};
        }
        // Keep the unpredicted host sample. Main applies the 150 ms prediction cap
        // exactly once, before the native tether jobs and alongside cart targets.
        const bool newest = aTick >= second.Tick;
        ObjectService::QueueVehiclePose(apActor, newest ? second.Tick : aTick,
            newest ? second.Position : position, newest ? second.Rotation : NiPoint3{glm::vec3{finalX, finalY, finalZ}},
            velocity, angular);
    }
    else if (!mainPlacement)
        apActor->SetRotation(finalX, finalY, creatorPreview ? creatorHeading : finalZ);
}

void InterpolationSystem::AddPoint(InterpolationComponent& aInterpolationComponent, const InterpolationComponent::TimePoint& acPoint) noexcept
{
    auto itor = std::begin(aInterpolationComponent.TimePoints);
    const auto end = std::cend(aInterpolationComponent.TimePoints);

    while (itor != end)
    {
        if (itor->Tick > acPoint.Tick)
        {
            aInterpolationComponent.TimePoints.insert(itor, acPoint);

            return;
        }

        ++itor;
    }

    aInterpolationComponent.TimePoints.push_back(acPoint);
}

InterpolationComponent& InterpolationSystem::Setup(World& aWorld, const entt::entity aEntity) noexcept
{
    return aWorld.emplace_or_replace<InterpolationComponent>(aEntity);
}

void InterpolationSystem::Clean(World& aWorld, const entt::entity aEntity) noexcept
{
    if (aWorld.all_of<InterpolationComponent>(aEntity))
        aWorld.remove<InterpolationComponent>(aEntity);
}

// Main thread (HookMainLoop): leave a stale seat with the engine's quick stop-interacting
// (Actor::StopInteractingQuick, ID 38697, VA 0x1406D2A40).
void InterpolationSystem::SetMainFramePlacement(const bool aEnabled) noexcept
{
    s_mainFramePlacement.store(aEnabled, std::memory_order_relaxed);
    if (!aEnabled)
    {
        std::lock_guard lock(s_placementLock);
        s_placements.clear();
    }
    spdlog::info("Remote actor placement: {}", aEnabled ? "main frame" : "world update");
}

bool InterpolationSystem::IsMainFramePlacement() noexcept
{
    return s_mainFramePlacement.load(std::memory_order_relaxed);
}

void InterpolationSystem::SetPresentationDelayMs(const uint32_t aDelayMs) noexcept
{
    s_presentationDelayMs.store(aDelayMs, std::memory_order_relaxed);
}

void InterpolationSystem::OnMainFrame() noexcept
{
    if (s_mainFramePlacement.load(std::memory_order_relaxed))
    {
        const double sharedNow = SmoothClock::NowMs();
        const double at = sharedNow - static_cast<double>(s_presentationDelayMs.load(std::memory_order_relaxed));
        const auto nowMs = GetTickCount64();
        std::vector<std::pair<uint32_t, Placement>> placements;
        {
            std::lock_guard lock(s_placementLock);
            // An actor the world update stopped publishing (dead, ragdoll, vehicle, gone) is left alone.
            std::erase_if(s_placements, [nowMs](const auto& entry) { return nowMs - entry.second.PublishedMs > 250; });
            placements.assign(s_placements.begin(), s_placements.end());
        }
        for (const auto& [formId, placement] : placements)
        {
            const auto& points = placement.Points;
            if (sharedNow <= 0.0 || points.size() < 2)
                continue;
            auto* pActor = Cast<Actor>(TESForm::GetById(formId));
            if (!pActor || !pActor->GetNiNode() || pActor->actorState.IsDeadState() ||
                CorpseRagdollService::IsFollowingOwner(formId))
                continue;
            size_t i = 0;
            while (i + 2 < points.size() && at > static_cast<double>(points[i + 1].Tick))
                ++i;
            const auto& first = points[i];
            const auto& second = points[i + 1];
            float delta = 0.f;
            if (at >= static_cast<double>(second.Tick))
                delta = 1.f;
            else if (at > static_cast<double>(first.Tick) && second.Tick > first.Tick)
                delta = static_cast<float>((at - static_cast<double>(first.Tick)) / static_cast<double>(second.Tick - first.Tick));
            const NiPoint3 position{TiltedPhoques::Lerp(first.Position, second.Position, delta)};
            pActor->ForcePosition(position);
            const auto& rotA = first.Rotation;
            const auto& rotB = second.Rotation;
            auto finalX = TiltedPhoques::Mod(rotA.x + TiltedPhoques::DeltaAngle(rotA.x, rotB.x, true) * delta, float(TiltedPhoques::Pi * 2));
            if (finalX > 0.f && finalX > float(TiltedPhoques::Pi / 2))
                finalX -= TiltedPhoques::Pi * 2;
            const auto finalY = TiltedPhoques::Mod(rotA.y + TiltedPhoques::DeltaAngle(rotA.y, rotB.y, true) * delta, float(TiltedPhoques::Pi * 2));
            const auto finalZ = TiltedPhoques::Mod(rotA.z + TiltedPhoques::DeltaAngle(rotA.z, rotB.z, true) * delta, float(TiltedPhoques::Pi * 2));
            pActor->SetRotation(finalX, finalY, finalZ);
        }
    }

    std::vector<uint32_t> unseat;
    {
        std::lock_guard lock(s_unseatLock);
        unseat.swap(s_unseat);
    }
    for (const auto formId : unseat)
    {
        auto* pActor = Cast<Actor>(TESForm::GetById(formId));
        if (!pActor || !pActor->GetNiNode())
            continue;
        using TStopInteractingQuick = void(Actor*, bool);
        POINTER_SKYRIMSE(TStopInteractingQuick, s_stopInteractingQuick, 38697);
        s_stopInteractingQuick.Get()(pActor, true);
    }
}

void InterpolationSystem::SetUnseatRemotePlayers(bool aEnabled) noexcept
{
    s_unseatRemotePlayers.store(aEnabled, std::memory_order_relaxed);
    spdlog::info("Unseat remote players: {}", aEnabled);
}

bool InterpolationSystem::IsUnseatRemotePlayers() noexcept
{
    return s_unseatRemotePlayers.load(std::memory_order_relaxed);
}
