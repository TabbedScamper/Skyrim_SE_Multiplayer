#include <Messages/NotifyQuestAliasFills.h>

void NotifyQuestAliasFills::SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    Fills.Serialize(aWriter);
    aWriter.WriteBits(Revision, 64);
    aWriter.WriteBits(LeaderPlayerId, 32);
}

void NotifyQuestAliasFills::DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    uint64_t leader{};
    m_valid = Fills.Deserialize(aReader) && aReader.ReadBits(Revision, 64) && aReader.ReadBits(leader, 32);
    LeaderPlayerId = static_cast<uint32_t>(leader);
    m_valid = m_valid && (!Fills.HasStage || Revision != 0);
}
