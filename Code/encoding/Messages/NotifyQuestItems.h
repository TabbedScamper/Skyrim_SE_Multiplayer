#pragma once
#include <Messages/Message.h>
#include <Messages/QuestItemState.h>
#include <TiltedCore/Stl.hpp>

struct NotifyQuestItems final : ServerMessage
{
    static constexpr ServerOpcode Opcode = kNotifyQuestItems;
    static constexpr size_t MaxItems = 64;
    NotifyQuestItems() : ServerMessage(Opcode) {}
    void SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept override;
    void DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept override;
    bool ValidPayload() const noexcept;
    bool operator==(const NotifyQuestItems& aOther) const noexcept
    {
        return Epoch == aOther.Epoch && Token == aOther.Token && Page == aOther.Page &&
            Snapshot == aOther.Snapshot && Complete == aOther.Complete && Items == aOther.Items;
    }
    uint64_t Epoch{}, Token{};
    uint32_t Page{};
    bool Snapshot{}, Complete{true};
    TiltedPhoques::Vector<QuestItemState> Items;
};
