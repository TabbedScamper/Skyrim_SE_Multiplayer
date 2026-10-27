#include <TiltedCore/Stl.hpp>
#include <TiltedCore/Allocator.hpp>
#include <TiltedCore/Buffer.hpp>
#include <TiltedCore/ViewBuffer.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <TiltedCore/Serialization.hpp>
#include <catch2/catch.hpp>

#include <Messages/RequestHealthChangeBroadcast.h>
#include <Messages/NotifyHealthChangeBroadcast.h>
#include <Messages/RequestDeathStateChange.h>
#include <Messages/NotifyDeathStateChange.h>
#include <Messages/PlayerCombatState.h>
#include <Messages/RequestPlayerCombatState.h>
#include <Messages/NotifyPlayerCombatState.h>
#include <Messages/ClientMessageFactory.h>
#include <Messages/ServerMessageFactory.h>

using namespace TiltedPhoques;

template <class T> void CheckCrimeRoundTrip(T aMessage)
{
    Buffer buffer(128);
    Buffer::Writer writer(&buffer);
    aMessage.SerializeRaw(writer);
    ViewBuffer view(buffer.GetWriteData(), writer.Size());
    Buffer::Reader reader(&view);
    T decoded;
    decoded.DeserializeRaw(reader);
    REQUIRE(decoded == aMessage);
}

template <class T> void CheckClientFactory(T aMessage)
{
    Buffer buffer(128);
    Buffer::Writer writer(&buffer);
    aMessage.Serialize(writer);
    ViewBuffer view(buffer.GetWriteData(), writer.Size());
    Buffer::Reader reader(&view);
    auto decoded = ClientMessageFactory{}.Extract(reader);
    REQUIRE(decoded);
    REQUIRE(static_cast<T&>(*decoded) == aMessage);
}

template <class T> void CheckServerFactory(T aMessage)
{
    Buffer buffer(128);
    Buffer::Writer writer(&buffer);
    aMessage.Serialize(writer);
    ViewBuffer view(buffer.GetWriteData(), writer.Size());
    Buffer::Reader reader(&view);
    auto decoded = ServerMessageFactory{}.Extract(reader);
    REQUIRE(decoded);
    REQUIRE(static_cast<T&>(*decoded) == aMessage);
}

TEST_CASE("Murder attribution survives health and death packets", "[encoding.crime_murder]")
{
    RequestHealthChangeBroadcast healthRequest;
    healthRequest.Id = 72;
    healthRequest.DeltaHealth = -47.f;
    healthRequest.KillerId = 91;
    CheckCrimeRoundTrip(healthRequest);
    CheckClientFactory(healthRequest);

    NotifyHealthChangeBroadcast healthNotify;
    healthNotify.Id = 72;
    healthNotify.DeltaHealth = -47.f;
    healthNotify.KillerId = 91;
    CheckCrimeRoundTrip(healthNotify);
    CheckServerFactory(healthNotify);

    RequestDeathStateChange deathRequest;
    deathRequest.Id = 72;
    deathRequest.OwnershipEpoch = 8;
    deathRequest.IsDead = true;
    deathRequest.KillerId = 91;
    CheckCrimeRoundTrip(deathRequest);
    CheckClientFactory(deathRequest);

    NotifyDeathStateChange deathNotify;
    deathNotify.Id = 72;
    deathNotify.OwnershipEpoch = 8;
    deathNotify.IsDead = true;
    deathNotify.KillerId = 91;
    CheckCrimeRoundTrip(deathNotify);
    CheckServerFactory(deathNotify);

    deathNotify.IsDead = false;
    deathNotify.KillerId = 0;
    CheckCrimeRoundTrip(deathNotify);
}

TEST_CASE("Charged crime carries witnesses and rejects oversized lists", "[encoding.crime_murder]")
{
    PlayerCombatState report;
    report.Type = PlayerCombatState::Crime;
    report.Epoch = 19;
    report.Sequence = 23;
    report.ActorId = 91;
    report.OwnershipEpoch = 8;
    report.CrimeId = 44;
    report.CrimeType = 0;
    report.CrimeGold = 5;
    report.CrimeFaction = GameId{2, 0x28170};
    report.Witnesses.push_back({72, 4});
    REQUIRE(report.IsValid());
    Buffer buffer(256);
    Buffer::Writer writer(&buffer);
    report.Serialize(writer);
    ViewBuffer view(buffer.GetWriteData(), writer.Size());
    Buffer::Reader reader(&view);
    PlayerCombatState decoded;
    REQUIRE(decoded.Deserialize(reader));
    REQUIRE(decoded == report);
    RequestPlayerCombatState request;
    request.State = report;
    Buffer requestBuffer(256);
    Buffer::Writer requestWriter(&requestBuffer);
    request.Serialize(requestWriter);
    ViewBuffer requestView(requestBuffer.GetWriteData(), requestWriter.Size());
    Buffer::Reader requestReader(&requestView);
    auto requestCopy = ClientMessageFactory{}.Extract(requestReader);
    REQUIRE(requestCopy);
    REQUIRE(static_cast<RequestPlayerCombatState&>(*requestCopy).State == report);

    NotifyPlayerCombatState notify;
    notify.State = report;
    Buffer notifyBuffer(256);
    Buffer::Writer notifyWriter(&notifyBuffer);
    notify.Serialize(notifyWriter);
    ViewBuffer notifyView(notifyBuffer.GetWriteData(), notifyWriter.Size());
    Buffer::Reader notifyReader(&notifyView);
    auto notifyCopy = ServerMessageFactory{}.Extract(notifyReader);
    REQUIRE(notifyCopy);
    REQUIRE(static_cast<NotifyPlayerCombatState&>(*notifyCopy).State == report);
    report.Witnesses.resize(PlayerCombatState::MaxWitnesses + 1);
    REQUIRE_FALSE(report.IsValid());
}
