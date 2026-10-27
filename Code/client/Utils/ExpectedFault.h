#pragma once

// Code that probes memory under its own __try/__except (AudioSwitch's freed-voice checks) marks the probe with this
// scope. The vectored crash handler runs before any __except, so without the mark it wrote a dump and closed the game on
// a fault the probe was written to absorb (2026-10-03 17:00: a Windows output switch during a stalled audio thread).
namespace ExpectedFault
{
inline thread_local int t_depth = 0;

inline bool Active() noexcept
{
    return t_depth > 0;
}

struct Scope
{
    Scope() noexcept { ++t_depth; }
    ~Scope() { --t_depth; }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;
};
} // namespace ExpectedFault
