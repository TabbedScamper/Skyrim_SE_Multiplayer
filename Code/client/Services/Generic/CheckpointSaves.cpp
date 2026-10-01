#include <Services/CheckpointSaves.h>
#include <Services/FarmMode.h>

#include <Games/TES.h>
#include <Interface/UI.h>
#include <Services/CharacterSnapshots.h>

#include <ShlObj.h>
#include <atomic>
#include <chrono>
#include <ctime>
#include <fstream>
#include <set>

namespace
{
using Clock = std::chrono::system_clock;
namespace fs = std::filesystem;

constexpr char kPrefix[] = "SSC_";

// BGSSaveLoadManager singleton pointer (ID 403340).
void* GetSaveLoadManager() noexcept
{
    POINTER_SKYRIMSE(void*, s_manager, 403340);
    return *s_manager.Get();
}

std::atomic<bool> s_localSaveMade{false};
std::atomic<int64_t> s_lastSaveDoneMs{0};

String s_pendingId;
int64_t s_pendingSinceMs{};
bool s_saveQueued{};

int64_t NowMs() noexcept
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count();
}

// BGSSaveLoadManager::Save_Impl(deviceId, outputStats, name) (ID 35727): every save, manual,
// quick, auto or queued, ends here. Only observed, never called directly.
TP_THIS_FUNCTION(TSaveImpl, bool, void, int32_t, uint32_t, const char*);
TSaveImpl* RealSaveImpl = nullptr;

// A save started off the main thread is handed to the engine's save queue. A menu save ran Save_Impl on a job thread
// (Scaleform ExternalInterface), whose screenshot (0x140665D30) waited for Renderer::Lock while the main thread held
// that lock in its pre-render pass (0x1406565B0) waiting for the job stage: the host froze on a manual save
// (2026-09-30 19:02:39; dump host-save-hang.dmp). The queued task (ID 35769, as Game.RequestSave) is the path the
// checkpoints already use; the next Save_Impl after queuing is that task and runs normally.
std::atomic<uint32_t> s_mainLoopThread{0};
// Set by every completed save: the next main frame writes the character snapshot beside the new save file.
std::atomic<bool> s_snapshotAfterSave{false};
// A Journal save deferred to the next main frame, with its own kind and name.
//
// The first fix re-queued it as an engine save task (35769, 0xF0000080), which is RequestSave: Save_Impl's first
// argument is the save kind (2 manual, 3 auto, 4 quick, 5 RequestSave), so a manual save came out as kind 5 with
// autosave-style naming (owner 2026-09-30). The hang needs the save to run off the main thread: Renderer::Lock
// (N77243, EnterCriticalSection on Renderer+0x27F0) is recursive, so the same kind-2 call on the main thread, at the
// top of the main loop where the engine pumps its own queued saves, cannot deadlock. SkyrimAtlas for-coop S2, S3.
struct DeferredSave
{
    void* Manager{};
    int32_t Kind{};
    uint32_t OutputStats{};
    std::string Name;
};
std::mutex s_deferredSaveLock;
std::optional<DeferredSave> s_deferredSave;

bool TP_MAKE_THISCALL(HookSaveImpl, void, int32_t aDevice, uint32_t aOutputStats, const char* apName)
{
    const auto mainThread = s_mainLoopThread.load(std::memory_order_acquire);
    // Only a save from the open Journal menu: autosaves and quicksaves keep their own kind and slots.
    auto* pUi = UI::Get();
    const bool fromMenu = pUi && pUi->GetMenuOpen(BSFixedString("Journal Menu"));
    if (fromMenu && mainThread && GetCurrentThreadId() != mainThread)
    {
        std::lock_guard lock(s_deferredSaveLock);
        s_deferredSave = DeferredSave{apThis, aDevice, aOutputStats, apName ? apName : ""};
        spdlog::info("Save {} (kind {}) requested on thread {} (not the main thread): runs on the next main frame",
            apName ? apName : "(unnamed)", aDevice, GetCurrentThreadId());
        return true;
    }
    const bool saved = TiltedPhoques::ThisCall(RealSaveImpl, apThis, aDevice, aOutputStats, apName);
    if (saved)
    {
        s_snapshotAfterSave.store(true, std::memory_order_release);
        s_lastSaveDoneMs.store(NowMs(), std::memory_order_release);
        if (!s_saveQueued)
            s_localSaveMade.store(true, std::memory_order_release);
    }
    return saved;
}

fs::path SavesDirectory() noexcept
{
    PWSTR pDocuments = nullptr;
    fs::path result;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &pDocuments)))
        result = fs::path(pDocuments) / "My Games" / "Skyrim Special Edition" / "Saves";
    CoTaskMemFree(pDocuments);
    if (FarmMode::Enabled() && !result.empty()) result /= fs::path("Farm") / FarmMode::Token();
    return result;
}

fs::path RegistryPath() noexcept
{
    if (FarmMode::Enabled()) return FarmMode::Root() / "checkpoints.txt";
    return TiltedPhoques::GetPath() / "checkpoints.txt";
}

// FNV-1a over the loaded plugins in load order (file name, light flag, index). A checkpoint
// written under one load order must not be loaded under another: the save's form IDs would map
// to different plugins, and the peers' worlds would differ from the first frame.
uint64_t LoadOrderHash() noexcept
{
    uint64_t hash = 14695981039346656037ull;
    const auto mix = [&hash](const void* apData, size_t aSize)
    {
        const auto* p = static_cast<const uint8_t*>(apData);
        for (size_t i = 0; i < aSize; ++i)
            hash = (hash ^ p[i]) * 1099511628211ull;
    };
    for (auto* pMod : ModManager::Get()->mods)
    {
        if (!pMod->IsLoaded())
            continue;
        const std::string name = pMod->filename;
        const uint32_t id = pMod->GetId();
        const uint8_t lite = pMod->IsLite() ? 1 : 0;
        mix(name.data(), name.size());
        mix(&lite, 1);
        mix(&id, sizeof(id));
    }
    return hash;
}

// The load-order hash recorded with checkpoint acId, or 0 if none was recorded.
uint64_t RecordedLoadOrder(const String& acId) noexcept
{
    std::ifstream registry(RegistryPath());
    std::string line;
    uint64_t recorded = 0;
    while (std::getline(registry, line))
    {
        const auto tab = line.find('\t');
        if (tab != std::string::npos && line.substr(0, tab) == acId.c_str())
            recorded = std::strtoull(line.c_str() + tab + 1, nullptr, 16);
    }
    return recorded;
}

int64_t FileTimeMs(const fs::path& acPath) noexcept
{
    std::error_code error;
    const auto time = fs::last_write_time(acPath, error);
    if (error)
        return 0;
    const auto system = std::chrono::clock_cast<Clock>(time);
    return std::chrono::duration_cast<std::chrono::milliseconds>(system.time_since_epoch()).count();
}

// The newest finished ordinary save written at or after aSinceMs.
fs::path NewestSaveSince(const int64_t aSinceMs) noexcept
{
    fs::path newest;
    int64_t newestMs = 0;
    std::error_code error;
    for (const auto& entry : fs::directory_iterator(SavesDirectory(), error))
    {
        const auto& path = entry.path();
        if (path.extension() != ".ess" || path.stem().string().rfind(kPrefix, 0) == 0)
            continue;
        const auto ms = FileTimeMs(path);
        if (ms >= aSinceMs && ms > newestMs)
        {
            newest = path;
            newestMs = ms;
        }
    }
    return newest;
}
} // namespace

namespace CheckpointSaves
{
bool TakeLocalSaveMade() noexcept
{
    return s_localSaveMade.exchange(false, std::memory_order_acq_rel);
}

String NewCheckpointId(const uint64_t aEpoch) noexcept
{
    return fmt::format("{:08x}_{}", static_cast<uint32_t>(aEpoch ^ (aEpoch >> 32)), static_cast<uint64_t>(std::time(nullptr))).c_str();
}

void Begin(const String& acId) noexcept
{
    s_pendingId = acId;
    const auto now = NowMs();
    const auto lastSave = s_lastSaveDoneMs.load(std::memory_order_acquire);
    if (lastSave && now - lastSave < 30000)
    {
        // The leader's own save (or one this PC just made) is the checkpoint.
        s_pendingSinceMs = lastSave - 2000;
        s_saveQueued = false;
        return;
    }
    // BGSSaveLoadManager::QueueSaveLoadTask (ID 35769) with the task Papyrus Game.RequestSave uses.
    auto* pManager = GetSaveLoadManager();
    if (!pManager)
        return;
    using TQueueTask = void(void*, uint32_t);
    POINTER_SKYRIMSE(TQueueTask, s_queueTask, 35769);
    s_pendingSinceMs = now - 1000;
    s_saveQueued = true;
    s_queueTask.Get()(pManager, 0xF0000080u);
    spdlog::info("Checkpoint {}: queued a save", acId);
}

std::mutex s_queuedLoadLock;
String s_queuedLoad;
// 0 while queued, 1 when the engine started the load, -1 when it refused it.
std::atomic<int> s_queuedLoadState{0};

void QueueLoad(const String& acId) noexcept
{
    std::lock_guard lock(s_queuedLoadLock);
    s_queuedLoad = acId;
    s_queuedLoadState = 0;
}

int QueuedLoadState() noexcept
{
    return s_queuedLoadState.load();
}

void MarkMainThread() noexcept
{
    s_mainLoopThread.store(GetCurrentThreadId(), std::memory_order_release);
    // Every save made with the mod carries its character (<save>.snap), so the join picker never loads a save.
    if (s_snapshotAfterSave.exchange(false, std::memory_order_acq_rel))
    {
        const auto save = NewestSaveSince(s_lastSaveDoneMs.load(std::memory_order_acquire) - 10000);
        if (!save.empty())
            CharacterSnapshots::QueueCaptureTo(std::filesystem::path(save).replace_extension(".snap"));
    }
    String load;
    {
        std::lock_guard lock(s_queuedLoadLock);
        load = std::exchange(s_queuedLoad, String{});
    }
    if (!load.empty())
        s_queuedLoadState = Load(load) ? 1 : -1;
    std::optional<DeferredSave> save;
    {
        std::lock_guard lock(s_deferredSaveLock);
        save.swap(s_deferredSave);
    }
    if (save)
    {
        // Through the hook (now on the main thread), so the save's snapshot bookkeeping runs as for any save.
        const bool saved = HookSaveImpl(save->Manager, save->Kind, save->OutputStats,
            save->Name.empty() ? nullptr : save->Name.c_str());
        spdlog::info("Deferred save {} (kind {}) on the main thread: {}", save->Name, save->Kind, saved ? "saved" : "FAILED");
    }
}

bool Poll() noexcept
{
    if (s_pendingId.empty())
        return true;
    const auto source = NewestSaveSince(s_pendingSinceMs);
    // Wait until the save is finished (renamed from .tmp) and has not changed for a second.
    if (source.empty() || NowMs() - FileTimeMs(source) < 1000)
    {
        if (NowMs() - s_pendingSinceMs > 120000)
        {
            spdlog::error("Checkpoint {}: no save appeared; Continue will not match the leader", s_pendingId);
            s_pendingId.clear();
            s_saveQueued = false;
        }
        return false;
    }
    const auto target = SavesDirectory() / fmt::format("{}{}.ess", kPrefix, s_pendingId.c_str());
    std::error_code error;
    fs::copy_file(source, target, fs::copy_options::overwrite_existing, error);
    auto coSave = source;
    coSave.replace_extension(".skse");
    if (!error && fs::exists(coSave))
    {
        auto coTarget = target;
        coTarget.replace_extension(".skse");
        fs::copy_file(coSave, coTarget, fs::copy_options::overwrite_existing, error);
    }
    // The character written with the save travels with the checkpoint.
    auto snapshot = source;
    snapshot.replace_extension(".snap");
    if (!error && fs::exists(snapshot))
    {
        auto snapshotTarget = target;
        snapshotTarget.replace_extension(".snap");
        std::error_code snapshotError;
        fs::copy_file(snapshot, snapshotTarget, fs::copy_options::overwrite_existing, snapshotError);
    }
    if (error)
        spdlog::error("Checkpoint save {}: copying {} failed: {}", target.filename().string(), source.filename().string(), error.message());
    else
    {
        spdlog::info("Checkpoint save {}{}: written (from {})", kPrefix, s_pendingId.c_str(), source.filename().string());
        // A drop-in save is the joiner's copy of the leader's world, not a shared checkpoint.
        if (std::string_view(s_pendingId.c_str()).rfind("dropin_", 0) != 0)
        {
            std::ofstream registry(RegistryPath(), std::ios::app);
            registry << s_pendingId.c_str() << '\t' << std::hex << LoadOrderHash() << '\n';
        }
    }
    s_pendingId.clear();
    s_saveQueued = false;
    return true;
}

bool IsPending() noexcept
{
    return !s_pendingId.empty();
}

fs::path PathOf(const String& acId) noexcept
{
    return SavesDirectory() / fmt::format("{}{}.ess", kPrefix, acId.c_str());
}

bool ValidId(const String& acId) noexcept
{
    // Ids become file names in the Saves folder: letters, digits, '_' and '-' only, like PartyService checks.
    return !acId.empty() && acId.size() <= 64 &&
           std::all_of(acId.begin(), acId.end(), [](char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-'; });
}

bool WriteReceived(const String& acId, const std::string& acBytes) noexcept
{
    if (!ValidId(acId))
        return false;
    const auto target = PathOf(acId);
    auto temporary = target;
    temporary += ".tmp";
    {
        std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
        file.write(acBytes.data(), static_cast<std::streamsize>(acBytes.size()));
        if (!file)
            return false;
    }
    std::error_code error;
    fs::rename(temporary, target, error);
    if (error)
        spdlog::error("Drop-in save {} not written: {}", target.filename().string(), error.message());
    return !error;
}

String Latest() noexcept
{
    std::ifstream registry(RegistryPath());
    std::string line, latest;
    while (std::getline(registry, line))
    {
        if (!line.empty())
            latest = line.substr(0, line.find('\t'));
    }
    return latest.c_str();
}

namespace
{
std::string JsonText(const std::string& acText)
{
    std::string out;
    for (const auto c : acText)
    {
        if (c == '"' || c == '\\')
            out += '\\';
        if (static_cast<unsigned char>(c) >= 0x20)
            out += c;
    }
    return out;
}

std::string Base64(const uint8_t* apData, size_t aSize)
{
    static constexpr char cTable[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((aSize + 2) / 3 * 4);
    for (size_t i = 0; i < aSize; i += 3)
    {
        const uint32_t chunk = (apData[i] << 16) | ((i + 1 < aSize ? apData[i + 1] : 0) << 8) | (i + 2 < aSize ? apData[i + 2] : 0);
        out += cTable[(chunk >> 18) & 63];
        out += cTable[(chunk >> 12) & 63];
        out += i + 1 < aSize ? cTable[(chunk >> 6) & 63] : '=';
        out += i + 2 < aSize ? cTable[chunk & 63] : '=';
    }
    return out;
}

// The .ess header (UESP "Skyrim Mod:Save File Format", SE version 12): "TESV_SAVEGAME", header size, then version,
// save number, player name, level, location, game date, race editor id (u16-length strings), sex, current and
// level-up experience, FILETIME, screenshot width/height, compression type; then the screenshot, uncompressed RGBA.
struct SaveHeader
{
    std::string Player, Location, GameDate;
    uint32_t Level{};
    std::string Image;
};

bool ReadSaveHeader(const fs::path& acPath, SaveHeader& aHeader) noexcept
{
    std::ifstream file(acPath, std::ios::binary);
    char magic[13]{};
    if (!file.read(magic, 13) || std::string_view(magic, 13) != "TESV_SAVEGAME")
        return false;
    const auto u32 = [&file](uint32_t& aValue) { return static_cast<bool>(file.read(reinterpret_cast<char*>(&aValue), 4)); };
    const auto text = [&file](std::string& aValue) {
        uint16_t length{};
        if (!file.read(reinterpret_cast<char*>(&length), 2) || length > 1024)
            return false;
        aValue.resize(length);
        return length == 0 || static_cast<bool>(file.read(aValue.data(), length));
    };
    uint32_t headerSize{}, version{}, saveNumber{};
    std::string race;
    if (!u32(headerSize) || !u32(version) || !u32(saveNumber) || !text(aHeader.Player) || !u32(aHeader.Level) ||
        !text(aHeader.Location) || !text(aHeader.GameDate) || !text(race))
        return false;
    char skip[2 + 4 + 4 + 8];
    uint32_t width{}, height{};
    if (!file.read(skip, sizeof(skip)) || !u32(width) || !u32(height))
        return false;
    if (version >= 12)
        file.ignore(2);
    const uint32_t channels = version >= 11 ? 4 : 3;
    if (!width || !height || width > 4096 || height > 4096)
        return true;
    std::vector<uint8_t> pixels(static_cast<size_t>(width) * height * channels);
    if (!file.read(reinterpret_cast<char*>(pixels.data()), static_cast<std::streamsize>(pixels.size())))
        return true;
    // Down to at most 256 wide for the menu (nearest sample), as a top-down 32-bit BMP data URL.
    const uint32_t outWidth = (std::min)(width, 256u);
    const uint32_t outHeight = (std::max)(1u, height * outWidth / width);
    const uint32_t pixelBytes = outWidth * outHeight * 4;
    std::vector<uint8_t> bmp(54 + pixelBytes);
    const auto put32 = [&bmp](size_t aOffset, uint32_t aValue) { memcpy(bmp.data() + aOffset, &aValue, 4); };
    bmp[0] = 'B';
    bmp[1] = 'M';
    put32(2, static_cast<uint32_t>(bmp.size()));
    put32(10, 54);
    put32(14, 40);
    put32(18, outWidth);
    put32(22, static_cast<uint32_t>(-static_cast<int32_t>(outHeight)));
    bmp[26] = 1;
    bmp[28] = 32;
    put32(34, pixelBytes);
    for (uint32_t y = 0; y < outHeight; ++y)
        for (uint32_t x = 0; x < outWidth; ++x)
        {
            const auto* source = &pixels[(static_cast<size_t>(y * height / outHeight) * width + x * width / outWidth) * channels];
            auto* target = &bmp[54 + (static_cast<size_t>(y) * outWidth + x) * 4];
            target[0] = source[2];
            target[1] = source[1];
            target[2] = source[0];
            target[3] = 255;
        }
    aHeader.Image = "data:image/bmp;base64," + Base64(bmp.data(), bmp.size());
    return true;
}
} // namespace

std::string ListJson(size_t aMaximum) noexcept
{
    std::vector<std::string> ids;
    {
        std::ifstream registry(RegistryPath());
        std::string line;
        while (std::getline(registry, line))
            if (!line.empty())
                ids.push_back(line.substr(0, line.find('\t')));
    }
    std::string json = "[";
    std::set<std::string> seen;
    size_t count = 0;
    // Newest first, each id once, only those whose save is on this PC.
    for (auto it = ids.rbegin(); it != ids.rend() && count < aMaximum; ++it)
    {
        if (!seen.insert(*it).second || !Has(it->c_str()))
            continue;
        const auto path = SavesDirectory() / fmt::format("{}{}.ess", kPrefix, *it);
        SaveHeader header;
        if (!ReadSaveHeader(path, header))
            continue;
        if (count++)
            json += ',';
        json += fmt::format("{{\"id\":\"{}\",\"player\":\"{}\",\"level\":{},\"location\":\"{}\",\"gameDate\":\"{}\","
            "\"savedMs\":{},\"image\":\"{}\"}}", JsonText(*it), JsonText(header.Player), header.Level,
            JsonText(header.Location), JsonText(header.GameDate), FileTimeMs(path), header.Image);
    }
    return json + "]";
}

std::string CharactersJson(size_t aMaximum) noexcept
{
    struct Found
    {
        fs::path Snap;
        int64_t Ms{};
        CharacterSnapshot Character;
    };
    std::vector<Found> found;
    std::error_code error;
    for (const auto& entry : fs::directory_iterator(SavesDirectory(), error))
    {
        if (error || !entry.is_regular_file() || entry.path().extension() != ".snap")
            continue;
        // Drop-in copies are someone else's world loaded for a join, not a character to pick.
        if (entry.path().filename().string().rfind("SSC_dropin_", 0) == 0)
            continue;
        Found item{entry.path(), FileTimeMs(entry.path())};
        if (CharacterSnapshots::ReadFile(entry.path(), item.Character) && !item.Character.Name.empty())
            found.push_back(std::move(item));
    }
    std::sort(found.begin(), found.end(), [](const Found& a, const Found& b) { return a.Ms > b.Ms; });
    std::set<std::string> names;
    std::string json = "[";
    size_t count = 0;
    for (const auto& item : found)
    {
        if (count >= aMaximum || !names.insert(item.Character.Name.c_str()).second)
            continue;
        auto save = item.Snap;
        save.replace_extension(".ess");
        SaveHeader header;
        ReadSaveHeader(save, header);
        if (count++)
            json += ',';
        json += fmt::format("{{\"path\":\"{}\",\"name\":\"{}\",\"level\":{},\"location\":\"{}\",\"savedMs\":{},\"image\":\"{}\"}}",
            JsonText(item.Snap.generic_string()), JsonText(item.Character.Name.c_str()), item.Character.Level,
            JsonText(header.Location), item.Ms, header.Image);
    }
    return json + "]";
}

bool Has(const String& acId) noexcept
{
    std::error_code error;
    return !acId.empty() && fs::exists(SavesDirectory() / fmt::format("{}{}.ess", kPrefix, acId.c_str()), error);
}

bool Load(const String& acId) noexcept
{
    auto* pManager = GetSaveLoadManager();
    if (!pManager || !ValidId(acId) || !Has(acId))
    {
        spdlog::warn("Checkpoint {}{} is not on this PC", kPrefix, acId.c_str());
        return false;
    }
    const auto name = fmt::format("{}{}", kPrefix, acId.c_str());
    if (const auto recorded = RecordedLoadOrder(acId); recorded && recorded != LoadOrderHash())
    {
        spdlog::error("Checkpoint {} was written under a different load order ({:x}, now {:x}); not loading it", name,
            recorded, LoadOrderHash());
        return false;
    }

    // Mirror the console LoadGame command (FUN_140375220): raise its load flag (ID 403444),
    // tear down the running game (ID 36604), then Load_Impl(name, -1, 0, checkForMods).
    POINTER_SKYRIMSE(uint8_t, s_loadFlag, 403444);
    using TPrepareLoad = void();
    POINTER_SKYRIMSE(TPrepareLoad, s_prepareLoad, 36604);
    TP_THIS_FUNCTION(TLoadImpl, bool, void, const char*, int32_t, uint32_t, bool);
    POINTER_SKYRIMSE(TLoadImpl, s_loadImpl, 35728);

    *s_loadFlag.Get() = 1;
    s_prepareLoad.Get()();
    const bool started = TiltedPhoques::ThisCall(s_loadImpl.Get(), pManager, name.c_str(), -1, 0u, true);
    spdlog::info("Checkpoint load {}: {}", name, started ? "started" : "FAILED");
    return started;
}
} // namespace CheckpointSaves

static TiltedPhoques::Initializer s_checkpointSaveHooks(
    []()
    {
        POINTER_SKYRIMSE(TSaveImpl, saveImpl, 35727);
        RealSaveImpl = saveImpl.Get();
        TP_HOOK(&RealSaveImpl, HookSaveImpl);
    });
