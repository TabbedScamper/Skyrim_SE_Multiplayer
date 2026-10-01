#pragma once
#include <Messages/Message.h>

// Joining a running session (owner design 2026-09-30): the host's world rules, the joiner brings a character.
// Join (joiner -> server), Capture (server -> leader), Chunk and Done (leader -> server -> joiner, the leader's fresh
// save in pieces), Loaded (joiner -> server, the world is loaded and the character applied), Admit (server -> joiner),
// Abort (either way). One attempt id ties the steps together; the server binds the sender from the connection.
enum class DropInOp : uint8_t
{
    Join,
    Capture,
    Chunk,
    Done,
    Loaded,
    Admit,
    Abort
};

struct DropInData
{
    static constexpr size_t MaxText = 256;
    static constexpr size_t MaxChunk = 16384;
    static constexpr uint64_t MaxFile = 256ull << 20;

    uint64_t Attempt{};
    uint32_t Joiner{};
    DropInOp Op{};
    // Join: the joiner's character name; Done: the checkpoint id; Abort: the reason.
    String Text;
    // Chunk: this piece's offset; Done: the file size.
    uint64_t Offset{};
    uint64_t Total{};
    // Done: FNV-1a 64 of the whole file.
    uint64_t Hash{};
    String Bytes;

    bool Valid() const noexcept
    {
        if (!Attempt || Op > DropInOp::Abort || Text.size() > MaxText || Bytes.size() > MaxChunk || Total > MaxFile)
            return false;
        if (Op == DropInOp::Chunk)
            return !Bytes.empty() && Offset <= MaxFile && Bytes.size() <= MaxFile - Offset;
        return Bytes.empty();
    }
    void Serialize(TiltedPhoques::Buffer::Writer& w) const noexcept
    {
        w.WriteBits(Attempt, 64);
        w.WriteBits(Joiner, 32);
        w.WriteBits(static_cast<uint8_t>(Op), 8);
        w.WriteBits(Text.size(), 16);
        for (unsigned char c : Text)
            w.WriteBits(c, 8);
        w.WriteBits(Offset, 64);
        w.WriteBits(Total, 64);
        w.WriteBits(Hash, 64);
        w.WriteBits(Bytes.size(), 16);
        for (unsigned char c : Bytes)
            w.WriteBits(c, 8);
    }
    bool Deserialize(TiltedPhoques::Buffer::Reader& r) noexcept
    {
        uint64_t v{};
        if (!r.ReadBits(Attempt, 64) || !r.ReadBits(v, 32))
            return false;
        Joiner = static_cast<uint32_t>(v);
        if (!r.ReadBits(v, 8))
            return false;
        Op = static_cast<DropInOp>(v);
        if (!r.ReadBits(v, 16) || v > MaxText)
            return false;
        Text.clear();
        for (size_t i = 0, size = static_cast<size_t>(v); i < size; ++i)
        {
            if (!r.ReadBits(v, 8))
                return false;
            Text.push_back(static_cast<char>(v));
        }
        if (!r.ReadBits(Offset, 64) || !r.ReadBits(Total, 64) || !r.ReadBits(Hash, 64) || !r.ReadBits(v, 16) || v > MaxChunk)
            return false;
        Bytes.clear();
        Bytes.reserve(static_cast<size_t>(v));
        for (size_t i = 0, size = static_cast<size_t>(v); i < size; ++i)
        {
            uint64_t byte{};
            if (!r.ReadBits(byte, 8))
                return false;
            Bytes.push_back(static_cast<char>(byte));
        }
        return Valid();
    }
    bool operator==(const DropInData&) const = default;
};

struct RequestDropIn final : ClientMessage, DropInData
{
    static constexpr ClientOpcode Opcode = kRequestDropIn;
    RequestDropIn() : ClientMessage(Opcode) {}
    void SerializeRaw(TiltedPhoques::Buffer::Writer& w) const noexcept override { DropInData::Serialize(w); }
    void DeserializeRaw(TiltedPhoques::Buffer::Reader& r) noexcept override { m_valid = DropInData::Deserialize(r); }
    bool operator==(const RequestDropIn& other) const noexcept { return static_cast<const DropInData&>(*this) == other; }
};

struct NotifyDropIn final : ServerMessage, DropInData
{
    static constexpr ServerOpcode Opcode = kNotifyDropIn;
    NotifyDropIn() : ServerMessage(Opcode) {}
    void SerializeRaw(TiltedPhoques::Buffer::Writer& w) const noexcept override { DropInData::Serialize(w); }
    void DeserializeRaw(TiltedPhoques::Buffer::Reader& r) noexcept override { m_valid = DropInData::Deserialize(r); }
    bool operator==(const NotifyDropIn& other) const noexcept { return static_cast<const DropInData&>(*this) == other; }
};
