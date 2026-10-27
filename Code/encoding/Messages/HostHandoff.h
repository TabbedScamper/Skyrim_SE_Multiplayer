#pragma once
#include <Messages/Message.h>
#include <Messages/CheckpointRoster.h>

// A client reports that its checkpoint, roster and carry-home data are durable.
struct RequestHandoffAck final : ClientMessage
{
    static constexpr ClientOpcode Opcode = kRequestHandoffAck;
    RequestHandoffAck() : ClientMessage(Opcode) {}
    String CheckpointId;
    void SerializeRaw(TiltedPhoques::Buffer::Writer& w) const noexcept override
    {
        w.WriteBits(CheckpointId.size(), 8);
        for (unsigned char c : CheckpointId) w.WriteBits(c, 8);
    }
    void DeserializeRaw(TiltedPhoques::Buffer::Reader& r) noexcept override
    {
        uint64_t size{}, byte{};
        if (!r.ReadBits(size, 8) || !size || size > 64) { m_valid = false; return; }
        for (size_t i = 0; i < size; ++i)
        {
            if (!r.ReadBits(byte, 8)) { m_valid = false; return; }
            CheckpointId.push_back(static_cast<char>(byte));
        }
        m_valid = true;
    }
};

// The successor host sends the old party order and roster to its fresh server.
struct RequestHandoffSeed final : ClientMessage
{
    static constexpr ClientOpcode Opcode = kRequestHandoffSeed;
    RequestHandoffSeed() : ClientMessage(Opcode) {}
    String CheckpointId;
    TiltedPhoques::Vector<uint64_t> SteamIds;
    TiltedPhoques::Vector<RosterPosition> Entries;
    void SerializeRaw(TiltedPhoques::Buffer::Writer& w) const noexcept override
    {
        w.WriteBits(CheckpointId.size(), 8);
        for (unsigned char c : CheckpointId) w.WriteBits(c, 8);
        w.WriteBits(SteamIds.size(), 8);
        for (auto id : SteamIds) w.WriteBits(id, 64);
        w.WriteBits(Entries.size(), 8);
        for (const auto& entry : Entries) entry.Serialize(w);
    }
    void DeserializeRaw(TiltedPhoques::Buffer::Reader& r) noexcept override
    {
        uint64_t size{}, value{};
        if (!r.ReadBits(size, 8) || !size || size > 64) { m_valid = false; return; }
        for (size_t i = 0; i < size; ++i)
        {
            if (!r.ReadBits(value, 8)) { m_valid = false; return; }
            CheckpointId.push_back(static_cast<char>(value));
        }
        if (!r.ReadBits(size, 8) || !size || size > 16) { m_valid = false; return; }
        SteamIds.resize(size);
        for (auto& id : SteamIds)
            if (!r.ReadBits(id, 64)) { m_valid = false; return; }
        if (!r.ReadBits(size, 8) || size > 16) { m_valid = false; return; }
        for (size_t i = 0; i < size; ++i)
        {
            RosterPosition entry;
            if (!entry.Deserialize(r)) { m_valid = false; return; }
            Entries.push_back(std::move(entry));
        }
        m_valid = true;
    }
};

// The new server computes a common durable checkpoint from all connected PCs.
struct RequestCheckpointAvailability final : ClientMessage
{
    static constexpr ClientOpcode Opcode = kRequestCheckpointAvailability;
    RequestCheckpointAvailability() : ClientMessage(Opcode) {}
    TiltedPhoques::Vector<String> Checkpoints;
    void SerializeRaw(TiltedPhoques::Buffer::Writer& w) const noexcept override
    {
        w.WriteBits(Checkpoints.size(), 8);
        for (const auto& id : Checkpoints)
        {
            w.WriteBits(id.size(), 8);
            for (unsigned char c : id) w.WriteBits(c, 8);
        }
    }
    void DeserializeRaw(TiltedPhoques::Buffer::Reader& r) noexcept override
    {
        uint64_t count{}, size{}, value{};
        if (!r.ReadBits(count, 8) || count > 32) { m_valid = false; return; }
        for (size_t i = 0; i < count; ++i)
        {
            if (!r.ReadBits(size, 8) || !size || size > 64) { m_valid = false; return; }
            String id;
            for (size_t j = 0; j < size; ++j)
            {
                if (!r.ReadBits(value, 8)) { m_valid = false; return; }
                id.push_back(static_cast<char>(value));
            }
            Checkpoints.push_back(std::move(id));
        }
        m_valid = true;
    }
};
