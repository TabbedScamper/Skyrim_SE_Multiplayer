#pragma once
#include <Messages/WorldState.h>

struct RequestWorldStateCell final : ClientMessage
{
    static constexpr ClientOpcode Opcode = kRequestWorldStateCell;
    RequestWorldStateCell() : ClientMessage(Opcode) {}
    void SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept override
    {
        aWriter.WriteBits(Epoch, 64);
        aWriter.WriteBits(Cell.ModId, 32);
        aWriter.WriteBits(Cell.BaseId, 32);
    }
    void DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept override
    {
        uint64_t mod{}, base{};
        m_valid = aReader.ReadBits(Epoch, 64) && aReader.ReadBits(mod, 32) && aReader.ReadBits(base, 32);
        Cell = GameId{static_cast<uint32_t>(mod), static_cast<uint32_t>(base)};
        m_valid = m_valid && Epoch && base && base <= 0xFFFFFF && mod != UINT32_MAX;
    }
    bool operator==(const RequestWorldStateCell& aOther) const noexcept { return Epoch == aOther.Epoch && Cell == aOther.Cell; }
    uint64_t Epoch{};
    GameId Cell{};
};
