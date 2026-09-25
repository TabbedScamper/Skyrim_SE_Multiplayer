#pragma once

// Skyrim Together integration shim for the vendored Auto Audio Output Switch
// (see README.md here). The upstream file is built against CommonLibSSE-NG and
// SKSE; this header provides the few helpers it uses on top of this client's
// VersionDb (Address Library IDs) and spdlog. Upstream sources are otherwise
// unmodified.

#include <TiltedOnlinePCH.h>

#include <Windows.h>

#include <cstdint>
#include <cstring>
#include <format>

namespace REL
{
struct Module
{
    static Module& get() noexcept
    {
        static Module s_module;
        return s_module;
    }
    std::uintptr_t base() const noexcept { return reinterpret_cast<std::uintptr_t>(GetModuleHandleA(nullptr)); }
};

// The client only runs on the AE-family 1.7.104 runtime: use the AE ID.
struct RelocationID
{
    RelocationID(std::uint64_t, std::uint64_t aAeId) noexcept
        : m_id(aAeId)
    {
    }
    std::uintptr_t address() const noexcept { return reinterpret_cast<std::uintptr_t>(VersionDb::Get().FindAddressById(m_id)); }

private:
    std::uint64_t m_id;
};

template <class T> constexpr T Relocate(T, T aAe) noexcept
{
    return aAe;
}

void safe_write(std::uintptr_t aAddress, const void* apData, std::size_t aSize) noexcept;
} // namespace REL

namespace SKSE
{
struct Trampoline
{
    // Rewrites the 5-byte rel32 call at aSite to aTarget and returns the
    // previous callee. Returns 0 and leaves the site untouched when it is not
    // a rel32 call or aTarget is out of rel32 range.
    template <std::size_t N> std::uintptr_t write_call(std::uintptr_t aSite, std::uintptr_t aTarget) noexcept
    {
        static_assert(N == 5, "only 5-byte calls are used by the vendored code");
        return WriteCall5(aSite, aTarget);
    }

    static std::uintptr_t WriteCall5(std::uintptr_t aSite, std::uintptr_t aTarget) noexcept;
};

inline Trampoline& GetTrampoline() noexcept
{
    static Trampoline s_trampoline;
    return s_trampoline;
}
} // namespace SKSE

namespace RE
{
struct BSAudioManager
{
    static BSAudioManager* GetSingleton() noexcept;
};
} // namespace RE
