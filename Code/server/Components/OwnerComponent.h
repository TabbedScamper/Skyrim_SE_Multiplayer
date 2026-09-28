#pragma once

#ifndef TP_INTERNAL_COMPONENTS_GUARD
#error Include Components.h instead
#endif

struct Player;
struct OwnerComponent
{
    OwnerComponent(Player* apPlayer, uint32_t aOwnershipEpoch = 1)
        : pOwner(apPlayer)
        , OwnershipEpoch(aOwnershipEpoch)
    {
    }

    Player* GetOwner() const { return reinterpret_cast<Player*>(pOwner); }

    void SetOwner(Player* apPlayer) { pOwner = apPlayer; }

    void RecordRelease(const Player* apPlayer, bool aRelinquish, uint64_t aTick, bool aLeader = false)
    {
        if (std::find(InvalidOwners.begin(), InvalidOwners.end(), apPlayer) == InvalidOwners.end())
            InvalidOwners.push_back(apPlayer);
        Released = true;
        ReleasedAt = aTick;
        if (aRelinquish)
            RetryLeader = true;
        else if (aLeader)
            RetryLeader = false;
    }

    void FinishGrant(bool aLeader)
    {
        Released = false;
        // A follower grant must not consume the releasing leader's one retry.
        if (aLeader)
            RetryLeader = false;
    }

    bool IsCurrentOwner(const Player* apPlayer, uint32_t aOwnershipEpoch) const noexcept
    {
        return apPlayer && GetOwner() == apPlayer && aOwnershipEpoch != 0 && OwnershipEpoch == aOwnershipEpoch;
    }

    Player* pOwner;
    uint32_t OwnershipEpoch;
    Vector<const Player*> InvalidOwners{};
    std::optional<uint32_t> PartyId;
    uint64_t PartyEpoch{};
    uint32_t LastOwnerId{};
    uint64_t ReleasedAt{};
    bool Released{};
    bool RetryLeader{};
};
