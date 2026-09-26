#include <Messages/BusyLockData.h>
#include <Structs/CheckedRead.h>

void BusyLockData::SerializeData(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    using TiltedPhoques::Serialization;
    Reference.Serialize(aWriter);
    Serialization::WriteVarInt(aWriter, Epoch);
    Serialization::WriteVarInt(aWriter, RequestId);
    Serialization::WriteVarInt(aWriter, static_cast<uint8_t>(Action));
    Serialization::WriteVarInt(aWriter, static_cast<uint8_t>(Kind));
    Serialization::WriteVarInt(aWriter, static_cast<uint8_t>(Reason));
    Serialization::WriteVarInt(aWriter, Holder.size());
    aWriter.WriteBytes(reinterpret_cast<const uint8_t*>(Holder.data()), Holder.size());
    Serialization::WriteVarInt(aWriter, HolderPlayerId);
}

void BusyLockData::DeserializeData(TiltedPhoques::Buffer::Reader& aReader)
{
    const auto base = CheckedRead::VarInt(aReader);
    const auto mod = CheckedRead::VarInt(aReader);
    Epoch = CheckedRead::VarInt(aReader);
    RequestId = CheckedRead::VarInt(aReader);
    const auto action = CheckedRead::VarInt(aReader);
    const auto kind = CheckedRead::VarInt(aReader);
    const auto reason = CheckedRead::VarInt(aReader);
    const auto length = CheckedRead::VarInt(aReader);
    if (base > UINT32_MAX || mod > UINT32_MAX || !RequestId ||
        action > static_cast<uint8_t>(BusyLockAction::Unavailable) ||
        kind > static_cast<uint8_t>(BusyLockKind::Bartering) ||
        reason > static_cast<uint8_t>(BusyLockReason::Disconnected) ||
        length > 80 || length > CheckedRead::RemainingBits(aReader) / 8)
        throw std::runtime_error("invalid busy lock packet");
    Reference = GameId(static_cast<uint32_t>(mod), static_cast<uint32_t>(base));
    if (!Reference)
        throw std::runtime_error("empty busy lock reference");
    Action = static_cast<BusyLockAction>(action);
    Kind = static_cast<BusyLockKind>(kind);
    Reason = static_cast<BusyLockReason>(reason);
    Holder.resize(static_cast<size_t>(length));
    CheckedRead::Bytes(aReader, reinterpret_cast<uint8_t*>(Holder.data()), Holder.size());
    const auto holderPlayerId = CheckedRead::VarInt(aReader);
    if (holderPlayerId > UINT32_MAX)
        throw std::runtime_error("invalid busy lock holder");
    HolderPlayerId = static_cast<uint32_t>(holderPlayerId);
}

bool BusyLockData::operator==(const BusyLockData& aOther) const noexcept
{
    return Reference == aOther.Reference && Epoch == aOther.Epoch && RequestId == aOther.RequestId &&
        Action == aOther.Action && Kind == aOther.Kind && Reason == aOther.Reason && Holder == aOther.Holder &&
        HolderPlayerId == aOther.HolderPlayerId;
}
