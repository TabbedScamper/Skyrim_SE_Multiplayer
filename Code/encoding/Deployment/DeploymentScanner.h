#pragma once

#include <Structs/DeploymentManifest.h>

#include <filesystem>
#include <string>
#include <vector>

struct DeploymentScanResult
{
    DeploymentManifest Manifest{};
    uint32_t HashedFiles{};
    uint32_t CachedFiles{};
    uint32_t SkippedFiles{};
    std::vector<std::string> Errors{};
};

struct DeploymentScanner
{
    [[nodiscard]] static DeploymentScanResult Scan(
        const std::filesystem::path& acDataDirectory, const std::filesystem::path& acCachePath = {}) noexcept;
};

