#pragma once

#include <Events/PacketEvent.h>

struct World;
struct CameraStateRequest;

class CameraService
{
public:
    CameraService(World& aWorld, entt::dispatcher& aDispatcher) noexcept;

    TP_NOCOPYMOVE(CameraService);

private:
    void OnCameraState(const PacketEvent<CameraStateRequest>& acMessage) noexcept;

    World& m_world;
    entt::scoped_connection m_cameraStateConnection;
};
