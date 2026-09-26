#pragma once

#include "Message.h"
#include "UnstuckMove.h"
#include "RequestPartyUnstuck.h"

struct NotifyPartyUnstuck final : ServerMessage
{
    static constexpr ServerOpcode Opcode = kNotifyPartyUnstuck;
    NotifyPartyUnstuck() : ServerMessage(Opcode) {}

    void SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept override;
    void DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept override;
    bool operator==(const NotifyPartyUnstuck& acRhs) const noexcept
    {
        return GetOpcode() == acRhs.GetOpcode() && Move == acRhs.Move &&
            LeaderId == acRhs.LeaderId && Slot == acRhs.Slot && State == acRhs.State;
    }

    UnstuckMove Move{};
    uint32_t LeaderId{};
    uint32_t Slot{};
    PartyUnstuckState State{};
};
