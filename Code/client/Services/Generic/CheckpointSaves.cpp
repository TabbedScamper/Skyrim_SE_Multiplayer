#include <Services/CheckpointSaves.h>

#include <Games/TES.h>

#include <ShlObj.h>
#include <atomic>
#include <chrono>
#include <ctime>
#include <fstream>

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

bool TP_MAKE_THISCALL(HookSaveImpl, void, int32_t aDevice, uint32_t aOutputStats, const char* apName)
{
    const bool saved = TiltedPhoques::ThisCall(RealSaveImpl, apThis, aDevice, aOutputStats, apName);
    if (saved)
    {
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
    return result;
}

fs::path RegistryPath() noexcept
{
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
    if (error)
        spdlog::error("Checkpoint save {}: copying {} failed: {}", target.filename().string(), source.filename().string(), error.message());
    else
    {
        spdlog::info("Checkpoint save {}{}: written (from {})", kPrefix, s_pendingId.c_str(), source.filename().string());
        std::ofstream registry(RegistryPath(), std::ios::app);
        registry << s_pendingId.c_str() << '\t' << std::hex << LoadOrderHash() << '\n';
    }
    s_pendingId.clear();
    s_saveQueued = false;
    return true;
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

bool Has(const String& acId) noexcept
{
    std::error_code error;
    return !acId.empty() && fs::exists(SavesDirectory() / fmt::format("{}{}.ess", kPrefix, acId.c_str()), error);
}

bool Load(const String& acId) noexcept
{
    auto* pManager = GetSaveLoadManager();
    if (!pManager || !Has(acId))
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
