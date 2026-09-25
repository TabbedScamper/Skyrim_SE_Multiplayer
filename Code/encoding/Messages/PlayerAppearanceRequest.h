#pragma once

#include "Message.h"
#include <Structs/Tints.h>

// Player -> server: this player's current look (TESNPC appearance and face tints), sent live while
// the player is in the character creator so the other players watch it change.
struct PlayerAppearanceRequest final : ClientMessage
{
    static constexpr ClientOpcode Opcode = kPlayerAppearanceRequest;
    PlayerAppearanceRequest() : ClientMessage(Opcode) {}

    void SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept override;
    void DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept override;
    bool operator==(const PlayerAppearanceRequest& acRhs) const noexcept
    {
        return GetOpcode() == acRhs.GetOpcode() && ServerId == acRhs.ServerId && ChangeFlags == acRhs.ChangeFlags &&
               AppearanceBuffer == acRhs.AppearanceBuffer && FaceTints == acRhs.FaceTints && InCreator == acRhs.InCreator;
    }

    // The player's character (server id).
    uint32_t ServerId{};
    uint32_t ChangeFlags{};
    TiltedPhoques::String AppearanceBuffer{};
    Tints FaceTints{};
    // Still editing (the creator is open); false for the final look.
    bool InCreator{};
};
