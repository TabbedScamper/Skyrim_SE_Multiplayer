#include <Messages/RequestPartyUnstuck.h>

bool PartyUnstuckState::IsValid(const UnstuckMove& acMove) const noexcept
{
    return acMove.IsValid() && Controls.IsValid() && Controls.Epoch == acMove.Epoch &&
        Controls.Sequence == acMove.Sequence && (Furniture || !FurnitureMarker) &&
        (!Controls.Free || (!AIDriven && !InputBlocked));
}

void PartyUnstuckState::Serialize(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    Controls.Serialize(aWriter);
    aWriter.WriteBits(Furniture.ModId, 32);
    aWriter.WriteBits(Furniture.BaseId, 32);
    aWriter.WriteBits(FurnitureMarker, 32);
    for (const auto value : {Sneaking, WeaponDrawn, FirstPerson, CartMode, HudCartMode, AIDriven, InputBlocked})
        aWriter.WriteBits(value, 1);
}

bool PartyUnstuckState::Deserialize(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    if (!Controls.Deserialize(aReader))
        return false;
    uint64_t value{};
    for (auto* field : {&Furniture.ModId, &Furniture.BaseId, &FurnitureMarker})
    {
        if (!aReader.ReadBits(value, 32))
            return false;
        *field = static_cast<uint32_t>(value);
    }
    for (auto* field : {&Sneaking, &WeaponDrawn, &FirstPerson, &CartMode, &HudCartMode, &AIDriven, &InputBlocked})
    {
        if (!aReader.ReadBits(value, 1))
            return false;
        *field = value != 0;
    }
    return true;
}

void RequestPartyUnstuck::SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    Move.Serialize(aWriter);
    State.Serialize(aWriter);
}

void RequestPartyUnstuck::DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    ClientMessage::DeserializeRaw(aReader);
    m_valid = Move.Deserialize(aReader) && State.Deserialize(aReader) && State.IsValid(Move);
}
