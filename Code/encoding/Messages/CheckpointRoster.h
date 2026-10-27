#pragma once
#include <Messages/Message.h>
#include <Structs/GameId.h>
#include <bit>
#include <cmath>

// A checkpoint records one local player at the save barrier. The server binds the sender to its party.
struct RosterPosition
{
    uint64_t SteamId{};
    String CharacterKey;
    String CharacterName;
    GameId WorldSpaceId{};
    GameId CellId{};
    float X{}, Y{}, Z{}, Heading{};
    bool Downed{};

    bool Valid() const noexcept
    {
        return CharacterKey.size() <= 128 && CharacterName.size() <= 128 &&
            std::isfinite(X) && std::isfinite(Y) && std::isfinite(Z) && std::isfinite(Heading) &&
            (WorldSpaceId || CellId);
    }
    void Serialize(TiltedPhoques::Buffer::Writer& w) const noexcept
    {
        w.WriteBits(SteamId, 64);
        const auto write = [&w](const String& value)
        {
            w.WriteBits(value.size(), 8);
            for (unsigned char c : value) w.WriteBits(c, 8);
        };
        write(CharacterKey);
        write(CharacterName);
        WorldSpaceId.Serialize(w);
        CellId.Serialize(w);
        for (float value : {X, Y, Z, Heading}) w.WriteBits(std::bit_cast<uint32_t>(value), 32);
        w.WriteBits(Downed, 1);
    }
    bool Deserialize(TiltedPhoques::Buffer::Reader& r) noexcept
    {
        uint64_t value{};
        if (!r.ReadBits(SteamId, 64)) return false;
        const auto read = [&r, &value](String& out) -> bool
        {
            if (!r.ReadBits(value, 8) || value > 128) return false;
            out.clear();
            for (size_t i = 0, count = static_cast<size_t>(value); i < count; ++i)
            {
                if (!r.ReadBits(value, 8)) return false;
                out.push_back(static_cast<char>(value));
            }
            return true;
        };
        if (!read(CharacterKey) || !read(CharacterName)) return false;
        WorldSpaceId.Deserialize(r);
        CellId.Deserialize(r);
        float* floats[]{&X, &Y, &Z, &Heading};
        for (auto* item : floats)
        {
            if (!r.ReadBits(value, 32)) return false;
            *item = std::bit_cast<float>(static_cast<uint32_t>(value));
        }
        if (!r.ReadBits(value, 1)) return false;
        Downed = value != 0;
        return Valid();
    }
    bool operator==(const RosterPosition&) const = default;
};

struct RequestCheckpointRoster final : ClientMessage
{
    static constexpr ClientOpcode Opcode = kRequestCheckpointRoster;
    RequestCheckpointRoster() : ClientMessage(Opcode) {}
    String CheckpointId;
    RosterPosition Position;
    void SerializeRaw(TiltedPhoques::Buffer::Writer& w) const noexcept override
    {
        w.WriteBits(CheckpointId.size(), 8);
        for (unsigned char c : CheckpointId) w.WriteBits(c, 8);
        Position.Serialize(w);
    }
    void DeserializeRaw(TiltedPhoques::Buffer::Reader& r) noexcept override
    {
        uint64_t size{}, byte{};
        if (!r.ReadBits(size, 8) || size > 64) { m_valid = false; return; }
        CheckpointId.clear();
        for (size_t i = 0; i < size; ++i)
        {
            if (!r.ReadBits(byte, 8)) { m_valid = false; return; }
            CheckpointId.push_back(static_cast<char>(byte));
        }
        m_valid = !CheckpointId.empty() && Position.Deserialize(r);
    }
};

// Full roster is sent to every member after each member reports, for local sidecars.
struct NotifyCheckpointRoster final : ServerMessage
{
    static constexpr ServerOpcode Opcode = kNotifyCheckpointRoster;
    NotifyCheckpointRoster() : ServerMessage(Opcode) {}
    String CheckpointId;
    TiltedPhoques::Vector<RosterPosition> Entries;
    void SerializeRaw(TiltedPhoques::Buffer::Writer& w) const noexcept override
    {
        w.WriteBits(CheckpointId.size(), 8);
        for (unsigned char c : CheckpointId) w.WriteBits(c, 8);
        w.WriteBits(Entries.size(), 8);
        for (const auto& item : Entries) item.Serialize(w);
    }
    void DeserializeRaw(TiltedPhoques::Buffer::Reader& r) noexcept override
    {
        uint64_t size{}, byte{};
        if (!r.ReadBits(size, 8) || size > 64) { m_valid = false; return; }
        CheckpointId.clear();
        for (size_t i = 0; i < size; ++i)
        {
            if (!r.ReadBits(byte, 8)) { m_valid = false; return; }
            CheckpointId.push_back(static_cast<char>(byte));
        }
        if (!r.ReadBits(size, 8) || size > 16) { m_valid = false; return; }
        Entries.clear();
        for (size_t i = 0; i < size; ++i)
        {
            RosterPosition item;
            if (!item.Deserialize(r)) { m_valid = false; return; }
            Entries.push_back(std::move(item));
        }
        m_valid = !CheckpointId.empty();
    }
};
