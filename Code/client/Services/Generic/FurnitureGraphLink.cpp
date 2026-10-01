#include <Services/FurnitureGraphLink.h>

#include <World.h>
#include <Events/UpdateEvent.h>
#include <Games/TES.h>
#include <Actor.h>
#include <AI/AIProcess.h>
#include <BSAnimationGraphManager.h>

namespace
{
constexpr uint64_t kCheckEveryMs = 1000;
// MiddleHigh+0x208: the furniture the actor uses (handle); written by SetSitSleepState N39912 and cleared by
// AIProcess::ClearFurniture N39798 and every demotion (SkyrimAtlas ai-process P9).
constexpr size_t kOccupiedFurniture = 0x208;
// BSAnimationGraphManager dependents: BSTArray at +0x58, count at +0x68, pointers tagged in bit 0 (N63364 reads them).
constexpr size_t kDependentCount = 0x68;
constexpr uint8_t kFurnitureFormType = 40;

struct Manager
{
    explicit Manager(TESObjectREFR* apRef) noexcept { apRef->animationGraphHolder.GetBSAnimationGraph(&Value); }
    ~Manager() noexcept
    {
        if (Value)
            Value->Release();
    }
    TP_NOCOPYMOVE(Manager);
    BSAnimationGraphManager* Value{};
};

uint32_t DependentCount(BSAnimationGraphManager* apManager) noexcept
{
    return *reinterpret_cast<volatile uint32_t*>(reinterpret_cast<uint8_t*>(apManager) + kDependentCount);
}

// Whether apDependent is already among apManager's dependents (probe only; the add checks this itself under its lock).
bool HasDependent(BSAnimationGraphManager* apManager, BSAnimationGraphManager* apDependent) noexcept
{
    auto* pBase = reinterpret_cast<uint8_t*>(apManager);
    const auto count = DependentCount(apManager);
    const auto* pData = *reinterpret_cast<uintptr_t* volatile*>(pBase + 0x58);
    if (!pData || count > 64)
        return false;
    for (uint32_t i = 0; i < count; ++i)
        if ((pData[i] & ~uintptr_t{1}) == reinterpret_cast<uintptr_t>(apDependent))
            return true;
    return false;
}

// N63364: adds apDependent to apManager's dependents unless already there; true when present afterwards. The flag 0
// matches Papyrus AddDependentAnimatedObjectReference (N56144), without its save-persisted extra data.
bool AddDependent(BSAnimationGraphManager* apManager, BSAnimationGraphManager* apDependent) noexcept
{
    using TAdd = bool(BSAnimationGraphManager*, BSAnimationGraphManager**, bool);
    POINTER_SKYRIMSE(TAdd, s_add, 63364);
    return s_add.Get()(apManager, &apDependent, false);
}
} // namespace

FurnitureGraphLink::FurnitureGraphLink(World& aWorld, entt::dispatcher& aDispatcher) noexcept
    : m_world(aWorld)
{
    m_updateConnection = aDispatcher.sink<UpdateEvent>().connect<&FurnitureGraphLink::OnUpdate>(this);
}

void FurnitureGraphLink::OnUpdate(const UpdateEvent&) noexcept
{
    // Off unless a test turns it on (see the header): Muse found worker-thread races, and the engine relinked by itself.
    if (!m_repairEnabled)
        return;
    const auto now = GetTickCount64();
    if (now < m_nextCheck)
        return;
    m_nextCheck = now + kCheckEveryMs;
    Reconcile(m_repairEnabled, nullptr);
}

std::string FurnitureGraphLink::Describe() noexcept
{
    std::string json = "[";
    Reconcile(false, &json);
    json += "]";
    return fmt::format("{{\"repairs\":{},\"actors\":{}}}", m_repairs, json);
}

uint32_t FurnitureGraphLink::Reconcile(bool aRepair, std::string* aJson) noexcept
{
    auto* pLists = ProcessLists::Get();
    if (!pLists)
        return 0;
    uint32_t repaired = 0;
    // High and middle-high actors are the ones with furniture state (MiddleHigh exists from middle-high up).
    for (auto* pHandles : {&pLists->highActorHandleArray, &pLists->middleHighActorHandleArray})
    {
        for (uint32_t i = 0; i < pHandles->length; ++i)
        {
            auto* pActor = Cast<Actor>(TESObjectREFR::GetByHandle((*pHandles)[i]));
            auto* pProcess = pActor ? pActor->currentProcess : nullptr;
            auto* pMiddle = pProcess ? reinterpret_cast<uint8_t*>(pProcess->middleProcess) : nullptr;
            if (!pMiddle || !pActor->GetNiNode())
                continue;
            const auto furnitureHandle = *reinterpret_cast<uint32_t*>(pMiddle + kOccupiedFurniture);
            auto* pFurniture = furnitureHandle ? TESObjectREFR::GetByHandle(furnitureHandle) : nullptr;
            if (!pFurniture || !pFurniture->baseForm || static_cast<uint8_t>(pFurniture->baseForm->formType) != kFurnitureFormType ||
                !pFurniture->GetNiNode() || pFurniture->IsDisabled())
                continue;
            Manager actorGraph(pActor);
            Manager furnitureGraph(pFurniture);
            // Only animated furniture has a graph manager; a plain chair has none and needs no link.
            if (!actorGraph.Value || !furnitureGraph.Value || !furnitureGraph.Value->animationGraphs.size)
                continue;
            const auto before = DependentCount(actorGraph.Value);
            bool added = false;
            if (aRepair)
            {
                AddDependent(actorGraph.Value, furnitureGraph.Value);
                added = DependentCount(actorGraph.Value) > before;
                if (added)
                {
                    ++repaired;
                    ++m_repairs;
                    spdlog::warn("Furniture graph link: {:X} (manager {}) was not linked to the animated furniture {:X} "
                        "(manager {}) it uses; relinked", pActor->formID, static_cast<void*>(actorGraph.Value),
                        pFurniture->formID, static_cast<void*>(furnitureGraph.Value));
                }
            }
            else if (!aJson && !HasDependent(actorGraph.Value, furnitureGraph.Value))
            {
                // Repair switched off (test control): report the missing link once per actor and furniture.
                static std::pair<uint32_t, uint32_t> s_reported{};
                if (s_reported != std::make_pair(pActor->formID, pFurniture->formID))
                {
                    s_reported = {pActor->formID, pFurniture->formID};
                    spdlog::warn("Furniture graph link: {:X} is not linked to the animated furniture {:X} (repair off)",
                        pActor->formID, pFurniture->formID);
                }
            }
            if (aJson)
            {
                if (aJson->size() > 1)
                    *aJson += ",";
                *aJson += fmt::format("{{\"actor\":\"{:X}\",\"furniture\":\"{:X}\",\"linked\":{},\"dependents\":{},\"graphs\":{}}}",
                    pActor->formID, pFurniture->formID, HasDependent(actorGraph.Value, furnitureGraph.Value), before,
                    furnitureGraph.Value->animationGraphs.size);
            }
        }
    }
    return repaired;
}
