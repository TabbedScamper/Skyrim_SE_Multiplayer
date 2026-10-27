#include <TiltedCore/Stl.hpp>
#include <TiltedCore/Allocator.hpp>
#include <TiltedCore/Buffer.hpp>
#include <TiltedCore/Serialization.hpp>
#include <TiltedCore/ViewBuffer.hpp>
#include <catch2/catch.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <Messages/CorpseRagdollRequest.h>
#include <Messages/NotifyCorpseRagdoll.h>
#include <Structs/Movement.h>
#include <limits>

using namespace TiltedPhoques;
namespace
{
template <class T> T RoundTrip(const T& source, size_t trim = 0)
{
    Buffer buffer(8192);
    Buffer::Writer writer(&buffer);
    source.SerializeRaw(writer);
    ViewBuffer view(buffer.GetWriteData(), writer.Size() - trim);
    Buffer::Reader reader(&view);
    T result;
    result.DeserializeRaw(reader);
    return result;
}
CorpseRagdollRequest Sample()
{
    CorpseRagdollRequest result;
    result.ServerId = 17; result.Tick = 456; result.Dying = true; result.KnockState = 2;
    result.Origin[0] = 50000.f; result.Origin[1] = -70000.f;
    result.Bodies.resize(64);
    for (size_t i = 0; i < result.Bodies.size(); ++i)
    {
        auto& b = result.Bodies[i];
        b.Position[0] = float(i); b.LinearVelocity[0] = -2.5f;
        b.AngularVelocity[2] = 1.75f;
        const uint8_t types[]{1, 2, 3, 4, 5, 6};
        b.MotionType = types[i % 6];
    }
    return result;
}
}
TEST_CASE("Full corpse state survives owner and server relay with all native motions", "[corpse_ragdoll]")
{
    const auto source = Sample();
    const auto request = RoundTrip(source);
    REQUIRE(request.IsValid());
    REQUIRE(request == source);
    NotifyCorpseRagdoll relay;
    relay.ServerId = request.ServerId; relay.Tick = request.Tick;
    relay.Dying = request.Dying; relay.KnockState = request.KnockState; relay.Bodies = request.Bodies;
    std::copy_n(request.Origin, 3, relay.Origin);
    const auto received = RoundTrip(relay);
    REQUIRE(received.IsValid());
    REQUIRE(received == relay);
}
TEST_CASE("Partial corpse arrays never authorize partial constrained playback", "[corpse_ragdoll]")
{
    const auto source = Sample();
    Buffer buffer(8192); Buffer::Writer writer(&buffer); source.SerializeRaw(writer);
    for (size_t size = 0; size + 1 < writer.Size(); ++size)
    {
        ViewBuffer view(buffer.GetWriteData(), size); Buffer::Reader reader(&view);
        CorpseRagdollRequest result; result.DeserializeRaw(reader);
        REQUIRE_FALSE(result.IsValid());
    }
    auto tooMany = source; tooMany.Bodies.resize(65);
    REQUIRE_FALSE(RoundTrip(tooMany).IsValid());
}
TEST_CASE("Nonfinite velocity and unsupported motion cannot enter steering", "[corpse_ragdoll]")
{
    for (const auto value : {std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN(), 10000.f})
    {
        auto source = Sample(); source.Bodies[7].LinearVelocity[1] = value;
        REQUIRE_FALSE(RoundTrip(source).IsValid());
        source = Sample(); source.Bodies[7].AngularVelocity[1] = value;
        REQUIRE_FALSE(RoundTrip(source).IsValid());
    }
    for (const uint8_t type : {0, 7, 8, 255})
    {
        auto source = Sample(); source.Bodies[7].MotionType = type;
        REQUIRE_FALSE(RoundTrip(source).IsValid());
    }
}
TEST_CASE("Settled heartbeats terminal states and reliable heads retain protocol semantics", "[corpse_ragdoll]")
{
    auto source = Sample(); source.Settled = true;
    REQUIRE(RoundTrip(source) == source);
    source.Limb = 1; source.DismemberTick = 400; source.Bodies.resize(1);
    REQUIRE(RoundTrip(source).IsValid());
    REQUIRE(RoundTrip(source) == source);
    source.DismemberTick = source.Tick + 1;
    REQUIRE_FALSE(RoundTrip(source).IsValid());
    source.DismemberTick = 400; source.Active = false; source.Bodies.clear();
    REQUIRE(RoundTrip(source).IsValid());
    REQUIRE(RoundTrip(source) == source);
    source.Bodies.resize(1);
    REQUIRE_FALSE(RoundTrip(source).IsValid());
}

TEST_CASE("Owner get-up and NPC combat states survive the wire", "[corpse_ragdoll][combat_copy]")
{
    for (uint8_t state = 0; state < 8; ++state)
    {
        auto request = Sample();
        request.Active = false;
        request.Bodies.clear();
        request.KnockState = state;
        REQUIRE(RoundTrip(request) == request);

        Movement movement{};
        movement.LifeState = state;
        movement.Essential = (state & 1) != 0;
        movement.Fleeing = (state & 2) != 0;
        Buffer buffer(4096);
        Buffer::Writer writer(&buffer);
        movement.Serialize(writer);
        ViewBuffer view(buffer.GetWriteData(), writer.Size());
        Buffer::Reader reader(&view);
        Movement received{};
        received.Deserialize(reader);
        REQUIRE(received == movement);
    }
}
