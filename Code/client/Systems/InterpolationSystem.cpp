#include <TiltedOnlinePCH.h>

#include <Systems/InterpolationSystem.h>
#include <Components.h>

#include <AI/AIProcess.h>
#include <Misc/MiddleProcess.h>

#include <Games/References.h>
#include <World.h>
#include <Services/ObjectService.h>

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

    const NiPoint3 position{TiltedPhoques::Lerp(first.Position, second.Position, delta)};

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
        // Diagnostic: each remote actor's sit state, every 10 s.
        static std::unordered_map<uint32_t, uint64_t> s_nextSitLog;
        auto& next = s_nextSitLog[apActor->formID];
        if (aTick >= next)
        {
            next = aTick + 10000;
            spdlog::info("Remote actor {:X}: sitSleepState {} flags1 {:08X} furniture-seated={}", apActor->formID, sitSleepState,
                apActor->actorState.flags1, sitSleepState == 2 || sitSleepState == 3);
        }
    }
    if (sitSleepState == 2 || sitSleepState == 3)
    {
        const auto& seated = aTick >= second.Tick ? second : first;
        apActor->LoadAnimationVariables(seated.Variables);
        return;
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

    apActor->SetRotation(finalX, finalY, finalZ);
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
