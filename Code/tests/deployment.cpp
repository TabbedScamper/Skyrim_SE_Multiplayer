#include <Deployment/DeploymentScanner.h>

#include <catch2/catch.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>

namespace
{
struct TemporaryTree
{
    TemporaryTree()
    {
        Path = std::filesystem::temp_directory_path() /
               ("skyrim-se-multiplayer-deployment-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directories(Path);
    }

    ~TemporaryTree()
    {
        std::error_code error;
        std::filesystem::remove_all(Path, error);
    }

    void Write(const std::filesystem::path& acRelativePath, const std::string_view acContents) const
    {
        const auto path = Path / acRelativePath;
        std::filesystem::create_directories(path.parent_path());
        std::ofstream stream(path, std::ios::binary);
        stream.write(acContents.data(), static_cast<std::streamsize>(acContents.size()));
    }

    std::filesystem::path Path;
};
}

TEST_CASE("Effective Data deployment fingerprints are deterministic and cached", "[encoding.deployment]")
{
    TemporaryTree tree;
    tree.Write("Example.esp", "plugin");
    tree.Write("Example.bsa", "archive");
    tree.Write("Scripts/Quest.pex", "script");
    tree.Write("SKSE/Plugins/Native.dll", "native");
    tree.Write("SKSE/Plugins/Native.toml", "configuration");
    tree.Write("Meshes/Actors/Character/Behaviors/Graph.hkx", "behavior");
    tree.Write("Textures/Example.dds", "asset");
    tree.Write("ignored.log", "ephemeral");
    tree.Write("SkyrimTogetherReborn/backups/old-client.exe", "ephemeral-backup");
    tree.Write("SkyrimTogetherReborn/cache/browser.bin", "ephemeral-cache");
    tree.Write("SkyrimTogetherReborn/debug-feedback/feedback.bmp", "ephemeral-report");
    tree.Write("SkyrimTogetherReborn/logs/session.txt", "ephemeral-log-sidecar");
    tree.Write("SkyrimSEMultiplayerBackups/old-menu/startmenu.swf", "ephemeral-backup");
    tree.Write("SkyrimTogetherReborn/config/STServer.ini", "machine-role-specific");
    tree.Write("SkyrimTogetherReborn/config/STServer.ini.bak", "ephemeral-backup");
    tree.Write("SkyrimTogetherReborn/crashpad_handler.exe", "optional-diagnostics");
    tree.Write("SkyrimTogetherReborn/crashpad_wer.dll", "optional-diagnostics");
    tree.Write("SkyrimTogetherReborn/SkyrimTogether.pre-update.exe", "ephemeral-backup");
    const auto cache = tree.Path.parent_path() / (tree.Path.filename().string() + ".cache");

    const auto first = DeploymentScanner::Scan(tree.Path, cache);
    REQUIRE(first.Manifest.Complete);
    REQUIRE(first.Manifest.AllFiles.FileCount == 7);
    REQUIRE(first.HashedFiles == 7);
    REQUIRE(first.SkippedFiles == 11);
    for (const auto& layer : first.Manifest.Layers)
        REQUIRE(layer.FileCount == 1);

    const auto second = DeploymentScanner::Scan(tree.Path, cache);
    REQUIRE(second.Manifest == first.Manifest);
    REQUIRE(second.CachedFiles == 7);
    REQUIRE(second.HashedFiles == 0);

    tree.Write("Scripts/Quest.pex", "changed-script-with-a-new-size");
    const auto changed = DeploymentScanner::Scan(tree.Path, cache);
    REQUIRE(changed.Manifest.Complete);
    REQUIRE(changed.Manifest.AllFiles.Root != first.Manifest.AllFiles.Root);
    REQUIRE(changed.Manifest.Layers[static_cast<size_t>(DeploymentManifest::Layer::Scripts)].Root !=
            first.Manifest.Layers[static_cast<size_t>(DeploymentManifest::Layer::Scripts)].Root);
    REQUIRE(changed.Manifest.Layers[static_cast<size_t>(DeploymentManifest::Layer::Assets)].Root ==
            first.Manifest.Layers[static_cast<size_t>(DeploymentManifest::Layer::Assets)].Root);

    std::error_code error;
    std::filesystem::remove(cache, error);
}
