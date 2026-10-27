#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <thread>
#include <Windows.h>
#include <VersionDb.h>

namespace MT
{
enum class Slot : uint8_t {
    None, S0a, S0b, S0c, S0d, S1, S2, S3_0, S3_1, S3_2,
    S3_3, S4, S4_5, S5, S5s, S6, S7, X01, X02, Count
};
enum class Ctx : uint8_t {
    None, AppLane, PhysicsCb, EquipCb, InventoryCb, UiProcess,
    InputPoll, Overlay, WndProc, LoadHook, Sink, VmNative, InStep
};
enum class LoadPhase : uint8_t { Idle, Loading, InFrameLoad, Ending, Settled };
constexpr size_t SlotCount = static_cast<size_t>(Slot::Count);
inline std::atomic<DWORD> MainId{};
inline std::atomic<uint64_t> X00Count{};
inline std::atomic<uint64_t> MainIdMismatch{};
inline std::atomic<uint64_t> CheckerReports{};
inline std::atomic<uint64_t> M1WrongSlot{};
inline std::atomic<uint64_t> M3aWrongSlot{};
inline std::atomic<uint64_t> M6WrongSlot{};
inline std::atomic<uint64_t> AppLaneNested{};
inline std::atomic<uint32_t> BinkDepth{};
inline std::atomic<uint64_t> MainStalls{};
inline std::atomic<uint64_t> ExtraBusy{};
inline std::atomic<uint32_t> LastExtraRef{};
inline std::atomic<uintptr_t> LastExtraLockWord{};
inline std::atomic<uint64_t> SharedWorldClosedFrames{};
inline std::atomic<uint64_t> HeldLooks{};
inline std::atomic<uint64_t> HeldSpawns{};
inline std::atomic<uint64_t> HeldAssignments{};
inline std::atomic<uint64_t> FrameSkipped{};
inline std::atomic<LoadPhase> CurrentLoadPhase{LoadPhase::Idle};
inline std::atomic<uint32_t> LoadSerial{};
inline std::atomic<uint32_t> ConnectionSerial{};
inline std::atomic<uint64_t> LoadGeneration{};
inline std::atomic<uint32_t> WorldGeneration{};
inline std::atomic<uint64_t> InFrameLoads{};
inline std::atomic<uint64_t> LoadNoops{};
inline std::atomic<uint64_t> LoadGenOrphans{};
inline std::atomic<uint32_t> LastLoadGenSerial{UINT32_MAX};
inline std::atomic<uint8_t> LastSlot{};
inline thread_local Slot t_slot = Slot::None;
inline thread_local Ctx t_context = Ctx::None;
inline std::atomic<uint32_t> CutMask{(1u << 1) | (1u << 3) | (1u << 6)};

inline void LoadCutFlags() noexcept
{
    static const bool loaded = []() noexcept {
        uint32_t mask = (1u << 1) | (1u << 3) | (1u << 6);
        wchar_t module[MAX_PATH]{};
        if (GetModuleFileNameW(nullptr, module, MAX_PATH))
        {
            const auto path = (std::filesystem::path(module).parent_path() /
                L"Data" / L"SkyrimTogetherReborn" / L"harness.ini").wstring();
            for (unsigned cut : {1u, 3u, 6u})
            {
                const auto key = cut == 3 ? L"CutM3a" : L"CutM" + std::to_wstring(cut);
                if (!GetPrivateProfileIntW(L"MTD", key.c_str(), 1, path.c_str()))
                    mask &= ~(1u << cut);
            }
        }
        CutMask.store(mask, std::memory_order_release);
        spdlog::info("MTD cut mask {:X} (harness.ini)", mask);
        return true;
    }();
    (void)loaded;
}

inline bool CutEnabled(unsigned aCut) noexcept
{
    return aCut >= 1 && aCut <= 6 &&
        (CutMask.load(std::memory_order_acquire) & (1u << aCut)) != 0;
}

inline bool CutM6bEnabled() noexcept
{
    static const bool enabled = [] {
        wchar_t module[MAX_PATH]{};
        if (!GetModuleFileNameW(nullptr, module, MAX_PATH))
            return true;
        const auto path = (std::filesystem::path(module).parent_path() /
            L"Data" / L"SkyrimTogetherReborn" / L"harness.ini").wstring();
        return GetPrivateProfileIntW(L"MTD", L"CutM6b", 1, path.c_str()) != 0;
    }();
    return enabled;
}

inline uint8_t LoadByte() noexcept
{
    static auto* byte = VersionDbPtr<uint8_t>(400336).Get(); // 0x14219E5F6
    return byte ? *byte : 0;
}
inline uint32_t JlmState() noexcept
{
    static auto* manager = VersionDbPtr<uint8_t*>(403107).Get(); // 0x143220A90
    return manager && *manager ? *reinterpret_cast<uint32_t*>(*manager + 0x6C) : 0;
}
inline bool Loading() noexcept { return LoadByte() != 0 || JlmState() == 2; }
inline bool IsMain() noexcept;
inline bool Quiet() noexcept { return IsMain() && t_slot != Slot::None && JlmState() == 0 && !Loading(); }

inline void OnLoadModeReturn(bool aStart, uint8_t aBefore, uint8_t aAfter, uint32_t aJlmAfter) noexcept
{
    if (aStart && aBefore == 0 && aAfter != 0)
    {
        if (aJlmAfter == 2)
        {
            LoadSerial.fetch_add(1, std::memory_order_acq_rel);
            CurrentLoadPhase.store(LoadPhase::Loading, std::memory_order_release);
        }
        else
        {
            CurrentLoadPhase.store(LoadPhase::InFrameLoad, std::memory_order_release);
            InFrameLoads.fetch_add(1, std::memory_order_relaxed);
        }
    }
    else if (!aStart && aBefore != 0 && aAfter == 0)
        CurrentLoadPhase.store(LoadPhase::Ending, std::memory_order_release);
    else
        LoadNoops.fetch_add(1, std::memory_order_relaxed);
}

inline void NoteLoadGeneration() noexcept
{
    const auto serial = LoadSerial.load(std::memory_order_acquire);
    const auto previous = LastLoadGenSerial.exchange(serial, std::memory_order_acq_rel);
    if (serial == 0 || previous == serial)
    {
        LoadGenOrphans.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    LoadGeneration.fetch_add(1, std::memory_order_release);
    WorldGeneration.fetch_add(1, std::memory_order_release);
}

struct BinkScope
{
    BinkScope() noexcept { BinkDepth.fetch_add(1, std::memory_order_acq_rel); }
    ~BinkScope() noexcept { BinkDepth.fetch_sub(1, std::memory_order_acq_rel); }
};

inline void StartWatchdog() noexcept
{
    static std::jthread watchdog([](std::stop_token stop) {
        uint64_t last = X00Count.load(std::memory_order_acquire);
        unsigned stillSeconds = 0;
        bool reported = false;
        static auto* alwaysActive = VersionDbPtr<uint8_t>(380768).Get(); // 0x1420B58C0
        while (!stop.stop_requested())
        {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            const auto now = X00Count.load(std::memory_order_acquire);
            if (now != last) { last = now; stillSeconds = 0; reported = false; continue; }
            DWORD foregroundPid = 0;
            GetWindowThreadProcessId(GetForegroundWindow(), &foregroundPid);
            const bool frameExpected = (alwaysActive && *alwaysActive) || foregroundPid == GetCurrentProcessId();
            if (!frameExpected)
            {
                FrameSkipped.fetch_add(1, std::memory_order_relaxed);
                stillSeconds = 0;
                reported = false;
                continue;
            }
            if (Loading() || BinkDepth.load(std::memory_order_acquire) != 0)
            {
                stillSeconds = 0;
                reported = false;
                continue;
            }
            if (++stillSeconds >= 10 && !reported)
            {
                MainStalls.fetch_add(1, std::memory_order_relaxed);
                spdlog::warn("mtd_main_stall x00={} last_slot={} load_phase={} bink_depth={}",
                    now, LastSlot.load(), static_cast<unsigned>(CurrentLoadPhase.load()), BinkDepth.load());
                const auto address = LastExtraLockWord.load(std::memory_order_acquire);
                if (address)
                {
                    uint32_t words[2]{};
                    SIZE_T bytes{};
                    if (ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(address),
                            words, sizeof(words), &bytes) && bytes == sizeof(words))
                        spdlog::warn("mtd_extra_lock_stall ref={:X} lock={:X} owner={} count={:X} writer={}",
                            LastExtraRef.load(), address, words[0], words[1], (words[1] & 0x80000000u) != 0);
                }
                reported = true;
            }
        }
    });
}

struct Timing
{
    std::atomic<uint64_t> Count{};
    std::atomic<uint64_t> TotalUs{};
    std::atomic<uint32_t> MaxUs{};
    std::array<std::atomic<uint32_t>, 64> RecentUs{};
};
inline std::array<Timing, SlotCount> SlotTiming{};

inline void BeginX00() noexcept
{
    LoadCutFlags();
    StartWatchdog();
    static auto* alwaysActive = VersionDbPtr<uint8_t>(380768).Get();
    if (alwaysActive) *alwaysActive = 1;
    DWORD expected = 0;
    MainId.compare_exchange_strong(expected, GetCurrentThreadId(), std::memory_order_relaxed);
    if (expected != 0 && expected != GetCurrentThreadId())
        MainIdMismatch.fetch_add(1, std::memory_order_relaxed);
    const auto phase = CurrentLoadPhase.load(std::memory_order_acquire);
    if (!Loading())
    {
        if (phase == LoadPhase::Ending || phase == LoadPhase::InFrameLoad)
            CurrentLoadPhase.store(LoadPhase::Settled, std::memory_order_release);
        else if (phase == LoadPhase::Settled)
            CurrentLoadPhase.store(LoadPhase::Idle, std::memory_order_release);
    }
    X00Count.fetch_add(1, std::memory_order_release);
}
inline void Report() noexcept { CheckerReports.fetch_add(1, std::memory_order_relaxed); }
inline bool IsMain() noexcept { return MainId.load(std::memory_order_acquire) == GetCurrentThreadId(); }
inline bool InRegistryOwner() noexcept { return (IsMain() && t_slot != Slot::None) || t_context == Ctx::AppLane; }
inline Slot CurrentSlot() noexcept { return t_slot; }
inline Ctx CurrentContext() noexcept { return t_context; }

struct Scope
{
    explicit Scope(Ctx aContext) noexcept : Previous(t_context) { t_context = aContext; }
    ~Scope() noexcept { t_context = Previous; }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;
    Ctx Previous;
};

struct SlotScope
{
    explicit SlotScope(Slot aSlot) noexcept : Current(aSlot), Previous(t_slot), Started(std::chrono::steady_clock::now())
    {
        t_slot = aSlot;
        LastSlot.store(static_cast<uint8_t>(aSlot), std::memory_order_release);
    }
    ~SlotScope() noexcept
    {
        const auto us = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - Started).count();
        auto& timing = SlotTiming[static_cast<size_t>(Current)];
        const auto elapsed = static_cast<uint32_t>((std::min)(int64_t{UINT32_MAX}, us));
        const auto sample = timing.Count.load(std::memory_order_relaxed);
        timing.RecentUs[sample & 63].store(elapsed, std::memory_order_relaxed);
        timing.TotalUs.fetch_add(elapsed, std::memory_order_relaxed);
        auto maximum = timing.MaxUs.load(std::memory_order_relaxed);
        while (maximum < elapsed && !timing.MaxUs.compare_exchange_weak(maximum, elapsed, std::memory_order_relaxed)) {}
        timing.Count.fetch_add(1, std::memory_order_release);
        t_slot = Previous;
    }
    SlotScope(const SlotScope&) = delete;
    SlotScope& operator=(const SlotScope&) = delete;
    Slot Current;
    Slot Previous;
    std::chrono::steady_clock::time_point Started;
};
}
