#include <TiltedOnlinePCH.h>

#include <PlayerCharacter.h>
#include <Forms/BGSHeadPart.h>
#include <Forms/TESNPC.h>

#include <Games/IFormFactory.h>

#include <SaveLoad.h>
#include <Games/Overrides.h>

TESForm* TESForm::GetById(const uint32_t aId)
{
    using TGetFormById = TESForm*(uint32_t);
    POINTER_SKYRIMSE(TGetFormById, getFormById, 14617);

    return getFormById.Get()(aId);
}

void TESNPC::Serialize(String* apSaveBuffer) const noexcept
{
    ScopedSaveLoadOverride saveLoadOverride;

    char buffer[1 << 15];

    BGSSaveFormBuffer saveBuffer;
    saveBuffer.buffer = buffer;
    saveBuffer.capacity = 1 << 15;
    saveBuffer.changeFlags = GetChangeFlags();

    Save(&saveBuffer);

    apSaveBuffer->assign(saveBuffer.buffer, saveBuffer.position);

    saveBuffer.buffer = nullptr;

    // The native NPC save (TESNPC vtable slot 14) writes no body weight or height, so the peer
    // built the remote player at the default (measured: the host's armor meshes loaded at [75%]
    // on the host and [50%] on the follower). Carry both as a trailer that Deserialize strips
    // before the native load. The armor loader (FUN_140218e50) takes the weight from the last
    // NPC in the faceNPC chain, not from this record (measured: player base 0x7 said 100 while
    // its meshes loaded at 75), so send that one; the peer's copy has no chain.
    const TESNPC* pBody = this;
    for (int depth = 0; pBody->faceNPC && pBody->faceNPC != pBody && depth < 16; ++depth)
        pBody = pBody->faceNPC;
    const float bodyWeight = pBody->weight;
    const float bodyHeight = height;
    apSaveBuffer->append(kBodyTrailerTag, sizeof(kBodyTrailerTag));
    apSaveBuffer->append(reinterpret_cast<const char*>(&bodyWeight), sizeof(bodyWeight));
    apSaveBuffer->append(reinterpret_cast<const char*>(&bodyHeight), sizeof(bodyHeight));
}

bool TESNPC::Deserialize(const String& acBuffer, uint32_t aChangeFlags, bool aLocalPlayerSnapshot) noexcept
{
    // Unchanged static NPC bases have no appearance payload. In particular,
    // spawning an FF copy must not run LoadGame on its shared base for a no-op.
    // A body-weight trailer is nonempty and still takes the normal path below.
    if (acBuffer.empty())
        return aChangeFlags == 0;

    // Network appearance must never load into the local player or its face template.
    // Checking the actor's ref ID alone does not protect a replica using the same base.
    // The one exception is deliberate: applying this player's own guest character (CharacterSnapshots::Apply).
    auto* pPlayer = PlayerCharacter::Get();
    auto* pLocalNpc = pPlayer && !aLocalPlayerSnapshot ? Cast<TESNPC>(pPlayer->baseForm) : nullptr;
    for (int depth = 0; pLocalNpc && depth < 16; ++depth)
    {
        if (this == pLocalNpc)
        {
            spdlog::error("Rejected network appearance for local player NPC {:X}", formID);
            return false;
        }
        if (pLocalNpc->faceNPC == pLocalNpc)
            break;
        pLocalNpc = pLocalNpc->faceNPC;
    }

    ScopedSaveLoadOverride saveLoadOverride;

    constexpr size_t cTrailerSize = sizeof(kBodyTrailerTag) + sizeof(float) * 2;
    size_t nativeSize = acBuffer.size();
    if (nativeSize >= cTrailerSize &&
        std::memcmp(acBuffer.data() + nativeSize - cTrailerSize, kBodyTrailerTag, sizeof(kBodyTrailerTag)) == 0)
    {
        nativeSize -= cTrailerSize;
        float bodyWeight{}, bodyHeight{};
        std::memcpy(&bodyWeight, acBuffer.data() + nativeSize + sizeof(kBodyTrailerTag), sizeof(float));
        std::memcpy(&bodyHeight, acBuffer.data() + nativeSize + sizeof(kBodyTrailerTag) + sizeof(float), sizeof(float));
        if (weight != bodyWeight || height != bodyHeight)
            spdlog::info("Applying body to NPC {:X}: weight {} -> {}, height {} -> {}", formID, weight, bodyWeight,
                height, bodyHeight);
        if (std::isfinite(bodyWeight) && bodyWeight >= 0.f && bodyWeight <= 100.f)
            weight = bodyWeight;
        if (std::isfinite(bodyHeight) && bodyHeight > 0.f && bodyHeight < 10.f)
            height = bodyHeight;
    }

    BGSLoadFormBuffer loadBuffer(aChangeFlags);
    loadBuffer.SetSize(nativeSize & 0xFFFFFFFF);
    loadBuffer.buffer = acBuffer.data();
    loadBuffer.formId = formID;
    loadBuffer.form = this;

    Load(&loadBuffer);

    loadBuffer.buffer = nullptr;
    return true;
}

void TESNPC::Initialize() noexcept
{
    auto pPlayerBaseForm = Cast<TESNPC>(PlayerCharacter::Get()->baseForm);

    // These values are all defaulted, if the other actor did not modify them they won't be loaded, therefore we need to force them before load
    attackDataForm.attackDataMap = pPlayerBaseForm->attackDataForm.attackDataMap;
    npcClass = pPlayerBaseForm->npcClass;
    combatStyle = pPlayerBaseForm->combatStyle;
    raceForm.race = pPlayerBaseForm->raceForm.race;
    defaultOutfit = pPlayerBaseForm->defaultOutfit;
    sleepOutfit = pPlayerBaseForm->sleepOutfit;
    spellList.Initialize();
    // End defaults

    flags |= 0x200000;
}

void TESForm::Save_Reversed(const uint32_t aChangeFlags, Buffer::Writer& aWriter)
{
    if (aChangeFlags & 1)
    {
        aWriter.WriteBytes(reinterpret_cast<uint8_t*>(&flags), 4);
        aWriter.WriteBytes(reinterpret_cast<uint8_t*>(&unk10), 2);
    }
}

void TESForm::SetSkipSaveFlag(bool aSet) noexcept
{
    if (aSet)
    {
        unk10 = 0xFFFF;
    }
    /*const uint32_t flag = 1 << 14;

    if (aSet)
        flags |= flag;
    else
        flags &= ~flag;*/
}

uint32_t TESForm::GetChangeFlags() const noexcept
{
    struct Unk
    {
        uint8_t unk0[0x330];
        void* unk330;
    };

    TP_THIS_FUNCTION(InternalGetChangeFlags, bool, void, uint32_t formId, ChangeFlags& changeFlags);

    POINTER_SKYRIMSE(InternalGetChangeFlags, internalGetChangeFlags, 35503);

    POINTER_SKYRIMSE(Unk*, s_singleton, 403330);

    const auto pUnk = *(s_singleton.Get());

    ChangeFlags changeFlags;
    const auto cResult = TiltedPhoques::ThisCall(internalGetChangeFlags, pUnk->unk330, formID, changeFlags);
    if (!cResult)
        return 0;

    return changeFlags.flags;
}

TESNPC* TESNPC::Create(const String& acBuffer, const uint32_t aChangeFlags) noexcept
{
    auto pNpc = IFormFactory::Create<TESNPC>();

    pNpc->Initialize();
    // Keep the factory's ownership: HeadData/headparts/tintLayers start null.
    // LoadGame (ID 24778, VA 1403C7B10) allocates HeadData and headparts per NPC,
    // and replaces the immutable FaceData sentinel with private storage before writes.
    // Received tints belong to FaceGenComponent. Never CopyFrom the player:
    // the native copy also installs a faceNPC template link (ID 24665).
    pNpc->Deserialize(acBuffer, aChangeFlags);

    // This forces facegen for some reason
    pNpc->originalRace = nullptr;

    return pNpc;
}

BGSHeadPart* TESNPC::GetHeadPart(uint32_t aType)
{
    if (headparts)
    {
        for (auto i = 0; i < headpartsCount; ++i)
        {
            if (headparts[i] && headparts[i]->type == aType)
                return headparts[i];
        }
    }

    return nullptr;
}
