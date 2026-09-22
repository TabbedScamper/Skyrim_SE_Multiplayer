
#include <Messages/NotifyQuestUpdate.h>
#include <TiltedCore/Serialization.hpp>

void NotifyQuestUpdate::SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    Id.Serialize(aWriter);
    aWriter.WriteBits(Stage, 16);
    aWriter.WriteBits(Status, 8);
    aWriter.WriteBits(ClientQuestType, 8);
    aWriter.WriteBits(TransactionId, 64);
    aWriter.WriteBits(Revision, 64);
    aWriter.WriteBits(AuthorityEpoch, 64);
}

void NotifyQuestUpdate::DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    ServerMessage::DeserializeRaw(aReader);
    Id.Deserialize(aReader);

    uint64_t tmp;
    aReader.ReadBits(tmp, 16);
    Stage = tmp & 0xFFFF;

    aReader.ReadBits(tmp, 8);
    Status = tmp & 0xFF;

    aReader.ReadBits(tmp, 8);
    ClientQuestType = tmp & 0xFF;

    aReader.ReadBits(TransactionId, 64);
    aReader.ReadBits(Revision, 64);
    aReader.ReadBits(AuthorityEpoch, 64);
}
