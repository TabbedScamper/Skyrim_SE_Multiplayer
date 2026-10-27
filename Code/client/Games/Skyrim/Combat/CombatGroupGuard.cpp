#include <TiltedOnlinePCH.h>

// CombatGroup::AddTarget (44704, 0x140818870) called on no group (2026-10-02, twice: Helgen on a combat job thread,
// and the lobby on the main thread right after a race switch). PlayerCharacter's combat update (41261, 0x140779090)
// creates the player's group, stores it at player+0x9E0, adds the player as a member and reads +0x9E0 back; it came back
// empty, and AddTarget locked null+0x160. 41238 (also run by Job_Combat_manager) and 41239 clear that field. Until the
// writer is pinned down, the one call is skipped: the player simply gains no target this update, and the next update
// creates the group again. Each skip is logged.
struct CombatGroup;
struct Actor;

namespace
{
TP_THIS_FUNCTION(TAddTarget, bool, CombatGroup, Actor*);
TAddTarget* RealAddTarget{};
std::atomic<uint32_t> s_skipped{};

bool TP_MAKE_THISCALL(HookAddTarget, CombatGroup, Actor* apTarget)
{
    if (!apThis)
    {
        const auto count = ++s_skipped;
        if (count <= 20 || count % 100 == 0)
            spdlog::warn("Combat: AddTarget on no combat group skipped (target {}, thread {}, {} so far)",
                fmt::ptr(apTarget), GetCurrentThreadId(), count);
        return false;
    }
    return TiltedPhoques::ThisCall(RealAddTarget, apThis, apTarget);
}

TiltedPhoques::Initializer s_combatGroupGuard(
    []()
    {
        POINTER_SKYRIMSE(TAddTarget, s_addTarget, 44704);
        RealAddTarget = s_addTarget.Get();
        TP_HOOK(&RealAddTarget, HookAddTarget);
    });
} // namespace
