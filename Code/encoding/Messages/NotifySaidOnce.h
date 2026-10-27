#pragma once
#include <Messages/Message.h>
#include <Structs/GameId.h>

struct NotifySaidOnce final : ServerMessage
{
    static constexpr ServerOpcode Opcode = kNotifySaidOnce;
    NotifySaidOnce() : ServerMessage(Opcode) {}
    void SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept override;
    void DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept override;
    bool operator==(const NotifySaidOnce& aOther) const noexcept { return Info == aOther.Info; }
    GameId Info{};
};
