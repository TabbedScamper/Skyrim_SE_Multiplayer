#include <Structs/Mods.h>
#include <TiltedCore/Serialization.hpp>
#include <algorithm>
#include <cctype>
#include <fstream>

#include <cryptopp/sha.h>

using TiltedPhoques::Serialization;

bool Mods::operator==(const Mods& acRhs) const noexcept
{
    return SchemaVersion == acRhs.SchemaVersion && ModList == acRhs.ModList && Deployment == acRhs.Deployment;
}

bool Mods::operator!=(const Mods& acRhs) const noexcept
{
    return !this->operator==(acRhs);
}

void Mods::Serialize(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    aWriter.WriteBits(SchemaVersion, 8);

    const uint16_t modCount = std::min(ModList.size(), size_t(4096)) & 0xFFFF;
    aWriter.WriteBits(modCount, 13);

    for (size_t i = 0; i < modCount; ++i)
    {
        const auto& entry = ModList[i];
        aWriter.WriteBits(entry.Id, 16);
        Serialization::WriteBool(aWriter, entry.IsLite);
        Serialization::WriteString(aWriter, entry.Filename);
        Serialization::WriteBool(aWriter, entry.HasFingerprint);
        if (entry.HasFingerprint)
        {
            aWriter.WriteBits(entry.ContentSize, 64);
            aWriter.WriteBytes(entry.ContentSha256.data(), entry.ContentSha256.size());
        }
        aWriter.WriteBits(entry.MismatchFlags, 8);
    }
    Deployment.Serialize(aWriter);
}

void Mods::Deserialize(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    uint64_t data = 0;
    aReader.ReadBits(data, 8);
    SchemaVersion = data & 0xFF;

    aReader.ReadBits(data, 13);

    const size_t modCount = data & 0xFFFF;
    ModList.resize(modCount);
    for (size_t i = 0; i < modCount; ++i)
    {
        aReader.ReadBits(data, 16);
        ModList[i].Id = data & 0xFFFF;
        ModList[i].IsLite = Serialization::ReadBool(aReader);
        ModList[i].Filename = Serialization::ReadString(aReader);
        ModList[i].HasFingerprint = Serialization::ReadBool(aReader);
        if (ModList[i].HasFingerprint)
        {
            aReader.ReadBits(ModList[i].ContentSize, 64);
            aReader.ReadBytes(ModList[i].ContentSha256.data(), ModList[i].ContentSha256.size());
        }
        aReader.ReadBits(data, 8);
        ModList[i].MismatchFlags = data & 0xFF;
    }
    Deployment.Deserialize(aReader);
}

bool Mods::FingerprintFile(const std::filesystem::path& acPath, Entry& aEntry) noexcept
{
    aEntry.HasFingerprint = false;
    aEntry.ContentSize = 0;
    aEntry.ContentSha256.fill(0);

    std::error_code error;
    const auto size = std::filesystem::file_size(acPath, error);
    if (error)
        return false;

    std::ifstream stream(acPath, std::ios::binary);
    if (!stream)
        return false;

    CryptoPP::SHA256 sha256;
    std::array<char, 64 * 1024> buffer{};
    while (stream)
    {
        stream.read(buffer.data(), buffer.size());
        const auto bytesRead = stream.gcount();
        if (bytesRead > 0)
            sha256.Update(reinterpret_cast<const CryptoPP::byte*>(buffer.data()), static_cast<size_t>(bytesRead));
    }

    if (!stream.eof())
        return false;

    sha256.Final(aEntry.ContentSha256.data());
    aEntry.ContentSize = size;
    aEntry.HasFingerprint = true;
    return true;
}

static bool CaseInsensitiveFilenameEquals(const String& acLeft, const String& acRight) noexcept
{
    if (acLeft.size() != acRight.size())
        return false;

    return std::equal(
        acLeft.begin(), acLeft.end(), acRight.begin(),
        [](const char aLeft, const char aRight)
        {
            const auto left = static_cast<unsigned char>(aLeft);
            const auto right = static_cast<unsigned char>(aRight);
            return std::tolower(left) == std::tolower(right);
        });
}

Vector<Mods::Difference> Mods::Compare(const Mods& acExpected, const Mods& acActual) noexcept
{
    Vector<Difference> differences;
    Vector<bool> matched(acActual.ModList.size(), false);

    for (const auto& expected : acExpected.ModList)
    {
        const auto actualIt = std::find_if(
            acActual.ModList.begin(), acActual.ModList.end(), [&](const Entry& acEntry) { return CaseInsensitiveFilenameEquals(expected.Filename, acEntry.Filename); });

        if (actualIt == acActual.ModList.end())
        {
            Difference difference;
            difference.Expected = expected;
            difference.HasExpected = true;
            difference.MismatchFlags = kMissing;
            differences.push_back(std::move(difference));
            continue;
        }

        const auto actualIndex = static_cast<size_t>(std::distance(acActual.ModList.begin(), actualIt));
        matched[actualIndex] = true;

        uint8_t flags = kNone;
        if (expected.IsLite != actualIt->IsLite)
            flags |= kPluginType;
        if (expected.Id != actualIt->Id)
            flags |= kLoadOrder;

        if (!expected.HasFingerprint || !actualIt->HasFingerprint)
        {
            flags |= kUnverifiable;
        }
        else
        {
            if (expected.ContentSize != actualIt->ContentSize)
                flags |= kContentSize;
            if (expected.ContentSha256 != actualIt->ContentSha256)
                flags |= kContentHash;
        }

        if (flags != kNone)
        {
            Difference difference;
            difference.Expected = expected;
            difference.Actual = *actualIt;
            difference.HasExpected = true;
            difference.HasActual = true;
            difference.MismatchFlags = flags;
            differences.push_back(std::move(difference));
        }
    }

    for (size_t i = 0; i < acActual.ModList.size(); ++i)
    {
        if (matched[i])
            continue;

        Difference difference;
        difference.Actual = acActual.ModList[i];
        difference.HasActual = true;
        difference.MismatchFlags = kUnexpected;
        differences.push_back(std::move(difference));
    }

    return differences;
}
