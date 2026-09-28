#include <TiltedCore/Stl.hpp>
#include <TiltedCore/Allocator.hpp>
#include <TiltedCore/Buffer.hpp>
#include <TiltedCore/ViewBuffer.hpp>
#include <TiltedCore/Serialization.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <optional>
#include <Messages/NpcInventory.h>
#include <catch2/catch.hpp>
#include "../client/Services/ReplaySyncMath.h"
#include "../client/Services/ReplaySyncPolicy.h"
#include "../client/Games/Skyrim/TriggerPartyContact.h"
#include "../client/Games/Skyrim/TriggerCapabilities.h"
#include "replay_recording.h"
#include <cstdlib>
#include <iostream>
#include <sstream>

namespace
{
const std::vector<ReplayRecording::Case>& Fixtures()
{
    static const auto cases = [] {
        const auto* path = std::getenv("TP_REPLAY_FIXTURE");
        return ReplayRecording::Load(path ? std::filesystem::path(path) :
            std::filesystem::path(__FILE__).parent_path() / "fixtures/replay/recorded_failures.rpl");
    }();
    return cases;
}
bool ObservedAcceptance()
{
    const auto* mode = std::getenv("TP_REPLAY_MODE");
    return mode && std::string(mode) == "observed";
}
Vector<NpcWornItem> Worn(const std::vector<ReplayRecording::Item>& aItems)
{
    Vector<NpcWornItem> result;
    for (const auto& x : aItems)
    {
        NpcWornItem item;
        item.Item.BaseId = {x.Mod, x.Base};
        item.Item.Count = 1;
        item.Item.ExtraWorn = true;
        item.Slots = x.Slots;
        result.push_back(item);
    }
    return result;
}
// In-memory presentation adapter. It implements the returned operations, not
// an assignment of the expected set. It owns no game inventory or loot stock.
Vector<NpcWornItem> ApplyPlan(Vector<NpcWornItem> aLocal, const Vector<NpcWornAction>& aPlan, bool aSupply)
{
    for (const auto& action : aPlan)
    {
        auto found = std::find_if(aLocal.begin(), aLocal.end(), [&](const auto& item) {
            return NpcSameItem(item.Item, action.Worn.Item, true);
        });
        if (action.Kind == NpcWornActionKind::AddRenderCopy)
        {
            if (aSupply)
            {
                auto copy = action.Worn;
                copy.Item.ExtraWorn = copy.Item.ExtraWornLeft = false;
                aLocal.push_back(copy);
            }
        }
        else if (found != aLocal.end())
        {
            found->Item.ExtraWorn = action.Kind == NpcWornActionKind::Equip;
            found->Item.ExtraWornLeft = false;
            found->Slots = action.Worn.Slots;
        }
    }
    std::erase_if(aLocal, [](const auto& item) { return !item.Item.IsWorn(); });
    return aLocal;
}
} // namespace

TEST_CASE("Recorded cart output violates the 30-unit step target", "[replay]")
{
    size_t count{};
    for (const auto& record : Fixtures())
    {
        if (record.Kind != 1) continue;
        ++count;
        INFO("unit=CartStepMetric; producer=ObjectService/native vehicle path (not resolved by observation)");
        INFO(record.Provenance);
        const double step = ReplaySync::CartStepMetric(record.Before, record.After);
        std::cout << "REPLAY CartStepMetric recorded=" << step << " target<=30 FAIL\n";
        if (ObservedAcceptance())
            CHECK(ReplaySync::CartStep(step) == ReplaySync::Verdict::Pass);
        else
        {
            CHECK(step == Approx(152.71513578227712).margin(0.00001));
            CHECK(ReplaySync::CartStep(step) == ReplaySync::Verdict::Fail);
        }
    }
    REQUIRE(count > 0);
}

TEST_CASE("Recorded Ralof empty set distinguishes missing supply from a complete worn plan", "[replay]")
{
    size_t count{};
    for (const auto& record : Fixtures())
    {
        if (record.Kind != 2) continue;
        ++count;
        INFO("unit=PlanNpcWorn/apply adapter; captured projection includes form and slot, not instance extras");
        INFO(record.Provenance);
        const auto owner = Worn(record.Owner), local = Worn(record.Local);
        const auto plan = PlanNpcWorn(local, owner); // Actual production pure policy.
        const auto noSupply = ApplyPlan(local, plan, false);
        const auto supplied = ApplyPlan(local, plan, true);
        std::cout << "REPLAY PlanNpcWorn recorded=" << local.size() << " owner=" << owner.size()
                  << " no-supply=" << noSupply.size() << " complete-plan=" << supplied.size() << '\n';
        if (ObservedAcceptance())
            CHECK(NpcSameWorn(local, owner));
        else
        {
            REQUIRE(!owner.empty());
            REQUIRE(local.empty());
            CHECK_FALSE(NpcSameWorn(noSupply, owner));
            CHECK(NpcSameWorn(supplied, owner));
            CHECK(PlanNpcWorn(supplied, owner).empty()); // Idempotence, no duplicate copies.
            CHECK(plan.size() == owner.size() * 2);
        }
    }
    REQUIRE(count > 0);
}

TEST_CASE("Replay time selection and steering preserve the owned implementation", "[replay]")
{
    using namespace ReplaySync;
    const uint64_t ticks[]{1000, 1100, 1200};
    auto at = [&](uint32_t i) { return ticks[i]; };
    CHECK_FALSE(SelectPlaybackWindow(0, 1000, at).Available);
    CHECK(SelectPlaybackWindow(3, 999, at).A == 0);
    CHECK(SelectPlaybackWindow(3, 1150, at).Fraction == Approx(0.5f));
    CHECK_FALSE(SelectPlaybackWindow(3, 1500, at).Hold);
    CHECK(SelectPlaybackWindow(3, 1501, at).Hold);
    CHECK(ActorFraction(1000, 1100, 999) == 0.f);
    CHECK(ActorFraction(1000, 1100, 1050) == Approx(0.5f));
    CHECK(ActorFraction(1000, 1000, 1000) == 1.f);
    CHECK(ActorPosition({0,0,0}, {100,0,0}, 1000,1100,999,false).x == 0.f);
    CHECK(ActorPosition({0,0,0}, {100,0,0}, 1000,1100,1300,true).x == 250.f);
    CHECK(ActorPosition({0,0,0}, {100,0,0}, 1000,1100,1300,false).x == 100.f);
    const auto follow = DynamicSteering({0,0,0}, {1,0,0}, {2,0,0});
    CHECK_FALSE(follow.Teleport);
    CHECK(follow.Velocity.x == 12.f);
    const auto teleported = DynamicSteering({0,0,0}, {4,0,0}, {2,0,0});
    CHECK(teleported.Teleport);
    CHECK(teleported.Position.x == 4.f);
    CHECK(teleported.Velocity.x == 2.f); // Never add old error after teleport.
    CHECK(AssemblyGoal({0,0,0}, {10,0,0}, {0,0,0}, 0.016f).x * 70.f == Approx(10.f));
    CHECK(glm::length(BoundAssemblyVelocity({100,100,0}, .016f, false)) * .016f * 70.f == Approx(25.f));
    // This is a bound on commands. It does not assert a bound on native contacts.
}

TEST_CASE("Recorded cart median and horse gap exceed paired observation targets", "[replay]")
{
    std::vector<double> cartGaps, horseGaps;
    for (const auto& record : Fixtures())
    {
        if (record.Kind != 3 && record.Kind != 4) continue;
        const auto vector = [](auto p) { return glm::vec3{p[0], p[1], p[2]}; };
        const auto target = ReplaySync::ActorPosition(vector(record.Before), vector(record.After),
            record.FirstTick, record.SecondTick, record.Tick, false);
        const double gap = glm::length(target - vector(record.Observed));
        REQUIRE(std::isfinite(gap));
        (record.Kind == 3 ? cartGaps : horseGaps).push_back(gap);
    }
    REQUIRE(!cartGaps.empty());
    REQUIRE(!horseGaps.empty());
    std::sort(cartGaps.begin(), cartGaps.end());
    const size_t half = cartGaps.size() / 2;
    const double median = cartGaps.size() % 2 ? cartGaps[half] : (cartGaps[half-1] + cartGaps[half]) * .5;
    const double horse = *std::max_element(horseGaps.begin(), horseGaps.end());
    INFO("unit=ActorPosition/GapMetric; one-second observed reference window, not packet presentation proof");
    std::cout << "REPLAY GapMetric cart-window-median=" << median << " horse-window-max=" << horse << " target<5\n";
    if (ObservedAcceptance())
    {
        CHECK(ReplaySync::Below(median, 5) == ReplaySync::Verdict::Pass);
        CHECK(ReplaySync::Below(horse, 5) == ReplaySync::Verdict::Pass);
    }
    else
    {
        CHECK(ReplaySync::Below(median, 5) == ReplaySync::Verdict::Fail);
        CHECK(ReplaySync::Below(horse, 5) == ReplaySync::Verdict::Fail);
    }
}

TEST_CASE("Replay pass targets distinguish missing evidence and threshold failures", "[replay]")
{
    using namespace ReplaySync;
    const auto missing = std::numeric_limits<double>::quiet_NaN();
    CHECK(CartStep(30) == Verdict::Pass);
    CHECK(CartStep(30.001) == Verdict::Fail);
    CHECK(Below(4.99, 5) == Verdict::Pass); // cart median / horse gap / settled debris
    CHECK(Below(5, 5) == Verdict::Fail);
    CHECK(Below(30, 30) == Verdict::Fail); // moving debris
    CHECK(BoneTarget(4.99, 9.99, true) == Verdict::Pass);
    CHECK(BoneTarget(5, 0, true) == Verdict::Fail);
    CHECK(BoneTarget(0, 10, true) == Verdict::Fail);
    CHECK(BoneTarget(29.99, missing, false) == Verdict::Pass);
    CHECK(BoneTarget(30, 0, false) == Verdict::Fail);
    CHECK(BoneTarget(0, missing, true) == Verdict::Missing);
    std::array<float,4> q{};
    REQUIRE(RagdollRotation({0,0,0,1}, {0,0,0,-1}, .5f, q));
    CHECK(RagdollAngle(q, {0,0,0,-1}) == Approx(0.f));
    CHECK_FALSE(RagdollRotation({0,0,0,0}, {0,0,0,0}, .5f, q));
    CHECK(RagdollDistance({1,0,0}, {0,0,0}) == 70.f);
    CHECK(CameraPitch(1.5707963267948966, 0, .05) == Verdict::Fail);
    CHECK(CameraPitch(1.5707963267948966, 1.5707963267948966, .05) == Verdict::Pass);
    CHECK(CameraPitch(0, missing, .05) == Verdict::Missing);
    CHECK(CameraPitch(missing, 0, .05) == Verdict::Missing);
    CHECK(WornDeliveryCount(1, 1, 1) == 0);
    CHECK(WornDeliveryCount(1, 2, 1) == 1);
}

TEST_CASE("Recorded camera coverage does not invent player intent", "[replay]")
{
    size_t cameras{}, bones{}, debris{};
    for (const auto& record : Fixtures())
    {
        INFO(record.Provenance);
        if (record.Kind == 5)
        {
            ++cameras;
            const auto verdict = ReplaySync::CameraPitch(record.Pitch, record.Intent, record.Tolerance);
            std::cout << "REPLAY CameraPitch " << (verdict == ReplaySync::Verdict::Missing ? "MISSING pitch/intent/tolerance" : "measured") << '\n';
            if (ObservedAcceptance()) CHECK(verdict == ReplaySync::Verdict::Pass);
            else CHECK(verdict == ReplaySync::Verdict::Missing);
        }
        if (record.Kind == 6)
        {
            ++bones;
            const auto vector = [](auto p) { return glm::vec3{p[0], p[1], p[2]}; };
            const auto units = ReplaySync::RagdollDistance(vector(record.Before), vector(record.After));
            const auto angle = ReplaySync::RagdollAngle(record.TargetRotation, record.ObservedRotation);
            CHECK(ReplaySync::BoneTarget(units, angle, record.Settled) == ReplaySync::Verdict::Pass);
        }
        if (record.Kind == 7)
        {
            ++debris;
            const auto units = ReplaySync::CartStepMetric(record.Before, record.After);
            CHECK(ReplaySync::Below(units, record.Settled ? 5 : 30) == ReplaySync::Verdict::Pass);
        }
    }
    REQUIRE(cameras > 0);
    if (!bones) std::cout << "REPLAY RagdollBoneError MISSING paired body transforms/rotations and settled state\n";
    if (!debris) std::cout << "REPLAY DebrisGap MISSING paired body transforms and settled state\n";
    if (ObservedAcceptance())
    {
        CHECK(bones > 0);
        CHECK(debris > 0);
    }
}

TEST_CASE("Replay trigger occupancy and door policy support five and ten peers", "[replay]")
{
    for (uint32_t players : {2, 5, 10})
    {
        TriggerPartyContact::Occupancy occupancy;
        for (uint32_t player = 1; player <= players; ++player)
            CHECK(occupancy.Observe(TriggerPartyContact::Enter, player, 1) == (player == 1));
        for (uint32_t player = 1; player <= players; ++player)
            CHECK(occupancy.Observe(TriggerPartyContact::Leave, player, 2) == (player == players));
        CHECK(ReplayDoorVotePolicy::Decide(true,true,true,true,true,true,true,players) == ReplayDoorVotePolicy::Skip::None);
    }
    CHECK_FALSE(ReplayDoorVotePolicy::CanFollow(true, 1, 1, 0, 0));
    CHECK_FALSE(ReplayDoorVotePolicy::CanFollow(false, 1, 2, 0, 0));
    CHECK(TriggerCapabilities::Hold(TriggerCapabilities::Bucket::QuestOrScene));
    CHECK(TriggerCapabilities::Hold(TriggerCapabilities::Bucket::Unknown));
}

TEST_CASE("Recorded worn intent survives encoding and epoch fencing for five and ten peers", "[replay]")
{
    const auto& cases = Fixtures();
    const auto sample = std::find_if(cases.begin(), cases.end(), [](const auto& x) { return x.Kind == 2; });
    REQUIRE(sample != cases.end());
    NpcWornData source;
    source.ServerId = 5; source.OwnershipEpoch = 1; source.Sequence = 14;
    source.Items = Worn(sample->Owner);
    TiltedPhoques::Buffer bytes(4096);
    TiltedPhoques::Buffer::Writer writer(&bytes);
    source.Serialize(writer);
    TiltedPhoques::ViewBuffer view(bytes.GetWriteData(), writer.Size());
    TiltedPhoques::Buffer::Reader reader(&view);
    NpcWornData decoded;
    REQUIRE(decoded.Deserialize(reader));
    REQUIRE(decoded.Valid());
    CHECK(NpcSameWorn(decoded.Items, source.Items));
    for (const size_t players : {5, 10})
    {
        std::vector<NpcWornData> peers(players);
        for (auto& peer : peers)
        {
            REQUIRE(NpcWornNewer(peer, decoded));
            peer = decoded;
            CHECK_FALSE(NpcWornNewer(peer, decoded));
            auto nextOwner = decoded;
            nextOwner.OwnershipEpoch = 2; nextOwner.Sequence = 1; nextOwner.Items.clear();
            REQUIRE(NpcWornNewer(peer, nextOwner));
            peer = nextOwner;
            CHECK_FALSE(NpcWornNewer(peer, decoded));
            const auto cleared = ApplyPlan(source.Items, PlanNpcWorn(source.Items, peer.Items), true);
            CHECK(cleared.empty()); // Authoritative empty is distinct from missing evidence.
        }
    }
}

TEST_CASE("Replay recording rejects truncation and unknown versions", "[replay]")
{
    for (const auto bytes : {"", "RPL2", "RPL1"})
    {
        std::istringstream input(bytes);
        CHECK_THROWS(ReplayRecording::Read(input));
    }
    const auto* fixture = std::getenv("TP_REPLAY_FIXTURE");
    const auto path = fixture ? std::filesystem::path(fixture) :
        std::filesystem::path(__FILE__).parent_path() / "fixtures/replay/recorded_failures.rpl";
    std::ifstream file(path, std::ios::binary);
    REQUIRE(file.good());
    const std::string bytes{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
    REQUIRE(bytes.size() > 32);
    for (const size_t length : {size_t{8}, size_t{32}, bytes.size()/2, bytes.size()-1})
    {
        std::istringstream input(bytes.substr(0, length));
        CHECK_THROWS(ReplayRecording::Read(input));
    }
    std::istringstream trailing(bytes + "x");
    CHECK_THROWS(ReplayRecording::Read(trailing));
}
