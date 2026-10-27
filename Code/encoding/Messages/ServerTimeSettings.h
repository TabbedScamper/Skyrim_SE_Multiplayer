#pragma once

#include "Message.h"
#include "Structs/TimeModel.h"

struct ServerTimeSettings final : ServerMessage
{
    static constexpr ServerOpcode Opcode = kServerTimeSettings;

    ServerTimeSettings()
        : ServerMessage(Opcode)
    {
    }

    void SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept override;
    void DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept override;

    bool operator==(const ServerTimeSettings& achRhs) const noexcept
    {
        return GetOpcode() == achRhs.GetOpcode() && timeModel == achRhs.timeModel &&
            GameMs == achRhs.GameMs && SampleTick == achRhs.SampleTick && Seq == achRhs.Seq &&
            ServerNonce == achRhs.ServerNonce;
    }

    bool operator!=(const ServerTimeSettings& achRhs) const noexcept
    {
        return !this->operator==(achRhs);
    }

    TimeModel timeModel;
    uint64_t GameMs{};
    uint64_t SampleTick{};
    uint32_t Seq{};
    uint64_t ServerNonce{};
};
