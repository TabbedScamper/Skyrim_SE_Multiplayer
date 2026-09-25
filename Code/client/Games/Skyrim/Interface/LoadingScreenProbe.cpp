#include <TiltedOnlinePCH.h>

#include <Games/Skyrim/Interface/LoadingScreenProbe.h>

#include <Games/Skyrim/Forms/TESForm.h>
#include <Games/Skyrim/Interface/UI.h>
#include <Games/Skyrim/NetImmerse/NiAVObject.h>

namespace
{
constexpr uintptr_t cLoadingMenuLoadScreensOffset = 0x58;
constexpr uintptr_t cMistMenuCameraPathOffset = 0xC8;
constexpr uintptr_t cMistMenuLoadScreenModelOffset = 0xF0;
constexpr uintptr_t cMistMenuCameraFovOffset = 0x100;
constexpr uintptr_t cMistMenuAngleZOffset = 0x104;
constexpr uintptr_t cMistMenuShowMistOffset = 0x134;
constexpr uintptr_t cMistMenuShowLoadScreenOffset = 0x135;
constexpr uint8_t cLoadScreenFormType = 0x51;
constexpr uint32_t cMaxEligibleLoadScreens = 4096;

struct CandidateArrayView
{
    TESForm** Data{};
    alignas(sizeof(void*)) uint32_t Capacity{};
    alignas(sizeof(void*)) uint32_t Length{};
};
static_assert(sizeof(CandidateArrayView) == 0x18);

bool IsReadable(const void* apData, size_t aSize) noexcept
{
    if (!apData || aSize == 0)
        return false;

    auto address = reinterpret_cast<uintptr_t>(apData);
    if (address > std::numeric_limits<uintptr_t>::max() - aSize)
        return false;
    const auto end = address + aSize;

    while (address < end)
    {
        MEMORY_BASIC_INFORMATION info{};
        if (VirtualQuery(reinterpret_cast<const void*>(address), &info, sizeof(info)) != sizeof(info) || info.State != MEM_COMMIT ||
            (info.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0)
            return false;

        const auto regionEnd = reinterpret_cast<uintptr_t>(info.BaseAddress) + info.RegionSize;
        if (regionEnd <= address)
            return false;
        address = std::min(regionEnd, end);
    }

    return true;
}

template <class T> bool ReadAt(const void* apObject, uintptr_t aOffset, T& aValue) noexcept
{
    if (!apObject)
        return false;
    const auto* pValue = reinterpret_cast<const uint8_t*>(apObject) + aOffset;
    if (!IsReadable(pValue, sizeof(T)))
        return false;
    std::memcpy(&aValue, pValue, sizeof(T));
    return true;
}

uint64_t HashCandidates(const std::vector<uint32_t>& acIds) noexcept
{
    uint64_t hash = 1469598103934665603ULL;
    for (const auto id : acIds)
    {
        hash ^= id;
        hash *= 1099511628211ULL;
    }
    hash ^= acIds.size();
    return hash;
}
} // namespace

LoadingScreenProbe& LoadingScreenProbe::Get() noexcept
{
    static LoadingScreenProbe s_probe;
    return s_probe;
}

const LoadingScreenProbeSnapshot& LoadingScreenProbe::Sample() noexcept
{
    auto* pUI = UI::Get();
    auto* pLoadingMenu = pUI ? pUI->FindMenuByName(BSFixedString("Loading Menu")) : nullptr;
    auto* pMistMenu = pUI ? pUI->FindMenuByName(BSFixedString("Mist Menu")) : nullptr;

    m_snapshot.LoadingMenuPresent = pLoadingMenu != nullptr;
    m_snapshot.MistMenuPresent = pMistMenu != nullptr;
    m_snapshot.CandidateListReadable = false;
    m_snapshot.EligibleFormIds.clear();
    m_snapshot.MistStateReadable = false;
    m_snapshot.ShowMist = false;
    m_snapshot.ShowLoadScreen = false;
    m_snapshot.LoadScreenModelPresent = false;
    m_snapshot.LoadScreenModelHasUserData = false;
    m_snapshot.CameraPathPresent = false;
    m_snapshot.CameraFov = 0.f;
    m_snapshot.AngleZ = 0.f;
    m_snapshot.SelectedIdentityResolved = false;
    m_snapshot.SelectedFormId = 0;

    if (m_snapshot.LoadingMenuPresent && !m_wasLoadingMenuPresent)
    {
        ++m_snapshot.Epoch;
        spdlog::info("Loading-screen probe opened local epoch {}", m_snapshot.Epoch);
    }

    if (pLoadingMenu)
    {
        CandidateArrayView candidates{};
        if (ReadAt(pLoadingMenu, cLoadingMenuLoadScreensOffset, candidates) && candidates.Length <= candidates.Capacity && candidates.Length <= cMaxEligibleLoadScreens &&
            (candidates.Length == 0 || (candidates.Data && IsReadable(candidates.Data, sizeof(TESForm*) * candidates.Length))))
        {
            m_snapshot.CandidateListReadable = true;
            m_snapshot.EligibleFormIds.reserve(candidates.Length);
            for (uint32_t i = 0; i < candidates.Length; ++i)
            {
                TESForm* pForm{};
                std::memcpy(&pForm, candidates.Data + i, sizeof(pForm));
                uint32_t formId{};
                uint8_t formType{};
                if (ReadAt(pForm, offsetof(TESForm, formID), formId) && ReadAt(pForm, offsetof(TESForm, formType), formType) && formType == cLoadScreenFormType)
                    m_snapshot.EligibleFormIds.push_back(formId);
            }
        }
    }

    if (pMistMenu)
    {
        void* pCameraPath{};
        NiAVObject* pLoadScreenModel{};
        float cameraFov{};
        float angleZ{};
        bool showMist{};
        bool showLoadScreen{};
        if (ReadAt(pMistMenu, cMistMenuCameraPathOffset, pCameraPath) && ReadAt(pMistMenu, cMistMenuLoadScreenModelOffset, pLoadScreenModel) &&
            ReadAt(pMistMenu, cMistMenuCameraFovOffset, cameraFov) && ReadAt(pMistMenu, cMistMenuAngleZOffset, angleZ) && ReadAt(pMistMenu, cMistMenuShowMistOffset, showMist) &&
            ReadAt(pMistMenu, cMistMenuShowLoadScreenOffset, showLoadScreen) && std::isfinite(cameraFov) && std::isfinite(angleZ))
        {
            m_snapshot.MistStateReadable = true;
            m_snapshot.CameraPathPresent = pCameraPath != nullptr;
            m_snapshot.LoadScreenModelPresent = pLoadScreenModel != nullptr;
            m_snapshot.CameraFov = cameraFov;
            m_snapshot.AngleZ = angleZ;
            m_snapshot.ShowMist = showMist;
            m_snapshot.ShowLoadScreen = showLoadScreen;

            void* pUserData{};
            if (pLoadScreenModel && ReadAt(pLoadScreenModel, offsetof(NiAVObject, userData), pUserData))
                m_snapshot.LoadScreenModelHasUserData = pUserData != nullptr;
        }
    }

    const auto signature = HashCandidates(m_snapshot.EligibleFormIds);
    if (m_snapshot.LoadingMenuPresent && (!m_wasLoadingMenuPresent || signature != m_candidateSignature))
    {
        spdlog::info(
            "Loading-screen probe epoch {}: candidatesReadable={}, eligibleLSCRs={}, mistPresent={}, "
            "mistReadable={}, modelPresent={}, cameraPathPresent={}, selectedIdentityResolved=false",
            m_snapshot.Epoch, m_snapshot.CandidateListReadable, m_snapshot.EligibleFormIds.size(), m_snapshot.MistMenuPresent, m_snapshot.MistStateReadable,
            m_snapshot.LoadScreenModelPresent, m_snapshot.CameraPathPresent);
    }

    m_candidateSignature = signature;
    m_wasLoadingMenuPresent = m_snapshot.LoadingMenuPresent;
    return m_snapshot;
}
