#include <Services/EngineFixes.h>

#include <Games/TES.h>

#include <chrono>
#include <cstring>

namespace
{
// Havok frame-rate fix, following SSE Display Tweaks (HavokFPS): Skyrim steps Havok with a fixed
// maximum time step fMaxTime:HAVOK (1/60 s). Above 60 FPS each frame then advances physics by
// more time than actually passed, so physics objects glide and jitter and scripted physics
// (the Helgen carts) misbehave. In co-op the host's physics is what every player sees, so one
// fast host breaks it for everyone. Keep the step at the real frame time when it is shorter
// than 1/60; at 60 FPS or below the game's own value is left untouched.
struct HavokStep
{
    Setting* pSetting{};
    float Original{};
    float Applied{};
    double AverageDt{1.0 / 60.0};
    std::chrono::steady_clock::time_point Last{};
    bool Looked{};
};
HavokStep s_havok;

float ReadFloat(const Setting* apSetting) noexcept
{
    float value{};
    std::memcpy(&value, &apSetting->data, sizeof(value));
    return value;
}

void WriteFloat(Setting* apSetting, float aValue) noexcept
{
    std::memcpy(&apSetting->data, &aValue, sizeof(aValue));
}

void UpdateHavokStep() noexcept
{
    auto& h = s_havok;
    if (!h.Looked)
    {
        h.Looked = true;
        if (auto* pSettings = INISettingCollection::Get())
            h.pSetting = pSettings->GetSetting("fMaxTime:HAVOK");
        if (h.pSetting)
        {
            h.Original = ReadFloat(h.pSetting);
            h.Applied = h.Original;
            spdlog::info("Engine fix: Havok frame-rate step active (fMaxTime:HAVOK was {:.5f})", h.Original);
        }
    }
    if (!h.pSetting || h.Original <= 0.f)
        return;

    const auto now = std::chrono::steady_clock::now();
    if (h.Last != std::chrono::steady_clock::time_point{})
    {
        const double dt = std::chrono::duration<double>(now - h.Last).count();
        // Ignore loading screens and hitches; they are not the frame rate.
        if (dt > 0.0 && dt < 0.1)
            h.AverageDt += (dt - h.AverageDt) / 30.0;
    }
    h.Last = now;

    const float target = h.AverageDt < h.Original ? static_cast<float>((std::max)(h.AverageDt, 1.0 / 240.0)) : h.Original;
    if (std::abs(target - h.Applied) > h.Applied * 0.03f)
    {
        WriteFloat(h.pSetting, target);
        if ((target == h.Original) != (h.Applied == h.Original))
            spdlog::info("Engine fix: Havok step {:.5f} s ({:.0f} FPS)", target, 1.0 / h.AverageDt);
        h.Applied = target;
    }
}
} // namespace

namespace EngineFixes
{
void OnFrame() noexcept
{
    UpdateHavokStep();
}
} // namespace EngineFixes
