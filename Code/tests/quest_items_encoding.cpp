#include <TiltedCore/Stl.hpp>
#include <TiltedCore/Allocator.hpp>
#include <TiltedCore/Buffer.hpp>
#include <TiltedCore/ViewBuffer.hpp>
#include <TiltedCore/Serialization.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <catch2/catch.hpp>
#include <Messages/ClientMessageFactory.h>
#include <Messages/ServerMessageFactory.h>
#include <Messages/RequestQuestItems.h>
#include <Messages/NotifyQuestItems.h>

using namespace TiltedPhoques;

namespace
{
QuestItemState Item()
{
    return {{1, 0x39647}, {1, 0x39645}, {}, 11, 1, true, true, 0};
}

template <class T> auto Decode(Buffer& aBuffer, size_t aSize)
{
    ViewBuffer view(aBuffer.GetWriteData(), aSize);
    Buffer::Reader reader(&view);
    if constexpr (std::is_base_of_v<ClientMessage, T>)
        return ClientMessageFactory{}.Extract(reader);
    else
        return ServerMessageFactory{}.Extract(reader);
}
}

TEST_CASE("Quest item requests round trip snapshot acquisition and hand-in", "[encoding.quest_items]")
{
    RequestQuestItems request;
    request.Epoch = 5; request.Token = 99;
    request.Item = Item();
    request.Item.QuestInstance = 19;
    SECTION("Snapshot") { request.Item = {}; }
    SECTION("Pickup") { request.Action = QuestItemAction::Acquire; }
    SECTION("Hand-in")
    {
        request.Action = QuestItemAction::Release;
        request.Item.Active = false;
        request.Item.Revision = 37;
    }
    REQUIRE(request.ValidPayload());
    Buffer buffer(256);
    Buffer::Writer writer(&buffer);
    request.Serialize(writer);
    const auto size = writer.Size();
    auto decoded = Decode<RequestQuestItems>(buffer, size);
    REQUIRE(decoded);
    REQUIRE(static_cast<RequestQuestItems&>(*decoded) == request);
    for (size_t n = 0; n < size; ++n)
        REQUIRE_FALSE(Decode<RequestQuestItems>(buffer, n));
}

TEST_CASE("Quest item notifications round trip active and retired records in bounded pages", "[encoding.quest_items]")
{
    NotifyQuestItems message;
    message.Epoch = 5; message.Token = 99; message.Snapshot = true;
    const size_t count = GENERATE(size_t{0}, size_t{1}, NotifyQuestItems::MaxItems);
    for (size_t i = 0; i < count; ++i)
    {
        auto item = Item();
        item.AliasId = static_cast<uint32_t>(i);
        item.Revision = i + 1;
        item.Active = i % 2 == 0;
        message.Items.push_back(item);
    }
    REQUIRE(message.ValidPayload());
    Buffer buffer(4096);
    Buffer::Writer writer(&buffer);
    message.Serialize(writer);
    const auto size = writer.Size();
    auto decoded = Decode<NotifyQuestItems>(buffer, size);
    REQUIRE(decoded);
    REQUIRE(static_cast<NotifyQuestItems&>(*decoded) == message);
    for (size_t n = 0; n < size; ++n)
        REQUIRE_FALSE(Decode<NotifyQuestItems>(buffer, n));
}

TEST_CASE("Quest item protocol rejects malformed identities and invalid authority payloads", "[encoding.quest_items]")
{
    RequestQuestItems request;
    request.Epoch = 5; request.Token = 99; request.Action = QuestItemAction::Acquire; request.Item = Item();
    SECTION("Zero epoch") { request.Epoch = 0; }
    SECTION("Zero token") { request.Token = 0; }
    SECTION("Unknown action") { request.Action = static_cast<QuestItemAction>(255); }
    SECTION("Missing quest") { request.Item.QuestId = {}; }
    SECTION("Client temporary base") { request.Item.BaseId.ModId = UINT32_MAX; }
    SECTION("Client temporary reference") { request.Item.ReferenceId = {UINT32_MAX, 7}; }
    SECTION("Zero count") { request.Item.Count = 0; }
    SECTION("Excessive count") { request.Item.Count = 65536; }
    SECTION("Acquire carrying retirement") { request.Item.Active = false; }
    SECTION("Hand-in without acquired revision") { request.Action = QuestItemAction::Release; request.Item.Active = false; }
    REQUIRE_FALSE(request.ValidPayload());
    Buffer buffer(256);
    Buffer::Writer writer(&buffer);
    request.Serialize(writer);
    REQUIRE_FALSE(Decode<RequestQuestItems>(buffer, writer.Size()));
}

TEST_CASE("Quest item snapshot rejects oversized duplicate and incomplete pages", "[encoding.quest_items]")
{
    NotifyQuestItems message;
    message.Epoch = 5; message.Token = 99; message.Snapshot = true;
    auto item = Item(); item.Revision = 1;
    message.Items.push_back(item);
    SECTION("Duplicate identity") { message.Items.push_back(item); }
    SECTION("Oversized page") { message.Items.resize(NotifyQuestItems::MaxItems + 1, item); }
    SECTION("Non-final short page") { message.Complete = false; }
    SECTION("Uncommitted record") { message.Items[0].Revision = 0; }
    REQUIRE_FALSE(message.ValidPayload());
    Buffer buffer(4096);
    Buffer::Writer writer(&buffer);
    message.Serialize(writer);
    REQUIRE_FALSE(Decode<NotifyQuestItems>(buffer, writer.Size()));
}

TEST_CASE("Repeated quest instances have distinct durable wire identities", "[encoding.quest_items]")
{
    auto first = Item();
    first.Revision = 1;
    first.Active = false;
    auto next = first;
    next.QuestInstance = first.QuestInstance + 1;
    next.Revision = 2;
    next.Active = true;
    REQUIRE_FALSE(first.SameKey(next));
    NotifyQuestItems message;
    message.Epoch = 5; message.Token = 99; message.Snapshot = true;
    message.Items = {first, next};
    REQUIRE(message.ValidPayload());
}
