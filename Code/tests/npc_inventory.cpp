#include <TiltedCore/Stl.hpp>
#include <TiltedCore/Buffer.hpp>
#include <TiltedCore/ViewBuffer.hpp>
#include <TiltedCore/Serialization.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <catch2/catch.hpp>
#include <Messages/ClientMessageFactory.h>
#include <Messages/ServerMessageFactory.h>
#include <Messages/NpcInventory.h>

namespace
{
NpcWornItem Piece(uint32_t form, uint64_t slots, bool worn = true)
{
    NpcWornItem e; e.Slots = slots; e.Item.BaseId = {1, form}; e.Item.Count = 1; e.Item.ExtraWorn = worn; return e;
}
template<class T> auto Decode(TiltedPhoques::Buffer& b, size_t size)
{
    TiltedPhoques::ViewBuffer view(b.GetWriteData(), size); TiltedPhoques::Buffer::Reader reader(&view);
    if constexpr (std::is_base_of_v<ClientMessage, T>) return ClientMessageFactory{}.Extract(reader);
    else return ServerMessageFactory{}.Extract(reader);
}
template<class T> void RoundTrip(const T& message)
{
    TiltedPhoques::Buffer buffer(65536); TiltedPhoques::Buffer::Writer writer(&buffer); message.Serialize(writer);
    const auto decoded = Decode<T>(buffer, writer.Size()); REQUIRE(decoded);
    const auto& result = static_cast<const T&>(*decoded);
    if constexpr (std::is_base_of_v<NpcWornData, T>)
    {
        CHECK(result.ServerId == message.ServerId); CHECK(result.OwnershipEpoch == message.OwnershipEpoch);
        CHECK(result.Sequence == message.Sequence); CHECK(NpcSameWorn(result.Items, message.Items));
    }
    else
    {
        CHECK(result.Op == message.Op); CHECK(result.Reference == message.Reference); CHECK(result.Session == message.Session);
        CHECK(result.Token == message.Token); CHECK(result.Lease == message.Lease); CHECK(result.Peer == message.Peer);
        CHECK(result.ServerId == message.ServerId); CHECK(result.OwnershipEpoch == message.OwnershipEpoch);
        CHECK(result.Accepted == message.Accepted); CHECK(result.ContentsKnown == message.ContentsKnown); CHECK(result.Barter == message.Barter); CHECK(result.Gold == message.Gold);
        CHECK(NpcSameItem(result.Item, message.Item)); CHECK(result.Item.Count == message.Item.Count);
        REQUIRE(result.Contents.Entries.size() == message.Contents.Entries.size());
        for (size_t i = 0; i < result.Contents.Entries.size(); ++i)
            CHECK(NpcSameItem(result.Contents.Entries[i], message.Contents.Entries[i]));
    }
    for (size_t n = 0; n < writer.Size(); ++n) REQUIRE_FALSE(Decode<T>(buffer, n));
}
}

TEST_CASE("NPC worn snapshots and queries round trip through both factories", "[npc.inventory]")
{
    RequestNpcWorn request; request.ServerId = 23; request.OwnershipEpoch = 7;
    RoundTrip(request);
    request.Sequence = uint64_t{1} << 40;
    auto body = Piece(0x3C9FE, 4);
    body.Item.ExtraEnchantId = {UINT32_MAX, 0x123}; body.Item.ExtraEnchantCharge = 512;
    body.Item.EnchantData.Effects.push_back({7.5f, 1, 10, 22.f, {2, 123}});
    request.Items = {body, Piece(0x3CA00, 128)};
    REQUIRE(request.Valid()); RoundTrip(request);
    NotifyNpcWorn notify; static_cast<NpcWornData&>(notify) = request; RoundTrip(notify);
    notify.Items.clear(); ++notify.Sequence; RoundTrip(notify);
}

TEST_CASE("NPC loot fetch and transfer retain instance and transaction identity", "[npc.inventory]")
{
    const auto op = GENERATE(NpcLootOp::Fetch, NpcLootOp::Transfer, NpcLootOp::FetchResult, NpcLootOp::TransferResult);
    const auto count = GENERATE(-3, 2);
    RequestNpcLoot request; request.Op = op; request.ServerId = 8; request.OwnershipEpoch = 4;
    request.Reference = {1, 0x654FB}; request.Peer = 7; request.Session = 999; request.Token = 83; request.Lease = 44;
    request.Item = Piece(0x123, 4).Item; request.Item.Count = count; request.Item.ExtraHealth = 1.5f;
    request.Item.EnchantData.IsWeapon = true;
    request.Item.EnchantData.Effects.push_back({1, 2, 3, 4, {5, 6}});
    request.Accepted = true; request.ContentsKnown = true; request.Contents.Entries = {Piece(1, 4).Item, Piece(2, 128).Item};
    REQUIRE(request.Valid()); RoundTrip(request);
    NotifyNpcLoot notify; static_cast<NpcLootData&>(notify) = request; RoundTrip(notify);
    request.Barter = true; request.Gold = count < 0 ? 100 : -100; RoundTrip(request);
}

TEST_CASE("Worn policy is idempotent and an empty owner set unequips without deleting", "[npc.inventory]")
{
    const Vector<NpcWornItem> worn{Piece(1, 4), Piece(2, 128)};
    CHECK(PlanNpcWorn(worn, worn).empty());
    const auto actions = PlanNpcWorn(worn, {});
    REQUIRE(actions.size() == 2);
    for (const auto& a : actions) CHECK(a.Kind == NpcWornActionKind::Unequip);
    CHECK(PlanNpcWorn({Piece(1, 4, false)}, {}).empty());
}

TEST_CASE("Worn policy creates only missing instances and respects multi-slot armor", "[npc.inventory]")
{
    auto desired = Piece(1, 4 | 128);
    auto actions = PlanNpcWorn({Piece(2, 4)}, {desired});
    REQUIRE(actions.size() == 3);
    CHECK(actions[0].Kind == NpcWornActionKind::Unequip);
    CHECK(actions[1].Kind == NpcWornActionKind::AddRenderCopy);
    CHECK(actions[2].Kind == NpcWornActionKind::Equip);
    CHECK(actions[1].Worn.Slots == 132);
    actions = PlanNpcWorn({Piece(1, 132, false), Piece(7, 16, false)}, {desired});
    REQUIRE(actions.size() == 1); CHECK(actions[0].Kind == NpcWornActionKind::Equip);
}

TEST_CASE("Worn matching preserves enchant effects but ignores temper and quantity", "[npc.inventory]")
{
    auto a = Piece(1, 4), b = a;
    b.Item.ExtraHealth = 1.75f; b.Item.Count = 12;
    CHECK(PlanNpcWorn({b}, {a}).empty());
    b.Item.ExtraEnchantId = {2, 3};
    CHECK(PlanNpcWorn({b}, {a}).size() == 3);
    a.Item = b.Item; a.Item.Count = 1;
    a.Item.EnchantData.Effects.push_back({2, 0, 0, 0, {1, 2}});
    CHECK_FALSE(NpcSameItem(a.Item, b.Item, true));
}

TEST_CASE("Worn and loot validators reject contradictory and oversized input", "[npc.inventory]")
{
    RequestNpcWorn request; request.ServerId = 1; request.OwnershipEpoch = 2; request.Sequence = 3;
    request.Items = {Piece(1, 4), Piece(2, 4)}; CHECK(request.Valid());
    request.Items = {Piece(1, uint64_t{1} << 35)}; CHECK_FALSE(request.Valid());
    request.Items = {Piece(1, 4)}; request.Sequence = 0; CHECK_FALSE(request.Valid());
    RequestNpcLoot loot; loot.Session = 1; loot.Token = 1; loot.Lease = 1; loot.Reference = {1, 1};
    loot.Op = NpcLootOp::Transfer; loot.Item = Piece(1, 4).Item;
    CHECK(loot.Valid()); loot.Item.Count = 0; CHECK_FALSE(loot.Valid());
    loot.Item.Count = 1; loot.Item.EnchantData.Effects.resize(33); CHECK_FALSE(loot.Valid());
}

TEST_CASE("Dynamic enchant identities survive creation on another PC", "[npc.inventory]")
{
    auto owner = Piece(1, 4), copy = owner;
    owner.Item.ExtraEnchantId = {UINT32_MAX, 0x123}; copy.Item.ExtraEnchantId = {UINT32_MAX, 0x456};
    owner.Item.EnchantData.Effects.push_back({3, 0, 0, 1, {1, 2}});
    copy.Item.EnchantData = owner.Item.EnchantData;
    CHECK(PlanNpcWorn({copy}, {owner}).empty());
    copy.Item.EnchantData.Effects[0].Magnitude = 4;
    CHECK(PlanNpcWorn({copy}, {owner}).size() == 3);
}

TEST_CASE("Worn hand instances and loot transport limits are explicit", "[npc.inventory]")
{
    auto right = Piece(1, uint64_t{1} << 32), left = right;
    left.Slots = uint64_t{1} << 33; left.Item.ExtraWorn = false; left.Item.ExtraWornLeft = true;
    CHECK(PlanNpcWorn({right, left}, {right, left}).empty());
    CHECK(PlanNpcWorn({right}, {right, left}).size() == 2);
    auto stock = right; stock.Item.ExtraWorn = false; stock.Item.Count = 2;
    const auto actions = PlanNpcWorn({stock}, {right, left});
    REQUIRE(actions.size() == 2);
    for (const auto& action : actions) CHECK(action.Kind == NpcWornActionKind::Equip);
    RequestNpcLoot reply; reply.Session = reply.Token = reply.Lease = 1; reply.Reference = {1, 2};
    reply.ContentsKnown = true; reply.Contents.Entries.resize(1250, right.Item);
    CHECK_FALSE(reply.Valid());
}

TEST_CASE("Worn sequence floors survive stale and future ownership notifications", "[npc.inventory]")
{
    NpcWornData current;
    NpcWornData next; next.ServerId = 9; next.OwnershipEpoch = 5; next.Sequence = 7;
    next.Items = {Piece(1, 4)};
    CHECK(NpcWornNewer(current, next));
    current = next;
    CHECK_FALSE(NpcWornNewer(current, next));
    --next.Sequence; CHECK_FALSE(NpcWornNewer(current, next));
    next.OwnershipEpoch = 4; next.Sequence = 100;
    CHECK_FALSE(NpcWornNewer(current, next));
    next.OwnershipEpoch = 6; next.Sequence = 1;
    CHECK(NpcWornNewer(current, next));
    current = next;
    // An old-epoch snapshot cannot overwrite a future snapshot already received.
    next.OwnershipEpoch = 5; next.Sequence = 1000;
    CHECK_FALSE(NpcWornNewer(current, next));
    next = current; next.Sequence = 0;
    CHECK_FALSE(NpcWornNewer(current, next));
    next = current; ++next.Sequence; ++next.ServerId;
    CHECK_FALSE(NpcWornNewer(current, next));
    next = current; ++next.Sequence; next.Items.clear();
    CHECK(NpcWornNewer(current, next)); // Authoritative empty is still newer.
}

TEST_CASE("Worn comparisons preserve multiplicity and poison enchant variants", "[npc.inventory]")
{
    const auto body = Piece(1, 4), overlay = Piece(2, 4);
    CHECK_FALSE(NpcSameWorn({body, body}, {body, overlay}));
    CHECK(NpcSameWorn({overlay, body}, {body, overlay}));
    RequestNpcWorn layered; layered.ServerId = layered.OwnershipEpoch = 1; layered.Sequence = 1;
    layered.Items = {body, overlay}; REQUIRE(layered.Valid()); RoundTrip(layered);
    auto variant = body;
    variant.Item.ExtraPoisonId = {1, 3};
    CHECK_FALSE(NpcSameItem(body.Item, variant.Item, true));
    variant = body; variant.Item.ExtraCharge = 3;
    CHECK_FALSE(NpcSameItem(body.Item, variant.Item, true));
    auto a = body, b = body;
    a.Item.ExtraEnchantId = {UINT32_MAX, 1}; b.Item.ExtraEnchantId = {UINT32_MAX, 2};
    CHECK_FALSE(NpcSameItem(a.Item, b.Item, true)); // Missing effects cannot identify dynamic forms.
}

TEST_CASE("Worn plans reserve existing equipped instances before plain stock", "[npc.inventory]")
{
    const auto worn = Piece(1, 4);
    auto stock = worn; stock.Item.ExtraWorn = false; stock.Item.Count = 3;
    CHECK(PlanNpcWorn({stock, worn}, {worn}).empty());
    const auto duplicate = PlanNpcWorn({worn, worn}, {worn});
    REQUIRE(duplicate.size() == 1);
    CHECK(duplicate[0].Kind == NpcWornActionKind::Unequip);
    const auto right = Piece(7, uint64_t{1} << 32);
    auto left = right; left.Slots = uint64_t{1} << 33; left.Item.ExtraWorn = false; left.Item.ExtraWornLeft = true;
    const auto moved = PlanNpcWorn({right}, {left});
    REQUIRE(moved.size() == 2);
    CHECK(moved[0].Kind == NpcWornActionKind::Unequip);
    CHECK(moved[1].Kind == NpcWornActionKind::Equip);
}
