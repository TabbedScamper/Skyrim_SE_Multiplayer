#include <TiltedOnlinePCH.h>

#include <Games/References.h>
#include <Games/Overrides.h>

#include <World.h>
#include <Services/PapyrusService.h>
#include <Services/DoorVoteService.h>
#include <Services/WorldStateService.h>
#include <Services/Generic/BusyLockService.h>
#include <Services/Generic/SharedDropService.h>
#include <Events/ActivateEvent.h>
#include <Events/InventoryChangeEvent.h>
#include <Events/ScriptAnimationEvent.h>
#include <Events/LockChangeEvent.h>

#include <ExtraData/ExtraDataList.h>
#include <ExtraData/ExtraCharge.h>
#include <ExtraData/ExtraCount.h>
#include <ExtraData/ExtraEnchantment.h>
#include <ExtraData/ExtraHealth.h>
#include <ExtraData/ExtraPoison.h>
#include <ExtraData/ExtraSoul.h>
#include <ExtraData/ExtraTextDisplayData.h>
#include <ExtraData/ExtraWorn.h>
#include <ExtraData/ExtraWornLeft.h>
#include <Forms/EnchantmentItem.h>
#include <Forms/AlchemyItem.h>
#include <EquipManager.h>
#include <DefaultObjectManager.h>
#include <BSAnimationGraphManager.h>
#include <NetImmerse/NiNode.h>
#include <Havok/BShkbAnimationGraph.h>
#include <Havok/hkbBehaviorGraph.h>
#include <Havok/hkbVariableValueSet.h>
#include <Havok/ActorPoseDiagnosticViews.h>
#include <Havok/hkbStateMachine.h>
#include <Forms/TESObjectCELL.h>
#include <Forms/TESWorldSpace.h>
#include <Forms/TESActorBase.h>

#include <Structs/AnimationGraphDescriptorManager.h>
#include <Structs/AnimationVariables.h>

extern const AnimationGraphDescriptor* BehaviorVarPatch(BSAnimationGraphManager* pManager, Actor* pActor);

namespace
{
void QueueReferenceInventoryChange(TESObjectREFR* apReference, InventoryChangeEvent aEvent, TESObjectREFR* apTransferReference)
{
    if (const auto* pActor = Cast<Actor>(apReference))
    {
        auto ownershipToken = Utils::GetLocalOwnershipToken(pActor->formID);
        if (!ownershipToken && apTransferReference == PlayerCharacter::Get())
            ownershipToken = Utils::GetRemoteOwnershipToken(pActor->formID);

        if (!ownershipToken)
            return;

        aEvent.ServerId = ownershipToken->ServerId;
        aEvent.OwnershipEpoch = ownershipToken->OwnershipEpoch;
    }

    World::Get().GetRunner().Trigger(std::move(aEvent));
}
}

TP_THIS_FUNCTION(TActivate, bool, TESObjectREFR, TESObjectREFR* apActivator, uint8_t aUnk1, TESBoundObject* apObjectToGet, int32_t aCount, char aDefaultProcessing);
TP_THIS_FUNCTION(TAddInventoryItem, void, TESObjectREFR, TESBoundObject* apItem, ExtraDataList* apExtraData, int32_t aCount, TESObjectREFR* apOldOwner);
TP_THIS_FUNCTION(
    TRemoveInventoryItem, BSPointerHandle<TESObjectREFR>*, TESObjectREFR, BSPointerHandle<TESObjectREFR>* apResult, TESBoundObject* apItem, int32_t aCount, ITEM_REMOVE_REASON aReason, ExtraDataList* apExtraList, TESObjectREFR* apMoveToRef, const NiPoint3* apDropLoc, const NiPoint3* apRotate);
TP_THIS_FUNCTION(TPlayAnimationAndWait, bool, void, uint32_t auiStackID, TESObjectREFR* apSelf, BSFixedString* apAnimation, BSFixedString* apEventName);
TP_THIS_FUNCTION(TPlayAnimation, bool, void, uint32_t auiStackID, TESObjectREFR* apSelf, BSFixedString* apEventName);
TP_THIS_FUNCTION(TRotate, void, TESObjectREFR, float aAngle);
TP_THIS_FUNCTION(TLockChange, void, TESObjectREFR);
TP_THIS_FUNCTION(TSetLeveledCreature, void, TESObjectREFR, TESActorBase* apOriginalBase, TESActorBase* apTemplateBase);

static TActivate* RealActivate = nullptr;
static TAddInventoryItem* RealAddInventoryItem = nullptr;
static TRemoveInventoryItem* RealRemoveInventoryItem = nullptr;
static TPlayAnimationAndWait* RealPlayAnimationAndWait = nullptr;
static TPlayAnimation* RealPlayAnimation = nullptr;
static TRotate* RealRotateX = nullptr;
static TRotate* RealRotateY = nullptr;
static TRotate* RealRotateZ = nullptr;
static TLockChange* RealLockChange = nullptr;
static TSetLeveledCreature* RealSetLeveledCreature = nullptr;

#ifdef SAVE_STUFF

#include <Games/Skyrim/SaveLoad.h>

void TESObjectREFR::Save_Reversed(const uint32_t aChangeFlags, Buffer::Writer& aWriter)
{
    TESForm::Save_Reversed(aChangeFlags, aWriter);

    if (aChangeFlags & CHANGE_REFR_BASEOBJECT)
    {
        // save baseForm->formID;
        // we don't because each player has it's own form id system
    }

    if (aChangeFlags & CHANGE_REFR_SCALE)
    {
        // So skyrim does some weird conversion shit here, we are going to do the same for now
        float fScale = scale;
        fScale /= 100.f;
        aWriter.WriteBytes((uint8_t*)&fScale, 4);
    }

    // So skyrim
    uint32_t extraFlags = 0xA6021C40;
    if (formType == Character)
        extraFlags = 0xA6061840;

    if (aChangeFlags & extraFlags)
    {
        // We have flags to save
    }

    if (aChangeFlags & (CHANGE_REFR_INVENTORY | CHANGE_REFR_LEVELED_INVENTORY))
    {
    }

    if (aChangeFlags & CHANGE_REFR_ANIMATION)
    {
        // do something with animations
        // get extradata 0x41
    }
}

#endif

// AIProcess: an NPC's pending door activation (39408 / 0x1406F6E40). The native takes the door handle from the
// middle-high process (+0xD8), clears it, activates the door (ActivateRef 19796, through our hook) and then writes
// middleHigh+0x470 = 0 through a fresh read of process+0x10. A load door into a cell the local player has not loaded
// unloads the actor inside that activation and frees the middle-high process, so the write hit a null pointer
// (host crash 2026-09-28 18:02:24 at 0x1406F6ED7: Ralof leaving Helgen's cave while the host was still inside and
// the follower had already left). Same steps; the write is skipped when the process is gone.
using TRunPendingDoor = bool(void* apProcess, TESObjectREFR* apActor);
static TRunPendingDoor* RealRunPendingDoor = nullptr;

static bool HookRunPendingDoor(void* apProcess, TESObjectREFR* apActor)
{
    auto** ppMiddleHigh = reinterpret_cast<uint8_t**>(static_cast<uint8_t*>(apProcess) + 0x10);
    if (!*ppMiddleHigh)
        return false;
    auto* pHandle = reinterpret_cast<uint32_t*>(*ppMiddleHigh + 0xD8);
    auto* pDoor = TESObjectREFR::GetByHandle(*pHandle);
    if (!pDoor)
        return false;
    *pHandle = 0; // null handle
    if (apActor == PlayerCharacter::Get())
        return false;
    POINTER_SKYRIMSE(TActivate, s_activateEntry, 19796);
    TiltedPhoques::ThisCall(s_activateEntry.Get(), pDoor, apActor, 0, nullptr, 1, 0);
    if (auto* pMiddleHigh = *ppMiddleHigh)
        pMiddleHigh[0x470] = 0;
    else
        spdlog::info("Pending door {:X}: actor {:X} lost its middle-high process during the activation (unloaded)",
            pDoor->formID, apActor ? apActor->formID : 0);
    return true;
}

TESObjectREFR* TESObjectREFR::GetByHandle(uint32_t aHandle) noexcept
{
    TESObjectREFR* pResult = nullptr;

    using TGetRefrByHandle = void(uint32_t & aHandle, TESObjectREFR * &apResult);

    POINTER_SKYRIMSE(TGetRefrByHandle, s_getRefrByHandle, 17201);

    s_getRefrByHandle.Get()(aHandle, pResult);

    if (pResult)
        pResult->handleRefObject.DecRefHandle();

    return pResult;
}

BSPointerHandle<TESObjectREFR> TESObjectREFR::GetHandle() const noexcept
{
    TP_THIS_FUNCTION(TGetHandle, BSPointerHandle<TESObjectREFR>, const TESObjectREFR, BSPointerHandle<TESObjectREFR>* apResult);
    POINTER_SKYRIMSE(TGetHandle, s_getHandle, 19846);

    BSPointerHandle<TESObjectREFR> result{};
    TiltedPhoques::ThisCall(s_getHandle, this, &result);

    return result;
}

uint32_t* TESObjectREFR::GetNullHandle() noexcept
{
    POINTER_SKYRIMSE(uint32_t, s_nullHandle, 400312);

    return s_nullHandle.Get();
}

void TESObjectREFR::SetRotation(float aX, float aY, float aZ) noexcept
{
    TiltedPhoques::ThisCall(RealRotateX, this, aX);
    TiltedPhoques::ThisCall(RealRotateY, this, aY);
    TiltedPhoques::ThisCall(RealRotateZ, this, aZ);
}

bool TESObjectREFR::SetMotionType(MotionType aMotionType, bool aAllowActivate) noexcept
{
    auto* pNode = GetNiNode();
    if (!pNode)
        return false;

    return pNode->SetMotionType(static_cast<uint32_t>(aMotionType), true, false, aAllowActivate);
}

void TESObjectREFR::SetLeveledCreature(TESActorBase* apOriginalBase, TESActorBase* apTemplateA) noexcept
{
    TP_THIS_FUNCTION(TSetLeveledCreature, void, TESObjectREFR, TESActorBase*, TESActorBase*);
    POINTER_SKYRIMSE(TSetLeveledCreature, s_SetLeveledCreature, 20231);
    TiltedPhoques::ThisCall(s_SetLeveledCreature, this, apOriginalBase, apTemplateA);
}

using TiltedPhoques::Serialization;

void TESObjectREFR::SaveAnimationVariables(AnimationVariables& aVariables) const noexcept
{
    BSAnimationGraphManager* pManager = nullptr;
    if (animationGraphHolder.GetBSAnimationGraph(&pManager))
    {
        BSScopedLock<BSRecursiveLock> _{pManager->lock};

        if (pManager->animationGraphIndex < pManager->animationGraphs.size)
        {
            auto* pActor = Cast<Actor>(this);
            if (!pActor)
                return;

            const BShkbAnimationGraph* pGraph = nullptr;

            if (pActor->formID == 0x14)
                pGraph = pManager->animationGraphs.Get(0);
            else
                pGraph = pManager->animationGraphs.Get(pManager->animationGraphIndex);

            if (!pGraph)
                return;

            if (!pGraph->behaviorGraph || !pGraph->behaviorGraph->stateMachine || !pGraph->behaviorGraph->stateMachine->name)
                return;

            auto* pExtendedActor = pActor->GetExtension();
            if (pExtendedActor->GraphDescriptorHash == 0)
            {
                // Force third person graph to be used on player
                if (pActor->formID == 0x14)
                    pExtendedActor->GraphDescriptorHash = pManager->GetDescriptorKey(0);
                else
                    pExtendedActor->GraphDescriptorHash = pManager->GetDescriptorKey();
            }

            auto pDescriptor = AnimationGraphDescriptorManager::Get().GetDescriptor(pExtendedActor->GraphDescriptorHash);

            // Modded behavior check if descriptor wasn't found
            if (!pDescriptor)
                pDescriptor = BehaviorVarPatch(pManager, pActor);

            if (!pDescriptor)
                return;

            const auto* pVariableSet = pGraph->behaviorGraph->animationVariables;

            if (!pVariableSet)
                return;

            aVariables.Booleans.assign(pDescriptor->BooleanLookUpTable.size(), false);
            aVariables.Floats.assign(pDescriptor->FloatLookupTable.size(), 0.f);
            aVariables.Integers.assign(pDescriptor->IntegerLookupTable.size(), 0);

            for (size_t i = 0; i < pDescriptor->BooleanLookUpTable.size(); ++i)
            {
                const auto idx = pDescriptor->BooleanLookUpTable[i];

                if (pVariableSet->size > idx && pVariableSet->data[idx] != 0)
                    aVariables.Booleans[i] = true;
            }

            for (size_t i = 0; i < pDescriptor->FloatLookupTable.size(); ++i)
            {
                const auto idx = pDescriptor->FloatLookupTable[i];

                if (pVariableSet->size > idx)
                    aVariables.Floats[i] = *reinterpret_cast<float*>(&pVariableSet->data[idx]);
            }

            for (size_t i = 0; i < pDescriptor->IntegerLookupTable.size(); ++i)
            {
                const auto idx = pDescriptor->IntegerLookupTable[i];

                if (pVariableSet->size > idx)
                    aVariables.Integers[i] = *reinterpret_cast<uint32_t*>(&pVariableSet->data[idx]);
            }
        }

        pManager->Release();
    }
}

void TESObjectREFR::LoadAnimationVariables(const AnimationVariables& aVariables) const noexcept
{
    BSAnimationGraphManager* pManager = nullptr;
    if (animationGraphHolder.GetBSAnimationGraph(&pManager))
    {
        BSScopedLock<BSRecursiveLock> _{pManager->lock};

        if (pManager->animationGraphIndex < pManager->animationGraphs.size)
        {
            const auto* pGraph = pManager->animationGraphs.Get(pManager->animationGraphIndex);

            if (!pGraph)
                return;

            if (!pGraph->behaviorGraph || !pGraph->behaviorGraph->stateMachine || !pGraph->behaviorGraph->stateMachine->name)
                return;

            auto* pActor = Cast<Actor>(this);
            if (!pActor)
                return;

            auto* pExtendedActor = pActor->GetExtension();
            if (pExtendedActor->GraphDescriptorHash == 0)
                pExtendedActor->GraphDescriptorHash = pManager->GetDescriptorKey();

            auto pDescriptor = AnimationGraphDescriptorManager::Get().GetDescriptor(pExtendedActor->GraphDescriptorHash);

            // Modded behavior check if descriptor wasn't found
            if (!pDescriptor)
                pDescriptor = BehaviorVarPatch(pManager, pActor);

            if (!pDescriptor)
                return;

            const auto* pVariableSet = pGraph->behaviorGraph->animationVariables;

            if (!pVariableSet)
                return;

            for (size_t i = 0; i < pDescriptor->BooleanLookUpTable.size(); ++i)
            {
                const auto idx = pDescriptor->BooleanLookUpTable[i];

                if (pVariableSet->size > idx)
                {
                    pVariableSet->data[idx] = aVariables.Booleans.size() > i ? aVariables.Booleans[i] : false;
                }
            }

            for (size_t i = 0; i < pDescriptor->FloatLookupTable.size(); ++i)
            {
                const auto idx = pDescriptor->FloatLookupTable[i];

                if (pVariableSet->size > idx)
                {
                    *reinterpret_cast<float*>(&pVariableSet->data[idx]) = aVariables.Floats.size() > i ? aVariables.Floats[i] : 0.f;
                }
            }

            for (size_t i = 0; i < pDescriptor->IntegerLookupTable.size(); ++i)
            {
                const auto idx = pDescriptor->IntegerLookupTable[i];

                if (pVariableSet->size > idx)
                {
                    *reinterpret_cast<uint32_t*>(&pVariableSet->data[idx]) = aVariables.Integers.size() > i ? aVariables.Integers[i] : 0;
                }
            }
        }

        pManager->Release();
    }
}

uint32_t TESObjectREFR::GetCellId() const noexcept
{
    if (!parentCell)
        return 0;

    const auto* pWorldSpace = parentCell->worldspace;

    return pWorldSpace != nullptr ? pWorldSpace->formID : parentCell->formID;
}

TESWorldSpace* TESObjectREFR::GetWorldSpace() const noexcept
{
    auto* pParentCell = GetParentCellEx();
    if (pParentCell && !(pParentCell->cellFlags & 1))
        return pParentCell->worldspace;

    return nullptr;
}

ExtraDataList* TESObjectREFR::GetExtraDataList() noexcept
{
    return &extraData;
}

// Delete() should only be used on temporaries
void TESObjectREFR::Delete() const noexcept
{
    using ObjectReference = TESObjectREFR;

    PAPYRUS_FUNCTION(void, ObjectReference, Delete);

    s_pDelete(this);
}

void TESObjectREFR::Disable() const noexcept
{
    using ObjectReference = TESObjectREFR;

    PAPYRUS_FUNCTION(void, ObjectReference, Disable, bool);

    s_pDisable(this, true);
}

void TESObjectREFR::Enable() const noexcept
{
    using ObjectReference = TESObjectREFR;

    PAPYRUS_FUNCTION(void, ObjectReference, Enable, bool);

    s_pEnable(this, true);
}

// Skyrim: MoveTo() can fail, causing the object to be deleted
void TESObjectREFR::MoveTo(TESObjectCELL* apCell, const NiPoint3& acPosition) const noexcept
{
    ScopedReferencesOverride recursionGuard;

    TP_THIS_FUNCTION(TInternalMoveTo, bool, const TESObjectREFR, uint32_t*&, TESObjectCELL*, TESWorldSpace*, const NiPoint3&, const NiPoint3&);

    POINTER_SKYRIMSE(TInternalMoveTo, s_internalMoveTo, 56626);

    TiltedPhoques::ThisCall(s_internalMoveTo, this, GetNullHandle(), apCell, apCell->worldspace, acPosition, rotation);
}

void TESObjectREFR::PayGold(int32_t aAmount) noexcept
{
    ScopedInventoryOverride _;
    PayGoldToContainer(nullptr, aAmount);
}

void TESObjectREFR::PayGoldToContainer(TESObjectREFR* pContainer, int32_t aAmount) noexcept
{
    TP_THIS_FUNCTION(TPayGoldToContainer, void, TESObjectREFR, TESObjectREFR*, int32_t);
    POINTER_SKYRIMSE(TPayGoldToContainer, s_payGoldToContainer, 37511);
    TiltedPhoques::ThisCall(s_payGoldToContainer, this, pContainer, aAmount);
}

Lock* TESObjectREFR::GetLock() const noexcept
{
    TP_THIS_FUNCTION(TGetLock, Lock*, const TESObjectREFR);
    POINTER_SKYRIMSE(TGetLock, realGetLock, 20223);

    return TiltedPhoques::ThisCall(realGetLock, this);
}

Lock* TESObjectREFR::CreateLock() noexcept
{
    TP_THIS_FUNCTION(TCreateLock, Lock*, TESObjectREFR);
    POINTER_SKYRIMSE(TCreateLock, realCreateLock, 20221);

    return TiltedPhoques::ThisCall(realCreateLock, this);
}

void TESObjectREFR::LockChange() noexcept
{
    TiltedPhoques::ThisCall(RealLockChange, this);
}

const float TESObjectREFR::GetHeight() noexcept
{
    auto boundMax = GetBoundMax();
    return boundMax.z - GetBoundMin().z;
}

TESObjectREFR::OpenState TESObjectREFR::GetOpenState() noexcept
{
    using ObjectReference = TESObjectREFR;

    PAPYRUS_FUNCTION(TESObjectREFR::OpenState, ObjectReference, GetOpenState);

    return s_pGetOpenState(this);
}

ExtraContainerChanges::Data* TESObjectREFR::GetContainerChanges() const noexcept
{
    TP_THIS_FUNCTION(TGetContainterChanges, ExtraContainerChanges::Data*, const TESObjectREFR);

    POINTER_SKYRIMSE(TGetContainterChanges, s_getContainerChangs, 16040);

    return TiltedPhoques::ThisCall(s_getContainerChangs, this);
}

void TESObjectREFR::RemoveAllItems() noexcept
{
    using ObjectReference = TESObjectREFR;

    PAPYRUS_FUNCTION(void, ObjectReference, RemoveAllItems, TESObjectREFR*, bool, bool);

    ScopedEquipOverride equipOverride;

    s_pRemoveAllItems(this, nullptr, false, true);
}

TESContainer* TESObjectREFR::GetContainer() const noexcept
{
    TP_THIS_FUNCTION(TGetContainer, TESContainer*, const TESObjectREFR);

    POINTER_SKYRIMSE(TGetContainer, s_getContainer, 19702);

    return TiltedPhoques::ThisCall(s_getContainer, this);
}

int64_t TESObjectREFR::GetItemCountInInventory(TESForm* apItem) const noexcept
{
    int64_t count = GetContainer()->GetItemCount(apItem);

    auto* pContainerChanges = GetContainerChanges()->entries;
    for (auto pChange : *pContainerChanges)
    {
        if (pChange && pChange->form)
        {
            if (pChange->form->formID == apItem->formID)
            {
                count += pChange->count;
                break;
            }
        }
    }

    return count;
}

TESObjectCELL* TESObjectREFR::GetParentCellEx() const noexcept
{
    return parentCell ? parentCell : GetSaveParentCell();
}

void TESObjectREFR::GetItemFromExtraData(Inventory::Entry& arEntry, ExtraDataList* apExtraDataList) noexcept
{
    auto& modSystem = World::Get().GetModSystem();

    if (ExtraCount* pExtraCount = Cast<ExtraCount>(apExtraDataList->GetByType(ExtraDataType::Count)))
    {
        arEntry.Count = pExtraCount->count;
    }

    if (ExtraCharge* pExtraCharge = Cast<ExtraCharge>(apExtraDataList->GetByType(ExtraDataType::Charge)))
    {
        arEntry.ExtraCharge = pExtraCharge->fCharge;
    }

    if (ExtraEnchantment* pExtraEnchantment = Cast<ExtraEnchantment>(apExtraDataList->GetByType(ExtraDataType::Enchantment)))
    {
        TP_ASSERT(pExtraEnchantment->pEnchantment, "Null enchantment in ExtraEnchantment");

        modSystem.GetServerModId(pExtraEnchantment->pEnchantment->formID, arEntry.ExtraEnchantId);

        if (pExtraEnchantment->pEnchantment->formID & 0xFF000000)
        {
            for (EffectItem* pEffectItem : pExtraEnchantment->pEnchantment->listOfEffects)
            {
                TP_ASSERT(pEffectItem, "pEffectItem is null.");
                if (!pEffectItem)
                    continue;

                Inventory::EffectItem effect;
                effect.Magnitude = pEffectItem->data.fMagnitude;
                effect.Area = pEffectItem->data.iArea;
                effect.Duration = pEffectItem->data.iDuration;
                effect.RawCost = pEffectItem->fRawCost;
                modSystem.GetServerModId(pEffectItem->pEffectSetting->formID, effect.EffectId);
                arEntry.EnchantData.Effects.push_back(effect);
            }

            uint32_t objectId = modSystem.GetGameId(arEntry.BaseId);
            TESForm* pObject = TESForm::GetById(objectId);
            if (pObject)
                arEntry.EnchantData.IsWeapon = pObject->formType == FormType::Weapon;
        }

        arEntry.ExtraEnchantCharge = pExtraEnchantment->usCharge;
        arEntry.ExtraEnchantRemoveUnequip = pExtraEnchantment->bRemoveOnUnequip;
    }

    if (ExtraHealth* pExtraHealth = Cast<ExtraHealth>(apExtraDataList->GetByType(ExtraDataType::Health)))
    {
        arEntry.ExtraHealth = pExtraHealth->fHealth;
    }

    if (ExtraPoison* pExtraPoison = Cast<ExtraPoison>(apExtraDataList->GetByType(ExtraDataType::Poison)))
    {
        TP_ASSERT(pExtraPoison->pPoison, "Null poison in ExtraPoison");
        if (pExtraPoison && pExtraPoison->pPoison)
        {
            modSystem.GetServerModId(pExtraPoison->pPoison->formID, arEntry.ExtraPoisonId);
            arEntry.ExtraPoisonCount = pExtraPoison->uiCount;
        }
    }

    if (ExtraSoul* pExtraSoul = Cast<ExtraSoul>(apExtraDataList->GetByType(ExtraDataType::Soul)))
    {
        arEntry.ExtraSoulLevel = (int32_t)pExtraSoul->cSoul;
    }

    /*
    if (ExtraTextDisplayData* pExtraTextDisplayData = Cast<ExtraTextDisplayData>(apExtraDataList->GetByType(ExtraDataType::TextDisplayData)))
    {
        if (pExtraTextDisplayData->DisplayName)
            arEntry.ExtraTextDisplayName = pExtraTextDisplayData->DisplayName;
        else
            arEntry.ExtraTextDisplayName = "";
    }
    */

    arEntry.ExtraWorn = apExtraDataList->Contains(ExtraDataType::Worn);
    arEntry.ExtraWornLeft = apExtraDataList->Contains(ExtraDataType::WornLeft);

    arEntry.IsQuestItem = apExtraDataList->HasQuestObjectAlias();
}

ExtraDataList* TESObjectREFR::GetExtraDataFromItem(const Inventory::Entry& arEntry) noexcept
{
    auto& modSystem = World::Get().GetModSystem();

    ExtraDataList* pExtraDataList = nullptr;

    if (!arEntry.ContainsExtraData())
        return pExtraDataList;

    pExtraDataList = ExtraDataList::New();

    if (arEntry.ExtraCharge > 0.f)
    {
        pExtraDataList->SetChargeData(arEntry.ExtraCharge);
    }

    if (arEntry.ExtraEnchantId != 0)
    {
        EnchantmentItem* pEnchantment = nullptr;
        if (arEntry.ExtraEnchantId.ModId == 0xFFFFFFFF)
        {
            pEnchantment = EnchantmentItem::Create(arEntry.EnchantData);
        }
        else
        {
            uint32_t enchantId = modSystem.GetGameId(arEntry.ExtraEnchantId);
            pEnchantment = Cast<EnchantmentItem>(TESForm::GetById(enchantId));
        }

        TP_ASSERT(pEnchantment, "No Enchantment created or found.");

        pExtraDataList->SetEnchantmentData(pEnchantment, arEntry.ExtraEnchantCharge, arEntry.ExtraEnchantRemoveUnequip);
    }

    if (arEntry.ExtraPoisonId != 0)
    {
        // TODO: does poison have the same temp problem as enchants?
        // doesn't seem to be the case, there are only like 3 poisons, and no custom ones
        TP_ASSERT(arEntry.ExtraPoisonId.ModId != 0xFFFFFFFF, "Poison is sent as temp!");

        uint32_t poisonId = modSystem.GetGameId(arEntry.ExtraPoisonId);
        if (AlchemyItem* pPoison = Cast<AlchemyItem>(TESForm::GetById(poisonId)))
        {
            pExtraDataList->SetPoison(pPoison, arEntry.ExtraPoisonCount);
        }
    }

    if (arEntry.ExtraHealth > 0.f)
    {
        pExtraDataList->SetHealth(arEntry.ExtraHealth);
    }

    if (arEntry.ExtraSoulLevel > 0 && arEntry.ExtraSoulLevel <= 5)
    {
        pExtraDataList->SetSoulData(static_cast<SOUL_LEVEL>(arEntry.ExtraSoulLevel));
    }

    if (arEntry.ExtraWorn)
    {
        pExtraDataList->SetWorn(false);
    }

    if (arEntry.ExtraWornLeft)
    {
        pExtraDataList->SetWorn(true);
    }

    // TODO: this is causing crashes
    /*
    if (!arEntry.ExtraTextDisplayName.empty())
    {
        ExtraTextDisplayData* pExtraText = Memory::Allocate<ExtraTextDisplayData>();
        *((uint64_t*)pExtraText) = 0x1416244D0;
        pExtraText->next = nullptr;
        pExtraText->DisplayName = arEntry.ExtraTextDisplayName.c_str();
        pExtraText->usCustomNameLength = arEntry.ExtraTextDisplayName.length();
        pExtraText->iOwnerInstance = -2;
        pExtraText->fTemperFactor = 1.0F;
        pExtraDataList->Add(ExtraDataType::TextDisplayData, pExtraText);
    }
    */

    if (pExtraDataList->data == nullptr)
    {
        Memory::Delete(pExtraDataList->bitfield);
        Memory::Delete(pExtraDataList);
        pExtraDataList = nullptr;
    }

    return pExtraDataList;
}

Inventory TESObjectREFR::GetInventory() const noexcept
{
    return GetInventory([](TESForm& aForm) { return true; });
}

Inventory TESObjectREFR::GetInventory(std::function<bool(TESForm&)> aFilter) const noexcept
{
    auto& modSystem = World::Get().GetModSystem();
    Inventory inventory{};

    if (TESContainer* pBaseContainer = GetContainer())
    {
        for (int i = 0; i < pBaseContainer->count; i++)
        {
            TESContainer::Entry* pGameEntry = pBaseContainer->entries[i];
            if (!pGameEntry || !pGameEntry->form)
            {
                spdlog::warn("Entry or form for inventory item is null.");
                continue;
            }

            if (!aFilter(*pGameEntry->form))
                continue;

            Inventory::Entry entry;
            modSystem.GetServerModId(pGameEntry->form->formID, entry.BaseId);
            entry.Count = pGameEntry->count;

            inventory.Entries.push_back(std::move(entry));
        }
    }

    Inventory extraInventory{};

    auto pExtraContChangesEntries = GetContainerChanges()->entries;
    for (auto pGameEntry : *pExtraContChangesEntries)
    {
        if (!pGameEntry)
            continue;

        if (!aFilter(*pGameEntry->form))
            continue;

        Inventory::Entry entry{};
        modSystem.GetServerModId(pGameEntry->form->formID, entry.BaseId);
        entry.Count = pGameEntry->count;

        for (ExtraDataList* pExtraDataList : *pGameEntry->dataList)
        {
            if (!pExtraDataList)
                continue;

            Inventory::Entry innerEntry;
            innerEntry.BaseId = entry.BaseId;
            innerEntry.Count = 1;

            GetItemFromExtraData(innerEntry, pExtraDataList);

            entry.Count -= innerEntry.Count;

            extraInventory.Entries.push_back(std::move(innerEntry));
        }

        if (entry.Count != 0)
            extraInventory.Entries.push_back(std::move(entry));
    }

    spdlog::debug("ExtraInventory count: {}", extraInventory.Entries.size());

    Inventory minimizedExtraInventory{};

    for (auto& entry : extraInventory.Entries)
    {
        auto duplicate = std::find_if(minimizedExtraInventory.Entries.begin(), minimizedExtraInventory.Entries.end(), [entry](const Inventory::Entry& newEntry) { return newEntry.CanBeMerged(entry); });

        if (duplicate == std::end(minimizedExtraInventory.Entries))
        {
            minimizedExtraInventory.Entries.push_back(entry);
            continue;
        }

        duplicate->Count += entry.Count;
    }

    spdlog::debug("MinExtraInventory count: {}", minimizedExtraInventory.Entries.size());

    for (auto& entry : minimizedExtraInventory.Entries)
    {
        if (entry.ContainsExtraData())
            continue;

        auto duplicate = std::find_if(inventory.Entries.begin(), inventory.Entries.end(), [entry](const Inventory::Entry& newEntry) { return newEntry.CanBeMerged(entry); });

        if (duplicate == std::end(inventory.Entries))
            continue;

        entry.Count += duplicate->Count;
        duplicate->Count = 0;
    }

    spdlog::debug("MinExtraInventory count after: {}", minimizedExtraInventory.Entries.size());

    inventory.Entries.insert(inventory.Entries.end(), minimizedExtraInventory.Entries.begin(), minimizedExtraInventory.Entries.end());

    spdlog::debug("Inventory count before: {}", inventory.Entries.size());

    inventory.RemoveByFilter([](const auto& entry) { return entry.Count == 0; });

    spdlog::debug("Inventory count after: {}", inventory.Entries.size());

    return inventory;
}

Inventory TESObjectREFR::GetArmor() const noexcept
{
    return GetInventory([](TESForm& aForm) { return aForm.formType == FormType::Armor; });
}

Inventory TESObjectREFR::GetWornArmor() const noexcept
{
    Inventory wornArmor = GetArmor();
    wornArmor.RemoveByFilter([](const auto& entry) { return !entry.IsWorn(); });
    return wornArmor;
}

bool TESObjectREFR::IsItemInInventory(uint32_t aFormID) const noexcept
{
    Inventory inventory = GetInventory([aFormID](TESForm& aForm) { return aForm.formID == aFormID; });
    return !inventory.Entries.empty();
}

void TESObjectREFR::SetInventory(const Inventory& aInventory) noexcept
{
    spdlog::debug("Setting inventory for {:X}", formID);

    ScopedInventoryOverride _;

    RemoveAllItems();

    for (const Inventory::Entry& entry : aInventory.Entries)
    {
        if (entry.Count != 0)
            AddOrRemoveItem(entry, true);
    }
}

Vector<uint32_t> TESObjectREFR::RemoveNonQuestItems(Inventory& aCurrentInventory) noexcept
{
    ScopedEquipOverride equipOverride_;

    Vector<uint32_t> questEntries{};

    auto& modSystem = World::Get().GetModSystem();

    for (auto& entry : aCurrentInventory.Entries)
    {
        if (entry.IsQuestItem)
        {
            uint32_t gameId = modSystem.GetGameId(entry.BaseId);
            questEntries.emplace_back(gameId);
            continue;
        }

        if (entry.Count <= 0)
            continue;

        entry.Count = -entry.Count;
        AddOrRemoveItem(entry, true);
    }

    return questEntries;
}

void TESObjectREFR::SetInventoryRetainingQuestItems(Inventory& aCurrentInventory, const Inventory& acSourceInventory) noexcept
{
    spdlog::debug("Setting inventory for {:X}", formID);

    ScopedInventoryOverride _;

    // Remove all non-quest items first
    Vector<uint32_t> questItemIds = RemoveNonQuestItems(aCurrentInventory);
    auto& modSystem = World::Get().GetModSystem();

    for (const auto& entry : acSourceInventory.Entries)
    {
        uint32_t gameId = modSystem.GetGameId(entry.BaseId);

        // If the item is not in the list of quest items, add it
        if (std::find(questItemIds.begin(), questItemIds.end(), gameId) == questItemIds.end())
        {
            if (entry.Count != 0)
                AddOrRemoveItem(entry, true);
        }
    }
}

void TESObjectREFR::AddOrRemoveItem(const Inventory::Entry& arEntry, bool aIsSettingInventory) noexcept
{
    ModSystem& modSystem = World::Get().GetModSystem();

    uint32_t objectId = modSystem.GetGameId(arEntry.BaseId);
    TESBoundObject* pObject = Cast<TESBoundObject>(TESForm::GetById(objectId));
    if (!pObject)
    {
        spdlog::warn("{}: Object to add not found, {:X}:{:X}.", __FUNCTION__, arEntry.BaseId.ModId, arEntry.BaseId.BaseId);
        return;
    }

    ExtraDataList* pExtraDataList = GetExtraDataFromItem(arEntry);

    if (arEntry.Count > 0)
    {
        bool isWorn = false;
        bool isWornLeft = false;
        if (pExtraDataList)
        {
            isWorn = pExtraDataList->Contains(ExtraDataType::Worn);
            isWornLeft = pExtraDataList->Contains(ExtraDataType::WornLeft);
        }

        spdlog::debug("Adding item {:X}, count {}", pObject->formID, arEntry.Count);
        AddObjectToContainer(pObject, pExtraDataList, arEntry.Count, nullptr);

        // TODO: check Actor cast first?
        if (isWorn)
            EquipManager::Get()->Equip(Cast<Actor>(this), pObject, nullptr, arEntry.Count, DefaultObjectManager::Get().rightEquipSlot, false, true, false, false);
        else if (isWornLeft)
            EquipManager::Get()->Equip(Cast<Actor>(this), pObject, nullptr, arEntry.Count, DefaultObjectManager::Get().leftEquipSlot, false, true, false, false);
    }
    else if (arEntry.Count < 0)
    {
        spdlog::debug("Removing item {:X}, count {}", pObject->formID, -arEntry.Count);
        RemoveItem(pObject, -arEntry.Count, ITEM_REMOVE_REASON::kRemove, pExtraDataList, nullptr);
    }

    // TODO(cosideci): this is still flawed. Adding the refr to the quest leader is hard.
    // It is still recommended that the quest leader loots all quest items.
    if (arEntry.IsQuestItem && arEntry.Count > 0 && !aIsSettingInventory)
    {
        PlayerCharacter* pPlayer = PlayerCharacter::Get();

        if (!pPlayer->IsItemInInventory(objectId))
        {
            Actor* pActor = Cast<Actor>(this);
            if (pActor && pActor->GetExtension()->IsRemotePlayer())
                pPlayer->AddOrRemoveItem(arEntry);
        }
    }

    UpdateItemList(nullptr);
}

void TESObjectREFR::UpdateItemList(TESForm* pUnkForm) noexcept
{
    TP_THIS_FUNCTION(TUpdateItemList, void, TESObjectREFR, TESForm*);
    POINTER_SKYRIMSE(TUpdateItemList, updateItemList, 52849);
    TiltedPhoques::ThisCall(updateItemList, this, pUnkForm);
}

bool TESObjectREFR::Activate(TESObjectREFR* apActivator, uint8_t aUnk1, TESBoundObject* aObjectToGet, int32_t aCount, char aDefaultProcessing) noexcept
{
    ScopedActivateOverride _;

    return TiltedPhoques::ThisCall(RealActivate, this, apActivator, aUnk1, aObjectToGet, aCount, aDefaultProcessing);
}

void TESObjectREFR::EnableImpl() noexcept
{
    TP_THIS_FUNCTION(TEnableImpl, void, TESObjectREFR, bool aResetInventory);

    POINTER_SKYRIMSE(TEnableImpl, s_enable, 19800);

    TiltedPhoques::ThisCall(s_enable, this, false);
}

uint32_t TESObjectREFR::GetAnimationVariableInt(BSFixedString* apVariableName) noexcept
{
    using ObjectReference = TESObjectREFR;

    PAPYRUS_FUNCTION(uint32_t, ObjectReference, GetAnimationVariableInt, BSFixedString*);

    return s_pGetAnimationVariableInt(this, apVariableName);
}

static thread_local bool s_cancelAnimationWaitEvent = false;

bool TESObjectREFR::PlayAnimationAndWait(BSFixedString* apAnimation, BSFixedString* apEventName) noexcept
{
    using ObjectReference = TESObjectREFR;

    PAPYRUS_FUNCTION(bool, ObjectReference, PlayAnimationAndWait, BSFixedString*, BSFixedString*);

    s_cancelAnimationWaitEvent = true;
    bool result = s_pPlayAnimationAndWait(this, apAnimation, apEventName);
    s_cancelAnimationWaitEvent = false;
    return result;
}

#define OBJECT_ANIM_SYNC 1

bool TP_MAKE_THISCALL(HookPlayAnimationAndWait, void, uint32_t auiStackID, TESObjectREFR* apSelf, BSFixedString* apAnimation, BSFixedString* apEventName)
{
    if (apSelf && apAnimation) WorldStateService::TraceAnimation(apSelf->formID, apAnimation->AsAscii());
    spdlog::debug("Animation: {}, EventName: {}", apAnimation->AsAscii(), apEventName->AsAscii());

#if OBJECT_ANIM_SYNC
    if (!s_cancelAnimationWaitEvent && apSelf && apSelf->formType == Actor::Type && (apSelf->formID < 0xFF000000))
        World::Get().GetRunner().Trigger(ScriptAnimationEvent(apSelf->formID, apAnimation->AsAscii(), apEventName->AsAscii()));
#endif

    return TiltedPhoques::ThisCall(RealPlayAnimationAndWait, apThis, auiStackID, apSelf, apAnimation, apEventName);
}

static thread_local bool s_cancelAnimationEvent = false;

bool TESObjectREFR::PlayAnimation(BSFixedString* apEventName) noexcept
{
    using ObjectReference = TESObjectREFR;

    PAPYRUS_FUNCTION(bool, ObjectReference, PlayAnimation, BSFixedString*);

    s_cancelAnimationEvent = true;
    bool result = s_pPlayAnimation(this, apEventName);
    s_cancelAnimationEvent = false;
    return result;
}

bool TESObjectREFR::SendAnimationEvent(BSFixedString* apEventName) noexcept
{
    return animationGraphHolder.SendAnimationEvent(apEventName);
}

bool TP_MAKE_THISCALL(HookPlayAnimation, void, uint32_t auiStackID, TESObjectREFR* apSelf, BSFixedString* apEventName)
{
    if (apSelf && apEventName) WorldStateService::TraceAnimation(apSelf->formID, apEventName->AsAscii());
    spdlog::debug("EventName: {}", apEventName->AsAscii());

#if OBJECT_ANIM_SYNC
    if (!s_cancelAnimationEvent && apSelf && apSelf->formType == Actor::Type && (apSelf->formID < 0xFF000000))
        World::Get().GetRunner().Trigger(ScriptAnimationEvent(apSelf->formID, String{}, apEventName->AsAscii()));
#endif

    return TiltedPhoques::ThisCall(RealPlayAnimation, apThis, auiStackID, apSelf, apEventName);
}

bool TP_MAKE_THISCALL(HookActivate, TESObjectREFR, TESObjectREFR* apActivator, uint8_t aUnk1, TESBoundObject* apObjectToGet, int32_t aCount, char aDefaultProcessing)
{
    if (World::Get().GetSharedDropService().TryHold(apThis, apActivator))
        return true;

    if (World::Get().GetDoorVoteService().TryHold(apThis, apActivator, aUnk1, apObjectToGet, aCount, aDefaultProcessing, _ReturnAddress()))
        return true;

    if (World::Get().GetBusyLockService().TryHold(apThis, apActivator, aUnk1, apObjectToGet, aCount, aDefaultProcessing, _ReturnAddress()))
        return true;

    Actor* pActivator = Cast<Actor>(apActivator);

    // Exclude books from activation since only reading them removes them from the cell
    // Note: Books are now unsynced 
    if (pActivator && apThis->baseForm->formType != FormType::Book)
    {
        auto openState = TESObjectREFR::kNone;
        if (apThis->baseForm->formType == FormType::Door)
            openState = apThis->GetOpenState();

        World::Get().GetRunner().Trigger(
            ActivateEvent(apThis, pActivator, apObjectToGet, aCount, aDefaultProcessing, aUnk1, openState)
        );
    }

    return TiltedPhoques::ThisCall(RealActivate, apThis, apActivator, aUnk1, apObjectToGet, aCount, aDefaultProcessing);
}

void TP_MAKE_THISCALL(HookAddInventoryItem, TESObjectREFR, TESBoundObject* apItem, ExtraDataList* apExtraData, int32_t aCount, TESObjectREFR* apOldOwner)
{
    if (!ScopedInventoryOverride::IsOverriden())
    {
        auto& modSystem = World::Get().GetModSystem();

        Inventory::Entry item{};
        modSystem.GetServerModId(apItem->formID, item.BaseId);
        item.Count = aCount;

        if (apExtraData)
            apThis->GetItemFromExtraData(item, apExtraData);

        QueueReferenceInventoryChange(apThis, InventoryChangeEvent(apThis->formID, std::move(item)), apOldOwner);
    }

    spdlog::debug("Adding inventory item {:X} to {:X}", apItem->formID, apThis->formID);

    TiltedPhoques::ThisCall(RealAddInventoryItem, apThis, apItem, apExtraData, aCount, apOldOwner);
}

BSPointerHandle<TESObjectREFR>*
TP_MAKE_THISCALL(HookRemoveInventoryItem, TESObjectREFR, BSPointerHandle<TESObjectREFR>* apResult, TESBoundObject* apItem, int32_t aCount, ITEM_REMOVE_REASON aReason, ExtraDataList* apExtraList, TESObjectREFR* apMoveToRef, const NiPoint3* apDropLoc, const NiPoint3* apRotate)
{
    // A dead or dying NPC copy's inventory belongs to its owner. Its own death scripts also run here (Lokir's
    // MQ101LokirScript.OnDeath: RemoveItem(PrisonerCuffs)); that local removal unequipped the cuffs during the native
    // death transition and cleared every worn flag, a naked corpse for about half a second on the follower (run
    // 20260928-095901: worn [3 items] at +0 ms, cuffs unequipped +16 ms on the script thread, worn [] at +33 ms).
    // The owner runs the same script and its removal arrives through sync (cuffs removed cleanly at +1926 ms).
    // Only plain removals are skipped: looting into a container or player, drops, and our own sync still apply.
    if (!ScopedInventoryOverride::IsOverriden() && aReason == ITEM_REMOVE_REASON::kRemove && !apMoveToRef && !apDropLoc)
    {
        auto* pActor = Cast<Actor>(apThis);
        const auto* pExtension = pActor ? pActor->GetExtension() : nullptr;
        if (pExtension && pExtension->IsRemote() && !pExtension->IsPlayer() &&
            (((pActor->actorState.flags1 >> 21) & 0xF) == 1 || ((pActor->actorState.flags1 >> 21) & 0xF) == 2) &&
            World::Get().GetTransport().IsConnected())
        {
            static std::atomic<uint32_t> s_skipped{};
            if (s_skipped.fetch_add(1, std::memory_order_relaxed) < 32)
                spdlog::info("Dead copy {:X}: local removal of {:X} x{} skipped; the owner's inventory decides",
                    apThis->formID, apItem ? apItem->formID : 0, aCount);
            if (apResult)
                *apResult = {};
            return apResult;
        }
    }
    if (!ScopedInventoryOverride::IsOverriden())
    {
        auto& modSystem = World::Get().GetModSystem();

        Inventory::Entry item{};
        modSystem.GetServerModId(apItem->formID, item.BaseId);

        if (apExtraList)
        {
            ScopedExtraDataOverride _;
            apThis->GetItemFromExtraData(item, apExtraList);
        }

        item.Count = -aCount;

        QueueReferenceInventoryChange(apThis, InventoryChangeEvent(apThis->formID, std::move(item)), apMoveToRef);
    }

    spdlog::debug("Removing inventory item {:X} from {:X}", apItem->formID, apThis->formID);

    ScopedEquipOverride _;

    return TiltedPhoques::ThisCall(RealRemoveInventoryItem, apThis, apResult, apItem, aCount, aReason, apExtraList, apMoveToRef, apDropLoc, apRotate);
}

void TP_MAKE_THISCALL(HookRotateX, TESObjectREFR, float aAngle)
{
    if (apThis->formType == Actor::Type)
    {
        const auto pActor = static_cast<Actor*>(apThis);
        // We don't allow remotes to move
        if (pActor->GetExtension()->IsRemote())
            return;
    }

    return TiltedPhoques::ThisCall(RealRotateX, apThis, aAngle);
}

void TP_MAKE_THISCALL(HookRotateY, TESObjectREFR, float aAngle)
{
    if (apThis->formType == Actor::Type)
    {
        const auto pActor = static_cast<Actor*>(apThis);
        // We don't allow remotes to move
        if (pActor->GetExtension()->IsRemote())
            return;
    }

    return TiltedPhoques::ThisCall(RealRotateY, apThis, aAngle);
}

void TP_MAKE_THISCALL(HookRotateZ, TESObjectREFR, float aAngle)
{
    if (apThis->formType == Actor::Type)
    {
        const auto pActor = static_cast<Actor*>(apThis);
        // We don't allow remotes to move
        if (pActor->GetExtension()->IsRemote())
            return;
    }

    return TiltedPhoques::ThisCall(RealRotateZ, apThis, aAngle);
}

void TP_MAKE_THISCALL(HookLockChange, TESObjectREFR)
{
    const auto id = apThis->formID;
    TiltedPhoques::ThisCall(RealLockChange, apThis);
    // Preserve the immediate, per-call path from 25fa030d. World-state sampling
    // may coalesce durable values, but must not coalesce these existing events.
    const auto* pLock = apThis->GetLock();
    if (pLock)
        World::Get().GetRunner().Trigger(LockChangeEvent(id, pLock->IsLocked(), pLock->lockLevel));
    else
        World::Get().GetRunner().Trigger(LockChangeEvent(id, false, 0));
    WorldStateService::ObserveId(id, WorldStateKind::Count);
}

static TiltedPhoques::Initializer s_objectReferencesHooks(
    []()
    {
        POINTER_SKYRIMSE(TLockChange, s_lockChange, 19512);
        // POINTER_SKYRIMSE(TSetLeveledCreature, s_SetLeveledCreature, 20231);
        POINTER_SKYRIMSE(TRotate, s_rotateX, 19787);
        POINTER_SKYRIMSE(TRotate, s_rotateY, 19788);
        POINTER_SKYRIMSE(TRotate, s_rotateZ, 19789);
        POINTER_SKYRIMSE(TActivate, s_activate, 19796);
        POINTER_SKYRIMSE(TAddInventoryItem, s_addInventoryItem, 19708);
        POINTER_SKYRIMSE(TRemoveInventoryItem, s_removeInventoryItem, 19689);
        POINTER_SKYRIMSE(TPlayAnimationAndWait, s_playAnimationAndWait, 56206);
        POINTER_SKYRIMSE(TPlayAnimation, s_playAnimation, 56205);

        RealLockChange = s_lockChange.Get();
        // RealSetLeveledCreature = s_SetLeveledCreature.Get();
        RealRotateX = s_rotateX.Get();
        RealRotateY = s_rotateY.Get();
        RealRotateZ = s_rotateZ.Get();
        RealActivate = s_activate.Get();
        RealAddInventoryItem = s_addInventoryItem.Get();
        RealRemoveInventoryItem = s_removeInventoryItem.Get();
        RealPlayAnimationAndWait = s_playAnimationAndWait.Get();
        RealPlayAnimation = s_playAnimation.Get();

        TP_HOOK(&RealLockChange, HookLockChange);
        // TP_HOOK(&RealSetLeveledCreature, HookSetLeveledCreature);
        TP_HOOK(&RealRotateX, HookRotateX);
        TP_HOOK(&RealRotateY, HookRotateY);
        TP_HOOK(&RealRotateZ, HookRotateZ);
        TP_HOOK(&RealActivate, HookActivate);
        POINTER_SKYRIMSE(TRunPendingDoor, s_runPendingDoor, 39408);
        RealRunPendingDoor = s_runPendingDoor.Get();
        TP_HOOK(&RealRunPendingDoor, HookRunPendingDoor);
        TP_HOOK(&RealAddInventoryItem, HookAddInventoryItem);
        TP_HOOK(&RealRemoveInventoryItem, HookRemoveInventoryItem);
        TP_HOOK(&RealPlayAnimationAndWait, HookPlayAnimationAndWait);
        TP_HOOK(&RealPlayAnimation, HookPlayAnimation);
    });


namespace
{
// ABI/callsite audit and limitations are recorded in world_state_encoding.cpp.
using WorldFlagFn = void(TESObjectREFR*, bool);
using WorldDamageFn = void(void*, TESObjectREFR*, float, bool);
using WorldClearFn = void(TESObjectREFR*);
using WorldFinishedFn = void(TESObjectREFR*, const char*);
using WorldSet3DFn = void(TESObjectREFR*, NiNode*, bool);
using WorldOpenFn = bool(TESObjectREFR*, bool, bool);
using WorldSetOpenFn = void(void*, uint32_t, TESObjectREFR*, bool);
WorldFlagFn* s_worldDisabled{};
WorldFlagFn* s_worldDestroyed{};
WorldDamageFn* s_worldDamage{};
WorldClearFn* s_worldClear{};
WorldFinishedFn* s_worldFinished{};
WorldSet3DFn* s_worldSet3D{};
WorldSetOpenFn* s_worldSetOpen{};
WorldOpenFn* s_worldOpen{};

float WorldHealth(TESObjectREFR* ref)
{
    using Fn = float(ExtraDataList*);
    POINTER_SKYRIMSE(Fn, fn, 12004);
    return fn.Get()(&ref->extraData);
}
uint32_t WorldStage(TESObjectREFR* ref)
{
    using Fn = uint32_t(TESObjectREFR*);
    POINTER_SKYRIMSE(Fn, fn, 14170);
    return fn.Get()(ref);
}
uint32_t WorldOpenState(TESObjectREFR* ref)
{
    using Fn = uint32_t(TESObjectREFR*);
    POINTER_SKYRIMSE(Fn, fn, 14288);
    return fn.Get()(ref);
}
const char* WorldFinished(TESObjectREFR* ref)
{
    using Fn = const char*(ExtraDataList*);
    POINTER_SKYRIMSE(Fn, fn, 11730); // ExtraLastFinishedSequence, 140164EB0
    return fn.Get()(&ref->extraData);
}
void HookWorldDisabled(TESObjectREFR* ref, bool disabled)
{
    const auto id = ref->formID;
    s_worldDisabled(ref, disabled);
    WorldStateService::ObserveId(id, WorldStateKind::Count);
}
void HookWorldDestroyed(TESObjectREFR* ref, bool destroyed)
{
    const auto id = ref->formID;
    s_worldDestroyed(ref, destroyed);
    WorldStateService::ObserveId(id, WorldStateKind::Count);
}
void HookWorldDamage(void* destructible, TESObjectREFR* ref, float damage, bool force)
{
    const auto id = ref ? ref->formID : 0;
    s_worldDamage(destructible, ref, damage, force);
    WorldStateService::ObserveId(id, WorldStateKind::Count);
}
void HookWorldClear(TESObjectREFR* ref)
{
    const auto id = ref->formID;
    s_worldClear(ref);
    WorldStateService::ObserveId(id, WorldStateKind::Count);
}
void HookWorldFinished(TESObjectREFR* ref, const char* name)
{
    const auto id = ref->formID;
    s_worldFinished(ref, name);
    // Observe the engine's finished-sequence property, never a graph start event.
    WorldStateService::ObserveId(id, WorldStateKind::Count);
}
void HookWorldSet3D(TESObjectREFR* ref, NiNode* node, bool queue)
{
    const auto id = ref->formID;
    s_worldSet3D(ref, node, queue);
    WorldStateService::Attached(id);
}
bool HookWorldOpen(TESObjectREFR* ref, bool open, bool snap)
{
    const auto id = ref ? ref->formID : 0;
    const auto result = s_worldOpen(ref, open, snap);
    if (result) WorldStateService::ObserveId(id, WorldStateKind::Open, open, 1);
    return result;
}
void HookWorldSetOpen(void* vm, uint32_t stack, TESObjectREFR* ref, bool open)
{
    const auto id = ref ? ref->formID : 0;
    s_worldSetOpen(vm, stack, ref, open);
    // Only script SetOpen; player Activate/DoorVote is already replicated.
    WorldStateService::ObserveId(id, WorldStateKind::Open, open);
}
static TiltedPhoques::Initializer s_worldStateHooks([] {
    POINTER_SKYRIMSE(WorldFlagFn, disabled, 14646);
    POINTER_SKYRIMSE(WorldFlagFn, destroyed, 14649);
    POINTER_SKYRIMSE(WorldDamageFn, damage, 14158);
    POINTER_SKYRIMSE(WorldClearFn, clear, 14181);
    POINTER_SKYRIMSE(WorldFinishedFn, finished, 19513);
    POINTER_SKYRIMSE(WorldSet3DFn, set3D, 19729);
    POINTER_SKYRIMSE(WorldSetOpenFn, setOpen, 56233);
    POINTER_SKYRIMSE(WorldOpenFn, open, 14287);
    s_worldDisabled = disabled.Get(); s_worldDestroyed = destroyed.Get();
    s_worldDamage = damage.Get(); s_worldClear = clear.Get();
    s_worldFinished = finished.Get(); s_worldSet3D = set3D.Get(); s_worldSetOpen = setOpen.Get(); s_worldOpen = open.Get();
    TP_HOOK(&s_worldDisabled, HookWorldDisabled);
    TP_HOOK(&s_worldDestroyed, HookWorldDestroyed);
    TP_HOOK(&s_worldDamage, HookWorldDamage);
    TP_HOOK(&s_worldClear, HookWorldClear);
    TP_HOOK(&s_worldFinished, HookWorldFinished);
    TP_HOOK(&s_worldSet3D, HookWorldSet3D);
    TP_HOOK(&s_worldSetOpen, HookWorldSetOpen);
    TP_HOOK(&s_worldOpen, HookWorldOpen);
});
}

uint32_t WorldStateService::EnableParent(TESObjectREFR* ref) noexcept
{
    if (!ref) return UINT32_MAX;
    const auto* extra = ref->extraData.GetByType(static_cast<ExtraDataType>(0x36));
    if (!extra) return 0;
    uint32_t handle{};
    std::memcpy(&handle, reinterpret_cast<const uint8_t*>(extra) + 0x14, sizeof(handle));
    using Resolve = bool(uint32_t&, TESObjectREFR*&);
    POINTER_SKYRIMSE(Resolve, resolve, 17201);
    TESObjectREFR* parent{};
    resolve.Get()(handle, parent);
    // Read BEFORE releasing the smart-pointer ownership returned by 17201.
    const auto id = parent ? parent->formID : UINT32_MAX;
    if (parent) parent->handleRefObject.DecRefHandle();
    return id;
}

namespace
{
// Opt-in, main-tail observations only. No serializer, animation event, motion
// setter or physics mutation is called. Failed reads stay explicitly unknown.
template <class T> bool WorldProbeRead(const void* address, T& value)
{
    SIZE_T read{};
    return address && ReadProcessMemory(GetCurrentProcess(), address, &value, sizeof(value), &read) && read == sizeof(value);
}
std::string WorldProbeFloats(const float* values, size_t count)
{
    std::string result = "[";
    for (size_t i = 0; i < count; ++i)
    {
        if (i) result += ',';
        result += std::isfinite(values[i]) ? fmt::format("{:.4f}", values[i]) : "null";
    }
    return result + ']';
}
bool WorldProbeIsRigidCollision(NiObject* object)
{
    // NiRTTI has name/parent; 20014 uses the same chain for bhkRigidBody.
    struct Type { const char* Name; const void* Parent; } type{};
    auto* current = reinterpret_cast<const void*>(object->GetRTTI());
    for (unsigned depth = 0; current && depth < 16; ++depth)
    {
        if (!WorldProbeRead(current, type)) return false;
        char name[21]{}; // "bhkNiCollisionObject" including NUL
        if (WorldProbeRead(type.Name, name) && std::memcmp(name, "bhkNiCollisionObject", sizeof(name)) == 0) return true;
        current = type.Parent;
    }
    return false;
}
std::string WorldCollisionDiagnostic(TESObjectREFR* ref)
{
    std::string result = "\"collisionNodes\":[";
    unsigned slots{}, emitted{};
    bool truncated{};
    auto walk = [&](auto&& self, NiAVObject* node, const std::string& path, unsigned depth) -> void {
        if (!node) return;
        if (depth > 16 || slots >= 128 || emitted >= 32) { truncated = true; return; }
        ++slots;
        if (auto* collision = reinterpret_cast<NiObject*>(node->collisionObject))
        {
            void* wrapper{};
            void* body{};
            ActorPoseDiagnosticViews::RigidBody state{};
            uint32_t flags{};
            const bool rigidCollision = WorldProbeIsRigidCollision(collision);
            if (rigidCollision)
            {
                using GetBody = void*(void*);
                POINTER_SKYRIMSE(GetBody, getBody, 20014);
                wrapper = getBody.Get()(collision); // native RTTI check rejects phantoms
                WorldProbeRead(reinterpret_cast<const uint8_t*>(collision) + 0x18, flags);
            }
            const bool readable = wrapper && WorldProbeRead(static_cast<const uint8_t*>(wrapper) + 0x10, body) &&
                WorldProbeRead(body, state) && state.motionType >= 1 && state.motionType <= 7;
            result += fmt::format("{}{{\"path\":\"{}\",\"rigidCollision\":{},\"bodyReadable\":{},\"flags\":{},"
                "\"inWorld\":{},\"motionType\":{},\"nodeWorld\":{},\"nodeRotation\":{},\"nodeScale\":{},\"bodyTransformHavok\":{},\"velocityHavok\":{}}}",
                emitted++ ? "," : "", path, rigidCollision, readable, flags, readable && state.world,
                readable ? int(state.motionType) : -1, WorldProbeFloats(&node->world.translate.x, 3),
                WorldProbeFloats(&node->world.rotate.entry[0][0], 9), WorldProbeFloats(&node->world.scale, 1),
                readable ? WorldProbeFloats(state.transform, 16) : "null",
                readable ? WorldProbeFloats(state.linearVelocity, 3) : "null");
        }
        if (auto* parent = node->AsNode(); parent && parent->children.data)
            for (uint16_t i = 0; i < parent->children.length; ++i)
            {
                // Count null array slots too, so sparse trees cannot evade the budget.
                if (slots >= 128 || emitted >= 32) { truncated = true; break; }
                auto* child = parent->children.data[i];
                if (!child) { ++slots; continue; }
                self(self, child, path + '/' + std::to_string(i), depth + 1);
            }
    };
    walk(walk, ref->GetNiNode(), "root", 0);
    return result + fmt::format("],\"collisionSlots\":{},\"collisionTruncated\":{}", slots, truncated);
}
std::string WorldGraphDiagnostic(BSAnimationGraphManager* manager)
{
    using namespace ActorPoseDiagnosticViews;
    std::string result = "\"graphs\":[";
    if (!manager) return result + ']';
    BSScopedLock<BSRecursiveLock> lock(manager->lock);
    const auto count = (std::min)(manager->animationGraphs.size, 4u);
    for (uint32_t graphIndex = 0; graphIndex < count; ++graphIndex)
    {
        auto* graph = manager->animationGraphs.Get(graphIndex);
        BehaviorGraph behavior{};
        ActiveNodeList nodes{};
        const bool readable = graph && WorldProbeRead(graph->behaviorGraph, behavior) &&
            WorldProbeRead(behavior.activeNodes, nodes) && nodes.size >= 0 && nodes.size <= 1024;
        result += fmt::format("{}{{\"index\":{},\"activeNodesReadable\":{},\"active\":{},\"count\":{},\"truncated\":{},\"states\":[",
            graphIndex ? "," : "", graphIndex, readable, readable && behavior.isActive,
            readable ? nodes.size : -1, readable && nodes.size > 64);
        unsigned emitted{};
        for (int i = 0; readable && nodes.data && i < (std::min)(nodes.size, 64); ++i)
        {
            ActiveNodeInfo info{};
            StateMachine state{};
            // RTTI proves the active clone's type before reading StateMachine fields.
            if (!WorldProbeRead(static_cast<const ActiveNodeInfo*>(nodes.data) + i, info) || !info.nodeClone ||
                !Cast<hkbStateMachine>(reinterpret_cast<hkbGenerator*>(info.nodeClone)) ||
                !WorldProbeRead(info.nodeClone, state)) continue;
            result += fmt::format("{}{{\"nodeId\":{},\"state\":{},\"previous\":{},\"active\":{}}}",
                emitted++ ? "," : "", state.nodeID, state.currentStateID, state.previousStateID, state.isActive);
        }
        result += "]}";
    }
    return result + fmt::format("],\"graphsTruncated\":{}", manager->animationGraphs.size > count);
}
}

std::string WorldStateService::AnimationDiagnostic(TESObjectREFR* ref) noexcept
{
    BSAnimationGraphManager* manager{};
    ref->animationGraphHolder.GetBSAnimationGraph(&manager);
    const bool hasGraph = manager != nullptr;
    const auto graphState = WorldGraphDiagnostic(manager);
    if (manager) manager->Release();
    const char* name = WorldFinished(ref);
    std::string escaped;
    if (name) for (const unsigned char c : std::string(name))
    {
        if (c == '\\' || c == '"') escaped.push_back('\\');
        if (c >= 0x20) escaped.push_back(static_cast<char>(c));
    }
    auto result = fmt::format("\"hasGraph\":{},\"finishedSequence\":\"{}\",\"destructionStage\":{},\"destructionHealth\":{},\"openState\":{}",
        hasGraph, escaped, ref->baseForm ? WorldStage(ref) : UINT32_MAX, WorldHealth(ref), WorldOpenState(ref));
    if (const auto* node = ref->GetNiNode())
        result += fmt::format(",\"rootCollisionPresent\":{},\"rootWorld\":[{},{},{}]", node->collisionObject != nullptr,
            node->world.translate.x, node->world.translate.y, node->world.translate.z);
    result += ',' + graphState + ',' + WorldCollisionDiagnostic(ref);
    return result;
}

void WorldStateService::Sample(TESObjectREFR* ref) noexcept
{
    if (!Eligible(ref)) return;
    if (!EnableParent(ref)) Observe(ref, WorldStateKind::Disabled, ref->IsDisabled());
    Observe(ref, WorldStateKind::Destroyed, (ref->flags & 0x800000) != 0);
    if (ref->flags & 0x1000000)
        Observe(ref, WorldStateKind::DestructionHealth, WorldStage(ref), WorldHealth(ref));
    if (ref->baseForm->formType == FormType::Door)
    {
        const auto open = WorldOpenState(ref);
        if (open == 1 || open == 3) Observe(ref, WorldStateKind::Open, open == 1, 3);
        const auto* lock = ref->GetLock();
        Observe(ref, WorldStateKind::Lock, lock ? uint32_t(lock->lockLevel) | (lock->IsLocked() ? 0x100u : 0u) : 0u);
    }
    // Door animation/activation and lock events keep their existing live paths.
    // A completed sequence is durable on ordinary animated objects only.
    if (ref->baseForm->formType != FormType::Door)
        if (const auto* name = WorldFinished(ref); name && *name)
            Observe(ref, WorldStateKind::FinishedSequence, 0, 0, name);
}

bool WorldStateService::Matches(TESObjectREFR* ref, const WorldState& state) noexcept
{
    switch (state.Kind)
    {
    case WorldStateKind::Disabled: return ref->IsDisabled() == (state.Value != 0);
    case WorldStateKind::Destroyed: return ((ref->flags & 0x800000) != 0) == (state.Value != 0);
    case WorldStateKind::DestructionHealth: return WorldStage(ref) == state.Value && std::abs(WorldHealth(ref) - state.Scalar) <= 0.001f;
    case WorldStateKind::Open:
    {
        const auto open = WorldOpenState(ref);
        return open == (state.Value ? 1u : 3u); // settled open/closed, not opening/closing
    }
    case WorldStateKind::Lock:
    {
        const auto* lock = ref->GetLock();
        return (lock ? uint32_t(lock->lockLevel) | (lock->IsLocked() ? 0x100u : 0u) : 0u) == state.Value;
    }
    // A matching name is not proof that a freshly attached 3D has its pose.
    default: return false;
    }
}

bool WorldStateService::Apply(TESObjectREFR* ref, const WorldState& state) noexcept
{
    if (!IsMainThread()) return false;
    switch (state.Kind)
    {
    case WorldStateKind::Disabled:
    {
        if (EnableParent(ref)) return false; // never force an enable-parent child
        using Enable = void(TESObjectREFR*);
        using Disable = void(TESObjectREFR*);
        POINTER_SKYRIMSE(Enable, enable, 19800);
        POINTER_SKYRIMSE(Disable, disable, 19801);
        if (!Matches(ref, state))
        {
            if (state.Value) disable.Get()(ref);
            else enable.Get()(ref);
        }
        return Matches(ref, state);
    }
    case WorldStateKind::Destroyed:
        s_worldDestroyed(ref, state.Value != 0);
        return Matches(ref, state);
    case WorldStateKind::Open:
        if (!WorldStateTable::ShouldDeliverLive(state) && state.Scalar != 2) return false;
        if (ref->baseForm->formType != FormType::Door) return false;
        if (!ref->GetNiNode() || ref->IsDisabled()) return false;
        // A newly attached node can have a stale pose despite matching flags.
        if (state.Scalar == 2)
            return s_worldOpen(ref, state.Value != 0, true) && Matches(ref, state);
        if (Matches(ref, state)) return true;
        {
            const auto open = WorldOpenState(ref);
            // Do not restart an animation already moving toward the requested state.
            if (open != (state.Value ? 2u : 4u))
                s_worldSetOpen(nullptr, 0, ref, state.Value != 0);
        }
        // 56233 does not read VM/stack in 1.7.104. Acceptance is not completion.
        return Matches(ref, state);
    case WorldStateKind::Lock:
    {
        // Same native level + SetLock + LockChange sequence as ObjectService.
        if (Matches(ref, state)) return true;
        auto* lock = ref->GetLock();
        if (!lock) lock = ref->CreateLock();
        if (!lock) return false;
        lock->lockLevel = static_cast<uint8_t>(state.Value);
        lock->SetLock((state.Value & 0x100) != 0);
        TiltedPhoques::ThisCall(RealLockChange, ref);
        return Matches(ref, state);
    }
    case WorldStateKind::DestructionHealth:
    case WorldStateKind::FinishedSequence:
        // Withdraw this task's speculative restore paths. A flag/name or native
        // return value does not establish graph, replacement-model or collision
        // convergence. Keep authoritative state pending for a verified adapter.
        return false;
    default: return false;
    }
}
