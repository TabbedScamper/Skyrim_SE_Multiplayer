#include <Services/SmoothClock.h>

#include <atomic>
#include <chrono>
#include <cmath>

namespace
{
// Offset (shared time minus local steady time), microseconds. INT64_MIN until observed.
std::atomic<int64_t> s_offsetUs{INT64_MIN};

// Beyond this the shared clock really jumped (reconnect, first real sync): follow at once.
constexpr int64_t kSnapUs = 250'000;
// Per observation (about a frame) move this fraction of the way, at most kMaxStepUs: about a
// second to absorb a resync step, and far below what a frame of motion can show.
constexpr double kGain = 0.02;
constexpr int64_t kMaxStepUs = 250;

int64_t SteadyUs() noexcept
{
    return std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
} // namespace

namespace SmoothClock
{
void Observe(const uint64_t aSharedTick) noexcept
{
    if (!aSharedTick)
        return;
    // The shared tick is floored to a millisecond: its true value is half a millisecond later.
    const int64_t raw = static_cast<int64_t>(aSharedTick) * 1000 + 500 - SteadyUs();
    const int64_t current = s_offsetUs.load(std::memory_order_relaxed);
    if (current == INT64_MIN || std::llabs(raw - current) > kSnapUs)
    {
        s_offsetUs.store(raw, std::memory_order_relaxed);
        return;
    }
    int64_t step = static_cast<int64_t>(std::llround(static_cast<double>(raw - current) * kGain));
    if (step > kMaxStepUs)
        step = kMaxStepUs;
    else if (step < -kMaxStepUs)
        step = -kMaxStepUs;
    s_offsetUs.store(current + step, std::memory_order_relaxed);
}

double NowMs() noexcept
{
    const int64_t offset = s_offsetUs.load(std::memory_order_relaxed);
    if (offset == INT64_MIN)
        return 0.0;
    return static_cast<double>(SteadyUs() + offset) / 1000.0;
}

uint64_t NowTick() noexcept
{
    const double now = NowMs();
    return now > 0.0 ? static_cast<uint64_t>(now) : 0;
}
} // namespace SmoothClock
