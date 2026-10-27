#include <TiltedCore/Stl.hpp>
#include <TiltedCore/Allocator.hpp>
#include <TiltedCore/Buffer.hpp>
#include <TiltedCore/Serialization.hpp>
#include <TiltedCore/ViewBuffer.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <catch2/catch.hpp>
#include <Messages/ClientMessageFactory.h>
#include <Messages/ServerMessageFactory.h>
#include <Messages/NotifyRevive.h>
#include <Messages/ReviveRequest.h>

using namespace TiltedPhoques;

// 2026-09-28: the server's forwarded Hold/Cancel notices (who is reviving the downed player) decoded as invalid and
// were dropped, so the downed player never saw the revive meter. The bleedout meter rides in every revive state.
TEST_CASE("Revive notices carry holds, cancels and the bleedout meter", "[revive]")
{
    for (const auto action : {ReviveAction::State, ReviveAction::Hold, ReviveAction::Cancel, ReviveAction::Grant,
             ReviveAction::Raise})
    {
        NotifyRevive notice;
        notice.Action = action;
        notice.Epoch = 7;
        notice.Revision = 3;
        notice.PlayerId = 2;
        notice.ReviverId = 1;
        notice.Down = true;
        notice.Alive = true;
        notice.InCombat = action == ReviveAction::Hold;
        notice.Bleed = 0.625f;
        notice.Dead = action == ReviveAction::State;
        Buffer buffer(256);
        Buffer::Writer writer(&buffer);
        notice.Serialize(writer);
        Buffer::Reader reader(&buffer);
        const ServerMessageFactory factory;
        auto decoded = factory.Extract(reader);
        REQUIRE(decoded);
        auto& received = static_cast<NotifyRevive&>(*decoded);
        REQUIRE(received.IsValid());
        REQUIRE(received.Action == action);
        REQUIRE(received.ReviverId == 1);
        REQUIRE(std::abs(received.Bleed - 0.625f) < 1.f / 65535.f);
        REQUIRE(received.Dead == (action == ReviveAction::State));
    }

    ReviveRequest request;
    request.Action = ReviveAction::State;
    request.Down = true;
    request.Bleed = 0.f;
    Buffer buffer(256);
    Buffer::Writer writer(&buffer);
    request.Serialize(writer);
    Buffer::Reader reader(&buffer);
    const ClientMessageFactory factory;
    auto decoded = factory.Extract(reader);
    REQUIRE(decoded);
    REQUIRE(static_cast<ReviveRequest&>(*decoded).Bleed == 0.f);
}

// 2026-09-28: a fallen player is called back by an ally (Raise); the fallen flag rides every state so a reloaded
// or reconnected player comes back fallen.
TEST_CASE("Revive requests carry the fallen flag and the raise action", "[revive]")
{
    ReviveRequest request;
    request.Action = ReviveAction::Raise;
    request.Epoch = 3;
    request.Revision = 9;
    request.PlayerId = 4;
    request.Dead = true;
    Buffer buffer(256);
    Buffer::Writer writer(&buffer);
    request.Serialize(writer);
    Buffer::Reader reader(&buffer);
    const ClientMessageFactory factory;
    auto decoded = factory.Extract(reader);
    REQUIRE(decoded);
    auto& received = static_cast<ReviveRequest&>(*decoded);
    REQUIRE(received.IsValid());
    REQUIRE(received.Action == ReviveAction::Raise);
    REQUIRE(received.PlayerId == 4);
    REQUIRE(received.Dead);
}
