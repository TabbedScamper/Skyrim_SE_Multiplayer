
#include <Components.h>
#include <es_loader/ESLoader.h>

#include <charconv>
#include <cctype>
#include <fstream>

namespace
{
constexpr char kManifestHeader[] = "SkyrimSEMultiplayer-DeploymentManifest-v2";

String ToHex(const std::array<uint8_t, 32>& acHash)
{
    constexpr char digits[] = "0123456789abcdef";
    String result;
    result.reserve(acHash.size() * 2);
    for (const auto byte : acHash)
    {
        result.push_back(digits[byte >> 4]);
        result.push_back(digits[byte & 0x0F]);
    }
    return result;
}

bool FromHex(const std::string_view acText, std::array<uint8_t, 32>& aHash)
{
    if (acText.size() != aHash.size() * 2)
        return false;

    auto nibble = [](const char aCharacter) -> int
    {
        if (aCharacter >= '0' && aCharacter <= '9')
            return aCharacter - '0';
        if (aCharacter >= 'a' && aCharacter <= 'f')
            return aCharacter - 'a' + 10;
        if (aCharacter >= 'A' && aCharacter <= 'F')
            return aCharacter - 'A' + 10;
        return -1;
    };

    for (size_t i = 0; i < aHash.size(); ++i)
    {
        const int high = nibble(acText[i * 2]);
        const int low = nibble(acText[i * 2 + 1]);
        if (high < 0 || low < 0)
            return false;
        aHash[i] = static_cast<uint8_t>((high << 4) | low);
    }
    return true;
}

bool FilenameEquals(const String& acLeft, const String& acRight)
{
    return acLeft.size() == acRight.size() && std::equal(
                                                  acLeft.begin(), acLeft.end(), acRight.begin(), [](const char aLeft, const char aRight)
                                                  { return std::tolower(static_cast<unsigned char>(aLeft)) == std::tolower(static_cast<unsigned char>(aRight)); });
}

bool ParseFingerprintLine(const String& acLine, const std::string_view acPrefix, DeploymentManifest::Fingerprint& aFingerprint)
{
    const auto first = acLine.find('\t');
    const auto second = first == String::npos ? first : acLine.find('\t', first + 1);
    const auto third = second == String::npos ? second : acLine.find('\t', second + 1);
    if (first == String::npos || second == String::npos || third == String::npos || std::string_view(acLine.data(), first) != acPrefix)
        return false;

    const auto countText = std::string_view(acLine.data() + first + 1, second - first - 1);
    const auto sizeText = std::string_view(acLine.data() + second + 1, third - second - 1);
    const auto hashText = std::string_view(acLine.data() + third + 1, acLine.size() - third - 1);
    return std::from_chars(countText.data(), countText.data() + countText.size(), aFingerprint.FileCount).ec == std::errc{} &&
           std::from_chars(sizeText.data(), sizeText.data() + sizeText.size(), aFingerprint.TotalSize).ec == std::errc{} && FromHex(hashText, aFingerprint.Root);
}
} // namespace

ModsComponent::ModsComponent(std::filesystem::path aManifestPath)
    : m_manifestPath(std::move(aManifestPath))
{
}

uint32_t ModsComponent::AddStandard(const String& acpFilename) noexcept
{
    const auto itor = m_standardMods.find(acpFilename);
    if (itor != std::end(m_standardMods))
    {
        itor.value().refCount++;
        return itor->second.id;
    }

    const auto id = m_seed++;
    m_standardMods.emplace(acpFilename, Entry{id, 1});

    return id;
}

uint32_t ModsComponent::AddLite(const String& acpFilename) noexcept
{
    const auto itor = m_liteMods.find(acpFilename);
    if (itor != std::end(m_liteMods))
    {
        itor.value().refCount++;
        return itor->second.id;
    }

    const auto id = m_seed++;
    m_liteMods.emplace(acpFilename, Entry{id, 1});

    return id;
}

void ModsComponent::AddServerMod(const ESLoader::PluginData& acData)
{
    // kind of a hack since we want to store both, so we take the two byte value
    const uint32_t pluginId = acData.IsLite() ? acData.m_liteId : acData.m_standardId;
    m_serverMods.emplace(acData.m_filename, Entry{pluginId, 1});

    auto& manifestEntry = m_serverManifest.ModList.emplace_back();
    manifestEntry.Filename = acData.m_filename;
    manifestEntry.Id = static_cast<uint16_t>(pluginId);
    manifestEntry.IsLite = acData.IsLite();

    const auto pluginPath = std::filesystem::current_path() / "Data" / acData.m_filename.c_str();
    if (!Mods::FingerprintFile(pluginPath, manifestEntry))
        spdlog::error("ModPolicy could not fingerprint required plugin {}", pluginPath.string());
}

bool ModsComponent::IsInstalled(const String& acpFilename) const noexcept
{
    auto it = std::find_if(m_serverMods.begin(), m_serverMods.end(), [&](const TModList::value_type& aEntry) { return aEntry.first == acpFilename; });

    return it != m_serverMods.end();
}

bool ModsComponent::LoadPinnedManifest()
{
    if (m_manifestPath.empty() || !std::filesystem::exists(m_manifestPath))
        return false;

    // Existence pins the campaign even when parsing fails. A damaged manifest
    // must be repaired explicitly instead of silently trusting the next client.
    m_manifestPinned = true;
    std::ifstream stream(m_manifestPath);
    String header;
    if (!std::getline(stream, header) || header != kManifestHeader)
        return false;

    String line;
    if (!std::getline(stream, line))
        return false;
    const auto first = line.find('\t');
    const auto second = first == String::npos ? first : line.find('\t', first + 1);
    if (first == String::npos || second == String::npos || std::string_view(line.data(), first) != "deployment")
        return false;
    uint16_t schema{};
    const auto schemaText = std::string_view(line.data() + first + 1, second - first - 1);
    const auto completeText = std::string_view(line.data() + second + 1, line.size() - second - 1);
    if (std::from_chars(schemaText.data(), schemaText.data() + schemaText.size(), schema).ec != std::errc{} ||
        schema != DeploymentManifest::CurrentSchemaVersion || completeText != "1")
        return false;
    m_serverManifest.Deployment.SchemaVersion = static_cast<uint8_t>(schema);
    m_serverManifest.Deployment.Complete = true;

    if (!std::getline(stream, line) || !ParseFingerprintLine(line, "all", m_serverManifest.Deployment.AllFiles))
        return false;
    for (size_t i = 0; i < m_serverManifest.Deployment.Layers.size(); ++i)
    {
        if (!std::getline(stream, line) || !ParseFingerprintLine(line, "layer" + std::to_string(i), m_serverManifest.Deployment.Layers[i]))
            return false;
    }

    for (auto& expected : m_serverManifest.ModList)
    {
        if (!std::getline(stream, line))
            return false;

        const auto first = line.find('\t');
        const auto second = first == String::npos ? first : line.find('\t', first + 1);
        const auto third = second == String::npos ? second : line.find('\t', second + 1);
        const auto fourth = third == String::npos ? third : line.find('\t', third + 1);
        if (first == String::npos || second == String::npos || third == String::npos || fourth == String::npos)
            return false;

        uint16_t id{};
        uint64_t size{};
        const auto idText = std::string_view(line.data(), first);
        const auto liteText = std::string_view(line.data() + first + 1, second - first - 1);
        const auto sizeText = std::string_view(line.data() + second + 1, third - second - 1);
        const auto hashText = std::string_view(line.data() + third + 1, fourth - third - 1);
        const auto filename = std::string_view(line.data() + fourth + 1, line.size() - fourth - 1);
        if (std::from_chars(idText.data(), idText.data() + idText.size(), id).ec != std::errc{} ||
            std::from_chars(sizeText.data(), sizeText.data() + sizeText.size(), size).ec != std::errc{} || (liteText != "0" && liteText != "1") ||
            filename != expected.Filename.c_str() || id != expected.Id || (liteText == "1") != expected.IsLite || !FromHex(hashText, expected.ContentSha256))
            return false;

        expected.ContentSize = size;
        expected.HasFingerprint = true;
    }

    String trailing;
    return !std::getline(stream, trailing);
}

bool ModsComponent::SavePinnedManifest(const Mods& acManifest) const
{
    if (m_manifestPath.empty())
        return false;

    std::error_code error;
    if (const auto parent = m_manifestPath.parent_path(); !parent.empty())
        std::filesystem::create_directories(parent, error);
    if (error || std::filesystem::exists(m_manifestPath))
        return false;

    auto temporaryPath = m_manifestPath;
    temporaryPath += ".tmp";
    std::ofstream stream(temporaryPath, std::ios::trunc);
    if (!stream)
        return false;

    stream << kManifestHeader << '\n';
    if (!acManifest.Deployment.Complete)
        return false;
    const auto& deployment = acManifest.Deployment;
    stream << "deployment\t" << static_cast<uint16_t>(deployment.SchemaVersion) << "\t1\n";
    stream << "all\t" << deployment.AllFiles.FileCount << '\t' << deployment.AllFiles.TotalSize << '\t' << ToHex(deployment.AllFiles.Root).c_str() << '\n';
    for (size_t i = 0; i < deployment.Layers.size(); ++i)
    {
        const auto& layer = deployment.Layers[i];
        stream << "layer" << i << '\t' << layer.FileCount << '\t' << layer.TotalSize << '\t' << ToHex(layer.Root).c_str() << '\n';
    }
    for (const auto& entry : acManifest.ModList)
    {
        if (!entry.HasFingerprint)
            return false;
        stream << entry.Id << '\t' << (entry.IsLite ? 1 : 0) << '\t' << entry.ContentSize << '\t' << ToHex(entry.ContentSha256).c_str() << '\t' << entry.Filename.c_str() << '\n';
    }
    stream.close();
    if (!stream)
        return false;

    std::filesystem::rename(temporaryPath, m_manifestPath, error);
    return !error;
}

bool ModsComponent::TryPinManifest(const Mods& acClientManifest)
{
    if (m_manifestPinned)
        return true;
    if (!acClientManifest.Deployment.Complete || acClientManifest.Deployment.SchemaVersion != DeploymentManifest::CurrentSchemaVersion)
        return false;

    Mods candidate = m_serverManifest;
    const auto differences = Mods::Compare(candidate, acClientManifest);
    for (const auto& difference : differences)
    {
        if (difference.MismatchFlags != Mods::kUnverifiable || !difference.HasExpected || !difference.HasActual || !difference.Actual.HasFingerprint)
            return false;
    }

    for (auto& expected : candidate.ModList)
    {
        const auto actual = std::find_if(
            acClientManifest.ModList.begin(), acClientManifest.ModList.end(), [&](const Mods::Entry& acEntry) { return FilenameEquals(expected.Filename, acEntry.Filename); });
        if (actual == acClientManifest.ModList.end() || !actual->HasFingerprint)
            return false;
        expected.ContentSize = actual->ContentSize;
        expected.ContentSha256 = actual->ContentSha256;
        expected.HasFingerprint = true;
    }

    candidate.Deployment = acClientManifest.Deployment;

    if (!SavePinnedManifest(candidate))
        return false;
    m_serverManifest = std::move(candidate);
    m_manifestPinned = true;
    return true;
}
