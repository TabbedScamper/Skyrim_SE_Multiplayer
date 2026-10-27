#include <Messages/RequestEquipmentChanges.h>
#include <TiltedCore/Serialization.hpp>

void RequestEquipmentChanges::SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    Serialization::WriteVarInt(aWriter, ServerId);
    Serialization::WriteVarInt(aWriter, OwnershipEpoch);
    ItemId.Serialize(aWriter);
    EquipSlotId.Serialize(aWriter);
    Serialization::WriteVarInt(aWriter, Count);
    Serialization::WriteBool(aWriter, Unequip);
    Serialization::WriteBool(aWriter, IsSpell);
    Serialization::WriteBool(aWriter, IsShout);
    Serialization::WriteBool(aWriter, IsAmmo);
    Serialization::WriteBool(aWriter, HasStack);
    if (HasStack) Stack.Serialize(aWriter);
    CurrentInventory.Serialize(aWriter);
}

void RequestEquipmentChanges::DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    ClientMessage::DeserializeRaw(aReader);

    ServerId = Serialization::ReadVarInt(aReader) & 0xFFFFFFFF;
    OwnershipEpoch = Serialization::ReadVarInt(aReader) & 0xFFFFFFFF;
    ItemId.Deserialize(aReader);
    EquipSlotId.Deserialize(aReader);
    Count = Serialization::ReadVarInt(aReader) & 0xFFFFFFFF;
    Unequip = Serialization::ReadBool(aReader);
    IsSpell = Serialization::ReadBool(aReader);
    IsShout = Serialization::ReadBool(aReader);
    IsAmmo = Serialization::ReadBool(aReader);
    HasStack = Serialization::ReadBool(aReader);
    if (HasStack) Stack.Deserialize(aReader);
    CurrentInventory.Deserialize(aReader);
}
