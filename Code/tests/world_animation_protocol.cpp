#include <TiltedCore/Stl.hpp>
#include <TiltedCore/Allocator.hpp>
#include <TiltedCore/Buffer.hpp>
#include <TiltedCore/Serialization.hpp>
#include <TiltedCore/ViewBuffer.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <catch2/catch.hpp>
#include <Messages/WorldStateReplay.h>
#include "world_animation_fixture.h"
#include <array>

namespace
{
// Executed by the compiler even under /Zs. The runtime replay/factory cases
// below still need the coordinator's test executable.
consteval bool CheckAnimationGrammar()
{
    auto bytes = WorldAnimationFixture();
    if (!WorldAnimationData::Valid(bytes)) return false;
    bytes.pop_back();
    if (WorldAnimationData::Valid(bytes)) return false;
    bytes = WorldAnimationFixture(); bytes.push_back(0);
    if (WorldAnimationData::Valid(bytes)) return false;
    bytes = WorldAnimationFixture(); bytes[1] = 255;
    if (WorldAnimationData::Valid(bytes)) return false;
    bytes = WorldAnimationFixture(); bytes[28] = 255;
    if (WorldAnimationData::Valid(bytes)) return false;
    bytes.clear();
    return !WorldAnimationData::Valid(bytes);
}
static_assert(CheckAnimationGrammar());
consteval bool CheckVariableCompatibility()
{
    auto bytes = WorldAnimationFixture();
    // Insert one syntactically valid word variable before the terminator.
    WorldAnimationData::Reader reader{bytes};
    reader.Count(); reader.Skip(WorldAnimationData::String);
    reader.Array({WorldAnimationData::String, WorldAnimationData::Int32, WorldAnimationData::String,
                  WorldAnimationData::UInt32, WorldAnimationData::UInt32});
    reader.Array({WorldAnimationData::String, WorldAnimationData::Float});
    reader.Array({WorldAnimationData::String, WorldAnimationData::Float});
    const std::vector<uint8_t> variable{WorldAnimationData::String, 1, 0, 'v',
        WorldAnimationData::Bool, 1, WorldAnimationData::Int32, 0, 0, 0, 0};
    bytes.insert(bytes.begin() + reader.Position, variable.begin(), variable.end());
    if (!WorldAnimationData::Valid(bytes)) return false;
    if (!WorldAnimationData::Validate(bytes, [](uint32_t graph, const std::string& name, bool word) {
            return graph == 0 && name == "v" && word;
        })) return false;
    if (WorldAnimationData::Validate(bytes, [](uint32_t, const std::string&, bool word) { return !word; })) return false;
    return !WorldAnimationData::Validate(bytes, [](uint32_t, const std::string&, bool) { return false; });
}
static_assert(CheckVariableCompatibility());
WorldState Event(uint64_t seq, const char* event = "Open")
{
    WorldState state;
    state.Epoch = 17; state.Sequence = seq; state.Reference = GameId{3, 0x6CF54}; state.Cell = GameId{3, 0x3C};
    state.Kind = WorldStateKind::AnimationEvent; state.Animation = event;
    return state;
}
WorldState Snapshot(uint64_t seq, bool restore = false)
{
    auto state = Event(seq);
    state.Kind = WorldStateKind::AnimationSnapshot; state.Animation.clear();
    state.Value = 1; state.Scalar = restore ? 2.f : 0.f; state.AnimationData = WorldAnimationFixture();
    return state;
}
}
TEST_CASE("World animation accepts only complete bounded native value grammars", "[encoding.world_state][world_animation]")
{
    const auto valid = WorldAnimationFixture();
    REQUIRE(WorldAnimationData::Valid(valid));
    for (size_t size = 0; size < valid.size(); ++size)
        REQUIRE_FALSE(WorldAnimationData::Valid(std::vector<uint8_t>(valid.begin(), valid.begin() + size)));
    auto bytes = valid;
    bytes.push_back(0);
    REQUIRE_FALSE(WorldAnimationData::Valid(bytes));
    bytes = valid; bytes[1] = 255; // graph count
    REQUIRE_FALSE(WorldAnimationData::Valid(bytes));
    bytes = valid; bytes[28] = 255; // malformed typed stream
    REQUIRE_FALSE(WorldAnimationData::Valid(bytes));
    bytes.assign(WorldAnimationData::MaximumBytes + 1, 0);
    REQUIRE_FALSE(WorldAnimationData::Valid(bytes));
    auto event = Event(1); event.AnimationData = valid;
    REQUIRE_FALSE(event.Valid());
    auto snapshot = Snapshot(1); snapshot.Value = 2;
    REQUIRE_FALSE(snapshot.Valid());
}
TEST_CASE("World animation inputs cannot coalesce or overtake a failed head", "[encoding.world_state][world_animation]")
{
    WorldStateReplay replay;
    REQUIRE(replay.Receive(Event(1)));
    REQUIRE(replay.Receive(Event(2, "Reset")));
    REQUIRE(replay.Receive(Event(3)));
    REQUIRE_FALSE(replay.Receive(Event(3)));
    auto first = replay.Take(64);
    REQUIRE(first.size() == 1); REQUIRE(first.front().Sequence == 1);
    REQUIRE(replay.Failed(first.front(), 10));
    REQUIRE(replay.Take(64, 1009).empty());
    auto retry = replay.Take(64, 1010);
    REQUIRE(retry.size() == 1); REQUIRE(retry.front().Sequence == 1);
    replay.Applied(retry.front());
    for (uint64_t seq : {2, 3})
    {
        auto work = replay.Take(64, 1010);
        REQUIRE(work.size() == 1); REQUIRE(work.front().Sequence == seq);
        replay.Applied(work.front());
    }
    REQUIRE(replay.Take(64, 1010).empty());
}
TEST_CASE("World animation authority and checkpoints work for five and ten players", "[encoding.world_state][world_animation]")
{
    for (size_t players : {5, 10})
    {
        WorldStateTable table;
        table.SetAuthority(17, 23);
        auto forged = Event(1);
        REQUIRE_FALSE(table.Accept(24, forged));
        std::vector<WorldStateReplay> followers(players - 1);
        for (auto state : {Event(1), Event(2, "Reset"), Event(3), Snapshot(4)})
        {
            REQUIRE(state.Valid()); REQUIRE(table.Accept(23, state));
            for (auto& follower : followers) REQUIRE(follower.Receive(state));
        }
        for (auto& follower : followers)
            for (uint64_t seq : {1, 2, 3})
            {
                auto work = follower.Take(64);
                REQUIRE(work.size() == 1); REQUIRE(work.front().Sequence == seq);
                follower.Applied(work.front());
            }
        auto late = table.Snapshot(GameId{3, 0x3C});
        REQUIRE(late.size() == 1); REQUIRE(late.front().Kind == WorldStateKind::AnimationSnapshot);
        late.front().Scalar = 2;
        WorldStateReplay joiner;
        REQUIRE(joiner.Receive(late.front()));
        auto work = joiner.Take(1);
        REQUIRE(work.size() == 1); REQUIRE(work.front().Sequence == 4);
        joiner.Applied(work.front());
        REQUIRE(joiner.Take(64).empty()); // no replayed Open/Begin on late join
        for (auto& follower : followers)
        {
            follower.Attach(Event(1).Reference.LogFormat());
            work = follower.Take(1);
            REQUIRE(work.size() == 1); REQUIRE(work.front().Kind == WorldStateKind::AnimationSnapshot);
            REQUIRE(work.front().Scalar == 2); follower.Applied(work.front());
        }
        table.SetAuthority(17, 24);
        auto newEvent = Event(1, "Reset");
        REQUIRE_FALSE(table.Accept(23, newEvent));
        REQUIRE(table.Accept(24, newEvent)); REQUIRE(newEvent.Sequence > 4);
        table.SetAuthority(18, 24);
        REQUIRE(table.Snapshot(GameId{3, 0x3C}).empty());
        REQUIRE_FALSE(table.Accept(24, newEvent));
    }
}
TEST_CASE("World animation restores checkpoint before newer inputs after reload", "[encoding.world_state][world_animation]")
{
    WorldStateReplay replay;
    REQUIRE(replay.Receive(Event(1)));
    REQUIRE(replay.Receive(Snapshot(2)));
    REQUIRE(replay.Receive(Event(3, "Reset")));
    replay.Attach(Event(1).Reference.LogFormat());
    auto work = replay.Take(64);
    REQUIRE(work.size() == 1); REQUIRE(work.front().Sequence == 2);
    replay.Applied(work.front());
    work = replay.Take(64);
    REQUIRE(work.size() == 1); REQUIRE(work.front().Sequence == 3);
    replay.Applied(work.front());
    REQUIRE_FALSE(replay.Receive(Snapshot(1, true)));
    replay.Clear(); REQUIRE(replay.Take(64).empty());
}
TEST_CASE("A checkpoint arriving after an empty cell replay still restores a late join", "[encoding.world_state][world_animation]")
{
    WorldStateReplay replay;
    replay.Attach(Event(1).Reference.LogFormat());
    REQUIRE(replay.Receive(Snapshot(4))); // host baseline completes after OnCell
    auto work = replay.Take(64);
    REQUIRE(work.size() == 1); REQUIRE(work.front().Kind == WorldStateKind::AnimationSnapshot);
    replay.Applied(work.front());
    REQUIRE(replay.Take(64).empty());
}
TEST_CASE("Live checkpoint cannot skip pending inputs and precedes newer input", "[encoding.world_state][world_animation]")
{
    WorldStateReplay replay;
    REQUIRE(replay.Receive(Event(1)));
    REQUIRE(replay.Receive(Snapshot(2)));
    REQUIRE(replay.Receive(Event(3, "Reset")));
    for (uint64_t sequence : {1, 2, 3})
    {
        auto work = replay.Take(64);
        REQUIRE(work.size() == 1); REQUIRE(work.front().Sequence == sequence);
        replay.Applied(work.front());
    }
    REQUIRE(replay.Take(64).empty());
}
TEST_CASE("World animation snapshots round trip and reject every truncation", "[encoding.world_state][world_animation]")
{
    using namespace TiltedPhoques;
    const auto state = Snapshot(7, true);
    Buffer buffer(65536); Buffer::Writer writer(&buffer); state.Serialize(writer);
    for (size_t size = 0; size <= writer.Size(); ++size)
    {
        ViewBuffer view(buffer.GetWriteData(), size); Buffer::Reader reader(&view);
        WorldState decoded;
        if (size == writer.Size()) { REQUIRE(decoded.Deserialize(reader)); REQUIRE(decoded == state); }
        else REQUIRE_FALSE(decoded.Deserialize(reader));
    }
}

TEST_CASE("World animation history overflow requires a covering checkpoint", "[encoding.world_state][world_animation]")
{
    WorldAnimationReplay replay;
    const auto id = Event(1).Reference.LogFormat();
    for (uint64_t i = 1; i <= WorldAnimationReplay::MaximumHistory; ++i) REQUIRE(replay.Receive(Event(i)));
    REQUIRE_FALSE(replay.Receive(Event(257)));
    REQUIRE(replay.NeedsCheckpoint(id)); REQUIRE(replay.HistorySize(id) == 0);
    REQUIRE(replay.Take(8, 10000).empty());
    replay.Attach(id); REQUIRE(replay.Take(8, 10000).empty());
    REQUIRE_FALSE(replay.Receive(Event(258)));
    REQUIRE_FALSE(replay.Receive(Snapshot(257)));
    REQUIRE(replay.Receive(Snapshot(259)));
    REQUIRE_FALSE(replay.NeedsCheckpoint(id));
    REQUIRE(replay.Receive(Event(260, "Reset")));
    auto work = replay.Take(8, 0);
    REQUIRE(work.size() == 1); REQUIRE(work.front().Sequence == 259); REQUIRE(work.front().Scalar == 2);
    replay.Applied(work.front());
    work = replay.Take(8, 0); REQUIRE(work.size() == 1); REQUIRE(work.front().Sequence == 260);
    replay.Applied(work.front()); REQUIRE(replay.Take(8, 0).empty());
}
TEST_CASE("A retained snapshot cannot reject a gap covering checkpoint", "[encoding.world_state][world_animation]")
{
    WorldAnimationReplay replay;
    const auto id = Event(1).Reference.LogFormat();
    REQUIRE(replay.Receive(Snapshot(500)));
    // No event <= the retained snapshot can establish or advance a gap.
    REQUIRE_FALSE(replay.Receive(Event(499)));
    REQUIRE_FALSE(replay.Receive(Event(500)));
    REQUIRE_FALSE(replay.NeedsCheckpoint(id));
    for (uint64_t seq = 501; seq <= 756; ++seq) REQUIRE(replay.Receive(Event(seq)));
    REQUIRE_FALSE(replay.Receive(Event(757)));
    REQUIRE(replay.NeedsCheckpoint(id));
    REQUIRE_FALSE(replay.Receive(Snapshot(499)));
    REQUIRE_FALSE(replay.Receive(Snapshot(756)));
    REQUIRE_FALSE(replay.Receive(Event(500)));
    REQUIRE(replay.NeedsCheckpoint(id));
    REQUIRE(replay.Receive(Snapshot(757)));
    REQUIRE_FALSE(replay.NeedsCheckpoint(id));
    const auto work = replay.Take(8, 0);
    REQUIRE(work.size() == 1); REQUIRE(work.front().Sequence == 757); REQUIRE(work.front().Scalar == 2);
}
TEST_CASE("Animation scope eviction and reconnect discard stale work", "[encoding.world_state][world_animation]")
{
    WorldAnimationReplay replay;
    const auto id = Event(1).Reference.LogFormat();
    REQUIRE(replay.Receive(Event(1)));
    replay.EvictOutside({0x3C}); REQUIRE(replay.HistorySize(id) == 1);
    replay.EvictOutside({0x5DE24}); REQUIRE(replay.HistorySize(id) == 0);
    REQUIRE(replay.Take(8, 0).empty());
    REQUIRE(replay.Receive(Event(50)));
    replay.Clear(); // same epoch reconnect must accept a new sequence origin
    REQUIRE(replay.Receive(Event(1, "Reset")));
    auto work = replay.Take(8, 0); REQUIRE(work.size() == 1); REQUIRE(work.front().Sequence == 1);
    REQUIRE(work.front().Animation == "Reset");
}
TEST_CASE("World state mailbox drains preserve ordering and byte budgets", "[encoding.world_state][world_animation]")
{
    std::deque<WorldState> source{Event(1), Snapshot(2), Event(3)}, output;
    WorldStateReplay::Drain(source, output, 64, sizeof(WorldState) + source.front().Animation.size());
    REQUIRE(output.size() == 1); REQUIRE(output.front().Sequence == 1); REQUIRE(source.size() == 2);
    output.clear(); WorldStateReplay::Drain(source, output, 1);
    REQUIRE(output.size() == 1); REQUIRE(output.front().Sequence == 2); REQUIRE(source.front().Sequence == 3);
    output.clear(); WorldStateReplay::Drain(source, output);
    REQUIRE(output.size() == 1); REQUIRE(output.front().Sequence == 3); REQUIRE(source.empty());
}
TEST_CASE("World state bursts and failed sends retain every input in FIFO order", "[encoding.world_state][world_animation]")
{
    std::deque<WorldState> queue, sending;
    // Cross both withdrawn admission limits (256 messages / 4096 inputs).
    for (uint64_t seq = 1; seq <= 4097; ++seq) queue.push_back(Event(seq));
    WorldStateReplay::Drain(queue, sending);
    REQUIRE(sending.size() == 64); REQUIRE(queue.size() == 4033);
    // Half of the batch succeeded while native publication was concurrent.
    for (unsigned i = 0; i < 32; ++i) sending.pop_front();
    for (uint64_t seq = 4098; seq <= 4129; ++seq) queue.push_back(Event(seq));
    WorldStateReplay::PrependFailed(queue, sending);
    REQUIRE(sending.empty()); REQUIRE(queue.size() == 4097);
    uint64_t expected = 33;
    while (!queue.empty())
    {
        WorldStateReplay::Drain(queue, sending);
        REQUIRE_FALSE(sending.empty()); REQUIRE(sending.size() <= 64);
        for (const auto& state : sending) REQUIRE(state.Sequence == expected++);
        sending.clear();
    }
    REQUIRE(expected == 4130);
}
TEST_CASE("World state scope pages equal the complete sorted snapshot", "[encoding.world_state][world_animation]")
{
    WorldStateTable table;
    table.SetAuthority(17, 23);
    for (uint32_t ref = 1; ref <= 13; ++ref)
        for (auto state : {Event(1), Snapshot(2)})
        {
            state.Reference.BaseId = ref; REQUIRE(table.Accept(23, state));
        }
    std::vector<WorldState> collected;
    uint64_t cursor{}; bool done{};
    for (unsigned page = 0; page < 4; ++page)
    {
        auto states = table.SnapshotPage(Event(1).Cell, cursor, done);
        REQUIRE(states.size() <= 4);
        collected.insert(collected.end(), states.begin(), states.end());
        REQUIRE(done == (page == 3));
    }
    REQUIRE(collected == table.Snapshot(Event(1).Cell));
    REQUIRE(table.SnapshotPage(Event(1).Cell, cursor, done).empty()); REQUIRE(done);
    table.SetAuthority(18, 24); cursor = 0;
    REQUIRE(table.SnapshotPage(Event(1).Cell, cursor, done).empty()); REQUIRE(done);
}
