#pragma once

#include <Messages/Message.h>
#include <Structs/Inventory.h>
#include <algorithm>

// Biped bits 0..31, right/left hand 32/33, ammunition 34. A multi-slot
// armor is one instance, not one copy per occupied bit.
struct NpcWornItem
{
    uint64_t Slots{};
    Inventory::Entry Item;
};

bool NpcSameItem(const Inventory::Entry& aLeft, const Inventory::Entry& aRight, bool aAppearance = false) noexcept;
bool NpcSameWorn(const Vector<NpcWornItem>& aLeft, const Vector<NpcWornItem>& aRight) noexcept;

enum class NpcWornActionKind { Unequip, AddRenderCopy, Equip };
struct NpcWornAction
{
    NpcWornActionKind Kind;
    NpcWornItem Worn;
};
// Pure policy. Never removes stock. Empty authoritative sets explicitly unequip.
Vector<NpcWornAction> PlanNpcWorn(const Vector<NpcWornItem>& aLocal, const Vector<NpcWornItem>& aOwner);

struct NpcWornData
{
    uint32_t ServerId{}, OwnershipEpoch{};
    uint64_t Sequence{}; // zero is a query, never an empty snapshot
    Vector<NpcWornItem> Items;
    bool Valid() const noexcept;
    void Serialize(TiltedPhoques::Buffer::Writer&) const noexcept;
    bool Deserialize(TiltedPhoques::Buffer::Reader&) noexcept;
};

// A future-epoch snapshot can arrive before the ECS ownership notification.
// Keep it intact until that epoch becomes current; never reset it to an older one.
bool NpcWornNewer(const NpcWornData& aCurrent, const NpcWornData& aIncoming) noexcept;

enum class NpcLootOp : uint8_t { Fetch, Transfer, FetchResult, TransferResult };
struct NpcLootData
{
    NpcLootOp Op{NpcLootOp::Fetch};
    GameId Reference{}; // canonical busy-lock reference, including dynamic server IDs
    uint32_t ServerId{}, OwnershipEpoch{}, Peer{};
    uint64_t Session{}, Lease{}, Token{};
    bool Accepted{}, Barter{}, ContentsKnown{};
    int32_t Gold{}; // signed owner gold change, opposite of item direction
    Inventory::Entry Item; // signed owner change: negative take, positive give
    Inventory Contents;
    bool Valid() const noexcept;
    void Serialize(TiltedPhoques::Buffer::Writer&) const noexcept;
    bool Deserialize(TiltedPhoques::Buffer::Reader&) noexcept;
};
