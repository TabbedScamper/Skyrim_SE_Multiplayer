#include <Messages/ScriptedActorState.h>

using TiltedPhoques::Serialization;

void ScriptedActorState::Serialize(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    Serialization::WriteVarInt(aWriter, ServerId);
    Serialization::WriteVarInt(aWriter, OwnershipEpoch);
    WorldSpaceId.Serialize(aWriter);
    CellId.Serialize(aWriter);
    Position.Serialize(aWriter);
    Serialization::WriteVarInt(aWriter, static_cast<uint8_t>(Phase));
    Serialization::WriteBool(aWriter, Disabled);
}

void ScriptedActorState::Deserialize(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    ServerId = Serialization::ReadVarInt(aReader) & 0xFFFFFFFF;
    OwnershipEpoch = Serialization::ReadVarInt(aReader) & 0xFFFFFFFF;
    WorldSpaceId.Deserialize(aReader);
    CellId.Deserialize(aReader);
    Position.Deserialize(aReader);
    const auto phase = Serialization::ReadVarInt(aReader);
    Phase = phase <= static_cast<uint8_t>(ScriptedActorPhase::Bind)
        ? static_cast<ScriptedActorPhase>(phase) : static_cast<ScriptedActorPhase>(0xFF);
    Disabled = Serialization::ReadBool(aReader);
}

bool ScriptedActorState::operator==(const ScriptedActorState& acRhs) const noexcept
{
    return ServerId == acRhs.ServerId && OwnershipEpoch == acRhs.OwnershipEpoch &&
        WorldSpaceId == acRhs.WorldSpaceId && CellId == acRhs.CellId &&
        Position == acRhs.Position && Phase == acRhs.Phase && Disabled == acRhs.Disabled;
}
