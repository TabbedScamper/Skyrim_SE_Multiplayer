#pragma once
#include <Messages/Message.h>
#include <Messages/QuestItemState.h>

enum class QuestItemAction : uint8_t { Snapshot, Acquire, Release };

struct RequestQuestItems final : ClientMessage
{
    static constexpr ClientOpcode Opcode = kRequestQuestItems;
    RequestQuestItems() : ClientMessage(Opcode) {}
    void SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept override;
    void DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept override;
    bool ValidPayload() const noexcept;
    bool operator==(const RequestQuestItems& aOther) const noexcept
    {
        return Epoch == aOther.Epoch && Token == aOther.Token && Action == aOther.Action && Item == aOther.Item;
    }
    uint64_t Epoch{}, Token{};
    QuestItemAction Action{QuestItemAction::Snapshot};
    // Revision is the expected durable revision, zero for a new acquisition.
    QuestItemState Item;
};
