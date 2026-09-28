#include <Misc/BSFixedString.h>
#include <Games/Skyrim/TESObjectREFR.h>
#include <Services/CharacterService.h>
#include <Windows.h>

// Animation-graph props (the Helgen headsman's AnimObjectExecutionerAxe, ANIO 2E8E5). The AnimationObjects manager
// (singleton 400328) keeps one 0x28-byte entry per (reference handle, ANIO): handle +0x00, ANIO +0x08, attached node
// +0x10 (null until the async model load finishes), load task +0x18, draw-when-loaded byte +0x20; entries at
// manager +0x08, count +0x18, spinlock +0x20 (0x1407D7DC0 / 0x1407D8080). Load (43571) adds the entry and attaches
// the prop hidden, Draw (43572) clears the node's hidden flag (+0xF4 bit 0). The actor's idle clip fires them through
// AnimationObjectLoadHandler / AnimationObjectDrawHandler (42888 / 42889, which look the payload up by editor ID,
// 14618), and a save restores the entries directly. A remote copy follows the owner's streamed pose and never plays
// the clip, so the owner's props are relayed and applied here with the same two manager calls.
using THandleAnimObject = bool(void* apThis, TESObjectREFR* apReference, const BSFixedString* apPayload);
static THandleAnimObject* RealLoadAnimObject = nullptr;
static THandleAnimObject* RealDrawAnimObject = nullptr;

namespace
{
using TLookupByEditorId = TESForm*(const BSFixedString* apEditorId);
using TManagerCall = void(void* apManager, const uint32_t* apHandle, TESForm* apAnimObject);
constexpr uint8_t kAnimObjectFormType = 0x53;

TESForm* AnimObjectFromPayload(const BSFixedString* apPayload) noexcept
{
    POINTER_SKYRIMSE(TLookupByEditorId, lookup, 14618);
    auto* pForm = apPayload && apPayload->data && *apPayload->data ? lookup.Get()(apPayload) : nullptr;
    return pForm && static_cast<uint8_t>(pForm->formType) == kAnimObjectFormType ? pForm : nullptr;
}

void* Manager() noexcept
{
    POINTER_SKYRIMSE(void*, singleton, 400328);
    return *singleton.Get();
}
} // namespace

template <bool Draw> static bool HookAnimObject(void* apThis, TESObjectREFR* apReference, const BSFixedString* apPayload)
{
    const bool result = (Draw ? RealDrawAnimObject : RealLoadAnimObject)(apThis, apReference, apPayload);
    if (apReference)
        if (auto* pAnimObject = AnimObjectFromPayload(apPayload))
            CharacterService::QueueAnimObjectEvent(apReference->formID, pAnimObject->formID, Draw);
    return result;
}

// Main thread. aKind: 0 load, 1 load + draw, 2 detach (43577 matches the entry by the ANIO's unload event name,
// TESObjectANIO +0x60, fades the prop and queues the node detach).
void ApplyAnimObject(TESObjectREFR* apReference, TESForm* apAnimObject, uint8_t aKind) noexcept
{
    auto* pManager = Manager();
    if (!pManager || !apReference || !apAnimObject || static_cast<uint8_t>(apAnimObject->formType) != kAnimObjectFormType)
        return;
    const uint32_t handle = apReference->GetHandle().handle.iBits;
    if (!handle)
        return;
    if (aKind == 2)
    {
        using TDetach = void(void* apManager, const uint32_t* apHandle, const void* apUnloadEvent);
        POINTER_SKYRIMSE(TDetach, detach, 43577);
        detach.Get()(pManager, &handle, reinterpret_cast<const uint8_t*>(apAnimObject) + 0x60);
        return;
    }
    POINTER_SKYRIMSE(TManagerCall, load, 43571);
    POINTER_SKYRIMSE(TManagerCall, draw, 43572);
    load.Get()(pManager, &handle, apAnimObject); // no-op when the entry exists
    if (aKind == 1)
        draw.Get()(pManager, &handle, apAnimObject);
}

// The manager's entries for the given references: (reference form id, ANIO form id, drawn).
void CollectAnimObjects(const std::function<bool(uint32_t)>& acWanted,
    std::vector<std::tuple<uint32_t, uint32_t, bool>>& arOut) noexcept
{
    arOut.clear();
    auto* pManager = static_cast<uint8_t*>(Manager());
    if (!pManager)
        return;
    auto* pLock = reinterpret_cast<volatile long*>(pManager + 0x20);
    while (InterlockedCompareExchange(pLock, 1, 0) != 0)
        Sleep(0);
    std::vector<std::tuple<uint32_t, TESForm*, bool>> entries;
    const auto* pEntries = *reinterpret_cast<uint8_t**>(pManager + 0x08);
    const auto count = *reinterpret_cast<uint32_t*>(pManager + 0x18);
    for (uint32_t i = 0; pEntries && i < count && i < 256; ++i)
    {
        const auto* pEntry = pEntries + i * 0x28;
        const auto* pNode = *reinterpret_cast<const uint8_t* const*>(pEntry + 0x10);
        const bool drawn = pNode ? (*reinterpret_cast<const uint32_t*>(pNode + 0xF4) & 1) == 0 : pEntry[0x20] != 0;
        entries.emplace_back(*reinterpret_cast<const uint32_t*>(pEntry), *reinterpret_cast<TESForm* const*>(pEntry + 0x08),
            drawn);
    }
    InterlockedExchange(pLock, 0);
    for (const auto& [handle, pAnimObject, drawn] : entries)
    {
        auto* pReference = TESObjectREFR::GetByHandle(handle);
        if (pReference && pAnimObject && acWanted(pReference->formID))
            arOut.emplace_back(pReference->formID, pAnimObject->formID, drawn);
    }
}

static TiltedPhoques::Initializer s_animObjectHooks(
    []()
    {
        POINTER_SKYRIMSE(THandleAnimObject, s_load, 42888);
        POINTER_SKYRIMSE(THandleAnimObject, s_draw, 42889);
        RealLoadAnimObject = s_load.Get();
        RealDrawAnimObject = s_draw.Get();
        TP_HOOK(&RealLoadAnimObject, HookAnimObject<false>);
        TP_HOOK(&RealDrawAnimObject, HookAnimObject<true>);
    });
