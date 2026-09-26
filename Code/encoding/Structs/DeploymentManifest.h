#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include <TiltedCore/Buffer.hpp>

struct DeploymentManifest
{
    static constexpr uint8_t CurrentSchemaVersion = 1;
    static constexpr size_t HashSize = 32;

    enum class Layer : uint8_t
    {
        Plugins,
        Archives,
        Scripts,
        Native,
        Configuration,
        Behaviors,
        Assets,
        Count
    };

    struct Fingerprint
    {
        uint32_t FileCount{};
        uint64_t TotalSize{};
        std::array<uint8_t, HashSize> Root{};

        bool operator==(const Fingerprint& acRhs) const noexcept = default;
    };

    uint8_t SchemaVersion{CurrentSchemaVersion};
    bool Complete{};
    Fingerprint AllFiles{};
    std::array<Fingerprint, static_cast<size_t>(Layer::Count)> Layers{};

    bool operator==(const DeploymentManifest& acRhs) const noexcept = default;

    void Serialize(TiltedPhoques::Buffer::Writer& aWriter) const noexcept;
    [[nodiscard]] bool Deserialize(TiltedPhoques::Buffer::Reader& aReader) noexcept;

    [[nodiscard]] uint16_t MismatchedLayers(const DeploymentManifest& acRhs) const noexcept;
};
