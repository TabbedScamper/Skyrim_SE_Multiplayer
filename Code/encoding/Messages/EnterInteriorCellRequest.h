#pragma once

#include "Message.h"
#include <Structs/GameId.h>

using TiltedPhoques::String;

struct EnterInteriorCellRequest final : ClientMessage
{
    static constexpr ClientOpcode Opcode = kEnterInteriorCellRequest;

    EnterInteriorCellRequest()
        : ClientMessage(Opcode)
    {
    }

    virtual ~EnterInteriorCellRequest() = default;

    void SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept override;
    void DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept override;

    bool operator==(const EnterInteriorCellRequest& acRhs) const noexcept { return CellId == acRhs.CellId && GetOpcode() == acRhs.GetOpcode(); }

    GameId CellId{};
    // A periodic re-report of the cell the player is in: the server acts only when its record differs.
    bool Heartbeat{};
};
