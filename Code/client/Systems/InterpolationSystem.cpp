#include <TiltedOnlinePCH.h>

#include <Systems/InterpolationSystem.h>
#include <Components.h>

#include <AI/AIProcess.h>
#include <Misc/MiddleProcess.h>

#include <Games/References.h>
#include <World.h>
#include <PlayerCharacter.h>
#include <Services/ObjectService.h>
#include <Services/CharacterService.h>
#include <Services/CreatorTogether.h>

void InterpolationSystem::Update(Actor* apActor, InterpolationComponent& aInterpolationComponent, const uint64_t aTick) noexcept
{
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
    if (apActor->actorState.IsDeadState())
        return;

    // Seated (ActorState1 sitSleepState, bits 14-17: 2 sitting down, 3 sitting): this PC's engine
    // attaches the actor to its seat every frame, as the host's does, on a chair or on a moving cart
    // alike. Placing it from the actor stream instead fought the seat and left the cart's driver
    // and passengers trailing their cart. The owner's pose still drives the body.
    const uint32_t sitSleepState = (apActor->actorState.flags1 >> 14) & 0xF;
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
        static std::unordered_map<uint32_t, std::pair<uint64_t, bool>> s_seatedDrift;
        if (pRoot && (sitSleepState == 2 || sitSleepState == 3 || s_seatedDrift.contains(apActor->formID)))
        {
            auto& [nextCheck, away] = s_seatedDrift[apActor->formID];
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
                            "reference at {:.0f}, {:.0f}, {:.0f})", apActor->formID, away ? "drawn away from" : "back at", drift,
                            sitSleepState, w.x, w.y, w.z, apActor->position.x, apActor->position.y, apActor->position.z);
                    }
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
