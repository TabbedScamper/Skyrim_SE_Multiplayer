#include <Deployment/DeploymentScanner.h>
#include <catch2/catch.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>

TEST_CASE("Only the runtime checkpoint registry is excluded from deployment identity", "[encoding.deployment]")
{
    const auto directory = std::filesystem::temp_directory_path() /
        ("checkpoint-registry-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(directory / "SkyrimTogetherReborn");
    struct Cleanup
    {
        std::filesystem::path Path;
        ~Cleanup()
        {
            std::error_code error;
            std::filesystem::remove_all(Path, error);
        }
    } cleanup{directory};
    const auto registry = directory / "SkyrimTogetherReborn" / "checkpoints.txt";
    const auto asset = directory / "checkpoints.txt";
    std::ofstream(registry) << "first checkpoint";
    std::ofstream(asset) << "mod asset";
    std::ofstream(directory / "SkyrimTogetherReborn" / "checkpoints.txt.extra") << "another asset";

    const auto first = DeploymentScanner::Scan(directory);
    REQUIRE(first.Manifest.Complete);
    REQUIRE(first.Manifest.AllFiles.FileCount == 2);
    REQUIRE(first.SkippedFiles == 1);

    std::ofstream(registry, std::ios::app) << "\nsecond checkpoint";
    REQUIRE(DeploymentScanner::Scan(directory).Manifest == first.Manifest);

    std::ofstream(asset) << "changed mod asset";
    REQUIRE(DeploymentScanner::Scan(directory).Manifest != first.Manifest);
}
