#include <Messages/NpcInventory.h>
#include <cmath>
#include <cstring>

namespace
{
using Writer = TiltedPhoques::Buffer::Writer;
using Reader = TiltedPhoques::Buffer::Reader;
void Id(Writer& w, GameId id) { w.WriteBits(id.ModId, 32); w.WriteBits(id.BaseId, 32); }
bool U32(Reader& r, uint32_t& out) { uint64_t v{}; if (!r.ReadBits(v, 32)) return false; out = uint32_t(v); return true; }
bool Id(Reader& r, GameId& id) { return U32(r, id.ModId) && U32(r, id.BaseId); }
void Float(Writer& w, float f) { uint32_t v; std::memcpy(&v, &f, 4); w.WriteBits(v, 32); }
bool Float(Reader& r, float& f) { uint32_t v; if (!U32(r, v)) return false; std::memcpy(&f, &v, 4); return std::isfinite(f); }
bool ItemValid(const Inventory::Entry& e)
{
    if (!e.BaseId || e.Count <= -1000000 || e.Count >= 1000000 || e.EnchantData.Effects.size() > 32 ||
        !std::isfinite(e.ExtraHealth) || !std::isfinite(e.ExtraCharge)) return false;
    for (const auto& effect : e.EnchantData.Effects)
        if (!std::isfinite(effect.Magnitude) || !std::isfinite(effect.RawCost)) return false;
    return true;
}
// Do not call the legacy unbounded Entry::Deserialize on network input.
void Item(Writer& w, const Inventory::Entry& e)
{
    Id(w, e.BaseId); w.WriteBits(uint32_t(e.Count), 32);
    Float(w, e.ExtraCharge); Id(w, e.ExtraEnchantId); w.WriteBits(e.ExtraEnchantCharge, 16);
    Float(w, e.ExtraHealth); Id(w, e.ExtraPoisonId); w.WriteBits(e.ExtraPoisonCount, 32);
    w.WriteBits(uint32_t(e.ExtraSoulLevel), 32);
    w.WriteBits(uint8_t(e.EnchantData.IsWeapon) | (e.ExtraEnchantRemoveUnequip << 1) |
        (e.ExtraWorn << 2) | (e.ExtraWornLeft << 3) | (e.IsQuestItem << 4), 8);
    w.WriteBits(e.EnchantData.Effects.size(), 8);
    for (const auto& effect : e.EnchantData.Effects)
    {
        Float(w, effect.Magnitude); w.WriteBits(uint32_t(effect.Area), 32);
        w.WriteBits(uint32_t(effect.Duration), 32); Float(w, effect.RawCost); Id(w, effect.EffectId);
    }
}
bool Item(Reader& r, Inventory::Entry& e)
{
    e = {};
    uint32_t count{}, soul{}; uint64_t charge{}, flags{}, size{};
    if (!Id(r, e.BaseId) || !U32(r, count) || !Float(r, e.ExtraCharge) || !Id(r, e.ExtraEnchantId) ||
        !r.ReadBits(charge, 16) || !Float(r, e.ExtraHealth) || !Id(r, e.ExtraPoisonId) ||
        !U32(r, e.ExtraPoisonCount) || !U32(r, soul) || !r.ReadBits(flags, 8) || flags > 31 ||
        !r.ReadBits(size, 8) || size > 32) return false;
    e.Count = int32_t(count); e.ExtraSoulLevel = int32_t(soul); e.ExtraEnchantCharge = uint16_t(charge);
    e.EnchantData.IsWeapon = flags & 1; e.ExtraEnchantRemoveUnequip = flags & 2;
    e.ExtraWorn = flags & 4; e.ExtraWornLeft = flags & 8; e.IsQuestItem = flags & 16;
    for (size_t i = 0; i < size; ++i)
    {
        Inventory::EffectItem effect; uint32_t area{}, duration{};
        if (!Float(r, effect.Magnitude) || !U32(r, area) || !U32(r, duration) ||
            !Float(r, effect.RawCost) || !Id(r, effect.EffectId)) return false;
        effect.Area = int32_t(area); effect.Duration = int32_t(duration); e.EnchantData.Effects.push_back(effect);
    }
    return true;
}
}

bool NpcSameItem(const Inventory::Entry& l, const Inventory::Entry& r, bool appearance) noexcept
{
    auto a = l, b = r;
    a.Count = b.Count = 1;
    if (a.ExtraEnchantId.ModId == UINT32_MAX && b.ExtraEnchantId.ModId == UINT32_MAX &&
        !a.EnchantData.Effects.empty() && !b.EnchantData.Effects.empty())
        a.ExtraEnchantId.BaseId = b.ExtraEnchantId.BaseId = 0; // local dynamic IDs differ; effects identify the enchant
    a.ExtraWorn = b.ExtraWorn = a.ExtraWornLeft = b.ExtraWornLeft = false;
    if (appearance)
    {
        // The requested appearance channel deliberately ignores tempering.
        // Keep charge, poison and soul identity; they must not collapse variants.
        a.ExtraHealth = b.ExtraHealth = 0;
        a.IsQuestItem = b.IsQuestItem = false;
    }
    if (a != b || a.EnchantData.IsWeapon != b.EnchantData.IsWeapon ||
        a.EnchantData.Effects.size() != b.EnchantData.Effects.size()) return false;
    for (size_t i = 0; i < a.EnchantData.Effects.size(); ++i)
    {
        const auto& x = a.EnchantData.Effects[i]; const auto& y = b.EnchantData.Effects[i];
        if (x.EffectId != y.EffectId || x.Magnitude != y.Magnitude || x.Area != y.Area ||
            x.Duration != y.Duration || x.RawCost != y.RawCost) return false;
    }
    return true;
}

bool NpcSameWorn(const Vector<NpcWornItem>& a, const Vector<NpcWornItem>& b) noexcept
{
    if (a.size() != b.size()) return false;
    Vector<bool> used(b.size(), false);
    for (const auto& x : a)
    {
        size_t i{};
        for (; i < b.size(); ++i)
            if (!used[i] && x.Slots == b[i].Slots && NpcSameItem(x.Item, b[i].Item, true)) break;
        if (i == b.size()) return false;
        used[i] = true;
    }
    return true;
}

bool NpcWornNewer(const NpcWornData& current, const NpcWornData& incoming) noexcept
{
    return incoming.Sequence && incoming.Valid() &&
        (!current.Sequence || (current.ServerId == incoming.ServerId &&
        (current.OwnershipEpoch < incoming.OwnershipEpoch ||
        (current.OwnershipEpoch == incoming.OwnershipEpoch && current.Sequence < incoming.Sequence))));
}

Vector<NpcWornAction> PlanNpcWorn(const Vector<NpcWornItem>& local, const Vector<NpcWornItem>& owner)
{
    Vector<NpcWornAction> result;
    Vector<int32_t> used(local.size(), 0);
    Vector<size_t> chosen(owner.size(), local.size());
    for (size_t n = 0; n < owner.size(); ++n)
    {
        const auto& desired = owner[n];
        // Prefer the instance already wearing this slot. An earlier plain stack
        // must not make an idempotent snapshot equip a second instance.
        for (unsigned pass = 0; pass < 3 && chosen[n] == local.size(); ++pass)
            for (size_t i = 0; i < local.size(); ++i)
            {
                const auto& candidate = local[i];
                const unsigned rank = candidate.Item.IsWorn() ? (candidate.Slots == desired.Slots ? 0 : 2) : 1;
                if (rank == pass && used[i] < candidate.Item.Count && NpcSameItem(candidate.Item, desired.Item, true))
                {
                    chosen[n] = i; ++used[i]; break;
                }
            }
    }
    for (size_t i = 0; i < local.size(); ++i)
    {
        if (!local[i].Item.IsWorn()) continue;
        bool retained = false;
        for (size_t n = 0; n < owner.size(); ++n)
            retained |= chosen[n] == i && local[i].Slots == owner[n].Slots;
        if (!retained) result.push_back({NpcWornActionKind::Unequip, local[i]});
    }
    for (size_t n = 0; n < owner.size(); ++n)
    {
        const auto i = chosen[n];
        if (i == local.size()) result.push_back({NpcWornActionKind::AddRenderCopy, owner[n]});
        if (i == local.size() || !local[i].Item.IsWorn() || local[i].Slots != owner[n].Slots)
            result.push_back({NpcWornActionKind::Equip, owner[n]});
    }
    return result;
}

bool NpcWornData::Valid() const noexcept
{
    if (!ServerId || !OwnershipEpoch || Items.size() > 35 || (!Sequence && !Items.empty())) return false;
    for (const auto& e : Items)
    {
        // Modded/layered worn instances can share biped bits. Capture them
        // without inferring that their native worn flags are contradictory.
        if (!ItemValid(e.Item) || !e.Item.IsWorn() || e.Item.Count != 1 || !e.Slots || e.Slots >> 35) return false;
    }
    return true;
}
void NpcWornData::Serialize(Writer& w) const noexcept
{
    w.WriteBits(ServerId, 32); w.WriteBits(OwnershipEpoch, 32); w.WriteBits(Sequence, 64); w.WriteBits(Items.size(), 8);
    for (const auto& e : Items) { w.WriteBits(e.Slots, 64); Item(w, e.Item); }
}
bool NpcWornData::Deserialize(Reader& r) noexcept
{
    Items.clear(); uint64_t size{};
    if (!U32(r, ServerId) || !U32(r, OwnershipEpoch) || !r.ReadBits(Sequence, 64) || !r.ReadBits(size, 8) || size > 35) return false;
    for (size_t i = 0; i < size; ++i) { NpcWornItem e; if (!r.ReadBits(e.Slots, 64) || !Item(r, e.Item)) return false; Items.push_back(e); }
    return Valid();
}
bool NpcLootData::Valid() const noexcept
{
    if ((!ContentsKnown && !Contents.Entries.empty()) || (Accepted && !ContentsKnown)) return false;
    if (Gold <= -100000000 || Gold >= 100000000 || (!Barter && Gold) || (Gold && ((Gold > 0) == (Item.Count > 0)))) return false;
    if (Op > NpcLootOp::TransferResult || !Reference || !Session || !Lease || !Token || Contents.Entries.size() > 4096) return false;
    if ((Op == NpcLootOp::Transfer || Op == NpcLootOp::TransferResult) && (!ItemValid(Item) || !Item.Count)) return false;
    size_t wireBytes = 128 + Item.EnchantData.Effects.size() * 24;
    for (const auto& e : Contents.Entries)
    {
        if (!ItemValid(e) || e.Count <= 0) return false;
        wireBytes += 48 + e.EnchantData.Effects.size() * 24;
    }
    if (wireBytes > 60000) return false; // transport allocates a 64 KiB message buffer
    return true;
}
void NpcLootData::Serialize(Writer& w) const noexcept
{
    w.WriteBits(uint8_t(Op), 8); Id(w, Reference); w.WriteBits(ServerId, 32); w.WriteBits(OwnershipEpoch, 32); w.WriteBits(Peer, 32);
    w.WriteBits(Session, 64); w.WriteBits(Lease, 64); w.WriteBits(Token, 64); w.WriteBits(Accepted, 8); w.WriteBits(Barter, 8); w.WriteBits(ContentsKnown, 8); w.WriteBits(uint32_t(Gold), 32); ::Item(w, Item);
    w.WriteBits(Contents.Entries.size(), 16); for (const auto& e : Contents.Entries) ::Item(w, e);
}
bool NpcLootData::Deserialize(Reader& r) noexcept
{
    uint64_t op{}, accepted{}, barter{}, known{}, size{}; uint32_t gold{}; Contents = {};
    if (!r.ReadBits(op, 8) || !Id(r, Reference) || !U32(r, ServerId) || !U32(r, OwnershipEpoch) || !U32(r, Peer) ||
        !r.ReadBits(Session, 64) || !r.ReadBits(Lease, 64) || !r.ReadBits(Token, 64) || !r.ReadBits(accepted, 8) || accepted > 1 ||
        !r.ReadBits(barter, 8) || barter > 1 || !r.ReadBits(known, 8) || known > 1 || !U32(r, gold) || !::Item(r, Item) || !r.ReadBits(size, 16) || size > 4096) return false;
    Op = NpcLootOp(op); Accepted = accepted != 0; Barter = barter != 0; ContentsKnown = known != 0; Gold = int32_t(gold);
    for (size_t i = 0; i < size; ++i) { Inventory::Entry e; if (!::Item(r, e)) return false; Contents.Entries.push_back(e); }
    return Valid();
}
