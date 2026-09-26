#include <Messages/NotifyQuestItems.h>
#include <Structs/CheckedRead.h>

bool NotifyQuestItems::ValidPayload() const noexcept
{
    if (!Epoch || !Token || Items.size() > MaxItems ||
        (!Snapshot && (Page || !Complete || Items.size() != 1)) ||
        (Snapshot && !Complete && Items.size() != MaxItems))
        return false;
    for (size_t i = 0; i < Items.size(); ++i)
    {
        if (!Items[i].IsValid() || !Items[i].Revision)
            return false;
        for (size_t j = 0; j < i; ++j)
            if (Items[i].SameKey(Items[j]))
                return false;
    }
    return true;
}

void NotifyQuestItems::SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    aWriter.WriteBits(Epoch, 64);
    aWriter.WriteBits(Token, 64);
    aWriter.WriteBits(Page, 32);
    aWriter.WriteBits(Snapshot, 1);
    aWriter.WriteBits(Complete, 1);
    aWriter.WriteBits(Items.size(), 16);
    for (const auto& item : Items)
        item.Serialize(aWriter);
}

void NotifyQuestItems::DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    try
    {
        uint64_t value{};
        CheckedRead::Bits(aReader, Epoch, 64);
        CheckedRead::Bits(aReader, Token, 64);
        CheckedRead::Bits(aReader, value, 32);
        Page = static_cast<uint32_t>(value);
        Snapshot = CheckedRead::Bool(aReader);
        Complete = CheckedRead::Bool(aReader);
        CheckedRead::Bits(aReader, value, 16);
        m_valid = value <= MaxItems && CheckedRead::RemainingBits(aReader) >= value * 354;
        if (!m_valid)
            return;
        Items.resize(static_cast<size_t>(value));
        for (auto& item : Items)
            if (!item.Deserialize(aReader)) { m_valid = false; return; }
        m_valid = ValidPayload();
    }
    catch (...) { m_valid = false; }
}
