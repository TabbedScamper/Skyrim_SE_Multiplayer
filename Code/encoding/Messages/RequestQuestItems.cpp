#include <Messages/RequestQuestItems.h>
#include <Structs/CheckedRead.h>

bool RequestQuestItems::ValidPayload() const noexcept
{
    return Epoch && Token && Action <= QuestItemAction::Release &&
        (Action == QuestItemAction::Snapshot || (Item.IsValid() &&
            Item.Active == (Action == QuestItemAction::Acquire) &&
            (Item.Active || Item.Revision)));
}

void RequestQuestItems::SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    aWriter.WriteBits(Epoch, 64);
    aWriter.WriteBits(Token, 64);
    aWriter.WriteBits(static_cast<uint8_t>(Action), 8);
    if (Action != QuestItemAction::Snapshot)
        Item.Serialize(aWriter);
}

void RequestQuestItems::DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    try
    {
        uint64_t action{};
        CheckedRead::Bits(aReader, Epoch, 64);
        CheckedRead::Bits(aReader, Token, 64);
        CheckedRead::Bits(aReader, action, 8);
        Action = static_cast<QuestItemAction>(action);
        m_valid = (Action == QuestItemAction::Snapshot || Item.Deserialize(aReader)) && ValidPayload();
    }
    catch (...) { m_valid = false; }
}
