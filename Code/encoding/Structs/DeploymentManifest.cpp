#include <Structs/DeploymentManifest.h>
#include <TiltedCore/Serialization.hpp>

using TiltedPhoques::Serialization;

namespace
{
void SerializeFingerprint(TiltedPhoques::Buffer::Writer& aWriter, const DeploymentManifest::Fingerprint& acFingerprint) noexcept
{
    Serialization::WriteVarInt(aWriter, acFingerprint.FileCount);
    aWriter.WriteBits(acFingerprint.TotalSize, 64);
    aWriter.WriteBytes(acFingerprint.Root.data(), acFingerprint.Root.size());
}

void DeserializeFingerprint(TiltedPhoques::Buffer::Reader& aReader, DeploymentManifest::Fingerprint& aFingerprint) noexcept
{
    aFingerprint.FileCount = Serialization::ReadVarInt(aReader) & 0xFFFFFFFF;
    aReader.ReadBits(aFingerprint.TotalSize, 64);
    aReader.ReadBytes(aFingerprint.Root.data(), aFingerprint.Root.size());
}
}

void DeploymentManifest::Serialize(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    aWriter.WriteBits(SchemaVersion, 8);
    Serialization::WriteBool(aWriter, Complete);
    SerializeFingerprint(aWriter, AllFiles);
    for (const auto& layer : Layers)
        SerializeFingerprint(aWriter, layer);
}

void DeploymentManifest::Deserialize(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    uint64_t value{};
    aReader.ReadBits(value, 8);
    SchemaVersion = value & 0xFF;
    Complete = Serialization::ReadBool(aReader);
    DeserializeFingerprint(aReader, AllFiles);
    for (auto& layer : Layers)
        DeserializeFingerprint(aReader, layer);
}

uint16_t DeploymentManifest::MismatchedLayers(const DeploymentManifest& acRhs) const noexcept
{
    if (!Complete || !acRhs.Complete || SchemaVersion != acRhs.SchemaVersion)
        return (uint16_t{1} << static_cast<size_t>(Layer::Count)) - 1;

    uint16_t result{};
    for (size_t i = 0; i < Layers.size(); ++i)
    {
        if (Layers[i] != acRhs.Layers[i])
            result |= uint16_t{1} << i;
    }
    return result;
}

