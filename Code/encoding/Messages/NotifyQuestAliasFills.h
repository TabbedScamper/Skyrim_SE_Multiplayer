#pragma once

#include <Messages/Message.h>
#include <Messages/QuestAliasFills.h>

struct NotifyQuestAliasFills final : ServerMessage
{
    static constexpr ServerOpcode Opcode = kNotifyQuestAliasFills;
    NotifyQuestAliasFills() : ServerMessage(Opcode) {}

    void SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept override;
    void DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept override;
    bool operator==(const NotifyQuestAliasFills& acRhs) const noexcept
    {
        return Fills == acRhs.Fills && Revision == acRhs.Revision && LeaderPlayerId == acRhs.LeaderPlayerId;
    }

    QuestAliasFills Fills;
    uint64_t Revision{};
    uint32_t LeaderPlayerId{};
};
