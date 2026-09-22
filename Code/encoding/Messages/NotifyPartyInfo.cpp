#include <Messages/NotifyPartyInfo.h>
#include <TiltedCore/Serialization.hpp>

void NotifyPartyInfo::SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    Serialization::WriteBool(aWriter, IsLeader);
    Serialization::WriteVarInt(aWriter, LeaderPlayerId);
    aWriter.WriteBits(PlayerIds.size() & 0xFF, 8);

    for (auto player : PlayerIds)
    {
        Serialization::WriteVarInt(aWriter, player);
    }
    aWriter.WriteBits(ReadyPlayerIds.size() & 0xFF, 8);
    for (auto player : ReadyPlayerIds)
        Serialization::WriteVarInt(aWriter, player);
    aWriter.WriteBits(CampaignMode, 2);
    aWriter.WriteBits(SessionState, 2);
    aWriter.WriteBits(StartEpoch, 64);
    Serialization::WriteString(aWriter, CheckpointId);
    Serialization::WriteBool(aWriter, LobbyOpen);
    Serialization::WriteBool(aWriter, PasswordProtected);
}

void NotifyPartyInfo::DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    IsLeader = Serialization::ReadBool(aReader);
    LeaderPlayerId = Serialization::ReadVarInt(aReader) & 0xFFFFFFFF;

    uint64_t count = 0;
    aReader.ReadBits(count, 8);

    PlayerIds.resize(count);

    for (auto i = 0u; i < count; ++i)
    {
        PlayerIds[i] = Serialization::ReadVarInt(aReader) & 0xFFFFFFFF;
    }
    aReader.ReadBits(count, 8);
    ReadyPlayerIds.resize(count);
    for (auto i = 0u; i < count; ++i)
        ReadyPlayerIds[i] = Serialization::ReadVarInt(aReader) & 0xFFFFFFFF;
    uint64_t value{};
    aReader.ReadBits(value, 2);
    CampaignMode = value & 0x3;
    aReader.ReadBits(value, 2);
    SessionState = value & 0x3;
    aReader.ReadBits(StartEpoch, 64);
    CheckpointId = Serialization::ReadString(aReader);
    LobbyOpen = Serialization::ReadBool(aReader);
    PasswordProtected = Serialization::ReadBool(aReader);
}
