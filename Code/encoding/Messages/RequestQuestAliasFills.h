#pragma once

#include <Messages/Message.h>
#include <Messages/QuestAliasFills.h>

struct RequestQuestAliasFills final : ClientMessage
{
    static constexpr ClientOpcode Opcode = kRequestQuestAliasFills;
    RequestQuestAliasFills() : ClientMessage(Opcode) {}

    void SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept override;
    void DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept override;
    bool operator==(const RequestQuestAliasFills& acRhs) const noexcept
    {
        return Fills == acRhs.Fills;
    }

    QuestAliasFills Fills;
};
