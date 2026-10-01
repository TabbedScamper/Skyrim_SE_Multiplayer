// SPDX-License-Identifier: GPL-3.0-only
// Skyrim SE Multiplayer is based on Skyrim Together Reborn by Tilted Phoques.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <bcrypt.h>
#include <tlhelp32.h>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <vector>
#include <algorithm>
#include <stdexcept>
#include <cctype>
#include <exception>
#include <iterator>

namespace fs = std::filesystem;

namespace
{
struct Json
{
    std::string value;
    std::map<std::string, Json> object;
    std::vector<Json> array;
    enum { String, Object, Array } type = String;
    const Json& at(const char* key) const
    {
        if (type != Object || !object.contains(key))
            throw std::runtime_error(std::string("missing field: ") + key);
        return object.at(key);
    }
    const std::string& str() const
    {
        if (type != String) throw std::runtime_error("invalid JSON field type");
        return value;
    }
};

class Parser
{
public:
    explicit Parser(const std::string& text) : m_text(text) {}
    Json parse()
    {
        auto result = item(0);
        space();
        if (m_pos != m_text.size()) throw std::runtime_error("trailing JSON data");
        return result;
    }
private:
    void space()
    {
        while (m_pos < m_text.size() && (m_text[m_pos] == ' ' || m_text[m_pos] == '\n' ||
                                          m_text[m_pos] == '\r' || m_text[m_pos] == '\t')) ++m_pos;
    }
    bool take(char c)
    {
        space();
        if (m_pos < m_text.size() && m_text[m_pos] == c) { ++m_pos; return true; }
        return false;
    }
    std::string string()
    {
        if (!take('"')) throw std::runtime_error("expected JSON string");
        std::string out;
        while (m_pos < m_text.size())
        {
            unsigned char c = m_text[m_pos++];
            if (c == '"') return out;
            if (c < 0x20) throw std::runtime_error("invalid JSON control character");
            if (c != '\\') { out += static_cast<char>(c); continue; }
            if (m_pos == m_text.size()) break;
            c = m_text[m_pos++];
            switch (c)
            {
            case '"': out += '"'; break;
            case '\\': out += '\\'; break;
            case '/': out += '/'; break;
            case 'n': out += '\n'; break;
            case 'r': out += '\r'; break;
            case 't': out += '\t'; break;
            default: throw std::runtime_error("unsupported JSON escape");
            }
        }
        throw std::runtime_error("unterminated JSON string");
    }
    Json item(int depth)
    {
        if (depth > 12) throw std::runtime_error("JSON nesting limit");
        space();
        Json result;
        if (take('{'))
        {
            result.type = Json::Object;
            if (take('}')) return result;
            do
            {
                auto key = string();
                if (!take(':')) throw std::runtime_error("expected colon");
                if (!result.object.emplace(key, item(depth + 1)).second)
                    throw std::runtime_error("duplicate JSON key");
            } while (take(','));
            if (!take('}')) throw std::runtime_error("expected object end");
        }
        else if (take('['))
        {
            result.type = Json::Array;
            if (take(']')) return result;
            do { result.array.push_back(item(depth + 1)); } while (take(','));
            if (!take(']')) throw std::runtime_error("expected array end");
        }
        else result.value = string();
        return result;
    }
    const std::string& m_text;
    size_t m_pos = 0;
};

std::string readFile(const fs::path& path, uintmax_t limit = 4 * 1024 * 1024)
{
    std::error_code ec;
    if (fs::file_size(path, ec) > limit || ec)
        throw std::runtime_error("update metadata is missing or too large");
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

std::string escape(const std::string& input)
{
    std::string out;
    for (unsigned char c : input)
    {
        if (c == '"' || c == '\\') { out += '\\'; out += c; }
        else if (c == '\n') out += "\\n";
        else if (c == '\r') out += "\\r";
        else if (c == '\t') out += "\\t";
        else if (c < 0x20) throw std::runtime_error("invalid text in update metadata");
        else out += c;
    }
    return out;
}

bool versionOk(const std::string& v)
{
    size_t p = 0;
    for (int part = 0; part < 3; ++part)
    {
        size_t start = p;
        while (p < v.size() && v[p] >= '0' && v[p] <= '9') ++p;
        if (p == start || p - start > 12) return false;
        if (part < 2 && (p == v.size() || v[p++] != '.')) return false;
    }
    if (p == v.size()) return true;
    if (v[p++] != '-' || p == v.size()) return false;
    for (; p < v.size(); ++p)
        if (!((v[p] >= '0' && v[p] <= '9') || (v[p] >= 'A' && v[p] <= 'Z') ||
              (v[p] >= 'a' && v[p] <= 'z') || v[p] == '.' || v[p] == '-')) return false;
    return true;
}

bool pathOk(const std::string& path)
{
    if (path.empty() || path.size() > 260 || path[0] == '/' || path[0] == '\\' ||
        path.find(':') != std::string::npos || path.find('\\') != std::string::npos ||
        path.find("..") != std::string::npos) return false;
    size_t start = 0;
    while (start < path.size())
    {
        auto end = path.find('/', start);
        if (end == start || (end == std::string::npos && start == path.size())) return false;
        start = end == std::string::npos ? path.size() : end + 1;
        if (end != std::string::npos && start == path.size()) return false;
    }
    auto slash = path.find('/');
    if (slash == std::string::npos)
        return path == "SkyrimSEMultiplayer.esp" || path == "SkyrimSEMultiplayerQuestPatches.esp";
    auto root = path.substr(0, slash);
    return root == "SkyrimTogetherReborn" || root == "Skyrim_SE_Multiplayer" ||
           root == "SkyrimTogetherRebornBehaviors" || root == "Skyrim_SE_MultiplayerBehaviors" ||
           root == "scripts" || root == "meshes";
}

void noReparse(const fs::path& root, const fs::path& relative)
{
    fs::path current = root;
    for (const auto& part : relative)
    {
        current /= part;
        DWORD attributes = GetFileAttributesW(current.c_str());
        if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT))
            throw std::runtime_error("update path crosses a reparse point");
    }
}

std::string hashFile(const fs::path& path)
{
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    DWORD objectSize = 0, length = 0;
    std::vector<UCHAR> object;
    UCHAR digest[32]{};
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("missing staged file");
    try
    {
        if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0 ||
            BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&objectSize),
                              sizeof(objectSize), &length, 0) < 0)
            throw std::runtime_error("SHA-256 initialization failed");
        object.resize(objectSize);
        if (BCryptCreateHash(algorithm, &hash, object.data(), objectSize, nullptr, 0, 0) < 0)
            throw std::runtime_error("SHA-256 initialization failed");
        char buffer[65536];
        while (in.read(buffer, sizeof(buffer)) || in.gcount())
            if (BCryptHashData(hash, reinterpret_cast<PUCHAR>(buffer), static_cast<ULONG>(in.gcount()), 0) < 0)
                throw std::runtime_error("SHA-256 read failed");
        if (!in.eof() || BCryptFinishHash(hash, digest, sizeof(digest), 0) < 0)
            throw std::runtime_error("SHA-256 read failed");
        BCryptDestroyHash(hash);
        BCryptCloseAlgorithmProvider(algorithm, 0);
    }
    catch (...)
    {
        if (hash) BCryptDestroyHash(hash);
        if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
        throw;
    }
    constexpr char hex[] = "0123456789ABCDEF";
    std::string out;
    for (UCHAR byte : digest) { out += hex[byte >> 4]; out += hex[byte & 15]; }
    return out;
}

struct Entry { std::string path, sha; int state = 0; bool existed = false; };
// UTF-8 text to a path (std::filesystem::u8path is deprecated in C++20).
static fs::path Utf8Path(const std::string& text)
{
    return fs::path(std::u8string(text.begin(), text.end()));
}

struct Pending { std::string version; fs::path payload; std::vector<Entry> files; };

Pending parsePending(const fs::path& path, const fs::path& updates)
{
    auto json = Parser(readFile(path)).parse();
    Pending p;
    p.version = json.at("version").str();
    if (!versionOk(p.version)) throw std::runtime_error("invalid update version");
    p.payload = Utf8Path(json.at("payload").str()).lexically_normal();
    const auto expected = (updates / Utf8Path(p.version) / "payload").lexically_normal();
    if (_wcsicmp(p.payload.c_str(), expected.c_str()) != 0)
        throw std::runtime_error("staged payload is outside its update folder");
    noReparse(updates, Utf8Path(p.version) / "payload");
    const auto& files = json.at("files");
    if (files.type != Json::Array || files.array.empty()) throw std::runtime_error("empty update file list");
    std::set<std::string> seen;
    for (const auto& file : files.array)
    {
        Entry e{file.at("path").str(), file.at("sha256").str()};
        if (!pathOk(e.path) || e.sha.size() != 64)
            throw std::runtime_error("invalid update file entry");
        std::string folded = e.path;
        std::transform(folded.begin(), folded.end(), folded.begin(), [](unsigned char c) { return std::tolower(c); });
        if (!seen.insert(folded).second) throw std::runtime_error("duplicate update path");
        std::transform(e.sha.begin(), e.sha.end(), e.sha.begin(), [](unsigned char c) { return std::toupper(c); });
        if (!std::all_of(e.sha.begin(), e.sha.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'F'); }))
            throw std::runtime_error("invalid update hash");
        p.files.push_back(std::move(e));
    }
    return p;
}

void durable(const fs::path& path, const std::string& data)
{
    auto temp = path;
    temp += L".tmp";
    HANDLE file = CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) throw std::runtime_error("could not write update journal");
    DWORD written = 0;
    bool ok = WriteFile(file, data.data(), static_cast<DWORD>(data.size()), &written, nullptr) &&
              written == data.size() && FlushFileBuffers(file);
    CloseHandle(file);
    if (!ok || !MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        throw std::runtime_error("could not flush update journal");
}

void journal(const fs::path& path, const Pending& p)
{
    std::string data = "{\"version\":\"" + escape(p.version) + "\",\"files\":[";
    for (const auto& e : p.files)
    {
        if (data.back() != '[') data += ',';
        data += "{\"path\":\"" + escape(e.path) + "\",\"sha256\":\"" + e.sha +
                "\",\"state\":\"" + std::to_string(e.state) + "\",\"existed\":\"" +
                (e.existed ? "1" : "0") + "\"}";
    }
    durable(path, data + "]}");
}

void rollback(const fs::path& data, const fs::path& backup, const Pending& p)
{
    for (auto it = p.files.rbegin(); it != p.files.rend(); ++it)
    {
        if (it->state < 2) continue;
        auto relative = Utf8Path(it->path);
        noReparse(data, relative);
        auto target = data / relative;
        auto saved = backup / relative;
        auto fresh = target;
        fresh += L".ssmnew";
        noReparse(data, fresh.lexically_relative(data));
        std::error_code ec;
        fs::remove(fresh, ec);
        if (it->existed)
        {
            if (!fs::exists(saved)) throw std::runtime_error("rollback backup is missing");
            if (_wcsicmp(target.filename().c_str(), L"Skyrim_SE_Multiplayer.exe") == 0 &&
                fs::exists(target))
            {
                auto old = target;
                old += L".old";
                std::error_code ec;
                fs::remove(old, ec);
                if (!MoveFileExW(target.c_str(), old.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
                    throw std::runtime_error("rollback failed: could not rename running starter");
            }
            if (!CopyFileW(saved.c_str(), fresh.c_str(), FALSE) ||
                !MoveFileExW(fresh.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
                throw std::runtime_error("could not restore previous file");
        }
        else fs::remove(target, ec);
    }
    auto version = data / "SkyrimTogetherReborn" / "version.json";
    auto previous = backup / ".previous-version.json";
    if (fs::exists(previous))
    {
        if (!CopyFileW(previous.c_str(), version.c_str(), FALSE))
            throw std::runtime_error("could not restore previous version marker");
    }
    else
    {
        std::error_code ec;
        fs::remove(version, ec);
    }
}

Pending readJournal(const fs::path& path, const Pending& expected)
{
    auto json = Parser(readFile(path, 8 * 1024 * 1024)).parse();
    if (json.at("version").str() != expected.version) throw std::runtime_error("journal version mismatch");
    auto result = expected;
    const auto& list = json.at("files");
    if (list.type != Json::Array || list.array.size() != result.files.size())
        throw std::runtime_error("journal file list mismatch");
    for (size_t i = 0; i < result.files.size(); ++i)
    {
        const auto& j = list.array[i];
        if (j.at("path").str() != result.files[i].path || j.at("sha256").str() != result.files[i].sha)
            throw std::runtime_error("journal file mismatch");
        const auto& state = j.at("state").str();
        const auto& existed = j.at("existed").str();
        if (state.size() != 1 || state[0] < '0' || state[0] > '3' || (existed != "0" && existed != "1"))
            throw std::runtime_error("invalid journal state");
        result.files[i].state = state[0] - '0';
        result.files[i].existed = existed == "1";
    }
    return result;
}

bool running(const wchar_t* name)
{
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return true;
    PROCESSENTRY32W entry{sizeof(entry)};
    bool found = false;
    if (Process32FirstW(snapshot, &entry))
        do { if (_wcsicmp(entry.szExeFile, name) == 0) { found = true; break; } }
        while (Process32NextW(snapshot, &entry));
    CloseHandle(snapshot);
    return found;
}

void apply(const fs::path& data, const fs::path& updates)
{
    const auto pendingPath = updates / "pending.json";
    if (!fs::exists(pendingPath)) return;
    Pending p = parsePending(pendingPath, updates);
    auto folder = updates / Utf8Path(p.version);
    auto backup = folder / "backup";
    auto log = folder / "journal.json";
    if (!fs::exists(folder / "ready")) throw std::runtime_error("update is not ready");
    if (fs::exists(log))
    {
        auto previous = readJournal(log, p);
        rollback(data, backup, previous);
        fs::remove(log);
    }
    for (const auto& e : p.files)
    {
        auto relative = Utf8Path(e.path);
        noReparse(data, relative);
        noReparse(p.payload, relative);
        if (hashFile(p.payload / relative) != e.sha)
            throw std::runtime_error("staged file hash mismatch: " + e.path);
    }
    try
    {
        fs::create_directories(backup);
        auto versionFile = data / "SkyrimTogetherReborn" / "version.json";
        auto previousVersion = backup / ".previous-version.json";
        if (fs::exists(versionFile) && !CopyFileW(versionFile.c_str(), previousVersion.c_str(), FALSE))
            throw std::runtime_error("could not back up version marker");
        journal(log, p);
        for (auto& e : p.files)
        {
            auto relative = Utf8Path(e.path);
            auto target = data / relative;
            auto source = p.payload / relative;
            auto saved = backup / relative;
            auto fresh = target;
            fresh += L".ssmnew";
            noReparse(data, relative);
            noReparse(backup, relative);
            noReparse(data, fresh.lexically_relative(data));
            fs::create_directories(target.parent_path());
            fs::create_directories(saved.parent_path());
            e.existed = fs::exists(target);
            e.state = 1;
            journal(log, p);
            if (e.existed && !CopyFileW(target.c_str(), saved.c_str(), FALSE))
                throw std::runtime_error("could not back up: " + e.path);
            e.state = 2;
            journal(log, p);
            if (!CopyFileW(source.c_str(), fresh.c_str(), FALSE))
                throw std::runtime_error("could not stage: " + e.path);
            if (_wcsicmp(target.filename().c_str(), L"Skyrim_SE_Multiplayer.exe") == 0)
            {
                auto old = target;
                old += L".old";
                std::error_code ec;
                fs::remove(old, ec);
                if (fs::exists(target) && !MoveFileExW(target.c_str(), old.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
                    throw std::runtime_error("could not rename running starter");
            }
            if (!MoveFileExW(fresh.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
                throw std::runtime_error("could not install: " + e.path);
            e.state = 3;
            journal(log, p);
        }
        durable(versionFile, "{\"version\":\"" + p.version + "\"}");
        fs::remove(pendingPath);
        std::error_code cleanup;
        fs::remove(log, cleanup);
        for (fs::directory_iterator it(updates, cleanup), end; it != end && !cleanup; it.increment(cleanup))
        {
            const auto& item = *it;
            if (!item.is_symlink(cleanup) && item.is_directory(cleanup) && item.path() != folder &&
                versionOk(item.path().filename().string()))
                fs::remove_all(item.path(), cleanup);
        }
    }
    catch (...)
    {
        auto error = std::current_exception();
        try { rollback(data, backup, p); }
        catch (const std::exception& restore)
        {
            throw std::runtime_error(std::string("rollback failed: ") + restore.what());
        }
        std::error_code journalCleanup;
        fs::remove(log, journalCleanup);
        std::error_code ec;
        fs::rename(pendingPath, updates / "pending.failed.json", ec);
        std::rethrow_exception(error);
    }
}

std::wstring args()
{
    const wchar_t* line = GetCommandLineW();
    if (*line == '"') { ++line; while (*line && *line != '"') ++line; if (*line) ++line; }
    else while (*line && *line != ' ' && *line != '\t') ++line;
    return line;
}

void launch(const fs::path& folder)
{
    auto executable = folder / "SkyrimTogether.exe";
    std::wstring command = L"\"" + executable.wstring() + L"\"" + args();
    STARTUPINFOW startup{sizeof(startup)};
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE, 0, nullptr,
                        folder.c_str(), &startup, &process))
        throw std::runtime_error("could not launch SkyrimTogether.exe");
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
}
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    HANDLE mutex = CreateMutexW(nullptr, FALSE, L"Local\\SkyrimSEMultiplayerStarter");
    DWORD lock = mutex ? WaitForSingleObject(mutex, 30000) : WAIT_FAILED;
    if (lock != WAIT_OBJECT_0 && lock != WAIT_ABANDONED)
    {
        MessageBoxW(nullptr, L"Another Skyrim SE Multiplayer starter is running.", L"Skyrim SE Multiplayer", MB_ICONINFORMATION);
        return 1;
    }
    try
    {
        wchar_t own[MAX_PATH];
        DWORD ownLength = GetModuleFileNameW(nullptr, own, MAX_PATH);
        if (!ownLength || ownLength == MAX_PATH) throw std::runtime_error("could not locate starter");
        const auto folder = fs::path(own).parent_path();
        const auto data = folder.parent_path();
        if (running(L"SkyrimSE.exe") || running(L"SkyrimTogether.exe"))
        {
            MessageBoxW(nullptr, L"Skyrim is already running. Close it before applying an update.",
                        L"Skyrim SE Multiplayer", MB_ICONINFORMATION);
            return 0;
        }
        std::error_code ec;
        fs::remove(folder / "Skyrim_SE_Multiplayer.exe.old", ec);
        wchar_t local[32768]{};
        if (GetEnvironmentVariableW(L"LOCALAPPDATA", local, static_cast<DWORD>(std::size(local))) > 0)
        {
            fs::path updates = fs::path(local) / "SkyrimSEMultiplayer" / "updates";
            if (fs::exists(updates / "pending.json"))
            {
                try { apply(data, updates); }
                catch (const std::exception& error)
                {
                    std::error_code renameError;
                    if (fs::exists(updates / "pending.json"))
                        fs::rename(updates / "pending.json", updates / "pending.failed.json", renameError);
                    std::string message = "The update could not be applied: ";
                    message += error.what();
                    if (message.find("rollback failed") != std::string::npos)
                        message += ". Check the backup folder before retrying.";
                    else message += ". Your previous version was restored.";
                    MessageBoxA(nullptr, message.c_str(), "Skyrim SE Multiplayer", MB_ICONERROR);
                }
            }
        }
        launch(folder);
    }
    catch (const std::exception& error)
    {
        MessageBoxA(nullptr, error.what(), "Skyrim SE Multiplayer", MB_ICONERROR);
        ReleaseMutex(mutex);
        CloseHandle(mutex);
        return 1;
    }
    ReleaseMutex(mutex);
    CloseHandle(mutex);
    return 0;
}
