#include <TiltedCore/Stl.hpp>
#include <TiltedCore/Allocator.hpp>
#include <TiltedCore/Buffer.hpp>
#include <TiltedCore/Serialization.hpp>
#include <TiltedCore/ViewBuffer.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <glm/gtc/quaternion.hpp>
#include <Messages/ClientMessageFactory.h>
#include <Messages/ServerMessageFactory.h>
#include <Structs/CheckedRead.h>
#include <catch2/catch.hpp>
#include <bit>
#include <limits>

using namespace TiltedPhoques;

namespace
{
template <class T> auto Extract(Buffer& aBuffer, size_t aSize)
{
    ViewBuffer view(aBuffer.GetWriteData(), aSize);
    Buffer::Reader reader(&view);
    if constexpr (std::is_base_of_v<ClientMessage, T>)
        return ClientMessageFactory{}.Extract(reader);
    else
        return ServerMessageFactory{}.Extract(reader);
}

template <class T> void WriteOpcode(Buffer::Writer& aWriter)
{
    aWriter.WriteBits(T::Opcode, sizeof(T::Opcode) * 8);
}

template <class T> void WritePhysicsHeader(Buffer::Writer& aWriter, uint64_t aCount)
{
    WriteOpcode<T>(aWriter);
    Serialization::WriteVarInt(aWriter, 1);
    if constexpr (std::is_same_v<T, NotifyPhysicsReferencesMove>)
        Serialization::WriteVarInt(aWriter, 1);
    Serialization::WriteVarInt(aWriter, aCount);
}
}

TEMPLATE_TEST_CASE("Movement factories reject malformed poses", "[encoding.rejection]",
    ClientReferencesMoveRequest, ServerReferencesMoveRequest)
{
    Buffer buffer(4096);
    Buffer::Writer writer(&buffer);
    WriteOpcode<TestType>(writer);
    Serialization::WriteVarInt(writer, 1);
    Serialization::WriteVarInt(writer, 1);
    Serialization::WriteVarInt(writer, 1);
    Movement{}.Serialize(writer);
    Serialization::WriteVarInt(writer, 0);
    Serialization::WriteVarInt(writer, 0);

    const bool visual = GENERATE(false, true);
    if (visual)
        writer.WriteBits(0, 8);

    SECTION("Oversized bone count")
    {
        writer.WriteBits(129, 8);
    }
    SECTION("Invalid transform")
    {
        writer.WriteBits(1, 8);
        writer.WriteBits(1, 64);
        writer.WriteBits(1, 64);
        if (visual)
        {
            writer.WriteBits(1, 1);
            writer.WriteBits(std::bit_cast<uint32_t>(std::numeric_limits<float>::infinity()), 32);
            for (int i = 0; i < 12; ++i)
                writer.WriteBits(0, 32);
            writer.WriteBits(0, 1);
        }
        else
        {
            writer.WriteBits(0, 1);
            writer.WriteBits(std::bit_cast<uint32_t>(std::numeric_limits<float>::quiet_NaN()), 32);
            writer.WriteBits(0, 64);
            writer.WriteBits(3, 2);
            for (int i = 0; i < 3; ++i)
                writer.WriteBits(16384, 15);
            writer.WriteBits(1, 1);
            writer.WriteBits(0, 8);
        }
    }
    SECTION("Truncated pose body")
    {
        writer.WriteBits(1, 8);
        writer.WriteBits(1, 64);
    }
    REQUIRE_FALSE(Extract<TestType>(buffer, writer.Size()));
}

TEMPLATE_TEST_CASE("Movement factories reject every truncated prefix", "[encoding.rejection]",
    ClientReferencesMoveRequest, ServerReferencesMoveRequest)
{
    TestType message;
    auto& update = message.Updates[1];
    update.EvaluatedPose.Bones.resize(1);
    update.EvaluatedPose.Bones[0].Rotation = {0.f, 0.f, 0.f, 1.f};
    update.EvaluatedPose.Bones[0].Scale = {1.f, 1.f, 1.f};
    update.VisualBones.Bones.resize(1);
    Buffer buffer(4096);
    Buffer::Writer writer(&buffer);
    message.Serialize(writer);
    REQUIRE(Extract<TestType>(buffer, writer.Size()));
    for (size_t size = 0; size < writer.Size(); ++size)
    {
        INFO(size);
        REQUIRE_FALSE(Extract<TestType>(buffer, size));
    }
}

TEMPLATE_TEST_CASE("Physics factories bound counts before allocation", "[encoding.rejection]",
    PhysicsReferencesMoveRequest, NotifyPhysicsReferencesMove)
{
    Buffer buffer(4096);
    Buffer::Writer writer(&buffer);
    SECTION("Protocol maximum")
    {
        WritePhysicsHeader<TestType>(writer, PhysicsReferenceUpdate::MaxUpdates + 1);
    }
    SECTION("Maximum varint")
    {
        WritePhysicsHeader<TestType>(writer, std::numeric_limits<uint64_t>::max());
    }
    SECTION("Missing records")
    {
        WritePhysicsHeader<TestType>(writer, 1);
    }
    SECTION("Oversized child list")
    {
        WritePhysicsHeader<TestType>(writer, 1);
        GameId{}.Serialize(writer);
        for (int i = 0; i < 6; ++i)
            writer.WriteBits(0, 32);
        writer.WriteBits(3, 8);
        for (int i = 0; i < 19; ++i)
            writer.WriteBits(0, 32);
        writer.WriteBits(PhysicsReferenceUpdate::kMaxChildBodies + 1, 8);
        for (size_t i = 0; i < (PhysicsReferenceUpdate::kMaxChildBodies + 1) * 7; ++i)
            writer.WriteBits(0, 32);
    }
    REQUIRE_FALSE(Extract<TestType>(buffer, writer.Size()));
}

TEMPLATE_TEST_CASE("Physics factories reject truncated records", "[encoding.rejection]",
    PhysicsReferencesMoveRequest, NotifyPhysicsReferencesMove)
{
    TestType message;
    message.Updates.resize(1);
    message.Updates[0].MotionType = 3;
    message.Updates[0].ChildBodies.resize(PhysicsReferenceUpdate::kMaxChildBodies);
    Buffer buffer(4096);
    Buffer::Writer writer(&buffer);
    message.Serialize(writer);
    REQUIRE(Extract<TestType>(buffer, writer.Size()));
    for (size_t size = 0; size < writer.Size(); ++size)
    {
        INFO(size);
        REQUIRE_FALSE(Extract<TestType>(buffer, size));
    }
}

TEST_CASE("Mod schemas and counts are bounded", "[encoding.rejection]")
{
    Buffer buffer(4096);
    Buffer::Writer writer(&buffer);
    SECTION("Unsupported schema")
    {
        writer.WriteBits(Mods::CurrentSchemaVersion + 1, 8);
    }
    SECTION("Too many mods")
    {
        writer.WriteBits(Mods::CurrentSchemaVersion, 8);
        writer.WriteBits(Mods::MaxMods + 1, 13);
    }
    SECTION("Missing mods")
    {
        writer.WriteBits(Mods::CurrentSchemaVersion, 8);
        writer.WriteBits(1, 13);
    }
    ViewBuffer view(buffer.GetWriteData(), writer.Size());
    Buffer::Reader reader(&view);
    Mods mods;
    REQUIRE_FALSE(mods.Deserialize(reader));
    REQUIRE(mods.ModList.empty());
}

TEST_CASE("Mods accept the writer limit and reject truncated fingerprints", "[encoding.rejection]")
{
    Mods source;
    source.ModList.resize(Mods::MaxMods);
    source.ModList[0].HasFingerprint = true;
    Buffer buffer(65536);
    Buffer::Writer writer(&buffer);
    source.Serialize(writer);
    ViewBuffer view(buffer.GetWriteData(), writer.Size());
    Buffer::Reader reader(&view);
    Mods result;
    REQUIRE(result.Deserialize(reader));
    REQUIRE(result == source);

    source.ModList.resize(1);
    Buffer::Writer shortWriter(&buffer);
    source.Serialize(shortWriter);
    for (size_t size = 0; size < shortWriter.Size(); ++size)
    {
        INFO(size);
        ViewBuffer truncated(buffer.GetWriteData(), size);
        Buffer::Reader truncatedReader(&truncated);
        REQUIRE_FALSE(result.Deserialize(truncatedReader));
    }
}

TEMPLATE_TEST_CASE("Authentication factories reject unsupported mod schemas", "[encoding.rejection]",
    AuthenticationRequest, AuthenticationResponse)
{
    TestType message;
    message.UserMods.SchemaVersion = Mods::CurrentSchemaVersion + 1;
    Buffer buffer(4096);
    Buffer::Writer writer(&buffer);
    message.Serialize(writer);
    REQUIRE_FALSE(Extract<TestType>(buffer, writer.Size()));
}

TEST_CASE("Checked varints reject overflow and missing continuation bytes", "[encoding.rejection]")
{
    Buffer buffer(16);
    Buffer::Writer writer(&buffer);
    const auto count = GENERATE(1, 10);
    for (int i = 0; i < count; ++i)
        writer.WriteBits(255, 8);
    ViewBuffer view(buffer.GetWriteData(), writer.Size());
    Buffer::Reader reader(&view);
    REQUIRE_THROWS(CheckedRead::VarInt(reader));
}
