#pragma once

struct TESCamera;

// CommonLibSSE-NG b93280e, runtime 1.7.104 candidate layout. This wrapper is
// intentionally read-only; native state transitions go through TESCamera.
struct TESCameraState
{
    void* vtable;         // 00
    uint32_t refCount;    // 08
    uint32_t pad0C;       // 0C
    TESCamera* camera;    // 10
    uint32_t id;          // 18 (PlayerCamera::CameraState)
    uint32_t pad1C;       // 1C
};

static_assert(offsetof(TESCameraState, id) == 0x18);
static_assert(sizeof(TESCameraState) == 0x20);
