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
        // A leader that hands an actor back within 5 s of its grant (the native is not loaded there, e.g. a body the
        // leader's quest disabled) gets one retry, then none until a cell entry: re-arming on every bounce looped the
        // grant every ~28 s (beheaded Stormcloak 654ED, epochs 50 -> 79), each round stalling the snapshot stream.
        const bool quickBounce = aLeader && aRelinquish && LeaderGrantedAt && aTick >= LeaderGrantedAt && aTick - LeaderGrantedAt < 5000;
        if (aLeader && aRelinquish && !quickBounce)
            QuickLeaderBounces = 0;
        if (quickBounce)
            ++QuickLeaderBounces;
        if (aRelinquish)
            RetryLeader = !quickBounce || QuickLeaderBounces <= 1;
        else if (aLeader)
            RetryLeader = false;
    }

    void FinishGrant(bool aLeader, uint64_t aTick = 0)
    {
        if (aLeader)
            LeaderGrantedAt = aTick;
        Released = false;
        // A follower grant must not consume the releasing leader's one retry.
        if (aLeader)
            RetryLeader = false;
    }

    bool IsCurrentOwner(const Player* apPlayer, uint32_t aOwnershipEpoch) const noexcept
    {
        return apPlayer && GetOwner() == apPlayer && aOwnershipEpoch != 0 && OwnershipEpoch == aOwnershipEpoch;
    }

    uint64_t LeaderGrantedAt{};

    uint32_t QuickLeaderBounces{};

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
