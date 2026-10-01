#include <Services/UpdateService.h>

#include <World.h>
#include <Events/UpdateEvent.h>
#include <Services/OverlayService.h>
#include <OverlayApp.hpp>
#include <BuildInfo.h>

#include <include/cef_parser.h>
#include <cryptopp/sha.h>
#include <cryptopp/hex.h>
#include <cryptopp/files.h>
#include <cryptopp/filters.h>

#include <ShlObj.h>
#include <winhttp.h>

#include <algorithm>
#include <chrono>
#include <fstream>
#include <functional>
#include <regex>
#include <set>

namespace
{
namespace fs = std::filesystem;

// Stable channel: GitHub's direct link to an asset of the newest release (no REST API, no rate limit).
constexpr wchar_t kManifestUrl[] = L"https://github.com/TabbedScamper/Skyrim_SE_Multiplayer/releases/latest/download/latest.json";
constexpr char kReleaseDownloadPrefix[] = "https://github.com/TabbedScamper/Skyrim_SE_Multiplayer/releases/download/";
constexpr uint64_t kManifestLimit = 4u << 20;
constexpr uint64_t kPackageLimit = 1ull << 30;
constexpr uint64_t kCheckEveryMs = 6ull * 60 * 60 * 1000;

uint64_t NowMs() noexcept
{
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count());
}

fs::path UpdatesRoot() noexcept
{
    PWSTR pLocal = nullptr;
    fs::path root;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &pLocal)))
        root = fs::path(pLocal) / "SkyrimSEMultiplayer" / "updates";
    CoTaskMemFree(pLocal);
    return root;
}

std::string Json(const std::string& acText)
{
    std::string out;
    for (const unsigned char c : acText)
    {
        if (c == '"' || c == '\\')
            out += '\\', out += static_cast<char>(c);
        else if (c == '\n')
            out += "\\n";
        else if (c == '\r' || c == '\t')
            out += ' ';
        else if (c >= 0x20)
            out += static_cast<char>(c);
    }
    return out;
}

// HTTPS GET following redirects (GitHub's release links redirect to its download host). Writes to aFile when given,
// else to aBody. Calls aProgress(bytes) as data arrives; it returns false to stop. False with aError on any failure or over aLimit bytes.
// The session handle is published in aActive so a quitting game can close it, which cancels a blocked read at once;
// whoever takes it out of aActive closes it.
bool HttpGet(const std::wstring& acUrl, uint64_t aLimit, std::string* aBody, const fs::path* aFile,
             const std::function<bool(uint64_t)>& aProgress, std::atomic<void*>& aActive, std::string& aError)
{
    URL_COMPONENTS parts{sizeof(parts)};
    wchar_t host[256]{}, path[2048]{};
    parts.lpszHostName = host;
    parts.dwHostNameLength = static_cast<DWORD>(std::size(host));
    parts.lpszUrlPath = path;
    parts.dwUrlPathLength = static_cast<DWORD>(std::size(path));
    if (!WinHttpCrackUrl(acUrl.c_str(), 0, 0, &parts) || parts.nScheme != INTERNET_SCHEME_HTTPS)
    {
        aError = "only https addresses are allowed";
        return false;
    }
    HINTERNET session = WinHttpOpen(L"SkyrimSEMultiplayer-Updater/1", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session)
    {
        aError = "network unavailable";
        return false;
    }
    aActive = session;
    WinHttpSetTimeouts(session, 10000, 10000, 20000, 60000);
    // Redirects only ever to https (GitHub to its asset host).
    DWORD policy = WINHTTP_OPTION_REDIRECT_POLICY_DISALLOW_HTTPS_TO_HTTP;
    WinHttpSetOption(session, WINHTTP_OPTION_REDIRECT_POLICY, &policy, sizeof(policy));
    HINTERNET connection = WinHttpConnect(session, host, parts.nPort, 0);
    HINTERNET request = connection ? WinHttpOpenRequest(connection, L"GET", path, nullptr, WINHTTP_NO_REFERER,
                                                       WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE)
                                   : nullptr;
    bool ok = request && WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
              WinHttpReceiveResponse(request, nullptr);
    DWORD status = 0, size = sizeof(status);
    if (ok)
        ok = WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                                 &status, &size, WINHTTP_NO_HEADER_INDEX) && status == 200;
    if (!ok)
        aError = status ? fmt::format("the server answered {}", status) : "could not reach GitHub";
    std::ofstream file;
    if (ok && aFile)
    {
        file.open(*aFile, std::ios::binary | std::ios::trunc);
        if (!file)
        {
            ok = false;
            aError = "could not write the download";
        }
    }
    uint64_t total = 0;
    std::vector<char> buffer(1 << 16);
    while (ok)
    {
        DWORD read = 0;
        if (!WinHttpReadData(request, buffer.data(), static_cast<DWORD>(buffer.size()), &read))
        {
            ok = false;
            aError = "the download was interrupted";
            break;
        }
        if (!read)
            break;
        total += read;
        if (total > aLimit)
        {
            ok = false;
            aError = "the download is larger than allowed";
            break;
        }
        if (aFile)
            file.write(buffer.data(), read);
        else
            aBody->append(buffer.data(), read);
        if (aProgress && !aProgress(total))
        {
            ok = false;
            aError = "stopped";
            break;
        }
    }
    if (ok && aFile)
    {
        file.close();
        if (!file)
        {
            ok = false;
            aError = "could not write the download (is the disk full?)";
        }
    }
    // Closing the session also closes its connection and request.
    if (aActive.exchange(nullptr) == session)
        WinHttpCloseHandle(session);
    else
        ok = false;
    return ok;
}

std::string Sha256File(const fs::path& acPath) noexcept
{
    try
    {
        // Opened by wide path: a non-ASCII Windows user name must not break the hash.
        std::ifstream file(acPath, std::ios::binary);
        if (!file)
            return {};
        std::string digest;
        CryptoPP::SHA256 hash;
        CryptoPP::FileSource(file, true,
            new CryptoPP::HashFilter(hash, new CryptoPP::HexEncoder(new CryptoPP::StringSink(digest), true)));
        return digest;
    }
    catch (...)
    {
        return {};
    }
}

// A manifest path stays inside Skyrim's Data folder and inside this mod's own files.
bool SafeDataPath(const std::string& acPath) noexcept
{
    if (acPath.empty() || acPath.size() > 260 || acPath.front() == '/' || acPath.front() == '\\' ||
        acPath.find(':') != std::string::npos || acPath.find("..") != std::string::npos ||
        acPath.find('\\') != std::string::npos || acPath.back() == '/' ||
        std::any_of(acPath.begin(), acPath.end(), [](char c) { return c < 0x20 || c > 0x7E; }) ||
        acPath.find("//") != std::string::npos)
        return false;
    const auto top = acPath.substr(0, acPath.find('/'));
    static const std::set<std::string> kRoots{"SkyrimTogetherReborn", "Skyrim_SE_Multiplayer", "SkyrimTogetherRebornBehaviors",
                                              "Skyrim_SE_MultiplayerBehaviors", "scripts", "meshes"};
    if (top == acPath)
        return acPath == "SkyrimSEMultiplayer.esp" || acPath == "SkyrimSEMultiplayerQuestPatches.esp";
    return kRoots.contains(top);
}
} // namespace

UpdateService::UpdateService(World& aWorld, entt::dispatcher& aDispatcher) noexcept
    : m_world(aWorld)
{
    m_updateConnection = aDispatcher.sink<UpdateEvent>().connect<&UpdateService::OnUpdate>(this);
}

UpdateService::~UpdateService() noexcept
{
    // Quitting mid-check or mid-download: cancel the network read and the unpacker so exit does not wait on them.
    // Holding m_workerLock keeps a late CheckNow/Download from the CEF thread from starting a new worker.
    std::lock_guard workerLock(m_workerLock);
    m_stopping = true;
    if (auto* session = m_session.exchange(nullptr))
        WinHttpCloseHandle(session);
    if (auto* extractor = m_extractor.exchange(nullptr))
        TerminateProcess(extractor, 1);
    Join();
}

void UpdateService::Join() noexcept
{
    if (m_worker.joinable())
        m_worker.join();
}

std::string UpdateService::CurrentVersion() noexcept
{
    std::string release = BUILD_RELEASE_VERSION;
    if (!release.empty() && (release.front() == 'v' || release.front() == 'V'))
        release.erase(0, 1);
    if (!release.empty() && release.front() >= '0' && release.front() <= '9')
        return release;
    return std::string("0.0.0-dev+") + BUILD_COMMIT;
}

bool UpdateService::IsOlder(const std::string& a, const std::string& b) noexcept
{
    // Semantic versioning 2.0 precedence; build metadata after '+' is ignored.
    const auto parse = [](std::string aVersion, uint64_t (&aParts)[3], std::vector<std::string>& aPre) {
        aVersion = aVersion.substr(0, aVersion.find('+'));
        const auto dash = aVersion.find('-');
        const std::string core = aVersion.substr(0, dash);
        aParts[0] = aParts[1] = aParts[2] = 0;
        size_t i = 0;
        for (int part = 0; part < 3 && i < core.size(); ++part)
        {
            aParts[part] = std::strtoull(core.c_str() + i, nullptr, 10);
            const auto dot = core.find('.', i);
            i = dot == std::string::npos ? core.size() : dot + 1;
        }
        aPre.clear();
        if (dash == std::string::npos)
            return;
        const std::string pre = aVersion.substr(dash + 1);
        for (size_t start = 0;;)
        {
            const auto dot = pre.find('.', start);
            aPre.push_back(pre.substr(start, dot == std::string::npos ? std::string::npos : dot - start));
            if (dot == std::string::npos)
                break;
            start = dot + 1;
        }
    };
    const auto numeric = [](const std::string& acPart) {
        return !acPart.empty() && std::all_of(acPart.begin(), acPart.end(), [](char c) { return c >= '0' && c <= '9'; });
    };
    uint64_t pa[3], pb[3];
    std::vector<std::string> preA, preB;
    parse(a, pa, preA);
    parse(b, pb, preB);
    for (int i = 0; i < 3; ++i)
        if (pa[i] != pb[i])
            return pa[i] < pb[i];
    // 1.2.3-test.1 < 1.2.3.
    if (preA.empty() != preB.empty())
        return !preA.empty();
    for (size_t i = 0; i < std::min(preA.size(), preB.size()); ++i)
    {
        if (preA[i] == preB[i])
            continue;
        const bool numA = numeric(preA[i]), numB = numeric(preB[i]);
        if (numA && numB)
            return preA[i].size() != preB[i].size() ? preA[i].size() < preB[i].size() : preA[i] < preB[i];
        if (numA != numB)
            return numA;
        return preA[i] < preB[i];
    }
    return preA.size() < preB.size();
}

std::string UpdateService::MismatchText(const std::string& acTheirWireVersion) noexcept
{
    // SSM/1/<release>/<commit>; the commit is the last part.
    constexpr std::string_view kPrefix = "SSM/1/";
    if (acTheirWireVersion.rfind(kPrefix, 0) != 0)
        return "That session is not running Skyrim SE Multiplayer, or runs a much older version of it. "
               "Both of you need the latest release.";
    const auto rest = acTheirWireVersion.substr(kPrefix.size());
    const auto slash = rest.rfind('/');
    const std::string theirs = slash == std::string::npos ? std::string{} : rest.substr(0, slash);
    const std::string theirCommit = slash == std::string::npos ? rest : rest.substr(slash + 1);
    const std::string mine = BUILD_RELEASE_VERSION;
    if (theirs.empty() || mine.empty() || theirs == mine)
        return fmt::format("You and your friend are on different builds ({} and {}). Both of you should install the "
                           "latest release from Options > Updates.",
                           theirs.empty() ? theirCommit : theirs, mine.empty() ? std::string(BUILD_COMMIT) : mine);
    if (IsOlder(mine, theirs))
        return fmt::format("Your friend has version {} and you have {}. Open Options > Updates to update, then join "
                           "again.", theirs, mine);
    return fmt::format("Your friend has version {}, older than yours ({}). They need to update from Options > Updates.",
                       theirs, mine);
}

void UpdateService::SetPhase(const char* acPhase, const std::string& acError) noexcept
{
    std::lock_guard lock(m_lock);
    m_phase = acPhase;
    m_error = acError;
}

std::string UpdateService::StateJson() const noexcept
{
    std::lock_guard lock(m_lock);
    return fmt::format("{{\"current\":\"{}\",\"latest\":\"{}\",\"phase\":\"{}\",\"notes\":\"{}\",\"publishedAt\":\"{}\","
                       "\"received\":{},\"size\":{},\"error\":\"{}\",\"lastChecked\":{}}}",
        Json(CurrentVersion()), Json(m_latest), m_phase, Json(m_notes), Json(m_publishedAt), m_received, m_zipSize,
        Json(m_error), m_lastChecked);
}

void UpdateService::CheckNow() noexcept
{
    std::lock_guard workerLock(m_workerLock);
    if (m_stopping || m_busy.exchange(true))
        return;
    Join();
    SetPhase("checking");
    m_worker = std::thread([this]() {
        RunCheck();
        m_busy = false;
    });
}

void UpdateService::Download() noexcept
{
    {
        std::lock_guard lock(m_lock);
        if (m_phase != "available")
            return;
    }
    std::lock_guard workerLock(m_workerLock);
    if (m_stopping || m_busy.exchange(true))
        return;
    Join();
    SetPhase("downloading");
    m_worker = std::thread([this]() {
        RunDownload();
        m_busy = false;
    });
}

void UpdateService::RunCheck() noexcept
{
    std::string body, error;
    if (!HttpGet(kManifestUrl, kManifestLimit, &body, nullptr, [this](uint64_t) { return !m_stopping.load(); }, m_session, error))
    {
        {
            std::lock_guard lock(m_lock);
            m_lastChecked = NowMs();
        }
        SetPhase("offline", error);
        spdlog::info("Update check: {}", error);
        return;
    }
    auto value = CefParseJSON(body, JSON_PARSER_RFC);
    auto manifest = value ? value->GetDictionary() : nullptr;
    auto zip = manifest ? manifest->GetDictionary("zip") : nullptr;
    if (!manifest || !zip || manifest->GetInt("schema") != 1 || manifest->GetString("version").empty())
    {
        SetPhase("error", "the release manifest is not readable");
        return;
    }
    const std::string latest = manifest->GetString("version").ToString();
    // The version names a folder on disk: strict semantic version text only.
    static const std::regex kVersion(R"(^\d{1,6}\.\d{1,6}\.\d{1,6}(-[0-9A-Za-z.-]{1,40})?$)");
    if (!std::regex_match(latest, kVersion))
    {
        SetPhase("error", "the release manifest has an invalid version");
        return;
    }
    {
        std::lock_guard lock(m_lock);
        m_latest = latest;
        m_notes = manifest->GetString("notes").ToString();
        m_publishedAt = manifest->GetString("publishedAt").ToString();
        m_zipUrl = zip->GetString("url").ToString();
        m_zipSha = zip->GetString("sha256").ToString();
        m_zipSize = static_cast<uint64_t>(zip->GetDouble("size"));
        m_manifest = body;
        m_lastChecked = NowMs();
    }
    // An update already downloaded for this version waits for the restart.
    std::error_code fsError;
    const bool staged = fs::exists(UpdatesRoot() / "pending.json", fsError) &&
                        fs::exists(UpdatesRoot() / latest / "ready", fsError);
    SetPhase(staged ? "ready" : IsOlder(CurrentVersion(), latest) ? "available" : "current");
    spdlog::info("Update check: this build {}, latest {}", CurrentVersion(), latest);
}

void UpdateService::RunDownload() noexcept
{
    std::string latest, url, sha, manifestText;
    {
        std::lock_guard lock(m_lock);
        latest = m_latest;
        url = m_zipUrl;
        sha = m_zipSha;
        manifestText = m_manifest;
        m_received = 0;
    }
    std::transform(sha.begin(), sha.end(), sha.begin(), ::toupper);
    // Only this project's own release assets (latest was validated as a plain version in RunCheck).
    if (url.rfind(kReleaseDownloadPrefix, 0) != 0 || url.find("..") != std::string::npos || sha.size() != 64 ||
        UpdatesRoot().empty() || latest.empty())
    {
        SetPhase("error", "the release manifest has no valid download");
        return;
    }
    const auto folder = UpdatesRoot() / latest;
    const auto zipPath = folder / "package.zip";
    const auto payload = folder / "payload";
    std::error_code fsError;
    fs::remove_all(folder, fsError);
    fs::create_directories(payload, fsError);
    std::string error;
    const std::wstring wideUrl(url.begin(), url.end());
    if (!HttpGet(wideUrl, kPackageLimit, nullptr, &zipPath, [this](uint64_t aBytes) {
            std::lock_guard lock(m_lock);
            m_received = aBytes;
            return !m_stopping.load();
        }, m_session, error))
    {
        SetPhase("error", error);
        return;
    }
    if (Sha256File(zipPath) != sha)
    {
        SetPhase("error", "the download is damaged (checksum mismatch); try again");
        return;
    }
    SetPhase("verifying");
    // Windows' own tar (bsdtar, Windows 10 1803 and later) unpacks zip archives. Without -P it refuses entries with
    // ".." and strips absolute paths and drive letters, so nothing lands outside the payload folder; the walk below
    // then rejects links and anything the manifest does not list.
    wchar_t system[MAX_PATH]{};
    GetSystemDirectoryW(system, MAX_PATH);
    std::wstring commandLine = L"\"" + (fs::path(system) / "tar.exe").wstring() + L"\" -xf \"" + zipPath.wstring() +
                               L"\" -C \"" + payload.wstring() + L"\"";
    STARTUPINFOW startup{sizeof(startup)};
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(nullptr, commandLine.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process))
    {
        SetPhase("error", "could not unpack the update");
        return;
    }
    CloseHandle(process.hThread);
    // Published so a quitting game can end it; whoever takes it back out of m_extractor closes it.
    m_extractor = process.hProcess;
    DWORD exitCode = 1;
    if (WaitForSingleObject(process.hProcess, 120000) != WAIT_OBJECT_0)
    {
        TerminateProcess(process.hProcess, 1);
        WaitForSingleObject(process.hProcess, 5000);
    }
    else
        GetExitCodeProcess(process.hProcess, &exitCode);
    if (m_extractor.exchange(nullptr) == process.hProcess)
        CloseHandle(process.hProcess);
    else
        exitCode = 1;
    if (exitCode != 0)
    {
        SetPhase("error", "could not unpack the update");
        return;
    }
    // Every file the release lists, checked against its own SHA-256 and kept inside the mod's own folders.
    auto value = CefParseJSON(manifestText, JSON_PARSER_RFC);
    auto manifest = value ? value->GetDictionary() : nullptr;
    auto files = manifest ? manifest->GetList("files") : nullptr;
    if (!files || !files->GetSize())
    {
        SetPhase("error", "the release manifest lists no files");
        return;
    }
    // The unpacked tree holds exactly the listed files: no links, nothing extra.
    std::set<std::string> listed;
    for (size_t i = 0; i < files->GetSize(); ++i)
        if (auto entry = files->GetDictionary(i))
            listed.insert(entry->GetString("path").ToString());
    for (auto it = fs::recursive_directory_iterator(payload, fsError); !fsError && it != fs::recursive_directory_iterator();
         it.increment(fsError))
    {
        const auto attributes = GetFileAttributesW(it->path().c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_REPARSE_POINT))
        {
            SetPhase("error", "the update contains a link, which is not allowed");
            return;
        }
        if (attributes & FILE_ATTRIBUTE_DIRECTORY)
            continue;
        const auto relative = fs::relative(it->path(), payload, fsError).generic_u8string();
        if (!listed.contains(std::string(relative.begin(), relative.end())))
        {
            SetPhase("error", "the update contains an unlisted file");
            return;
        }
    }
    if (fsError)
    {
        SetPhase("error", "could not read the unpacked update");
        return;
    }
    const auto payloadText = payload.generic_u8string();
    std::string pending = fmt::format("{{\"version\":\"{}\",\"payload\":\"{}\",\"files\":[", Json(latest),
                                      Json(std::string(payloadText.begin(), payloadText.end())));
    for (size_t i = 0; i < files->GetSize(); ++i)
    {
        auto entry = files->GetDictionary(i);
        const std::string path = entry ? entry->GetString("path").ToString() : std::string{};
        std::string fileSha = entry ? entry->GetString("sha256").ToString() : std::string{};
        std::transform(fileSha.begin(), fileSha.end(), fileSha.begin(), ::toupper);
        if (!SafeDataPath(path) || fileSha.size() != 64 || Sha256File(payload / fs::u8path(path)) != fileSha)
        {
            SetPhase("error", "the update failed verification at " + path);
            return;
        }
        pending += fmt::format("{}{{\"path\":\"{}\",\"sha256\":\"{}\"}}", i ? "," : "", Json(path), fileSha);
    }
    pending += "]}";
    fs::remove(zipPath, fsError);
    {
        std::ofstream marker(folder / "ready");
    }
    const auto pendingPath = UpdatesRoot() / "pending.json";
    bool written = false;
    {
        std::ofstream file(fs::path(pendingPath).concat(".tmp"), std::ios::binary | std::ios::trunc);
        file << pending;
        file.close();
        written = !file.fail();
    }
    if (written)
        fs::rename(fs::path(pendingPath).concat(".tmp"), pendingPath, fsError);
    if (!written || fsError)
    {
        SetPhase("error", "could not stage the update");
        return;
    }
    SetPhase("ready");
    spdlog::info("Update {} downloaded and verified ({} files); applied at the next launch", latest, files->GetSize());
}

void UpdateService::OnUpdate(const UpdateEvent&) noexcept
{
    const auto now = NowMs();
    if (!m_busy && now >= m_nextCheck)
    {
        m_nextCheck = now + kCheckEveryMs;
        CheckNow();
    }
    // The Options > Updates panel follows the state.
    auto state = StateJson();
    if (state != m_pushed)
    {
        m_pushed = state;
        auto arguments = CefListValue::Create();
        arguments->SetString(0, state);
        if (auto* pApp = m_world.GetOverlayService().GetOverlayApp())
            pApp->ExecuteAsync("updateState", arguments);
    }
}
