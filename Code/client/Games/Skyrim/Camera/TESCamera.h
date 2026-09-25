#pragma once

#include <Games/Primitives.h>

struct NiNode;
struct NiObject;
struct NiCamera;
struct TESCameraState;

struct TESCamera
{
    virtual ~TESCamera(){};

    virtual void SetNode(NiNode* node){};
    virtual void Update(){};

    NiCamera* GetNiCamera();
    bool SetState(TESCameraState* apState) noexcept;

    float rotZ;
    float rotX;
    NiPoint3 pos;
    float zoom;
    NiNode* cameraNode;
    TESCameraState* state;
    bool unk;
};

static_assert(offsetof(TESCamera, cameraNode) == 0x20);
static_assert(offsetof(TESCamera, state) == 0x28);
static_assert(sizeof(TESCamera) == 0x38);
