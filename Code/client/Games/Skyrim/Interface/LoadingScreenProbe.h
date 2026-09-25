#pragma once

#include <cstdint>
#include <vector>

struct LoadingScreenProbeSnapshot
{
    uint64_t Epoch{};
    bool LoadingMenuPresent{};
    bool CandidateListReadable{};
    std::vector<uint32_t> EligibleFormIds{};

    bool MistMenuPresent{};
    bool MistStateReadable{};
    bool ShowMist{};
    bool ShowLoadScreen{};
    bool LoadScreenModelPresent{};
    bool LoadScreenModelHasUserData{};
    bool CameraPathPresent{};
    float CameraFov{};
    float AngleZ{};

    // CommonLib exposes the eligible LSCR array and the instantiated NIF, but
    // not the chosen TESLoadScreen pointer. This remains false until a
    // pre-instantiation selection seam is identified and validated on 1.7.104.
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
