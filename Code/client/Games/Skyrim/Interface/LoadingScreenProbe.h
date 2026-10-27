#pragma once

#include <cstdint>
#include <vector>

struct LoadingScreenProbeSnapshot
{
    uint64_t Epoch{};
    bool LoadingMenuPresent{};
    bool RemainingTipsReadable{};
    std::vector<uint32_t> RemainingTipFormIds{};

    bool MistMenuPresent{};
    bool MistStateReadable{};
    bool ShowMist{};
    bool ShowLoadScreen{};
    bool LoadScreenModelPresent{};
    bool LoadScreenModelHasUserData{};
    bool CameraPathPresent{};
    float CameraFov{};
    float AngleZ{};

    bool SelectedIdentityResolved{};
    uint32_t SelectedFormId{};
};

class LoadingScreenProbe
{
public:
    [[nodiscard]] static LoadingScreenProbe& Get() noexcept;

    // Must be called on Skyrim's game thread. This probe never mutates menu,
    // form, scene-graph, or loading state.
    [[nodiscard]] const LoadingScreenProbeSnapshot& Sample() noexcept;

private:
    LoadingScreenProbeSnapshot m_snapshot{};
    bool m_wasLoadingMenuPresent{};
    uint64_t m_candidateSignature{};
};
