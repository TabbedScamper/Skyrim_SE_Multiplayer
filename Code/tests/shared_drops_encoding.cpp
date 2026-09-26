#include <TiltedCore/Stl.hpp>
#include <TiltedCore/Allocator.hpp>
#include <TiltedCore/Buffer.hpp>
#include <TiltedCore/ViewBuffer.hpp>
#include <TiltedCore/Serialization.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <catch2/catch.hpp>
#include <Messages/ClientMessageFactory.h>
#include <Messages/ServerMessageFactory.h>
#include <Messages/RequestSharedDrop.h>
#include <Messages/NotifySharedDrop.h>
#include <algorithm>
#include <array>
#include <limits>

using namespace TiltedPhoques;

namespace
{
SharedDropData Drop()
{
    SharedDropData d;
    d.Action = SharedDropAction::Create;
    d.Epoch = 23; d.Token = 71; d.Generation = 1; d.Owner = 4; d.Creator = 4; d.Replicas = 1;
    d.Item.BaseId = {2, 0x1234}; d.Item.Count = 7;
    d.Cell = {1, 0x5678}; d.WorldSpace = {1, 0x3c};
    d.Item.ExtraEnchantId = {UINT32_MAX, 0x123}; d.Item.ExtraEnchantCharge = 512;
    d.Item.EnchantData.IsWeapon = true;
    d.Item.EnchantData.Effects.push_back({15.5f, 0, 3, 17.f, {2, 0x789}});
    d.Item.ExtraEnchantRemoveUnequip = true;
    d.Item.ExtraCharge = 0.f; d.Item.ExtraHealth = 1.75f; d.ExtraMask = 3;
    d.Item.ExtraPoisonId = {1, 0x987}; d.Item.ExtraPoisonCount = 3; d.Item.ExtraSoulLevel = 4;
    d.Name = "Winter's Edge";
    d.Physics.Position = {70.f, 140.f, 210.f}; d.Physics.Rotation = {0.f, 0.f, 1.f};
    d.Physics.MotionType = 3;
    d.Physics.BodyTransform = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 1, 2, 3, 1};
    d.Physics.LinearVelocity = {1, 2, 3};
    return d;
}
template <class T> auto Decode(Buffer& aBuffer, size_t aSize)
{
    ViewBuffer view(aBuffer.GetWriteData(), aSize);
    Buffer::Reader reader(&view);
    if constexpr (std::is_base_of_v<ClientMessage, T>) return ClientMessageFactory{}.Extract(reader);
    else return ServerMessageFactory{}.Extract(reader);
}
}

TEST_CASE("Shared drop identity and complete item instance survive the wire", "[encoding.shared_drops]")
{
    RequestSharedDrop request;
    static_cast<SharedDropData&>(request) = Drop();
    REQUIRE(request.ValidPayload());
    Buffer buffer(8192); Buffer::Writer writer(&buffer); request.Serialize(writer);
    auto result = Decode<RequestSharedDrop>(buffer, writer.Size());
    REQUIRE(result);
    const auto& decoded = static_cast<RequestSharedDrop&>(*result);
    CHECK(decoded.Item == request.Item);
    REQUIRE(decoded.Item.EnchantData.Effects.size() == 1);
    CHECK(decoded.Item.EnchantData.Effects[0].EffectId == request.Item.EnchantData.Effects[0].EffectId);
    CHECK(decoded.Item.EnchantData.Effects[0].Magnitude == 15.5f);
    CHECK(decoded.Item.EnchantData.Effects[0].RawCost == 17.f);
    CHECK(decoded.Item.EnchantData.Effects[0].Duration == 3);
    CHECK(decoded.Item.EnchantData.IsWeapon);
    CHECK(decoded.ExtraMask == 3);
    CHECK(decoded.Item.ExtraCharge == 0.f);
    CHECK(decoded.Name == request.Name);
    CHECK(decoded.Epoch == request.Epoch);
    CHECK(decoded.Token == request.Token);
    CHECK(decoded.Physics.BodyTransform == request.Physics.BodyTransform);
    CHECK(decoded.Cell == request.Cell);
    CHECK(decoded.WorldSpace == request.WorldSpace);
    for (size_t n = 0; n < writer.Size(); ++n) REQUIRE_FALSE(Decode<RequestSharedDrop>(buffer, n));
}

TEST_CASE("Shared drop requests and lifecycle replies are factory registered and truncation safe", "[encoding.shared_drops]")
{
    const auto action = GENERATE(SharedDropAction::Pickup, SharedDropAction::Move, SharedDropAction::Snapshot,
        SharedDropAction::Ready, SharedDropAction::Upsert, SharedDropAction::Granted, SharedDropAction::Denied,
        SharedDropAction::Remove, SharedDropAction::Release, SharedDropAction::Local);
    auto data = Drop();
    data.Action = action; data.Id = 91; data.OriginToken = 71; data.Tick = 1001;
    data.Physics.Id = {SharedDropData::PhysicsModId, data.Id}; data.Winner = "Lydia";
    REQUIRE(data.ValidPayload());
    Buffer buffer(8192); Buffer::Writer writer(&buffer);
    if (action <= SharedDropAction::Ready)
    {
        RequestSharedDrop request; static_cast<SharedDropData&>(request) = data; request.Serialize(writer);
        auto result = Decode<RequestSharedDrop>(buffer, writer.Size());
        REQUIRE(result);
        const auto& decoded = static_cast<RequestSharedDrop&>(*result);
        CHECK(decoded.Action == action); CHECK(decoded.Id == 91); CHECK(decoded.Generation == 1);
        for (size_t n = 0; n < writer.Size(); ++n) REQUIRE_FALSE(Decode<RequestSharedDrop>(buffer, n));
    }
    else
    {
        NotifySharedDrop message; static_cast<SharedDropData&>(message) = data; message.Serialize(writer);
        auto result = Decode<NotifySharedDrop>(buffer, writer.Size());
        REQUIRE(result);
        const auto& decoded = static_cast<NotifySharedDrop&>(*result);
        CHECK(decoded.Action == action); CHECK(decoded.Id == 91); CHECK(decoded.Winner == "Lydia");
        CHECK(decoded.OriginToken == 71); CHECK(decoded.Replicas == 1);
        for (size_t n = 0; n < writer.Size(); ++n) REQUIRE_FALSE(Decode<NotifySharedDrop>(buffer, n));
    }
}

TEST_CASE("Shared drop rejects quest items, unsafe extras, and forged packet directions", "[encoding.shared_drops]")
{
    RequestSharedDrop request; static_cast<SharedDropData&>(request) = Drop();
    SECTION("Quest item") { request.Item.IsQuestItem = true; }
    SECTION("Negative count") { request.Item.Count = -1; }
    SECTION("Count outside engine extra count") { request.Item.Count = 32768; }
    SECTION("Nonfinite charge") { request.Item.ExtraCharge = std::numeric_limits<float>::quiet_NaN(); }
    SECTION("Nonfinite position") { request.Physics.Position.x = std::numeric_limits<float>::infinity(); }
    SECTION("Degenerate body rotation") { request.Physics.BodyTransform.fill(0); }
    SECTION("Oversized custom name") { request.Name.assign(129, 'x'); }
    SECTION("Embedded null") { request.Name = String("a\0b", 3); }
    SECTION("Excessive enchantment effects") { request.Item.EnchantData.Effects.resize(33); }
    SECTION("Temporary base") { request.Item.BaseId.ModId = UINT32_MAX; }
    SECTION("Missing session") { request.Epoch = 0; }
    SECTION("Missing create token") { request.Token = 0; }
    SECTION("Unknown action") { request.Action = static_cast<SharedDropAction>(255); }
    SECTION("Client-issued grant") { request.Action = SharedDropAction::Granted; request.Id = 91; }
    Buffer buffer(16384); Buffer::Writer writer(&buffer); request.Serialize(writer);
    REQUIRE_FALSE(Decode<RequestSharedDrop>(buffer, writer.Size()));
}

TEST_CASE("Shared drop server physics keeps its identity and lease generation", "[encoding.shared_drops]")
{
    NotifySharedDrop message;
    static_cast<SharedDropData&>(message) = Drop();
    message.Action = SharedDropAction::Move; message.Id = 91; message.Generation = 8; message.Tick = 1001;
    message.Physics.Id = {SharedDropData::PhysicsModId, message.Id};
    Buffer buffer(8192); Buffer::Writer writer(&buffer); message.Serialize(writer);
    auto result = Decode<NotifySharedDrop>(buffer, writer.Size());
    REQUIRE(result);
    const auto& decoded = static_cast<NotifySharedDrop&>(*result);
    CHECK(decoded.Action == SharedDropAction::Move);
    CHECK(decoded.Generation == 8);
    CHECK(decoded.Tick == 1001);
    CHECK(decoded.Physics.Id == message.Physics.Id);
    CHECK(decoded.Physics.BodyTransform == message.Physics.BodyTransform);
    for (size_t n = 0; n < writer.Size(); ++n) REQUIRE_FALSE(Decode<NotifySharedDrop>(buffer, n));
}

TEST_CASE("Shared drop pickup grants once for every five-player request ordering", "[encoding.shared_drops]")
{
    std::array<uint32_t, 5> players{0, 1, 2, 3, 4};
    do
    {
        SharedDropClaim claim;
        unsigned granted{};
        for (auto player : players) granted += claim.TryTake(player, true, true, 100.f);
        REQUIRE(granted == 1);
        REQUIRE(claim.Winner == players.front());
        for (auto player : players) REQUIRE_FALSE(claim.TryTake(player, true, true, 0.f));
        REQUIRE_FALSE(claim.CanMove(players.front(), players.front(), 1, 1));
    } while (std::next_permutation(players.begin(), players.end()));
}

TEST_CASE("Shared drop validates range and death without consuming the first valid claim", "[encoding.shared_drops]")
{
    SharedDropClaim claim;
    CHECK_FALSE(claim.TryTake(1, false, true, 0.f));
    CHECK_FALSE(claim.TryTake(1, true, false, 0.f));
    CHECK_FALSE(claim.TryTake(1, true, true, 257.f * 257.f));
    CHECK_FALSE(claim.TryTake(1, true, true, std::numeric_limits<float>::quiet_NaN()));
    CHECK_FALSE(claim.TryTake(1, true, true, std::numeric_limits<float>::infinity()));
    CHECK_FALSE(claim.TryTake(1, true, true, -1.f));
    CHECK(claim.TryTake(4, true, true, 256.f * 256.f));
    CHECK(claim.Winner == 4);
}

TEST_CASE("Shared drop physics authority fences old owners and old generations", "[encoding.shared_drops]")
{
    SharedDropClaim claim;
    CHECK(claim.CanMove(3, 3, 7, 7));
    CHECK_FALSE(claim.CanMove(2, 3, 7, 7));
    CHECK_FALSE(claim.CanMove(3, 3, 6, 7));
    CHECK_FALSE(claim.CanMove(3, 3, 0, 0));
    CHECK_FALSE(claim.CanMove(3, 4, 7, 8));
    CHECK(claim.CanMove(4, 4, 8, 8));
    REQUIRE(claim.TryTake(2, true, true, 0.f));
    CHECK_FALSE(claim.CanMove(4, 4, 8, 8));
}
