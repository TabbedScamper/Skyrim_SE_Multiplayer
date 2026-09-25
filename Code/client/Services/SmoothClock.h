#pragma once

#include <cstdint>

// The shared (server-synchronized) clock as a smooth, high-resolution time for playback.
//
// SynchronizedClock ticks in whole milliseconds, advances only when the transport updates it, and
// steps by half the measured ping on every resync. Played back directly, the cart advanced by an
// uneven amount of shared time every frame although the frames were evenly spaced; it showed as
// the follower's cart surging while the host's was smooth. Here a high-resolution local clock is
// locked to the shared clock by a slow correction: it follows the shared time (snapping only on
// a large jump) without carrying its steps or its millisecond rounding.
namespace SmoothClock
{
// Feed the shared clock (after each transport update). Any thread.
void Observe(uint64_t aSharedTick) noexcept;
// The shared time now, in milliseconds with a fractional part. 0 until the first Observe.
[[nodiscard]] double NowMs() noexcept;
[[nodiscard]] uint64_t NowTick() noexcept;
} // namespace SmoothClock
