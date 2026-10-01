#pragma once

#include <Forms/TESActorBase.h>

#include <Components/TESRaceForm.h>
#include <Components/BGSOverridePackCollection.h>

struct BGSColorForm;
struct BGSTextureSet;
struct TESClass;
struct TESCombatStyle;
struct TESObjectARMO;
struct BGSOutfit;
struct BGSHeadPart;
struct BGSRelationship;

struct TESNPC : TESActorBase
{
    static constexpr FormType Type = FormType::Npc;

    static TESNPC* Create(const String& acBuffer, uint32_t aChangeFlags) noexcept;

    TESNPC* GetTemplateBase() const noexcept
    {
        TESNPC* pTemplate = faceNPC;

        while (pTemplate && pTemplate->IsTemporary())
            pTemplate = pTemplate->faceNPC;

        return pTemplate;
    }

    struct FaceMorphs
    {
        float option[19];
        uint32_t presets[4];

        void CopyFrom(const FaceMorphs& acRhs)
        {
            std::copy(std::begin(acRhs.option), std::end(acRhs.option), std::begin(option));
            std::copy(std::begin(acRhs.presets), std::end(acRhs.presets), std::begin(presets));
        }
    };

    struct HeadData
    {
        BGSColorForm* hairColor;
        BGSTextureSet* headTexture;
    };

    TESRaceForm raceForm;
    BGSOverridePackCollection overridePacks;
    void* unkDC;
    uint8_t unk0E0[0x24];
    uint8_t pad1B4[0x6];
    uint16_t unk10A;
    TESClass* npcClass;

    HeadData* headData;
    uintptr_t unk114;
    TESCombatStyle* combatStyle;
    size_t unk11C;
    TESRace* originalRace;
    TESNPC* faceNPC;
    float height;
    float weight;
    void* sounds;
    BSFixedString shortName;
    TESObjectARMO* farSkin;
    BGSOutfit* defaultOutfit;
    BGSOutfit* sleepOutfit;
    uintptr_t unk144;
    TESFaction* faction;

    BGSHeadPart** headparts;
    uint8_t headpartsCount;

#if TP_PLATFORM_64
    uint8_t pad241[5];
#else
    uint8_t pad151[3];
#endif

    struct Color
    {
        uint8_t red, green, blue;
    } color;

    GameArray<BGSRelationship*>* relationships;
    FaceMorphs* faceMorphs;
    uintptr_t unk160;

    BGSHeadPart* GetHeadPart(uint32_t aType);
    // Marks the body weight/height trailer Serialize appends after the native NPC save.
    static constexpr char kBodyTrailerTag[4] = {'T', 'P', 'B', 'W'};
    void Serialize(String* apSaveBuffer) const noexcept;
    // aLocalPlayerSnapshot: only CharacterSnapshots::Apply may load into the local player's own NPC.
    bool Deserialize(const String& acBuffer, uint32_t aChangeFlags, bool aLocalPlayerSnapshot = false) noexcept;
    void Initialize() noexcept;
};

static_assert(offsetof(TESNPC, npcClass) == 0x1C0);
static_assert(sizeof(TESNPC::FaceMorphs) == 0x5C);
static_assert(offsetof(TESNPC, headData) == 0x1C8);
static_assert(offsetof(TESNPC, faceNPC) == 0x1F0);
static_assert(offsetof(TESNPC, headparts) == 0x238);
static_assert(offsetof(TESNPC, color) == 0x246);
static_assert(offsetof(TESNPC, relationships) == 0x250);
static_assert(offsetof(TESNPC, faceMorphs) == 0x258);
static_assert(offsetof(TESNPC, unk160) == 0x260); // CommonLib TESNPC::tintLayers
