#include <TiltedCore/Stl.hpp>
#include <TiltedCore/Buffer.hpp>
#include <TiltedCore/ViewBuffer.hpp>
#include <TiltedCore/Serialization.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <catch2/catch.hpp>

#include <Messages/ClientMessageFactory.h>
#include <Messages/ServerMessageFactory.h>
#include <Messages/SyncExperienceRequest.h>
#include <Messages/NotifySyncExperience.h>
#include <Messages/RequestEquipmentChanges.h>
#include <Messages/NotifyEquipmentChanges.h>
#include <Messages/ProjectileLaunchRequest.h>
#include <Messages/NotifyProjectileLaunch.h>

namespace
{
template <class T> T Decode(const T& message)
{
    TiltedPhoques::Buffer buffer(16384);
    TiltedPhoques::Buffer::Writer writer(&buffer);
    message.Serialize(writer);
    TiltedPhoques::ViewBuffer view(buffer.GetWriteData(), writer.Size());
    TiltedPhoques::Buffer::Reader reader(&view);
    if constexpr (std::is_base_of_v<ClientMessage, T>)
    {
        auto decoded = ClientMessageFactory{}.Extract(reader);
        REQUIRE(decoded);
        return static_cast<const T&>(*decoded);
    }
    else
    {
        auto decoded = ServerMessageFactory{}.Extract(reader);
        REQUIRE(decoded);
        return static_cast<const T&>(*decoded);
    }
}
}

TEST_CASE("Shared XP retains its source skill across both message directions", "[items.protocol]")
{
    SyncExperienceRequest request;
    request.Skill = 20; // Destruction, independent of the receiving player's last skill.
    request.Experience = 12.5f;
    const auto decodedRequest = Decode(request);
    CHECK(decodedRequest.Skill == 20);
    CHECK(decodedRequest.Experience == 12.5f);

    NotifySyncExperience notify;
    notify.Skill = decodedRequest.Skill;
    notify.Experience = decodedRequest.Experience;
    const auto decodedNotify = Decode(notify);
    CHECK(decodedNotify.Skill == 20);
    CHECK(decodedNotify.Experience == 12.5f);
}

TEST_CASE("An enchanted worn instance survives the equipment relay", "[items.protocol]")
{
    RequestEquipmentChanges request;
    request.ServerId = 17;
    request.OwnershipEpoch = 9;
    request.ItemId = GameId{0, 0x13989};
    request.HasStack = true;
    request.Stack.BaseId = request.ItemId;
    request.Stack.Count = 1;
    request.Stack.ExtraWorn = true;
    request.Stack.ExtraEnchantId = GameId{1, 0x800};
    request.Stack.ExtraHealth = 1.25f;
    request.Stack.ExtraPoisonId = GameId{0, 0x3EB42};
    request.Stack.ExtraPoisonCount = 2;
    const auto decodedRequest = Decode(request);
    REQUIRE(decodedRequest.HasStack);
    CHECK(decodedRequest.Stack.BaseId == request.ItemId);
    CHECK(decodedRequest.Stack.ExtraEnchantId == request.Stack.ExtraEnchantId);
    CHECK(decodedRequest.Stack.ExtraHealth == 1.25f);
    CHECK(decodedRequest.Stack.ExtraPoisonCount == 2);

    NotifyEquipmentChanges notify;
    notify.ServerId = decodedRequest.ServerId;
    notify.OwnershipEpoch = decodedRequest.OwnershipEpoch;
    notify.ItemId = decodedRequest.ItemId;
    notify.HasStack = decodedRequest.HasStack;
    notify.Stack = decodedRequest.Stack;
    const auto decodedNotify = Decode(notify);
    CHECK(decodedNotify.HasStack);
    CHECK(decodedNotify.Stack.ExtraEnchantId == request.Stack.ExtraEnchantId);
    CHECK(decodedNotify.Stack.ExtraPoisonId == request.Stack.ExtraPoisonId);
}

TEST_CASE("Dual cast intent survives projectile relay", "[items.protocol]")
{
    ProjectileLaunchRequest request;
    request.ShooterID = 25;
    request.SpellID = GameId{0, 0x12FCD};
    request.IsDualCasting = true;
    const auto decodedRequest = Decode(request);
    CHECK(decodedRequest.IsDualCasting);

    NotifyProjectileLaunch notify;
    notify.ShooterID = decodedRequest.ShooterID;
    notify.SpellID = decodedRequest.SpellID;
    notify.IsDualCasting = decodedRequest.IsDualCasting;
    const auto decodedNotify = Decode(notify);
    CHECK(decodedNotify.IsDualCasting);
}
