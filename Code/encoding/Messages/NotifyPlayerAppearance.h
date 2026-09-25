#pragma once

#include "Message.h"
#include <Structs/Tints.h>

// Server -> the other party members: a player's current look (see PlayerAppearanceRequest).
struct NotifyPlayerAppearance final : ServerMessage
{
    static constexpr ServerOpcode Opcode = kNotifyPlayerAppearance;
    NotifyPlayerAppearance() : ServerMessage(Opcode) {}

    void SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept override;
    void DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept override;
    bool operator==(const NotifyPlayerAppearance& acRhs) const noexcept
    {
        return GetOpcode() == acRhs.GetOpcode() && ServerId == acRhs.ServerId && ChangeFlags == acRhs.ChangeFlags &&
               AppearanceBuffer == acRhs.AppearanceBuffer && FaceTints == acRhs.FaceTints && InCreator == acRhs.InCreator;
    }

    uint32_t ServerId{};
    uint32_t ChangeFlags{};
    TiltedPhoques::String AppearanceBuffer{};
    Tints FaceTints{};
    bool InCreator{};
};
