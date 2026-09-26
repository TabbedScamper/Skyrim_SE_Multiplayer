#pragma once
#include <Messages/Message.h>
#include <Messages/SharedDropData.h>

struct NotifySharedDrop final : ServerMessage, SharedDropData
{
    static constexpr ServerOpcode Opcode = kNotifySharedDrop;
    NotifySharedDrop() : ServerMessage(Opcode) {}
    void SerializeRaw(TiltedPhoques::Buffer::Writer& w) const noexcept override { SerializeData(w); }
    void DeserializeRaw(TiltedPhoques::Buffer::Reader& r) noexcept override
    {
        try { DeserializeData(r); m_valid = Action == SharedDropAction::Move || Action >= SharedDropAction::Upsert; }
        catch (...) { m_valid = false; }
    }
};
