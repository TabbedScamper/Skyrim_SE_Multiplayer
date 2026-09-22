#pragma once

#include <array>
#include <cstdint>
#include <filesystem>

#include <Structs/DeploymentManifest.h>

using TiltedPhoques::String;
using TiltedPhoques::Vector;

struct Mods
{
    static constexpr uint8_t CurrentSchemaVersion = 1;

    enum Mismatch : uint8_t
    {
        kNone = 0,
        kMissing = 1 << 0,
        kUnexpected = 1 << 1,
        kPluginType = 1 << 2,
        kLoadOrder = 1 << 3,
        kContentSize = 1 << 4,
        kContentHash = 1 << 5,
        kUnverifiable = 1 << 6,
        kDeployment = 1 << 7
    };

    struct Entry
    {
        String Filename;
        uint16_t Id;
        bool IsLite;
        uint64_t ContentSize{};
        std::array<uint8_t, 32> ContentSha256{};
        bool HasFingerprint{};
        uint8_t MismatchFlags{kNone};

        bool operator==(const Entry& acRhs) const noexcept
        {
            return Filename == acRhs.Filename && Id == acRhs.Id && IsLite == acRhs.IsLite && ContentSize == acRhs.ContentSize && ContentSha256 == acRhs.ContentSha256 &&
                   HasFingerprint == acRhs.HasFingerprint && MismatchFlags == acRhs.MismatchFlags;
        }

        bool operator!=(const Entry& acRhs) const noexcept { return !this->operator==(acRhs); }
    };

    struct Difference
    {
        Entry Expected{};
        Entry Actual{};
        bool HasExpected{};
        bool HasActual{};
        uint8_t MismatchFlags{kNone};
    };

    Vector<Entry> ModList{};
    uint8_t SchemaVersion{CurrentSchemaVersion};
    DeploymentManifest Deployment{};

    Mods() = default;
    ~Mods() = default;

    bool operator==(const Mods& acRhs) const noexcept;
    bool operator!=(const Mods& acRhs) const noexcept;

    void Serialize(TiltedPhoques::Buffer::Writer& aWriter) const noexcept;
    void Deserialize(TiltedPhoques::Buffer::Reader& aReader) noexcept;

    [[nodiscard]] static bool FingerprintFile(const std::filesystem::path& acPath, Entry& aEntry) noexcept;
    [[nodiscard]] static Vector<Difference> Compare(const Mods& acExpected, const Mods& acActual) noexcept;
};
