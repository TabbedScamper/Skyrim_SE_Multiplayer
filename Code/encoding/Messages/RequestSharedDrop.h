#pragma once
#include <Messages/Message.h>
#include <Messages/SharedDropData.h>

struct RequestSharedDrop final : ClientMessage, SharedDropData
{
    static constexpr ClientOpcode Opcode = kRequestSharedDrop;
    RequestSharedDrop() : ClientMessage(Opcode) {}
    void SerializeRaw(TiltedPhoques::Buffer::Writer& w) const noexcept override { SerializeData(w); }
    void DeserializeRaw(TiltedPhoques::Buffer::Reader& r) noexcept override
    {
        try { DeserializeData(r); m_valid = Action <= SharedDropAction::Ready; }
        catch (...) { m_valid = false; }
    }
};
