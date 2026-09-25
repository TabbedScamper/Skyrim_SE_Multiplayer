#include <Messages/AssignCharacterResponse.h>

void AssignCharacterResponse::SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    Serialization::WriteVarInt(aWriter, Cookie);
    Serialization::WriteVarInt(aWriter, ServerId);
    Serialization::WriteVarInt(aWriter, PlayerId);
    Position.Serialize(aWriter);
    CellId.Serialize(aWriter);
    WorldSpaceId.Serialize(aWriter);
    AllActorValues.Serialize(aWriter);
    CurrentInventory.Serialize(aWriter);
    Serialization::WriteBool(aWriter, InventoryAuthoritative);
    ActionsToReplay.Serialize(aWriter);
    Serialization::WriteVarInt(aWriter, OwnershipEpoch);
    Serialization::WriteBool(aWriter, Owner);
    Serialization::WriteBool(aWriter, IsDead);
    Serialization::WriteBool(aWriter, IsWeaponDrawn);
    LeveledNpcPickId.Serialize(aWriter);
    Serialization::WriteVarInt(aWriter, MountedOnServerId);
}

void AssignCharacterResponse::DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    Cookie = Serialization::ReadVarInt(aReader) & 0xFFFFFFFF;
    ServerId = Serialization::ReadVarInt(aReader) & 0xFFFFFFFF;
    PlayerId = Serialization::ReadVarInt(aReader) & 0xFFFFFFFF;
    Position.Deserialize(aReader);
    CellId.Deserialize(aReader);
    WorldSpaceId.Deserialize(aReader);
    AllActorValues.Deserialize(aReader);
    CurrentInventory.Deserialize(aReader);
    InventoryAuthoritative = Serialization::ReadBool(aReader);
    ActionsToReplay.Deserialize(aReader);
    OwnershipEpoch = Serialization::ReadVarInt(aReader) & 0xFFFFFFFF;
    Owner = Serialization::ReadBool(aReader);
    IsDead = Serialization::ReadBool(aReader);
    IsWeaponDrawn = Serialization::ReadBool(aReader);
    LeveledNpcPickId.Deserialize(aReader);
    MountedOnServerId = Serialization::ReadVarInt(aReader) & 0xFFFFFFFF;
}
