#include <Deployment/DeploymentScanner.h>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <fstream>
#include <optional>
#include <string_view>
#include <unordered_map>

#include <cryptopp/sha.h>

#if defined(_WIN32)
#include <Windows.h>
#else
#include <sys/stat.h>
#endif

namespace
{
constexpr char kCacheHeader[] = "SkyrimSEMultiplayer-DeploymentHashCache-v2";

struct FileRecord
{
    std::filesystem::path Path;
    std::string RelativePath;
    DeploymentManifest::Layer Layer{};
    uint64_t Size{};
    int64_t WriteTime{};
    uint64_t FileIdentity{};
    std::array<uint8_t, DeploymentManifest::HashSize> Hash{};
};

struct CacheRecord
{
    uint64_t Size{};
    int64_t WriteTime{};
    uint64_t FileIdentity{};
    std::array<uint8_t, DeploymentManifest::HashSize> Hash{};
};

using Cache = std::unordered_map<std::string, CacheRecord>;

int HexNibble(const char aCharacter) noexcept
{
    if (aCharacter >= '0' && aCharacter <= '9')
        return aCharacter - '0';
    if (aCharacter >= 'a' && aCharacter <= 'f')
        return aCharacter - 'a' + 10;
    if (aCharacter >= 'A' && aCharacter <= 'F')
        return aCharacter - 'A' + 10;
    return -1;
}

bool FromHex(const std::string_view acText, std::array<uint8_t, DeploymentManifest::HashSize>& aHash) noexcept
{
    if (acText.size() != aHash.size() * 2)
        return false;
    for (size_t i = 0; i < aHash.size(); ++i)
    {
        const int high = HexNibble(acText[i * 2]);
        const int low = HexNibble(acText[i * 2 + 1]);
        if (high < 0 || low < 0)
            return false;
        aHash[i] = static_cast<uint8_t>((high << 4) | low);
    }
    return true;
}

std::string ToHex(const std::array<uint8_t, DeploymentManifest::HashSize>& acHash)
{
    constexpr char digits[] = "0123456789abcdef";
    std::string result;
    result.reserve(acHash.size() * 2);
    for (const auto byte : acHash)
    {
        result.push_back(digits[byte >> 4]);
        result.push_back(digits[byte & 0x0F]);
    }
    return result;
}

std::string NormalizeRelativePath(const std::filesystem::path& acPath)
{
    const auto utf8 = acPath.generic_u8string();
    std::string result(reinterpret_cast<const char*>(utf8.data()), utf8.size());
    std::transform(result.begin(), result.end(), result.begin(), [](const unsigned char aCharacter) {
        return static_cast<char>(std::tolower(aCharacter));
    });
    return result;
}

bool EndsWith(const std::string_view acValue, const std::string_view acSuffix) noexcept
{
    return acValue.size() >= acSuffix.size() && acValue.substr(acValue.size() - acSuffix.size()) == acSuffix;
}

bool ShouldSkip(const std::string_view acPath) noexcept
{
    return acPath.empty() || acPath.starts_with(".git/") || EndsWith(acPath, ".log") || EndsWith(acPath, ".tmp") || EndsWith(acPath, ".dmp") ||
           EndsWith(acPath, ".sqlite3") || EndsWith(acPath, ".sqlite3-wal") || EndsWith(acPath, ".sqlite3-shm") ||
           acPath == "skyrimsemultiplayer.plugins.manifest";
}

DeploymentManifest::Layer Classify(const std::string_view acPath) noexcept
{
    if (EndsWith(acPath, ".esp") || EndsWith(acPath, ".esm") || EndsWith(acPath, ".esl"))
        return DeploymentManifest::Layer::Plugins;
    if (EndsWith(acPath, ".bsa"))
        return DeploymentManifest::Layer::Archives;
    if (acPath.starts_with("scripts/") && EndsWith(acPath, ".pex"))
        return DeploymentManifest::Layer::Scripts;
    if ((acPath.starts_with("skse/plugins/") || acPath.starts_with("dllplugins/")) && EndsWith(acPath, ".dll"))
        return DeploymentManifest::Layer::Native;
    if (EndsWith(acPath, ".ini") || EndsWith(acPath, ".toml") || EndsWith(acPath, ".yaml") || EndsWith(acPath, ".yml") || EndsWith(acPath, ".cfg"))
        return DeploymentManifest::Layer::Configuration;
    if (EndsWith(acPath, ".hkx") || acPath.find("behaviors/") != std::string_view::npos)
        return DeploymentManifest::Layer::Behaviors;
    return DeploymentManifest::Layer::Assets;
}

bool HashFile(const std::filesystem::path& acPath, std::array<uint8_t, DeploymentManifest::HashSize>& aHash) noexcept
{
    std::ifstream stream(acPath, std::ios::binary);
    if (!stream)
        return false;

    CryptoPP::SHA256 sha256;
    std::array<char, 64 * 1024> buffer{};
    while (stream)
    {
        stream.read(buffer.data(), buffer.size());
        const auto count = stream.gcount();
        if (count > 0)
            sha256.Update(reinterpret_cast<const CryptoPP::byte*>(buffer.data()), static_cast<size_t>(count));
    }
    if (!stream.eof())
        return false;
    sha256.Final(aHash.data());
    return true;
}

uint64_t GetFileIdentity(const std::filesystem::path& acPath) noexcept
{
#if defined(_WIN32)
    const HANDLE handle = CreateFileW(
        acPath.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE)
        return 0;
    BY_HANDLE_FILE_INFORMATION information{};
    const bool succeeded = GetFileInformationByHandle(handle, &information) != FALSE;
    CloseHandle(handle);
    if (!succeeded)
        return 0;
    const uint64_t index = (static_cast<uint64_t>(information.nFileIndexHigh) << 32) | information.nFileIndexLow;
    return index ^ (static_cast<uint64_t>(information.dwVolumeSerialNumber) << 32);
#else
    struct stat information{};
    if (stat(acPath.c_str(), &information) != 0)
        return 0;
    return static_cast<uint64_t>(information.st_ino) ^ (static_cast<uint64_t>(information.st_dev) << 32);
#endif
}

Cache LoadCache(const std::filesystem::path& acPath)
{
    Cache cache;
    if (acPath.empty())
        return cache;

    std::ifstream stream(acPath);
    std::string line;
    if (!std::getline(stream, line) || line != kCacheHeader)
        return cache;

    while (std::getline(stream, line))
    {
        const auto first = line.find('\t');
        const auto second = first == std::string::npos ? first : line.find('\t', first + 1);
        const auto third = second == std::string::npos ? second : line.find('\t', second + 1);
        const auto fourth = third == std::string::npos ? third : line.find('\t', third + 1);
        if (first == std::string::npos || second == std::string::npos || third == std::string::npos || fourth == std::string::npos)
            continue;

        CacheRecord record;
        const auto sizeText = std::string_view(line.data(), first);
        const auto timeText = std::string_view(line.data() + first + 1, second - first - 1);
        const auto identityText = std::string_view(line.data() + second + 1, third - second - 1);
        const auto hashText = std::string_view(line.data() + third + 1, fourth - third - 1);
        const std::string path(line.data() + fourth + 1, line.size() - fourth - 1);
        if (std::from_chars(sizeText.data(), sizeText.data() + sizeText.size(), record.Size).ec != std::errc{} ||
            std::from_chars(timeText.data(), timeText.data() + timeText.size(), record.WriteTime).ec != std::errc{} ||
            std::from_chars(identityText.data(), identityText.data() + identityText.size(), record.FileIdentity).ec != std::errc{} || !FromHex(hashText, record.Hash))
            continue;
        cache.emplace(path, record);
    }
    return cache;
}

void SaveCache(const std::filesystem::path& acPath, const std::vector<FileRecord>& acFiles) noexcept
{
    if (acPath.empty())
        return;
    std::error_code error;
    if (const auto parent = acPath.parent_path(); !parent.empty())
        std::filesystem::create_directories(parent, error);
    if (error)
        return;

    std::ofstream stream(acPath, std::ios::trunc);
    if (!stream)
        return;
    stream << kCacheHeader << '\n';
    for (const auto& file : acFiles)
        stream << file.Size << '\t' << file.WriteTime << '\t' << file.FileIdentity << '\t' << ToHex(file.Hash) << '\t' << file.RelativePath << '\n';
}

std::array<uint8_t, DeploymentManifest::HashSize> BuildRoot(
    const std::vector<FileRecord>& acFiles, const std::optional<DeploymentManifest::Layer> acLayer = std::nullopt) noexcept
{
    CryptoPP::SHA256 sha256;
    constexpr std::string_view domain = "SkyrimSEMultiplayer-DeploymentRoot-v1";
    sha256.Update(reinterpret_cast<const CryptoPP::byte*>(domain.data()), domain.size());
    const auto updateUint32 = [&sha256](const uint32_t aValue)
    {
        const std::array<uint8_t, 4> encoded{
            static_cast<uint8_t>(aValue), static_cast<uint8_t>(aValue >> 8), static_cast<uint8_t>(aValue >> 16), static_cast<uint8_t>(aValue >> 24)};
        sha256.Update(encoded.data(), encoded.size());
    };
    const auto updateUint64 = [&sha256](const uint64_t aValue)
    {
        std::array<uint8_t, 8> encoded{};
        for (size_t i = 0; i < encoded.size(); ++i)
            encoded[i] = static_cast<uint8_t>(aValue >> (i * 8));
        sha256.Update(encoded.data(), encoded.size());
    };
    for (const auto& file : acFiles)
    {
        if (acLayer && file.Layer != *acLayer)
            continue;
        const uint8_t layer = static_cast<uint8_t>(file.Layer);
        const uint32_t pathSize = static_cast<uint32_t>(file.RelativePath.size());
        updateUint32(pathSize);
        sha256.Update(reinterpret_cast<const CryptoPP::byte*>(file.RelativePath.data()), file.RelativePath.size());
        sha256.Update(&layer, sizeof(layer));
        updateUint64(file.Size);
        sha256.Update(file.Hash.data(), file.Hash.size());
    }
    std::array<uint8_t, DeploymentManifest::HashSize> result{};
    sha256.Final(result.data());
    return result;
}
}

DeploymentScanResult DeploymentScanner::Scan(const std::filesystem::path& acDataDirectory, const std::filesystem::path& acCachePath) noexcept
{
    DeploymentScanResult result;
    std::vector<FileRecord> files;
    try
    {
        const auto cache = LoadCache(acCachePath);
        std::error_code iteratorError;
        std::filesystem::recursive_directory_iterator iterator(
            acDataDirectory, std::filesystem::directory_options::skip_permission_denied, iteratorError);
        const std::filesystem::recursive_directory_iterator end;
        if (iteratorError)
            result.Errors.push_back(iteratorError.message());

        for (; iterator != end; iterator.increment(iteratorError))
        {
            if (iteratorError)
            {
                result.Errors.push_back(iteratorError.message());
                iteratorError.clear();
                continue;
            }

            std::error_code metadataError;
            if (!iterator->is_regular_file(metadataError) || metadataError)
                continue;

            auto relative = std::filesystem::relative(iterator->path(), acDataDirectory, metadataError);
            if (metadataError)
            {
                result.Errors.push_back(iterator->path().string() + ": " + metadataError.message());
                continue;
            }

            FileRecord file;
            file.Path = iterator->path();
            file.RelativePath = NormalizeRelativePath(relative);
            if (ShouldSkip(file.RelativePath))
            {
                ++result.SkippedFiles;
                continue;
            }

            file.Layer = Classify(file.RelativePath);
            file.Size = iterator->file_size(metadataError);
            if (metadataError)
            {
                result.Errors.push_back(file.RelativePath + ": " + metadataError.message());
                continue;
            }
            file.WriteTime = iterator->last_write_time(metadataError).time_since_epoch().count();
            if (metadataError)
            {
                result.Errors.push_back(file.RelativePath + ": " + metadataError.message());
                continue;
            }
            file.FileIdentity = GetFileIdentity(file.Path);

            const auto cached = cache.find(file.RelativePath);
            if (file.FileIdentity != 0 && cached != cache.end() && cached->second.Size == file.Size && cached->second.WriteTime == file.WriteTime &&
                cached->second.FileIdentity == file.FileIdentity)
            {
                file.Hash = cached->second.Hash;
                ++result.CachedFiles;
            }
            else if (HashFile(file.Path, file.Hash))
            {
                ++result.HashedFiles;
            }
            else
            {
                result.Errors.push_back(file.RelativePath + ": could not hash file");
                continue;
            }
            files.push_back(std::move(file));
        }

        std::sort(files.begin(), files.end(), [](const FileRecord& acLeft, const FileRecord& acRight) { return acLeft.RelativePath < acRight.RelativePath; });
        result.Manifest.AllFiles.FileCount = static_cast<uint32_t>(files.size());
        result.Manifest.AllFiles.Root = BuildRoot(files);
        for (const auto& file : files)
        {
            result.Manifest.AllFiles.TotalSize += file.Size;
            auto& layer = result.Manifest.Layers[static_cast<size_t>(file.Layer)];
            ++layer.FileCount;
            layer.TotalSize += file.Size;
        }
        for (size_t i = 0; i < result.Manifest.Layers.size(); ++i)
            result.Manifest.Layers[i].Root = BuildRoot(files, static_cast<DeploymentManifest::Layer>(i));

        result.Manifest.Complete = result.Errors.empty();
        SaveCache(acCachePath, files);
    }
    catch (const std::exception& exception)
    {
        result.Errors.push_back(exception.what());
        result.Manifest.Complete = false;
    }
    catch (...)
    {
        result.Errors.push_back("unknown deployment scan failure");
        result.Manifest.Complete = false;
    }
    return result;
}
