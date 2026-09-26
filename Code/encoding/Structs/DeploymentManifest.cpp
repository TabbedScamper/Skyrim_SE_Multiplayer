#include <Structs/DeploymentManifest.h>
#include <TiltedCore/Serialization.hpp>
#include <Structs/CheckedRead.h>

using TiltedPhoques::Serialization;

namespace
{
void SerializeFingerprint(TiltedPhoques::Buffer::Writer& aWriter, const DeploymentManifest::Fingerprint& acFingerprint) noexcept
{
    Serialization::WriteVarInt(aWriter, acFingerprint.FileCount);
    aWriter.WriteBits(acFingerprint.TotalSize, 64);
    aWriter.WriteBytes(acFingerprint.Root.data(), acFingerprint.Root.size());
}

void DeserializeFingerprint(TiltedPhoques::Buffer::Reader& aReader, DeploymentManifest::Fingerprint& aFingerprint)
{
    aFingerprint.FileCount = CheckedRead::VarInt(aReader) & 0xFFFFFFFF;
    CheckedRead::Bits(aReader, aFingerprint.TotalSize, 64);
    CheckedRead::Bytes(aReader, aFingerprint.Root.data(), aFingerprint.Root.size());
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

bool DeploymentManifest::Deserialize(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    try
    {
        uint64_t value{};
        CheckedRead::Bits(aReader, value, 8);
        SchemaVersion = value & 0xFF;
        Complete = CheckedRead::Bool(aReader);
        DeserializeFingerprint(aReader, AllFiles);
        for (auto& layer : Layers)
            DeserializeFingerprint(aReader, layer);
        return true;
    }
    catch (...)
    {
        *this = {};
        return false;
    }
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

