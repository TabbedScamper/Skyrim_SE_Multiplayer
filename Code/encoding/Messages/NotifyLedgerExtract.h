#pragma once

#include <Messages/Message.h>
#include <cctype>
#include <cstddef>
#include <cstdint>

// A bounded piece of the checkpoint's campaign ledger extract. Pieces are keyed
// by campaign and checkpoint, so a later checkpoint cannot complete an older one.
struct NotifyLedgerExtract final : ServerMessage
{
    static constexpr ServerOpcode Opcode = kNotifyLedgerExtract;
    static constexpr size_t MaxChunkBytes = 16 * 1024;
    static constexpr uint16_t MaxChunks = 256;

    NotifyLedgerExtract() : ServerMessage(Opcode) {}

    uint64_t CampaignId{};
    String CheckpointId;
    uint16_t ChunkIndex{};
    uint16_t ChunkCount{};
    String Data;

    void SerializeRaw(TiltedPhoques::Buffer::Writer& w) const noexcept override
    {
        w.WriteBits(CampaignId, 64);
        w.WriteBits(CheckpointId.size(), 8);
        for (unsigned char c : CheckpointId) w.WriteBits(c, 8);
        w.WriteBits(ChunkIndex, 16);
        w.WriteBits(ChunkCount, 16);
        w.WriteBits(Data.size(), 16);
        for (unsigned char c : Data) w.WriteBits(c, 8);
    }

    void DeserializeRaw(TiltedPhoques::Buffer::Reader& r) noexcept override
    {
        uint64_t value{};
        if (!r.ReadBits(CampaignId, 64) || !CampaignId ||
            !r.ReadBits(value, 8) || !value || value > 64)
        { m_valid = false; return; }
        CheckpointId.clear();
        for (size_t i = 0; i < value; ++i)
        {
            uint64_t byte{};
            if (!r.ReadBits(byte, 8)) { m_valid = false; return; }
            CheckpointId.push_back(static_cast<char>(byte));
        }
        if (!r.ReadBits(value, 16)) { m_valid = false; return; }
        ChunkIndex = static_cast<uint16_t>(value);
        if (!r.ReadBits(value, 16)) { m_valid = false; return; }
        ChunkCount = static_cast<uint16_t>(value);
        if (!r.ReadBits(value, 16) || !value || value > MaxChunkBytes)
        { m_valid = false; return; }
        Data.clear();
        for (size_t i = 0; i < value; ++i)
        {
            uint64_t byte{};
            if (!r.ReadBits(byte, 8)) { m_valid = false; return; }
            Data.push_back(static_cast<char>(byte));
        }
        m_valid = ChunkCount && ChunkCount <= MaxChunks && ChunkIndex < ChunkCount;
        for (unsigned char c : CheckpointId)
            m_valid = m_valid && (std::isalnum(c) || c == '_' || c == '-');
    }
};
