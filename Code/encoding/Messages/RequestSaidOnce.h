#pragma once
#include <Messages/Message.h>
#include <Structs/GameId.h>

struct RequestSaidOnce final : ClientMessage
{
    static constexpr ClientOpcode Opcode = kRequestSaidOnce;
    RequestSaidOnce() : ClientMessage(Opcode) {}
    void SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept override;
    void DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept override;
    bool operator==(const RequestSaidOnce& aOther) const noexcept { return Info == aOther.Info; }
    GameId Info{};
};
