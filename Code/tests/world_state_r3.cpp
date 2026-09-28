#include <TiltedCore/Stl.hpp>
#include <TiltedCore/Allocator.hpp>
#include <TiltedCore/Buffer.hpp>
#include <TiltedCore/Serialization.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <catch2/catch.hpp>
#include <Messages/WorldStateReplay.h>
#include <array>

namespace
{
WorldState Door(uint64_t sequence, float mode, bool open)
{
    WorldState state;
    state.Epoch = 17;
    state.Sequence = sequence;
    state.Reference = GameId{3, 0xD9610};
    state.Cell = GameId{3, 0x3C};
    state.Kind = WorldStateKind::Open;
    state.Scalar = mode;
    state.Value = open;
    return state;
}
}

TEST_CASE("Cache observations cannot supersede a pending live door command", "[encoding.world_state]")
{
    WorldStateReplay replay;
    auto live = Door(2, 0, true);
    REQUIRE(replay.Receive(live));
    REQUIRE(replay.Take(1).front() == live);
    REQUIRE(replay.Failed(live, 10));
    for (float mode : {1.f, 3.f})
        REQUIRE_FALSE(replay.Receive(Door(100, mode, false)));
    REQUIRE(replay.Pending() == 1);
    REQUIRE(replay.Take(1, 1009).empty());
    auto retry = replay.Take(1, 1010);
    REQUIRE(retry.size() == 1);
    REQUIRE(retry.front() == live);
    replay.Applied(retry.front());
    // A rejected high revision cannot poison a legitimate later snapshot.
    auto snapshot = Door(3, 2, false);
    REQUIRE(replay.Receive(snapshot));
    REQUIRE(replay.Take(1).front() == snapshot);
}

TEST_CASE("Door cache and live routing preserve four followers and late join", "[encoding.world_state]")
{
    WorldStateTable table;
    table.SetAuthority(17, 23);
    std::array<WorldStateReplay, 4> followers;
    const auto route = [&](WorldState state) {
        REQUIRE(table.Accept(23, state));
        if (WorldStateTable::ShouldDeliverLive(state))
            for (auto& follower : followers) REQUIRE(follower.Receive(state));
    };
    route(Door(1, 3, false)); // host cell baseline
    route(Door(2, 1, true)); // activation already has another live path
    for (auto& follower : followers) REQUIRE(follower.Take(64).empty());
    route(Door(3, 0, false)); // script command
    for (auto& follower : followers)
    {
        auto work = follower.Take(64);
        REQUIRE(work.size() == 1);
        REQUIRE(work.front().Scalar == 0);
        REQUIRE(work.front().Value == 0);
        follower.Applied(work.front());
    }
    route(Door(4, 3, true)); // baseline must not snap an existing door
    for (auto& follower : followers) REQUIRE(follower.Take(64).empty());
    auto snapshot = table.Snapshot(GameId{3, 0x3C});
    REQUIRE(snapshot.size() == 1);
    REQUIRE(snapshot.front().Sequence == 4);
    REQUIRE(snapshot.front().Value == 1);
    // Same conversion as server OnCell. A reattached/late client receives end state.
    snapshot.front().Scalar = 2;
    WorldStateReplay late;
    REQUIRE(late.Receive(snapshot.front()));
    REQUIRE(late.Take(1).front() == snapshot.front());
    late.Applied(snapshot.front());
    late.Attach(snapshot.front().Reference.LogFormat());
    REQUIRE(late.Take(1).front().Scalar == 2);
}

TEST_CASE("Unsupported end states remain pending across retries and reload", "[encoding.world_state]")
{
    for (auto kind : {WorldStateKind::DestructionHealth, WorldStateKind::FinishedSequence})
    {
        WorldStateReplay replay;
        auto state = Door(1, 0, false);
        state.Kind = kind;
        if (kind == WorldStateKind::FinishedSequence) state.Animation = "CompletedSequence";
        REQUIRE(state.Valid());
        REQUIRE(replay.Receive(state));
        REQUIRE(replay.Take(1).size() == 1);
        REQUIRE(replay.Failed(state, 10, true));
        REQUIRE(replay.Take(1, UINT64_MAX).empty());
        replay.EvictOutside({});
        REQUIRE(replay.Pending() == 1);
        replay.Attach(state.Reference.LogFormat());
        REQUIRE(replay.Take(1).front() == state);
        REQUIRE_FALSE(replay.Failed(state, 20, true));
        REQUIRE(replay.Pending() == 1);
    }
}
