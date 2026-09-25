#pragma once

struct CombatTargetSelector;
struct CombatGroup;
struct CombatState;
struct CombatInventory;
struct CombatAimController;
struct CombatTargetSelector;

struct CombatController
{
    struct TargetAuthorityTrialDiagnostics
    {
        uint32_t ActorFormId{};
        uint64_t Calls{};
        uint64_t Overrides{};
        uint32_t LastRequestedFormId{};
        uint32_t LastPresentedFormId{};
        uint32_t LastNativeFormId{};
        uintptr_t LastCallerRva{};
    };

    static void SetTargetAuthorityTrialActor(uint32_t aActorFormId) noexcept;
    static TargetAuthorityTrialDiagnostics GetTargetAuthorityTrialDiagnostics() noexcept;
    void SetTarget(Actor* apTarget);
    void UpdateTarget();

    CombatGroup *pCombatGroup;
    CombatState *pState;
    CombatInventory *pInventory;
    void *pCombatBlackboard;
    void *pBehaviorController;
    uint32_t attackerHandle;
    uint32_t targetHandle;
    uint32_t previousTargetHandle;
    uint8_t unk34;
    bool startedCombat;
    uint8_t unk36;
    uint8_t unk37;
    TESCombatStyle *pCombatStyle;
    bool stoppedCombat;
    bool unk41;
    bool ignoringCombat;
    bool inactive;
    float unk44;
    float unk4C;
    GameArray<void*> aimControllers;
    uint64_t aimControllerLock;
    CombatAimController *pCurrentAimController;
    CombatAimController *pPreviousAimController;
    GameArray<void*> areas;
    CombatAreaStandard *pCurrentArea;
    GameArray<CombatTargetSelector*> targetSelectors;
    CombatTargetSelector *pActiveTargetSelector;
    CombatTargetSelector *pPreviousTargetSelector;
    uint32_t handleCount;
    int32_t unkCC;
    NiPointer<Actor> pCachedAttacker;
    NiPointer<Actor> pCachedTarget;
};

static_assert(offsetof(CombatController, targetHandle) == 0x2C);
static_assert(offsetof(CombatController, startedCombat) == 0x35);
static_assert(offsetof(CombatController, targetSelectors) == 0xA0);
static_assert(offsetof(CombatController, pActiveTargetSelector) == 0xB8);
static_assert(sizeof(CombatController) == 0xE0);
