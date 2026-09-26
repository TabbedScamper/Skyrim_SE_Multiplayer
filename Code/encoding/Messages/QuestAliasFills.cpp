#include <Messages/QuestAliasFills.h>
#include <Structs/CheckedRead.h>

bool QuestAliasFills::IsValid() const noexcept
{
    if (!Id.BaseId || Id.ModId == ServerReference || !Sequence || Entries.size() > MaxAliases ||
        (HasStage && (Status > 2 || !TransactionId)))
        return false;

    for (size_t i = 0; i < Entries.size(); ++i)
    {
        const auto& entry = Entries[i];
        if ((i && Entries[i - 1].AliasId >= entry.AliasId) ||
            (entry.Reference && !entry.Reference.BaseId))
            return false;
    }
    return true;
}

void QuestAliasFills::Serialize(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    aWriter.WriteBits(Id.ModId, 32);
    aWriter.WriteBits(Id.BaseId, 32);
    aWriter.WriteBits(AuthorityEpoch, 64);
    aWriter.WriteBits(Sequence, 64);
    aWriter.WriteBits(Running, 1);
    aWriter.WriteBits(HasStage, 1);
    if (HasStage)
    {
        aWriter.WriteBits(Stage, 16);
        aWriter.WriteBits(Status, 8);
        aWriter.WriteBits(ClientQuestType, 8);
        aWriter.WriteBits(TransactionId, 64);
    }
    aWriter.WriteBits(Entries.size(), 16);
    for (const auto& entry : Entries)
    {
        aWriter.WriteBits(entry.AliasId, 32);
        aWriter.WriteBits(entry.Reference.ModId, 32);
        aWriter.WriteBits(entry.Reference.BaseId, 32);
    }
}

bool QuestAliasFills::Deserialize(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    try
    {
        uint64_t value{};
        CheckedRead::Bits(aReader, value, 32);
        Id.ModId = static_cast<uint32_t>(value);
        CheckedRead::Bits(aReader, value, 32);
        Id.BaseId = static_cast<uint32_t>(value);
        CheckedRead::Bits(aReader, AuthorityEpoch, 64);
        CheckedRead::Bits(aReader, Sequence, 64);
        Running = CheckedRead::Bool(aReader);
        HasStage = CheckedRead::Bool(aReader);
        if (HasStage)
        {
            CheckedRead::Bits(aReader, value, 16);
            Stage = static_cast<uint16_t>(value);
            CheckedRead::Bits(aReader, value, 8);
            Status = static_cast<uint8_t>(value);
            CheckedRead::Bits(aReader, value, 8);
            ClientQuestType = static_cast<uint8_t>(value);
            CheckedRead::Bits(aReader, TransactionId, 64);
        }
        CheckedRead::Bits(aReader, value, 16);
        if (value > MaxAliases || CheckedRead::RemainingBits(aReader) < value * 96)
            return false;
        Entries.resize(static_cast<size_t>(value));
        for (auto& entry : Entries)
        {
            CheckedRead::Bits(aReader, value, 32);
            entry.AliasId = static_cast<uint32_t>(value);
            CheckedRead::Bits(aReader, value, 32);
            entry.Reference.ModId = static_cast<uint32_t>(value);
            CheckedRead::Bits(aReader, value, 32);
            entry.Reference.BaseId = static_cast<uint32_t>(value);
        }
        return IsValid();
    }
    catch (...)
    {
        return false;
    }
}
