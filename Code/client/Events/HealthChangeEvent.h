#pragma once

struct TESObjectREFR;

/**
 * @brief Dispatched when a local or remote actor's health changes locally.
 *
 * The exact hueristics for what hit is registered by who can be found in
 * the Actor::HookDamageActor() hook.
 */
struct HealthChangeEvent
{
    HealthChangeEvent(uint32_t aHitteeId, float aDeltaHealth, uint32_t aAttackerId = 0)
        : HitteeId(aHitteeId)
        , DeltaHealth(aDeltaHealth)
        , AttackerId(aAttackerId)
    {
    }

    uint32_t HitteeId;
    float DeltaHealth;
    uint32_t AttackerId;
};
