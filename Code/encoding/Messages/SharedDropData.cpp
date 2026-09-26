#include <Messages/SharedDropData.h>
#include <Structs/CheckedRead.h>
#include <bit>
#include <cmath>

namespace
{
using Writer = TiltedPhoques::Buffer::Writer;
using Reader = TiltedPhoques::Buffer::Reader;
void WriteId(Writer& w, const GameId& id) { w.WriteBits(id.ModId, 32); w.WriteBits(id.BaseId, 32); }
GameId ReadId(Reader& r)
{
    uint64_t mod{}, base{};
    CheckedRead::Bits(r, mod, 32); CheckedRead::Bits(r, base, 32);
    return {static_cast<uint32_t>(mod), static_cast<uint32_t>(base)};
}
void WriteFloat(Writer& w, float v) { w.WriteBits(std::bit_cast<uint32_t>(v), 32); }
float ReadFloat(Reader& r)
{
    uint64_t v{}; CheckedRead::Bits(r, v, 32);
    const float f = std::bit_cast<float>(static_cast<uint32_t>(v));
    if (!std::isfinite(f)) throw std::runtime_error("nonfinite shared drop");
    return f;
}
uint32_t Read32(Reader& r) { uint64_t v{}; CheckedRead::Bits(r, v, 32); return static_cast<uint32_t>(v); }
void WriteString(Writer& w, const String& s)
{
    w.WriteBits(s.size(), 16);
    w.WriteBytes(reinterpret_cast<const uint8_t*>(s.data()), s.size());
}
String ReadString(Reader& r)
{
    uint64_t n{}; CheckedRead::Bits(r, n, 16);
    if (n > SharedDropData::MaxName || n > CheckedRead::RemainingBits(r) / 8)
        throw std::runtime_error("shared drop string limit");
    String s; s.resize(static_cast<size_t>(n));
    CheckedRead::Bytes(r, reinterpret_cast<uint8_t*>(s.data()), s.size());
    if (s.find('\0') != String::npos) throw std::runtime_error("shared drop embedded null");
    return s;
}
bool Finite(const glm::vec3& v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }
}

bool SharedDropData::HasItem() const noexcept
{
    return Action == SharedDropAction::Create || Action == SharedDropAction::Upsert || Action == SharedDropAction::Granted;
}
bool SharedDropData::HasPhysics() const noexcept { return HasItem() || Action == SharedDropAction::Move; }

bool SharedDropData::ValidPayload() const noexcept
{
    if (!Epoch || Action > SharedDropAction::Local || Name.size() > MaxName || Winner.size() > MaxName ||
        Name.find('\0') != String::npos || Winner.find('\0') != String::npos)
        return false;
    if (Action == SharedDropAction::Create && (!Token || Id)) return false;
    if (Action == SharedDropAction::Pickup && !Token) return false;
    if (Action != SharedDropAction::Create && Action != SharedDropAction::Snapshot &&
        Action != SharedDropAction::Local && (!Id || !Generation)) return false;
    if (HasItem())
    {
        if (!Item.BaseId.BaseId || Item.BaseId.BaseId > 0xFFFFFF || Item.BaseId.ModId >= PhysicsModId || Item.Count < 1 || Item.Count > 32767 ||
            Item.IsQuestItem || Item.ExtraWorn || Item.ExtraWornLeft || ExtraMask > 3 ||
            !Cell.BaseId || Cell.BaseId > 0xFFFFFF || Cell.ModId >= PhysicsModId || WorldSpace.ModId >= PhysicsModId ||
            WorldSpace.BaseId > 0xFFFFFF || (!WorldSpace.BaseId && WorldSpace.ModId) ||
            Item.EnchantData.Effects.size() > MaxEffects || !std::isfinite(Item.ExtraCharge) ||
            !std::isfinite(Item.ExtraHealth) || Item.ExtraCharge < 0 || Item.ExtraHealth < 0 ||
            Item.ExtraSoulLevel < 0 || Item.ExtraSoulLevel > 5 || Item.ExtraPoisonCount > 32767 ||
            (Item.ExtraPoisonId && Item.ExtraPoisonId.ModId >= PhysicsModId) ||
            (Item.ExtraEnchantId.ModId == UINT32_MAX && Item.EnchantData.Effects.empty()) ||
            Item.ExtraEnchantId.ModId == PhysicsModId) return false;
        for (const auto& effect : Item.EnchantData.Effects)
            if (!effect.EffectId.BaseId || effect.EffectId.BaseId > 0xFFFFFF || effect.EffectId.ModId >= PhysicsModId || !std::isfinite(effect.Magnitude) ||
                !std::isfinite(effect.RawCost) || effect.Area < 0 || effect.Duration < 0) return false;
    }
    if (HasPhysics())
    {
        if (!Finite(Physics.Position) || !Finite(Physics.Rotation) || !Finite(Physics.LinearVelocity) ||
            (Physics.MotionType != 0 && Physics.MotionType != 3) ||
            Physics.ChildBodies.size() > PhysicsReferenceUpdate::kMaxChildBodies) return false;
        for (float value : Physics.BodyTransform) if (!std::isfinite(value)) return false;
        for (const auto& child : Physics.ChildBodies)
            for (float value : child) if (!std::isfinite(value)) return false;
        if (glm::length(Physics.Position) > 10000000.f || glm::length(Physics.LinearVelocity) > 200.f) return false;
        if (Physics.MotionType == 3)
        {
            const auto& t = Physics.BodyTransform;
            const glm::vec3 x{t[0], t[1], t[2]}, y{t[4], t[5], t[6]}, z{t[8], t[9], t[10]};
            if (std::abs(glm::dot(x, x) - 1.f) > 0.05f || std::abs(glm::dot(y, y) - 1.f) > 0.05f ||
                std::abs(glm::dot(z, z) - 1.f) > 0.05f || std::abs(glm::dot(x, y)) > 0.05f ||
                std::abs(glm::dot(x, z)) > 0.05f || std::abs(glm::dot(y, z)) > 0.05f ||
                glm::dot(glm::cross(x, y), z) < 0.95f ||
                glm::length(glm::vec3{t[12], t[13], t[14]} * 70.f - Physics.Position) > 256.f) return false;
        }
    }
    return true;
}

void SharedDropData::SerializeData(Writer& w) const noexcept
{
    w.WriteBits(static_cast<uint8_t>(Action), 8);
    w.WriteBits(Epoch, 64); w.WriteBits(Token, 64); w.WriteBits(OriginToken, 64); w.WriteBits(Tick, 64);
    for (auto v : {Id, Generation, Owner, Creator, Replicas}) w.WriteBits(v, 32);
    WriteString(w, Winner);
    if (HasPhysics()) Physics.Serialize(w);
    if (!HasItem()) return;
    WriteId(w, Cell); WriteId(w, WorldSpace); WriteId(w, Item.BaseId);
    w.WriteBits(Item.Count, 32); w.WriteBits(ExtraMask, 8);
    WriteString(w, Name);
    WriteFloat(w, Item.ExtraCharge); WriteFloat(w, Item.ExtraHealth);
    WriteId(w, Item.ExtraEnchantId); w.WriteBits(Item.ExtraEnchantCharge, 16);
    w.WriteBits(Item.ExtraEnchantRemoveUnequip, 1); w.WriteBits(Item.EnchantData.IsWeapon, 1);
    WriteId(w, Item.ExtraPoisonId); w.WriteBits(Item.ExtraPoisonCount, 32);
    w.WriteBits(Item.ExtraSoulLevel, 8); w.WriteBits(Item.IsQuestItem, 1);
    w.WriteBits(Item.ExtraWorn, 1); w.WriteBits(Item.ExtraWornLeft, 1);
    w.WriteBits(Item.EnchantData.Effects.size(), 8);
    for (const auto& e : Item.EnchantData.Effects)
    {
        WriteId(w, e.EffectId); WriteFloat(w, e.Magnitude); WriteFloat(w, e.RawCost);
        w.WriteBits(e.Area, 32); w.WriteBits(e.Duration, 32);
    }
}

void SharedDropData::DeserializeData(Reader& r)
{
    uint64_t v{}; CheckedRead::Bits(r, v, 8); Action = static_cast<SharedDropAction>(v);
    if (Action > SharedDropAction::Local) throw std::runtime_error("unknown shared drop action");
    CheckedRead::Bits(r, Epoch, 64); CheckedRead::Bits(r, Token, 64);
    CheckedRead::Bits(r, OriginToken, 64); CheckedRead::Bits(r, Tick, 64);
    Id = Read32(r); Generation = Read32(r); Owner = Read32(r); Creator = Read32(r); Replicas = Read32(r);
    Winner = ReadString(r);
    if (HasPhysics()) Physics.Deserialize(r);
    if (HasItem())
    {
        Cell = ReadId(r); WorldSpace = ReadId(r); Item.BaseId = ReadId(r);
        Item.Count = static_cast<int32_t>(Read32(r));
        CheckedRead::Bits(r, v, 8); ExtraMask = static_cast<uint8_t>(v); Name = ReadString(r);
        Item.ExtraCharge = ReadFloat(r); Item.ExtraHealth = ReadFloat(r);
        Item.ExtraEnchantId = ReadId(r); CheckedRead::Bits(r, v, 16); Item.ExtraEnchantCharge = static_cast<uint16_t>(v);
        Item.ExtraEnchantRemoveUnequip = CheckedRead::Bool(r); Item.EnchantData.IsWeapon = CheckedRead::Bool(r);
        Item.ExtraPoisonId = ReadId(r); Item.ExtraPoisonCount = Read32(r);
        CheckedRead::Bits(r, v, 8); Item.ExtraSoulLevel = static_cast<int32_t>(v);
        Item.IsQuestItem = CheckedRead::Bool(r); Item.ExtraWorn = CheckedRead::Bool(r); Item.ExtraWornLeft = CheckedRead::Bool(r);
        CheckedRead::Bits(r, v, 8);
        if (v > MaxEffects) throw std::runtime_error("shared drop effects limit");
        Item.EnchantData.Effects.resize(static_cast<size_t>(v));
        for (auto& e : Item.EnchantData.Effects)
        {
            e.EffectId = ReadId(r); e.Magnitude = ReadFloat(r); e.RawCost = ReadFloat(r);
            e.Area = static_cast<int32_t>(Read32(r)); e.Duration = static_cast<int32_t>(Read32(r));
        }
    }
    if (!ValidPayload()) throw std::runtime_error("invalid shared drop payload");
}

bool SharedDropClaim::TryTake(uint32_t aPlayer, bool aInCell, bool aAlive, float aDistanceSquared) noexcept
{
    if (Taken || !aInCell || !aAlive || !std::isfinite(aDistanceSquared) || aDistanceSquared < 0 ||
        aDistanceSquared > SharedDropData::PickupDistance * SharedDropData::PickupDistance) return false;
    Taken = true;
    Winner = aPlayer;
    return true;
}
