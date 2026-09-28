#include <TiltedCore/Stl.hpp>
#include <TiltedCore/Allocator.hpp>
#include <TiltedCore/Buffer.hpp>
#include <TiltedCore/Serialization.hpp>
#include <TiltedCore/ViewBuffer.hpp>
#include <catch2/catch.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <Messages/HarnessBarrier.h>
#include <Messages/ClientMessageFactory.h>
#include <Messages/ServerMessageFactory.h>

TEST_CASE("Harness factories preserve every operation and reject every truncated prefix", "[harness]")
{
    using namespace TiltedPhoques;
    for (auto op : {HarnessOp::Step, HarnessOp::Done, HarnessOp::Barrier, HarnessOp::Abort, HarnessOp::Finish, HarnessOp::Prepared, HarnessOp::Execute})
    {
        RequestHarness request;
        request.Epoch = 17; request.Run = 1234567890123; request.Sequence = 4; request.Sender = 19;
        request.Op = op; request.Payload = "{\"op\":\"capture\",\"tag\":\"roundtrip\"}";
        Buffer buffer(512); Buffer::Writer writer(&buffer); request.ClientMessage::Serialize(writer);
        ViewBuffer view(buffer.GetWriteData(), writer.Size()); Buffer::Reader reader(&view);
        auto decoded = ClientMessageFactory{}.Extract(reader);
        REQUIRE(decoded); REQUIRE(decoded->GetOpcode() == RequestHarness::Opcode);
        REQUIRE(static_cast<RequestHarness&>(*decoded) == request);
        for (size_t size = 0; size < writer.Size(); ++size)
        {
            ViewBuffer truncated(buffer.GetWriteData(), size); Buffer::Reader in(&truncated);
            REQUIRE_FALSE(ClientMessageFactory{}.Extract(in));
        }
        NotifyHarness notify; static_cast<HarnessData&>(notify) = request;
        Buffer output(512); Buffer::Writer out(&output); notify.ServerMessage::Serialize(out);
        ViewBuffer returned(output.GetWriteData(), out.Size()); Buffer::Reader input(&returned);
        auto received = ServerMessageFactory{}.Extract(input);
        REQUIRE(received); REQUIRE(received->GetOpcode() == NotifyHarness::Opcode);
        REQUIRE(static_cast<NotifyHarness&>(*received) == notify);
    }
}

TEST_CASE("Harness completion requires all five original participants", "[harness]")
{
    HarnessBarrier b;
    const std::set<uint32_t> members{1, 2, 3, 4, 5};
    REQUIRE(b.Begin(9, 100, 1, members));
    HarnessData d; d.Epoch = 9; d.Run = 100; d.Sequence = 1;
    REQUIRE_FALSE(b.Step(d, 2));
    REQUIRE(b.Step(d, 1));
    REQUIRE_FALSE(b.Step(d, 1)); // No duplicate tcl execution.
    REQUIRE_FALSE(b.Ack(d, 1)); // Completion cannot bypass preconditions.
    for (uint32_t id : {1, 2, 3, 4}) REQUIRE(b.Prepare(d, id));
    REQUIRE_FALSE(b.CanExecute());
    REQUIRE_FALSE(b.Ack(d, 1));
    REQUIRE_FALSE(b.Prepare(d, 4));
    REQUIRE_FALSE(b.Prepare(d, 6));
    REQUIRE(b.Prepare(d, 5));
    REQUIRE(b.CanExecute());
    for (uint32_t id : {1, 2, 3, 4}) REQUIRE(b.Ack(d, id));
    REQUIRE_FALSE(b.Ready());
    REQUIRE_FALSE(b.Ack(d, 4));
    REQUIRE_FALSE(b.Ack(d, 6));
    REQUIRE_FALSE(b.Current(9, 1, {1, 2, 3, 4}));
    REQUIRE_FALSE(b.Current(9, 2, members));
    REQUIRE_FALSE(b.Current(10, 1, members));
    REQUIRE(b.Ack(d, 5));
    REQUIRE(b.Ready());
    d.Sequence = 3; REQUIRE_FALSE(b.Step(d, 1));
    d.Sequence = 2; REQUIRE(b.Step(d, 1));
    REQUIRE_FALSE(b.CanExecute());
    REQUIRE_FALSE(b.Ready());
    d.Sequence = 1; REQUIRE_FALSE(b.Ack(d, 5));
    d.Sequence = 2; d.Run = 99; REQUIRE_FALSE(b.Ack(d, 5));
    d.Run = 100; d.Epoch = 8; REQUIRE_FALSE(b.Ack(d, 5));
    b.Aborted = true; d.Epoch = 9; REQUIRE_FALSE(b.Ack(d, 5));
}
TEST_CASE("Harness payloads reject invalid operations and oversized input", "[harness]")
{
    HarnessData d; d.Epoch = 1; d.Run = 1; d.Sequence = 1;
    REQUIRE_FALSE(d.Valid());
    d.Payload = "{\"op\":\"console\",\"cmd\":\"tcl\"}";
    REQUIRE(d.Valid());
    d.Payload.assign(4097, 'x'); REQUIRE_FALSE(d.Valid());
    d.Payload.assign(1, '\0'); REQUIRE_FALSE(d.Valid());
    d.Payload.clear(); d.Op = static_cast<HarnessOp>(255); REQUIRE_FALSE(d.Valid());
}
