#pragma once

// Routes the vendored Auto Audio Output Switch logging into this client's spdlog.
#include <format>
#include <utility>

namespace logger
{
template <class... Args> void debug(std::format_string<Args...> aFormat, Args&&... aArgs)
{
    spdlog::debug("[AudioSwitch] {}", std::format(aFormat, std::forward<Args>(aArgs)...));
}
template <class... Args> void info(std::format_string<Args...> aFormat, Args&&... aArgs)
{
    spdlog::info("[AudioSwitch] {}", std::format(aFormat, std::forward<Args>(aArgs)...));
}
template <class... Args> void warn(std::format_string<Args...> aFormat, Args&&... aArgs)
{
    spdlog::warn("[AudioSwitch] {}", std::format(aFormat, std::forward<Args>(aArgs)...));
}
template <class... Args> void error(std::format_string<Args...> aFormat, Args&&... aArgs)
{
    spdlog::error("[AudioSwitch] {}", std::format(aFormat, std::forward<Args>(aArgs)...));
}
} // namespace logger
