#pragma once

#ifndef TP_INTERNAL_COMPONENTS_GUARD
#error Include Components.h instead
#endif

#include <Structs/Mods.h>

namespace ESLoader
{
struct PluginData;
}

struct ModsComponent
{
    struct Entry
    {
        uint32_t id;
        uint32_t refCount;
    };

    explicit ModsComponent(std::filesystem::path aManifestPath = {});

    uint32_t AddStandard(const String& acpFilename) noexcept;
    uint32_t AddLite(const String& acpFilename) noexcept;

    void AddServerMod(const ESLoader::PluginData& acData);

    const auto& GetStandardMods() const noexcept { return m_standardMods; }
    const auto& GetLiteMods() const noexcept { return m_liteMods; }
    const auto& GetServerMods() const noexcept { return m_serverMods; }
    const auto& GetServerManifest() const noexcept { return m_serverManifest; }

    bool IsInstalled(const String& acpFileName) const noexcept;
    bool TryPinManifest(const Mods& acClientManifest);
    bool LoadPinnedManifest();
    [[nodiscard]] bool IsManifestPinned() const noexcept { return m_manifestPinned; }

    using TModList = TiltedPhoques::Map<String, Entry>;

private:
    uint32_t m_seed = 0;
    // Mappings of ids owned by the server
    TModList m_standardMods;
    TModList m_liteMods;

    // List of mods installed on the server.
    TModList m_serverMods;
    Mods m_serverManifest;
    std::filesystem::path m_manifestPath;
    bool m_manifestPinned{};

    bool SavePinnedManifest(const Mods& acManifest) const;
};
