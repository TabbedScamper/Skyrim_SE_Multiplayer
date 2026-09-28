#pragma once
#include <Messages/Message.h>

// Reliable transport is not an execution acknowledgment. Every action is
// identified by campaign epoch, run, and ordered step, including its replies.
enum class HarnessOp : uint8_t { Step, Done, Barrier, Abort, Finish, Prepared, Execute };
struct HarnessData
{
    uint64_t Epoch{}, Run{};
    uint32_t Sequence{}, Sender{};
    HarnessOp Op{};
    String Payload;
    static constexpr size_t MaxPayload = 4096;
    bool Valid() const noexcept
    {
        return Epoch && Run && Sequence && Op <= HarnessOp::Execute &&
            Payload.size() <= MaxPayload && Payload.find('\0') == String::npos &&
            (Op != HarnessOp::Step || !Payload.empty());
    }
    void Serialize(TiltedPhoques::Buffer::Writer& w) const noexcept
    {
        w.WriteBits(Epoch, 64); w.WriteBits(Run, 64);
        w.WriteBits(Sequence, 32); w.WriteBits(Sender, 32);
        w.WriteBits(static_cast<uint8_t>(Op), 8);
        w.WriteBits(Payload.size(), 16);
        for (unsigned char c : Payload) w.WriteBits(c, 8);
    }
    bool Deserialize(TiltedPhoques::Buffer::Reader& r) noexcept
    {
        uint64_t v{};
        if (!r.ReadBits(Epoch, 64) || !r.ReadBits(Run, 64) || !r.ReadBits(v, 32)) return false;
        Sequence = static_cast<uint32_t>(v);
        if (!r.ReadBits(v, 32)) return false;
        Sender = static_cast<uint32_t>(v);
        if (!r.ReadBits(v, 8)) return false;
        Op = static_cast<HarnessOp>(v);
        if (!r.ReadBits(v, 16) || v > MaxPayload) return false;
        const auto size = static_cast<size_t>(v);
        Payload.clear();
        for (size_t i = 0; i < size; ++i)
        {
            if (!r.ReadBits(v, 8)) return false;
            Payload.push_back(static_cast<char>(v));
        }
        return Valid();
    }
    bool operator==(const HarnessData&) const = default;
};
struct RequestHarness final : ClientMessage, HarnessData
{
    static constexpr ClientOpcode Opcode = kRequestHarness;
    RequestHarness() : ClientMessage(Opcode) {}
    void SerializeRaw(TiltedPhoques::Buffer::Writer& w) const noexcept override { HarnessData::Serialize(w); }
    void DeserializeRaw(TiltedPhoques::Buffer::Reader& r) noexcept override { m_valid = HarnessData::Deserialize(r); }
    bool operator==(const RequestHarness& other) const noexcept { return static_cast<const HarnessData&>(*this) == other; }
};
struct NotifyHarness final : ServerMessage, HarnessData
{
    static constexpr ServerOpcode Opcode = kNotifyHarness;
    NotifyHarness() : ServerMessage(Opcode) {}
    void SerializeRaw(TiltedPhoques::Buffer::Writer& w) const noexcept override { HarnessData::Serialize(w); }
    void DeserializeRaw(TiltedPhoques::Buffer::Reader& r) noexcept override { m_valid = HarnessData::Deserialize(r); }
    bool operator==(const NotifyHarness& other) const noexcept { return static_cast<const HarnessData&>(*this) == other; }
};
