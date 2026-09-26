#pragma once

#include <Structs/GameId.h>

enum class BusyLockAction : uint8_t { Acquire, Heartbeat, Release, Granted, Denied, Unavailable };
enum class BusyLockKind : uint8_t { Speaking, Searching, Bartering };
enum class BusyLockReason : uint8_t { Closed, Cancelled, Timeout, Load, Death, PartyLeft, Disconnected };

struct BusyLockData
{
    // Static references use their server mod ID. UINT32_MAX denotes a server entity,
    // never a client-local FF form ID. All interaction kinds share this key.
    GameId Reference{};
    uint64_t Epoch{};
    uint64_t RequestId{};
    BusyLockAction Action{BusyLockAction::Acquire};
    BusyLockKind Kind{BusyLockKind::Speaking};
    BusyLockReason Reason{BusyLockReason::Closed};
    TiltedPhoques::String Holder;
    // The holder's player id (for a second player who then listens in on the conversation).
    uint32_t HolderPlayerId{};

    void SerializeData(TiltedPhoques::Buffer::Writer& aWriter) const noexcept;
    void DeserializeData(TiltedPhoques::Buffer::Reader& aReader);
    bool operator==(const BusyLockData& aOther) const noexcept;
};
