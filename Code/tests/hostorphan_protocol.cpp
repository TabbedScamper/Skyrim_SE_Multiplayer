#include <TiltedCore/Stl.hpp>
#include <TiltedCore/Allocator.hpp>
#include <TiltedCore/Buffer.hpp>
#include <TiltedCore/Serialization.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <catch2/catch.hpp>
#include <optional>
#include <Messages/ClientMessageFactory.h>
#include <Messages/AssignCharacterRequest.h>
#include <Messages/RequestOwnershipTransfer.h>
#include "../server/Services/OwnershipPolicy.h"

using namespace TiltedPhoques;
#define TP_INTERNAL_COMPONENTS_GUARD
#include "../server/Components/CellIdComponent.h"
#include "../server/Components/OwnerComponent.h"
#undef TP_INTERNAL_COMPONENTS_GUARD

TEST_CASE("Assignment retains baseline wire format", "[hostorphan]")
{
    for (const uint32_t cookie : {0u, 42u, 0xFFFFFFFFu})
    {
        AssignCharacterRequest source;
        source.Cookie = cookie;
        source.ReferenceId = {1, 0x123};
        source.FormId = {1, 0x456};
        source.CellId = {1, 0x789};
        source.LeveledNpcPickId = {1, 0xABC};
        Buffer buffer(8192);
        Buffer::Writer writer(&buffer);
        source.Serialize(writer);
        Buffer::Reader reader(&buffer);
        auto decoded = ClientMessageFactory{}.Extract(reader);
        REQUIRE(decoded);
        REQUIRE(decoded->GetOpcode() == AssignCharacterRequest::Opcode);
        const auto& assignment = static_cast<AssignCharacterRequest&>(*decoded);
        REQUIRE(assignment == source);
        REQUIRE(assignment.Cookie == cookie);
    }
}

TEST_CASE("Exterior stored actor cannot be granted to an interior leader", "[hostorphan]")
{
    const CellIdComponent exterior{{1, 1}, {1, 2}, {0, 0}};
    const CellIdComponent keep{{1, 3}, {}, {}};
    const CellIdComponent otherInterior{{1, 4}, {}, {}};
    REQUIRE_FALSE(keep.IsInRange(exterior, false));
    REQUIRE_FALSE(keep.IsInRange(otherInterior, false));
    REQUIRE(keep.IsInRange(keep, false));
    REQUIRE_FALSE(OwnershipPolicy::EligibleTarget(true, true, true, keep.IsInRange(exterior, false), true, false));
    REQUIRE(OwnershipPolicy::EligibleTarget(true, true, true, keep.IsInRange(keep, false), true, false));
    REQUIRE(OwnershipPolicy::EligibleTarget(true, true, false, false, true, false));
    REQUIRE_FALSE(OwnershipPolicy::EligibleTarget(true, true, false, false, false, false));
}

TEST_CASE("Orphan rejects the former simulator and retains grant declines", "[hostorphan]")
{
    // Opaque identities only: the real OwnerComponent never dereferences them.
    int a = 0, b = 0;
    auto* former = reinterpret_cast<Player*>(&a);
    auto* leader = reinterpret_cast<Player*>(&b);
    OwnerComponent owner(former, 9);
    owner.InvalidOwners.push_back(leader);
    owner.RecordRelease(former, true, 1000);
    owner.RecordRelease(former, true, 1000);
    REQUIRE(owner.InvalidOwners.size() == 2);
    REQUIRE(owner.Released);
    REQUIRE(owner.RetryLeader);
    owner.RecordRelease(former, false, 1000, false);
    REQUIRE(owner.RetryLeader); // A follower decline also preserves the retry.
    owner.RecordRelease(leader, false, 1000, true);
    REQUIRE_FALSE(owner.RetryLeader); // Leader decline stops repeated grants.
    owner.RecordRelease(leader, true, 1000, true);
    owner.FinishGrant(false);
    REQUIRE_FALSE(owner.Released);
    REQUIRE(owner.RetryLeader); // Follower grant must not consume leader retry.
    REQUIRE_FALSE(OwnershipPolicy::ReleaseReady(1999, owner.ReleasedAt));
    REQUIRE(OwnershipPolicy::ReleaseReady(2000, owner.ReleasedAt));
    owner.SetOwner(nullptr);
    REQUIRE_FALSE(owner.IsCurrentOwner(former, 9));
    REQUIRE_FALSE(owner.IsCurrentOwner(nullptr, 9));
    REQUIRE(owner.InvalidOwners.size() == 2);
    REQUIRE_FALSE(OwnershipPolicy::EligibleTarget(true, true, true, true, true, !owner.InvalidOwners.empty()));
    owner.InvalidOwners.clear(); // New native/cell evidence, not a disconnect.
    owner.SetOwner(leader);
    owner.FinishGrant(true);
    REQUIRE_FALSE(owner.RetryLeader);
    owner.OwnershipEpoch = 10;
    REQUIRE(owner.IsCurrentOwner(leader, 10));
    REQUIRE_FALSE(owner.IsCurrentOwner(former, 9));
    REQUIRE_FALSE(owner.IsCurrentOwner(leader, 9));
}

TEST_CASE("Started session reclaims before follower transport timeout", "[hostorphan]")
{
    // Reproduce the service's session-or-disconnected expression. A connected
    // follower must not prevent the baseline leader authority in a running game.
    for (const bool connected : {false, true})
    {
        REQUIRE(OwnershipPolicy::ShouldClaim(false, true, true, true,
            3 >= 1 || !connected, false, true, false));
        REQUIRE(OwnershipPolicy::ShouldClaim(false, true, true, true,
            0 >= 1 || !connected, false, true, false) == !connected);
        REQUIRE_FALSE(OwnershipPolicy::ShouldClaim(false, true, false, true,
            true, false, true, false));
        REQUIRE_FALSE(OwnershipPolicy::ShouldClaim(false, true, true, true,
            true, false, true, true));
    }
}

TEST_CASE("Five candidates retain independent declines through handoff", "[hostorphan]")
{
    int identities[5]{};
    Player* members[5]{};
    for (unsigned i = 0; i < 5; ++i)
        members[i] = reinterpret_cast<Player*>(&identities[i]);
    for (unsigned releaser = 0; releaser < 5; ++releaser)
    for (unsigned mask = 0; mask < 32; ++mask)
    {
        OwnerComponent owner(members[releaser], 7);
        for (unsigned i = 0; i < 5; ++i)
            if (mask & (1u << i))
                owner.InvalidOwners.push_back(members[i]);
        owner.RecordRelease(members[releaser], true, 1000);
        owner.RecordRelease(members[releaser], true, 1000);
        owner.FinishGrant(false);
        REQUIRE(owner.RetryLeader);
        for (unsigned candidate = 0; candidate < 5; ++candidate)
        {
            const bool expectedDecline = candidate == releaser || (mask & (1u << candidate));
            const auto count = std::count(owner.InvalidOwners.begin(), owner.InvalidOwners.end(), members[candidate]);
            REQUIRE(count == (expectedDecline ? 1 : 0));
            REQUIRE(OwnershipPolicy::EligibleTarget(true, true, true, true, candidate == 0, count != 0) == !expectedDecline);
            REQUIRE_FALSE(OwnershipPolicy::EligibleTarget(false, true, true, true, candidate == 0, count != 0));
        }
        owner.RecordRelease(members[releaser], false, 2000, releaser == 0);
        REQUIRE(owner.RetryLeader == (releaser != 0)); // A follower decline retains the leader's pending retry.
        owner.RecordRelease(members[0], false, 2001, true);
        REQUIRE_FALSE(owner.RetryLeader); // A leader decline cannot arm a timer loop.
    }
}

TEST_CASE("Release reasons and ownership epochs survive the packet factory", "[hostorphan]")
{
    for (const auto reason : {OwnershipReleaseReason::Relinquish, OwnershipReleaseReason::DeclineGrant})
    {
        RequestOwnershipTransfer source;
        source.ServerId = 6;
        source.OwnershipEpoch = 19;
        source.Reason = reason;
        source.WorldSpaceId = {1, 0x3C};
        source.CellId = {1, 0x123};
        Buffer buffer(8192);
        Buffer::Writer writer(&buffer);
        source.Serialize(writer);
        Buffer::Reader reader(&buffer);
        auto decoded = ClientMessageFactory{}.Extract(reader);
        REQUIRE(decoded);
        REQUIRE(decoded->GetOpcode() == RequestOwnershipTransfer::Opcode);
        REQUIRE(static_cast<const RequestOwnershipTransfer&>(*decoded) == source);
    }
}

TEST_CASE("Survivors observe both launch barriers in order", "[hostorphan]")
{
    REQUIRE(OwnershipPolicy::AfterMemberLeft(1, true, false, false) == 1);
    REQUIRE(OwnershipPolicy::AfterMemberLeft(1, true, true, false) == 2);
    REQUIRE(OwnershipPolicy::AfterMemberLeft(1, true, true, true) == 2);
    REQUIRE(OwnershipPolicy::AfterMemberLeft(2, true, true, true) == 3);
    REQUIRE(OwnershipPolicy::AfterMemberLeft(3, true, false, false) == 3);
    REQUIRE(OwnershipPolicy::AfterMemberLeft(1, false, true, true) == 1);
}
