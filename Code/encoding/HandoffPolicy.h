#pragma once
#include <algorithm>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace HandoffPolicy
{
template <class OrderedIds>
uint64_t Successor(const OrderedIds& ids, uint64_t departed) noexcept
{
    if (ids.size() < 2) return 0;
    const auto it = std::find(ids.begin(), ids.end(), departed);
    if (it == ids.end()) return 0;
    for (size_t step = 1; step < ids.size(); ++step)
    {
        const auto candidate = ids[(static_cast<size_t>(it - ids.begin()) + step) % ids.size()];
        if (candidate && candidate != departed) return candidate;
    }
    return 0;
}

inline uint64_t CheckpointTime(std::string_view id) noexcept
{
    const auto underscore = id.rfind('_');
    if (underscore == id.npos || underscore + 1 == id.size()) return 0;
    uint64_t value{};
    const auto* begin = id.data() + underscore + 1;
    const auto result = std::from_chars(begin, id.data() + id.size(), value);
    return result.ec == std::errc{} && result.ptr == id.data() + id.size() ? value : 0;
}

inline bool Newer(std::string_view candidate, std::string_view current) noexcept
{
    const auto a = CheckpointTime(candidate), b = CheckpointTime(current);
    return a > b || (a == b && candidate > current);
}

inline bool SameFamily(std::string_view candidate, std::string_view anchor) noexcept
{
    const auto left = candidate.find('_'), right = anchor.find('_');
    return left != candidate.npos && right != anchor.npos && left == right &&
        candidate.substr(0, left) == anchor.substr(0, right);
}
} // namespace HandoffPolicy
