#pragma once

#include <NetImmerse/NiObjectNET.h>
#include <NetImmerse/NiTransform.h>

struct BSFixedString;

struct NiAVObject : NiObjectNET
{
    virtual ~NiAVObject();

    virtual void sub_26();
    virtual void sub_27();
    virtual void sub_28();
    virtual void sub_29();
    virtual NiAVObject* GetByName(BSFixedString& aName);

    bool SetMotionType(uint32_t aMotionType, bool aArg2, bool aArg3, bool aAllowActivate) noexcept;

    NiAVObject* parent;             // 030
    uint32_t parentIndex;           // 038
    uint32_t unk03C;                // 03C
    void* collisionObject;          // 040 (NiPointer<NiCollisionObject>)
    NiTransform local;              // 048
    NiTransform world;              // 07C
    NiTransform previousWorld;      // 0B0
    uint8_t worldBound[0x10];       // 0E4 (NiBound)
    uint32_t flags;                 // 0F4
    void* userData;                 // 0F8 (TESObjectREFR*)
    float fadeAmount;               // 100
    uint32_t lastUpdatedFrameCount; // 104
    uint8_t unk108;
    uint8_t flags02;
    uint16_t unk10A;
    uint32_t pad10C;
};

static_assert(offsetof(NiAVObject, local) == 0x48);
static_assert(offsetof(NiAVObject, world) == 0x7C);
static_assert(sizeof(NiAVObject) == 0x110);
