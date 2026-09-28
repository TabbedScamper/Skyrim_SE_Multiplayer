#include "Projectile.h"
#include <Games/Skyrim/Forms/TESObjectWEAP.h>
#include <Games/Skyrim/Forms/MagicItem.h>
#include <Games/Skyrim/Forms/TESAmmo.h>
#include <Actor.h>
#include <Games/ActorExtension.h>
#include <World.h>
#include <Events/ProjectileLaunchedEvent.h>
#include <Games/Skyrim/Forms/TESObjectCELL.h>
#include <Forms/SpellItem.h>
#include <Services/CombatService.h>
#include <Services/ObjectService.h>

TP_THIS_FUNCTION(TLaunch, BSPointerHandle<Projectile>*, BSPointerHandle<Projectile>, Projectile::LaunchData& arData);
static TLaunch* RealLaunch = nullptr;

BSPointerHandle<Projectile>* Projectile::Launch(BSPointerHandle<Projectile>* apResult, LaunchData& apLaunchData) noexcept
{
    BSPointerHandle<Projectile>* result = TiltedPhoques::ThisCall(RealLaunch, apResult, apLaunchData);

    TP_ASSERT(result, "No projectile handle returned.");
    if (!result)
    {
        spdlog::error("No projectile handle returned.");
        return nullptr;
    }

    TESObjectREFR* pObject = TESObjectREFR::GetByHandle(result->handle.iBits);
    Projectile* pProjectile = Cast<Projectile>(pObject);

    TP_ASSERT(pProjectile, "No projectile found.");
    if (!pProjectile)
    {
        spdlog::error("No projectile found.");
        return nullptr;
    }

    pProjectile->fPower = apLaunchData.fPower;

    return result;
}

thread_local bool t_replayingLaunch{};

void SetReplayingLaunch(const bool aReplaying) noexcept
{
    t_replayingLaunch = aReplaying;
}

BSPointerHandle<Projectile>* TP_MAKE_THISCALL(HookLaunch, BSPointerHandle<Projectile>, Projectile::LaunchData& arData)
{
    // sync concentration spells through spell cast sync, the rest through projectile sync
    if (arData.pSpell)
    {
        if (auto* pSpell = Cast<SpellItem>(arData.pSpell))
        {
            if (pSpell->eCastingType == MagicSystem::CastingType::CONCENTRATION)
            {
                return TiltedPhoques::ThisCall(RealLaunch, apThis, arData);
            }
        }
    }

    // Launch probe (Helgen: Alduin's meteors were never relayed; the shooter is not a networked local actor): log who
    // launches what, per shooter/projectile pair once.
    {
        static std::mutex s_launchLogLock;
        static std::unordered_set<uint64_t> s_launchLogged;
        const uint32_t shooterId = arData.pShooter ? arData.pShooter->formID : 0;
        const uint32_t baseId = arData.pProjectileBase ? arData.pProjectileBase->formID : 0;
        std::lock_guard lock(s_launchLogLock);
        if (s_launchLogged.size() < 256 && s_launchLogged.insert((uint64_t{shooterId} << 32) | baseId).second)
        {
            auto* pShooterActor = arData.pShooter ? Cast<Actor>(arData.pShooter) : nullptr;
            spdlog::info("Projectile launch: shooter {:X} (type {:X}, actor {}, remote {}) projectile {:X} spell {:X} weapon {:X}",
                shooterId, arData.pShooter ? static_cast<uint32_t>(arData.pShooter->formType) : 0, pShooterActor != nullptr,
                pShooterActor && pShooterActor->GetExtension() && pShooterActor->GetExtension()->IsRemote(), baseId,
                arData.pSpell ? arData.pSpell->formID : 0, arData.pFromWeapon ? arData.pFromWeapon->formID : 0);
        }
    }

    // World caster on a follower near the leader: remember the caster for replays and do not launch our own.
    if (arData.pShooter && !Cast<Actor>(arData.pShooter) && arData.pShooter->IsTemporary() && !t_replayingLaunch &&
        CombatService::FollowerDefersWorldProjectiles())
    {
        if (arData.pSpell)
            CombatService::RememberWorldCaster(arData.pSpell->formID, arData.pShooter->formID);
        apThis->handle.iBits = 0;
        return apThis;
    }

    if (arData.pShooter)
    {
        Actor* pActor = Cast<Actor>(arData.pShooter);
        if (pActor)
        {
            ActorExtension* pExtendedActor = pActor->GetExtension();
            if (pExtendedActor->IsRemote())
            {
                apThis->handle.iBits = 0;
                return apThis;
            }
        }
    }

    ProjectileLaunchedEvent Event{};
    Event.Origin = arData.Origin;
    if (arData.pProjectileBase)
        Event.ProjectileBaseID = arData.pProjectileBase->formID;
    if (arData.pShooter)
        Event.ShooterID = arData.pShooter->formID;
    if (arData.pFromWeapon)
        Event.WeaponID = arData.pFromWeapon->formID;
    if (arData.pFromAmmo)
        Event.AmmoID = arData.pFromAmmo->formID;
    Event.ZAngle = arData.fZAngle;
    Event.XAngle = arData.fXAngle;
    Event.YAngle = arData.fYAngle;
    if (arData.pParentCell)
        Event.ParentCellID = arData.pParentCell->formID;
    if (arData.pSpell)
        Event.SpellID = arData.pSpell->formID;
    Event.CastingSource = arData.eCastingSource;
    Event.UnkBool1 = arData.bUnkBool1;
    Event.Area = arData.iArea;
    Event.Power = arData.fPower;
    Event.Scale = arData.fScale;
    Event.AlwaysHit = arData.bAlwaysHit;
    Event.NoDamageOutsideCombat = arData.bNoDamageOutsideCombat;
    Event.AutoAim = arData.bAutoAim;
    Event.UnkBool2 = arData.bUnkBool2;
    Event.DeferInitialization = arData.bDeferInitialization;
    Event.ForceConeOfFire = arData.bForceConeOfFire;

    auto result = TiltedPhoques::ThisCall(RealLaunch, apThis, arData);

    TP_ASSERT(result, "No projectile handle returned.");

    TESObjectREFR* pObject = TESObjectREFR::GetByHandle(result->handle.iBits);
    Projectile* pProjectile = Cast<Projectile>(pObject);

    TP_ASSERT(pProjectile, "No projectile found.");

    Event.Power = pProjectile->fPower;

    World::Get().GetRunner().Trigger(Event);

    return result;
}

// Hazard::Create (43954 / 0x1407EDAB0) input, read from the function: base at +0x00, cell +0x08, position +0x10,
// impact normal +0x1C, actor cause (first dword: the owner actor handle) +0x38, lifetime +0x40, radius +0x44,
// ignore-spawn-interval +0x4C, permanent +0x4D. Returns nullptr when the global minimum spawn interval
// (fHazardMinimumSpawnInterval) or the per-base limit refuses it. Probe: the meteor debris (IPCT D07C1 ->
// HAZD D07BC FXHavokRockHazard, 10 dirt-clod rigid bodies) does not line up between PCs.
using THazardCreate = void*(void* apParams);
static THazardCreate* RealHazardCreate = nullptr;

static void* HookHazardCreate(void* apParams)
{
    void* pResult = RealHazardCreate(apParams);
    if (pResult)
        ObjectService::OnHazardCreated(static_cast<TESObjectREFR*>(pResult)->formID);
    static std::atomic<uint32_t> s_logs{};
    if (apParams && s_logs.fetch_add(1, std::memory_order_relaxed) < 400)
    {
        const auto* p = static_cast<const uint8_t*>(apParams);
        const auto* pBase = *reinterpret_cast<TESForm* const*>(p);
        const auto* pos = reinterpret_cast<const float*>(p + 0x10);
        const auto* normal = reinterpret_cast<const float*>(p + 0x1C);
        const auto* pCause = *reinterpret_cast<const uint32_t* const*>(p + 0x38);
        const auto owner = pCause ? *pCause : 0u;
        auto* pRef = static_cast<TESObjectREFR*>(pResult);
        spdlog::info("Hazard create: base {:X} at ({:.0f}, {:.0f}, {:.0f}) normal ({:.2f}, {:.2f}, {:.2f}) owner handle {:X} "
                     "lifetime {:.2f} radius {:.1f} force {} permanent {} caller {:X} -> {} {:X}",
            pBase ? pBase->formID : 0, pos[0], pos[1], pos[2], normal[0], normal[1], normal[2], owner,
            *reinterpret_cast<const float*>(p + 0x40), *reinterpret_cast<const float*>(p + 0x44), p[0x4C], p[0x4D],
            reinterpret_cast<uintptr_t>(_ReturnAddress()), pRef ? "created" : "refused", pRef ? pRef->formID : 0);
    }
    return pResult;
}

static TiltedPhoques::Initializer s_projectileHooks(
    []()
    {
        POINTER_SKYRIMSE(TLaunch, s_launch, 44108);

        RealLaunch = s_launch.Get();

        TP_HOOK(&RealLaunch, HookLaunch);

        POINTER_SKYRIMSE(THazardCreate, s_hazardCreate, 43954);
        RealHazardCreate = s_hazardCreate.Get();
        TP_HOOK(&RealHazardCreate, HookHazardCreate);

        VersionDbPtr<uint8_t> hookLoc(34452);

        struct C : TiltedPhoques::CodeGenerator
        {
            C(uint8_t* apLoc)
            {
                // replicate
                mov(rbx, ptr[rsp + 0x50]);

                // nullptr check
                cmp(rbx, 0);
                jz("exit");
                // jump back
                jmp_S(apLoc + 0x379);

                L("exit");
                // return false; scratch space from the registers
                mov(al, 0);
                add(rsp, 0x138);
                pop(r15);
                pop(r14);
                pop(r13);
                pop(r12);
                pop(rdi);
                pop(rsi);
                pop(rbx);
                pop(rbp);
                ret();
            }
        } gen(hookLoc.Get());
        TiltedPhoques::Jump(hookLoc.Get() + 0x374, gen.getCode());
    });
