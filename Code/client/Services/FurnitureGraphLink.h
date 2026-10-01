#pragma once

struct World;
struct UpdateEvent;

// Keeps an actor's animation graph linked to the animated furniture it uses.
//
// When an actor starts using furniture, AIProcess::SetSitSleepState (N39912, 0x1407246A0) stores the furniture handle
// in MiddleHigh+0x208 and adds the furniture's graph manager as a dependent of the actor's (N63364, 0x140BC0E60), so
// the actor's clip annotations reach the furniture: Alduin's "Open" opens the Helgen tower wall (6CF54), whose "Begin"
// sets MQ101DragonAttack stage 103. That link is made once. If either graph did not exist yet, or was rebuilt later
// (3D reload, cell reattach), nothing relinks it until a save is loaded, and the furniture silently ignores the actor:
// the tower wall never opened on the host in the 21:55 harness run (stage 50 -> 105, no 103). Source: SkyrimAtlas
// systems/animation-graph-and-furniture.md and for-coop/ANSWERS.md A1, A2, A4.
//
// Test tool, off by default (2026-10-01). It reports (furniture_links) and, only when a test turns the repair on
// (furniture_links_repair), relinks once a second with N63364 flag 0. What the tests showed: after loading the
// helgen_street2 checkpoint the link is missing on both PCs, but the engine links Alduin to the wall by itself before
// the smash; 12 of 12 checkpoint runs reached stage 103 with the repair on or off. The real failures (stage 103 never
// set, the escape scene moving on to 105) were only seen in full new-game runs and are not explained by this link.
// Muse refuted it as a shipped fix: it reads MiddleHigh and handle fields that worker-thread package ticks change, uses
// flag 0 where the sit path uses 1 (channel binding), and can leave a link behind when ClearFurniture (N39798) clears
// +0x208 without removing the dependent. Do not enable it outside tests.
struct FurnitureGraphLink
{
    FurnitureGraphLink(World& aWorld, entt::dispatcher& aDispatcher) noexcept;
    TP_NOCOPYMOVE(FurnitureGraphLink);

    // JSON: every actor using animated furniture now, with whether the link was present and whether it was repaired.
    [[nodiscard]] std::string Describe() noexcept;
    // Test control only: with the repair off the service still reports, so a run can show what happens without it.
    void SetRepairEnabled(bool aEnabled) noexcept { m_repairEnabled = aEnabled; }

private:
    void OnUpdate(const UpdateEvent&) noexcept;
    // Checks (and with aRepair, restores) every link; fills aJson with one entry per actor when given.
    uint32_t Reconcile(bool aRepair, std::string* aJson) noexcept;

    World& m_world;
    entt::scoped_connection m_updateConnection;
    uint64_t m_nextCheck{};
    uint32_t m_repairs{};
    bool m_repairEnabled{false};
};
