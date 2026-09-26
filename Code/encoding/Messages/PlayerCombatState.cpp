#include <Messages/PlayerCombatState.h>
#include <Structs/CheckedRead.h>
#include <bit>
#include <cmath>

void PlayerCombatState::Serialize(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    aWriter.WriteBits(Type, 2);
    TiltedPhoques::Serialization::WriteVarInt(aWriter, Epoch);
    TiltedPhoques::Serialization::WriteVarInt(aWriter, Sequence);
    aWriter.WriteBits(ActorId, 32);
    aWriter.WriteBits(OwnershipEpoch, 32);
    const auto writeFloat = [&](float value) { aWriter.WriteBits(std::bit_cast<uint32_t>(value), 32); };
    if (Type == Stealth)
    {
        aWriter.WriteBits(Sneaking, 1);
        aWriter.WriteBits(Moving, 1);
        aWriter.WriteBits(Running, 1);
        writeFloat(Speed);
        writeFloat(Light);
        writeFloat(ArmorWeight);
        for (float value : Values)
            writeFloat(value);
        TiltedPhoques::Serialization::WriteVarInt(aWriter, Perks.size());
        for (const auto& perk : Perks)
        {
            perk.Id.Serialize(aWriter);
            aWriter.WriteBits(perk.EntryIndex, 16);
        }
    }
    else if (Type == Detection)
    {
        aWriter.WriteBits(std::bit_cast<uint32_t>(DetectionLevel), 32);
        aWriter.WriteBits(MeterLevel, 8);
        aWriter.WriteBits(LOSCount, 32);
    }
    else
    {
        aWriter.WriteBits(TargetId, 32);
        aWriter.WriteBits(TargetOwnershipEpoch, 32);
        writeFloat(RawDamage);
        aWriter.WriteBits(KillMove, 1);
    }
}

bool PlayerCombatState::Deserialize(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    *this = {};
    try
    {
        const auto bits = [&](size_t count) { uint64_t value{}; CheckedRead::Bits(aReader, value, count); return static_cast<uint32_t>(value); };
        const auto readFloat = [&]() { return std::bit_cast<float>(bits(32)); };
        Type = static_cast<uint8_t>(bits(2));
        Epoch = CheckedRead::VarInt(aReader);
        Sequence = CheckedRead::VarInt(aReader);
        ActorId = bits(32);
        OwnershipEpoch = bits(32);
        if (Type == Stealth)
        {
            Sneaking = CheckedRead::Bool(aReader);
            Moving = CheckedRead::Bool(aReader);
            Running = CheckedRead::Bool(aReader);
            Speed = readFloat();
            Light = readFloat();
            ArmorWeight = readFloat();
            for (float& value : Values)
                value = readFloat();
            const auto count = CheckedRead::VarInt(aReader);
            if (count > MaxPerks)
                return false;
            for (uint64_t i = 0; i < count; ++i)
            {
                const auto base = CheckedRead::VarInt(aReader);
                const auto mod = CheckedRead::VarInt(aReader);
                if (base > UINT32_MAX || mod > UINT32_MAX)
                    return false;
                Perk perk{GameId(static_cast<uint32_t>(mod), static_cast<uint32_t>(base)), static_cast<uint16_t>(bits(16))};
                for (const auto& prior : Perks)
                    if (prior == perk)
                        return false;
                Perks.push_back(perk);
            }
        }
        else if (Type == Detection)
        {
            DetectionLevel = std::bit_cast<int32_t>(bits(32));
            MeterLevel = static_cast<uint8_t>(bits(8));
            LOSCount = bits(32);
        }
        else
        {
            TargetId = bits(32);
            TargetOwnershipEpoch = bits(32);
            RawDamage = readFloat();
            KillMove = CheckedRead::Bool(aReader);
        }
        return IsValid();
    }
    catch (...)
    {
        return false;
    }
}

bool PlayerCombatState::IsValid() const noexcept
{
    if (Type > Threat || !Epoch || !Sequence || !OwnershipEpoch)
        return false;
    if (Type == Stealth)
    {
        if (!std::isfinite(Speed) || Speed < 0.f || Speed > 100000.f ||
            !std::isfinite(Light) || Light < 0.f || Light > 100000.f ||
            !std::isfinite(ArmorWeight) || ArmorWeight < 0.f || ArmorWeight > 100000.f || Perks.size() > MaxPerks)
            return false;
        for (float value : Values)
            if (!std::isfinite(value) || std::abs(value) > 1000000.f)
                return false;
        for (const auto& perk : Perks)
            if (!perk.Id || perk.EntryIndex >= 4096)
                return false;
    }
    else if (Type == Detection)
        return DetectionLevel >= -1000 && DetectionLevel <= 1000000 && LOSCount <= 100000 && MeterLevel <= 100;
    else
        return TargetId != ActorId && TargetOwnershipEpoch && std::isfinite(RawDamage) && RawDamage > 0.f && RawDamage <= 1000000.f;
    return true;
}
