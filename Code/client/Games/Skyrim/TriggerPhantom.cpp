#include <TiltedOnlinePCH.h>

#include <TriggerPhantom.h>
#include <Games/ActorExtension.h>
#include <Actor.h>
#include <PlayerCharacter.h>
#include <Events/EventDispatcher.h>
#include <FunctionHook.hpp>

#include <atomic>

namespace
{
std::atomic_bool s_hostAuthority{};
constexpr uint32_t kLayerMask = 0x7f;
constexpr uint32_t kTriggerLayer = 12;
constexpr uint32_t kControllerLayer = 30;
constexpr uint32_t kNoCollision = 1u << 14;

using FindRef = TESObjectREFR*(const void*);
POINTER_SKYRIMSE(FindRef, s_findRef, 26003);
using Filter = bool(void*, uint32_t, uint32_t);
POINTER_SKYRIMSE(Filter, s_filter, 78553);
using PairFilter = uint8_t*(void*, uint8_t*, const uint8_t*, const uint8_t*);
PairFilter* s_pairFilter{};
using Contact = bool(uint8_t*, const uint8_t*, TESObjectREFR**);
Contact* s_contact{};

bool IsRemotePlayer(TESObjectREFR* apRef)
{
    auto* pActor = Cast<Actor>(apRef);
    return pActor && pActor->GetExtension()->IsRemotePlayer();
}

// CommonLib hkpCollidable: ownerOffset +20, broadPhaseHandle.type +28,
// filterInfo +2c. Native 78548 (14107c570) delegates to 78553 (14107c870)
// with this-10. The latter rejects bit 14 before consulting trigger layers.
uint8_t* HookPairFilter(void* apFilter, uint8_t* apResult, const uint8_t* apA, const uint8_t* apB)
{
    if (s_hostAuthority.load(std::memory_order_acquire) && apA && apB)
    {
        auto a = *reinterpret_cast<const uint32_t*>(apA + 0x2c);
        auto b = *reinterpret_cast<const uint32_t*>(apB + 0x2c);
        const uint8_t* pController = nullptr;
        if ((a & kLayerMask) == kTriggerLayer && (b & kLayerMask) == kControllerLayer)
            pController = apB;
        else if ((b & kLayerMask) == kTriggerLayer && (a & kLayerMask) == kControllerLayer)
            pController = apA;
        if (pController && ((a | b) & kNoCollision) && IsRemotePlayer(s_findRef.Get()(pController)))
        {
            // Only clear the entrant's bit in arguments to the native test.
            // Never mutate a live Havok body or re-enable solid player collisions.
            if (pController == apA)
                a &= ~kNoCollision;
            else
                b &= ~kNoCollision;
            *apResult = s_filter.Get()(static_cast<uint8_t*>(apFilter) - 0x10, a, b);
            return apResult;
        }
    }
    return s_pairFilter(apFilter, apResult, apA, apB);
}

bool HookContact(uint8_t* apEntry, const uint8_t* apBody, TESObjectREFR** apTrigger)
{
    // 26038 (140406d80): alive actors' biped bodies are intentionally excluded;
    // use the real character-controller overlap, not mesh/AABB approximations.
    if (!s_hostAuthority.load(std::memory_order_acquire) || !apBody || !apTrigger || !*apTrigger ||
        (*reinterpret_cast<const uint32_t*>(apBody + 0x2c) & kLayerMask) != kControllerLayer)
        return s_contact(apEntry, apBody, apTrigger);
    auto* pEntrant = s_findRef.Get()(apBody);
    auto* pPlayer = PlayerCharacter::Get();
    if (!pPlayer || !IsRemotePlayer(pEntrant))
        return s_contact(apEntry, apBody, apTrigger);

    // Native activator keyword filtering (not IsPlayerRef). Evaluate the same
    // requirement against PlayerRef, consistent with the VM action-ref proxy.
    auto* pBase = (*apTrigger)->baseForm;
    if (pBase && pBase->formType == FormType::Activator)
    {
        using Keyword = void*(const void*);
        POINTER_SKYRIMSE(Keyword, keyword, 17699); // 140279fa0
        if (auto* pKeyword = keyword.Get()(pBase))
        {
            using HasKeyword = bool(TESObjectREFR*, const void*);
            const auto hasKeyword = reinterpret_cast<HasKeyword*>((*reinterpret_cast<void***>(pPlayer))[0x48]);
            if (!hasKeyword(pPlayer, pKeyword))
                return false;
        }
    }

    // Preserve native deduplication, enter/leave queue, and repeat interval.
    // 26041/140407110 marks existing contacts or queues one enter for new ones.
    // 26036/1404066c0 sweeps contacts and emits their leaves after this callback.
    using Register = bool(void*, TESObjectREFR*);
    POINTER_SKYRIMSE(Register, record, 26041);
    using Elapsed = float(const void*);
    POINTER_SKYRIMSE(Elapsed, elapsed, 26102); // 140408f80
    POINTER_SKYRIMSE(float, interval, 370100); // 14209e480, native repeat threshold
    if (!record.Get()(apEntry + 0x28, pEntrant) || elapsed.Get()(apEntry + 0x58) <= *interval.Get())
        return false;
    TESTriggerEvent event{*apTrigger, pEntrant};
    // Match native ownership across synchronous sinks; the VM then acquires
    // its own argument reference before queueing Papyrus execution.
    struct EventReferences
    {
        TESTriggerEvent& Event;
        ~EventReferences()
        {
            Event.pActionRef->handleRefObject.DecRefHandle();
            Event.pTrigger->handleRefObject.DecRefHandle();
        }
    } references{event};
    InterlockedIncrement(&event.pTrigger->handleRefObject.refCount);
    InterlockedIncrement(&event.pActionRef->handleRefObject.refCount);
    using Dispatch = void(void*, const TESTriggerEvent*);
    POINTER_SKYRIMSE(Dispatch, dispatch, 26106); // 140409480, native OnTrigger dispatcher
    dispatch.Get()(&EventDispatcherManager::Get()->triggerEvent, &event);
    return true;
}
}

namespace TriggerPhantom
{
void SetAuthority(bool aHost) noexcept { s_hostAuthority.store(aHost, std::memory_order_release); }

void Install()
{
    static bool installed{};
    if (installed)
        return;
    installed = true;
    POINTER_SKYRIMSE(PairFilter, pair, 78548);
    POINTER_SKYRIMSE(Contact, contact, 26038);
    s_pairFilter = pair.Get();
    s_contact = contact.Get();
    // Resolve shared relocations before physics workers enter the hooks.
    s_findRef.Get();
    s_filter.Get();
    TP_HOOK(&s_pairFilter, HookPairFilter);
    TP_HOOK(&s_contact, HookContact);
}
}
