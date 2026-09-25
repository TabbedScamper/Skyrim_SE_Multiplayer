#pragma once

// Engine bug fixes the co-op experience depends on, built in so players do not have to assemble a
// fix list. Each one is enabled only where it is needed and credits the community fix it follows
// (see docs/CREDITS.md).
namespace EngineFixes
{
// Main thread, once per frame.
void OnFrame() noexcept;
} // namespace EngineFixes
