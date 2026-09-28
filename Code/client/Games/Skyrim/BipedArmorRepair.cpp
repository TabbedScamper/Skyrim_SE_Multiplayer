#include <TiltedOnlinePCH.h>
#include <BipedArmorRepair.h>
#include <Actor.h>
#include <NetImmerse/NiAVObject.h>
#include <NetImmerse/NiNode.h>

namespace
{
// Native BIPOBJECT/BipedAnim layout, verified against 15676/15678/15711.
// These views do not own the native smart pointers or resource handles.
struct Part
{
    TESForm* Item;
    void* Addon;
    void* Model;
    void* Texture;
    NiAVObject* Clone;
    uint8_t Resources[0x40];
    bool Skinned;
    uint8_t Tail[0xF];
};
constexpr uint32_t cSlots = 42;
struct Biped
{
    uint64_t RefCount;
    NiAVObject* Root;
    Part Current[cSlots];
    Part Buffered[cSlots];
    uint32_t OwnerHandle;
    uint32_t Padding;
};
static_assert(sizeof(Part) == 0x78 && offsetof(Part, Skinned) == 0x68);
static_assert(offsetof(Biped, Root) == 8 && offsetof(Biped, Buffered) == 0x13C0);
static_assert(offsetof(Biped, OwnerHandle) == 0x2770 && sizeof(Biped) == 0x2778);

using TRemovePart = void(Biped*, Part*, uint8_t, int32_t);

// Parent links are not owned by this observer. A queued native detach can
// change them on another thread. Unreadable data is unknown, never detached.
template <class T> bool ReadAt(const void* apBase, size_t aOffset, T& arValue) noexcept
{
    const auto base = reinterpret_cast<uintptr_t>(apBase);
    SIZE_T copied{};
    return base && aOffset <= UINTPTR_MAX - base &&
        ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(base + aOffset),
            &arValue, sizeof(T), &copied) && copied == sizeof(T);
}

// Hold the same native owner reference as LoadParts/CompletePart themselves.
struct OwnerReference
{
    TESObjectREFR* Reference{};
    explicit OwnerReference(uint32_t aHandle)
    {
        using TLookup = void(uint32_t&, TESObjectREFR*&);
        POINTER_SKYRIMSE(TLookup, lookup, 17201);
        lookup.Get()(aHandle, Reference);
    }
    ~OwnerReference()
    {
        if (Reference)
            Reference->handleRefObject.DecRefHandle();
    }
};

enum class Ancestry { Attached, Detached, Unknown };
Ancestry InTree(const NiAVObject* apNode, const NiAVObject* apRoot) noexcept
{
    if (!apRoot)
        return Ancestry::Unknown;
    for (uint32_t depth = 0; apNode && depth < 64; ++depth)
    {
        if (apNode == apRoot)
            return Ancestry::Attached;
        const NiAVObject* parent{};
        if (!ReadAt(apNode, offsetof(NiAVObject, parent), parent))
            return Ancestry::Unknown;
        apNode = parent;
    }
    // A depth limit is not evidence that a part belongs to an obsolete tree.
    return apNode ? Ancestry::Unknown : Ancestry::Detached;
}

bool SameLoad(Biped* apBiped, const Biped& aView, Actor* apActor, NiAVObject* apTree)
{
    NiAVObject* root{};
    uint32_t handle{};
    return ReadAt(apBiped, offsetof(Biped, Root), root) && root == aView.Root &&
        ReadAt(apBiped, offsetof(Biped, OwnerHandle), handle) && handle == aView.OwnerHandle &&
        apActor->GetNiNode() == apTree;
}

}

void TraceSpawnedCopyArmor(void* apData, const char* apPhase, uint64_t aRebuiltSlots)
{
    auto* apBiped = static_cast<Biped*>(apData);
    Biped view;
    if (!ReadAt(apBiped, 0, view))
        return;
    OwnerReference owner(view.OwnerHandle);
    auto* actor = Cast<Actor>(owner.Reference);
    if (!actor || !actor->IsTemporary())
        return;
    auto* root = actor->GetNiNode();
    auto* clone = view.Current[2].Clone;
    auto ancestry = InTree(view.Root, root) == Ancestry::Attached ?
        InTree(clone, root) : Ancestry::Unknown;
    NiAVObject* cloneAgain{};
    if (!SameLoad(apBiped, view, actor, root) ||
        !ReadAt(apBiped, offsetof(Biped, Current) + 2 * sizeof(Part) + offsetof(Part, Clone), cloneAgain) ||
        cloneAgain != clone)
        ancestry = Ancestry::Unknown;
    spdlog::info("Spawned copy {:X}: body attached {} phase={} ancestry={} rebuiltSlots={:X} "
        "actorRoot={} bipedRoot={} clone={} nonAtomic=1", actor->formID, ancestry == Ancestry::Attached,
        apPhase, static_cast<uint32_t>(ancestry), aRebuiltSlots, static_cast<void*>(root),
        static_cast<void*>(view.Root), static_cast<void*>(clone));
}

uint64_t PrepareBipedArmorLoad(void* apData)
{
    auto* apBiped = static_cast<Biped*>(apData);
    Biped view;
    if (!ReadAt(apBiped, 0, view))
        return 0;
    OwnerReference owner(view.OwnerHandle);
    auto* actor = Cast<Actor>(owner.Reference);
    uint64_t rebuiltSlots{};
    // 19727 publishes Set3D or the loader's TLS root before 24740 -> 15678.
    // GetNiNode (19735) honors that TLS root. Require the biped to belong to
    // this exact tree, not a shared scene ancestor or an older actor root.
    auto* tree = actor ? actor->GetNiNode() : nullptr;
    if (tree && InTree(view.Root, tree) == Ancestry::Attached)
    {
        // 15676 has already rotated old parts into Buffered and populated the
        // requested Current descriptors. 15678 otherwise accepts ANY nonnull
        // clone parent, even one under the skeleton replaced by 15657.
        for (uint32_t slot = 0; slot < cSlots; ++slot)
        {
            const auto& cached = view.Buffered[slot];
            const auto& requested = view.Current[slot];
            FormType type{};
            NiAVObject* parent{};
            if (!cached.Item || !cached.Clone || !requested.Model ||
                requested.Item != cached.Item || requested.Model != cached.Model ||
                !ReadAt(cached.Item, offsetof(TESForm, formType), type) || type != FormType::Armor ||
                !ReadAt(cached.Clone, offsetof(NiAVObject, parent), parent) || !parent ||
                InTree(cached.Clone, tree) != Ancestry::Detached)
                continue;

            // Recheck the exact entries before mutation. Guarded reads do not
            // lock the scene or prove an atomic snapshot; use the native loader
            // lifetime/scheduling contract, and abandon observed load changes.
            Part cachedAgain, requestedAgain;
            NiAVObject* parentAgain{};
            if (!SameLoad(apBiped, view, actor, tree) ||
                !ReadAt(apBiped, offsetof(Biped, Buffered) + slot * sizeof(Part), cachedAgain) ||
                !ReadAt(apBiped, offsetof(Biped, Current) + slot * sizeof(Part), requestedAgain) ||
                memcmp(&cached, &cachedAgain, sizeof(Part)) != 0 ||
                memcmp(&requested, &requestedAgain, sizeof(Part)) != 0 ||
                !ReadAt(cached.Clone, offsetof(NiAVObject, parent), parentAgain) || parentAgain != parent ||
                InTree(view.Root, tree) != Ancestry::Attached ||
                InTree(cached.Clone, tree) != Ancestry::Detached)
                continue;

            // Native teardown of the obsolete cached part only. Current item,
            // addon, model and pending source handles stay intact. Original
            // LoadParts then clones and skins through 15711/15712 (or async
            // 15701 -> 15711/15712) using the new biped root. Queued detach
            // retains the OLD clone pointer.
            POINTER_SKYRIMSE(TRemovePart, removePart, 15661);
            auto* nativePart = reinterpret_cast<Part*>(reinterpret_cast<uintptr_t>(apBiped) +
                offsetof(Biped, Buffered) + slot * sizeof(Part));
            removePart.Get()(apBiped, nativePart, true, 0);
            rebuiltSlots |= uint64_t{1} << slot;
        }
    }
    return rebuiltSlots;
}
