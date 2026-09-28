#pragma once

#include <cstdint>

// Called only at native biped loader boundaries, on the native calling thread.
uint64_t PrepareBipedArmorLoad(void* apBiped);
void TraceSpawnedCopyArmor(void* apBiped, const char* apPhase, uint64_t aRebuiltSlots = 0);
