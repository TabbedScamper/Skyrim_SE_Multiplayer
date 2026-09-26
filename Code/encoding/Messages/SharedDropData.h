#pragma once

#include <Structs/Inventory.h>
#include <Structs/PhysicsReferenceUpdate.h>

enum class SharedDropAction : uint8_t
{
    Create, Pickup, Move, Snapshot, Ready, Upsert, Granted, Denied, Remove, Release, Local
};

// Shared drops use a separate identity namespace, never a client's FF form ID.
struct SharedDropData
{
    static constexpr uint32_t PhysicsModId = UINT32_MAX - 1;
    static constexpr size_t MaxEffects = 32;
    static constexpr size_t MaxName = 128;
    static constexpr float PickupDistance = 256.f;
    uint64_t Epoch{}, Token{}, OriginToken{}, Tick{};
    uint32_t Id{}, Generation{}, Owner{}, Creator{}, Replicas{};
    SharedDropAction Action{SharedDropAction::Snapshot};
    Inventory::Entry Item{};
    String Name{}, Winner{};
    // Presence matters: an explicitly empty charge is different from no extra charge.
    uint8_t ExtraMask{};
    GameId Cell{}, WorldSpace{};
    PhysicsReferenceUpdate Physics{};

    bool HasItem() const noexcept;
    bool HasPhysics() const noexcept;
    bool ValidPayload() const noexcept;
    void SerializeData(TiltedPhoques::Buffer::Writer&) const noexcept;
    void DeserializeData(TiltedPhoques::Buffer::Reader&);
};

// Used by the server and protocol tests. A terminal claim never returns to live.
struct SharedDropClaim
{
    bool Taken{};
    uint32_t Winner{};
    bool TryTake(uint32_t aPlayer, bool aInCell, bool aAlive, float aDistanceSquared) noexcept;
    bool CanMove(uint32_t aPlayer, uint32_t aOwner, uint32_t aGeneration, uint32_t aCurrentGeneration) const noexcept
    {
        return !Taken && aPlayer == aOwner && aGeneration && aGeneration == aCurrentGeneration;
    }
};
