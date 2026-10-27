#include <Services/CheckpointSaves.h>
#include <Services/FarmMode.h>

#include <Games/TES.h>
#include <Interface/UI.h>
#include <Services/CharacterSnapshots.h>
#include <Games/References.h>
#include <World.h>
#include <PlayerCharacter.h>
#include <Forms/TESObjectCELL.h>
#include <Messages/PlayerRespawnRequest.h>
#include <Services/LobbyService.h>
#include <Interface/MainMenuIntegration.h>
#include <Utils/MainThread.h>

#include <ShlObj.h>
#include <atomic>
#include <chrono>
#include <ctime>
#include <fstream>
#include <map>
#include <set>
#include <unordered_map>
#include <unordered_set>

namespace
{
using Clock = std::chrono::system_clock;
namespace fs = std::filesystem;

constexpr char kPrefix[] = "SSC_";
fs::path SavesDirectory() noexcept;

// BGSSaveLoadManager singleton pointer (ID 403340).
void* GetSaveLoadManager() noexcept
{
    POINTER_SKYRIMSE(void*, s_manager, 403340);
    return *s_manager.Get();
}

std::atomic<bool> s_localSaveMade{false};
std::atomic<int64_t> s_lastSaveDoneMs{0};
std::atomic<uint64_t> s_saveSerial{0};
std::string s_carryKey;
std::string s_carryName;
std::mutex s_carrySourceLock;
std::atomic<bool> s_carryWriteQueued{false};
std::atomic<uint64_t> s_loadGeneration{0};
std::atomic<uint64_t> s_carryQueuedGeneration{0};
std::atomic<uint32_t> s_mainLoopThread{0};

fs::path CarryHomePath(const std::string& acKey) noexcept
{
    std::string safe;
    for (const auto c : acKey)
        if (std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-')
            safe += c;
        else
            safe += fmt::format("~{:02X}", static_cast<unsigned char>(c));
    return SavesDirectory() / ("carryhome_" + safe + ".snap");
}

String s_pendingId;
int64_t s_pendingSinceMs{};
int64_t s_pendingNextScanMs{};
std::atomic<bool> s_saveQueued{false};
std::mutex s_importSaveNameLock;
fs::path s_importExpectedSave;
std::atomic<bool> s_importAwaitingName{false};

int64_t NowMs() noexcept
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count();
}

// BGSSaveLoadManager::Save_Impl(deviceId, outputStats, name) (ID 35727): every save, manual,
// quick, auto or queued, ends here. Only observed, never called directly.
TP_THIS_FUNCTION(TSaveImpl, bool, void, int32_t, uint32_t, const char*);
TSaveImpl* RealSaveImpl = nullptr;
using TQueueSaveLoadTask = void(void*, uint32_t, uint64_t, uint64_t);
TQueueSaveLoadTask* RealQueueSaveLoadTask = nullptr;
using TQuitToMainMenu = void();
TQuitToMainMenu* RealQuitToMainMenu = nullptr;
std::atomic<bool> s_quitToMenuQueued{false};

// Save_Impl (35727) calls this naming path (35749) before it queues a kind-5 save operation. The name buffer is
// Save_Impl's own char name (a 260-byte buffer, 0x1406227F0), not wide text.
using TMakeSaveName = void(void*, char*, int32_t);
TMakeSaveName* RealMakeSaveName{};

void TP_MAKE_THISCALL(HookMakeSaveName, void, char* apName, int32_t aKind)
{
    TiltedPhoques::ThisCall(RealMakeSaveName, apThis, apName, aKind);
    if (aKind == 5 && apName && *apName &&
        GetCurrentThreadId() == s_mainLoopThread.load(std::memory_order_acquire) &&
        s_importAwaitingName.exchange(false, std::memory_order_acq_rel))
    {
        std::lock_guard lock(s_importSaveNameLock);
        const auto named = SavesDirectory() / fs::u8path(std::string(apName) + ".ess");
        std::error_code error;
        if (!fs::exists(named, error) && !error)
        {
            s_importExpectedSave = named;
            spdlog::info("Carry-home save will be {}", apName);
        }
        else
            spdlog::warn("Carry-home save name {} already exists or could not be checked", apName);
    }
}

TiltedPhoques::Initializer s_saveNameHook(
    []()
    {
        POINTER_SKYRIMSE(TMakeSaveName, s_makeSaveName, 35749);
        RealMakeSaveName = s_makeSaveName.Get();
        TP_HOOK(&RealMakeSaveName, HookMakeSaveName);
    });

void HookQuitToMainMenu()
{
    if (entt::locator<World>::has_value())
    {
        auto& party = World::Get().GetPartyService();
        if (party.IsInParty() && party.GetSessionState() >= 2)
        {
            if (GetCurrentThreadId() != s_mainLoopThread.load(std::memory_order_acquire))
            {
                s_quitToMenuQueued = true;
                return;
            }
            if (party.BeginHostDeparture(true))
                return;
            CheckpointSaves::WriteCarryHome();
        }
    }
    LobbyService::RequestReturnToLobby("quit_to_menu");
    RealQuitToMainMenu();
}
std::atomic<bool> s_deathReloadBlocked{false};

void TP_MAKE_THISCALL(HookQueueSaveLoadTask, void, uint32_t aTask, uint64_t aArg3, uint64_t aArg4)
{
    // 0xD0000100 is "load the last save": the death reload, but also the main menu's Continue (owner 2026-10-02: Continue
    // Together from the lobby was taken for a death reload and blocked; the host sat on black for 15 s). Only a dead
    // player's reload is the co-op respawn's to replace.
    auto* pPlayer = PlayerCharacter::Get();
    const bool playerDead = pPlayer && pPlayer->actorState.IsDeadState() && !LobbyService::InLobbyWorld() &&
                            !IsSharedCampaignLaunching();
    if (aTask == 0xD0000100u && playerDead && entt::locator<World>::has_value() && World::Get().GetTransport().IsConnected())
    {
        s_deathReloadBlocked.store(true, std::memory_order_release);
        spdlog::info("Blocked native death reload while connected; co-op respawn queued");
        return;
    }
    TiltedPhoques::ThisCall(RealQueueSaveLoadTask, apThis, aTask, aArg3, aArg4);
}

// A save started off the main thread is handed to the engine's save queue. A menu save ran Save_Impl on a job thread
// (Scaleform ExternalInterface), whose screenshot (0x140665D30) waited for Renderer::Lock while the main thread held
// that lock in its pre-render pass (0x1406565B0) waiting for the job stage: the host froze on a manual save
// (2026-09-30 19:02:39; dump host-save-hang.dmp). The queued task (ID 35769, as Game.RequestSave) is the path the
// checkpoints already use; the next Save_Impl after queuing is that task and runs normally.
// Set by every completed save: the next main frame writes the character snapshot beside the new save file.
std::atomic<bool> s_snapshotAfterSave{false};
std::mutex s_saveChangesLock;
std::unordered_map<uint32_t, uint32_t> s_saveChanges;
std::unordered_set<uint32_t> s_saveErases;
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
    if (entt::locator<World>::has_value())
    {
        auto& world = World::Get();
        if (world.GetTransport().IsConnected() && world.GetPartyService().IsInParty() &&
            !world.GetPartyService().IsLeader() && !(aDevice == 5 && s_saveQueued))
            return false;
    }
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
        s_saveSerial.fetch_add(1, std::memory_order_release);
        s_snapshotAfterSave.store(true, std::memory_order_release);
        s_lastSaveDoneMs.store(NowMs(), std::memory_order_release);
        if (!s_saveQueued)
            s_localSaveMade.store(true, std::memory_order_release);
    }
    else if (aDevice == 5 && s_saveQueued)
    {
        // Kinds 3 and 5 refuse silently (Save_Impl 35727): say so now instead of at Poll's 120 s timeout (Atlas QF-5).
        // The queued task runs here on the main thread, where Poll also runs.
        spdlog::warn("Checkpoint {}: the queued save was refused", s_pendingId);
        s_pendingId.clear();
        s_saveQueued = false;
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
std::string PlaythroughKey(const fs::path& acPath, const std::string& acPlayer) noexcept;
namespace
{
struct SaveHeader
{
    std::string Player, Location, GameDate;
    uint32_t Level{};
    std::string Image;
};
bool ReadSaveHeader(const fs::path& acPath, SaveHeader& aHeader, bool aImage) noexcept;
}

bool TakeLocalSaveMade() noexcept
{
    return s_localSaveMade.exchange(false, std::memory_order_acq_rel);
}

String NewCheckpointId(const uint64_t aEpoch) noexcept
{
    const auto clockStamp = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    static std::atomic<uint64_t> lastStamp{};
    uint64_t previous = lastStamp.load(std::memory_order_relaxed);
    uint64_t stamp{};
    do
    {
        stamp = std::max<uint64_t>(static_cast<uint64_t>(clockStamp), previous + 1);
    } while (!lastStamp.compare_exchange_weak(previous, stamp, std::memory_order_relaxed));
    return fmt::format("{:08x}_{}", static_cast<uint32_t>(aEpoch ^ (aEpoch >> 32)), stamp).c_str();
}

void Begin(const String& acId, bool aReuseOwnRecentSave) noexcept
{
    s_pendingId = acId;
    s_pendingNextScanMs = 0;
    const auto now = NowMs();
    const auto lastSave = s_lastSaveDoneMs.load(std::memory_order_acquire);
    // Only the leader's own save, the one that announced the checkpoint, may stand in for it. Every peer autosaves at a
    // load door (SkyrimAtlas S5), and a follower reusing that pre-settle autosave got the state the 5 s settle wait
    // exists to avoid (atlas audit finding 21).
    if (aReuseOwnRecentSave && lastSave && now - lastSave < 30000)
    {
        // The leader's own save is the checkpoint.
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

// Every save load (Load_Impl 35728: Continue, the Load screen, the console, the mod's own loads) by file name. Once its
// loading screen has come and gone, a save without a character file gets one (owner 2026-10-02: saves from before the
// mod wrote them showed the wrong clothes at the lobby; their first play now records the real character).
std::mutex s_loadedSaveLock;
std::string s_loadedSave;
int s_loadedSaveState{}; // 1 requested, 2 its loading screen seen
uint64_t s_loadedSaveAt{};
fs::path s_importPath;
fs::path s_importLoadedSave;
std::string s_importKey;
int s_importPhase{}; // 1 applying, 2 waiting for the native save
int64_t s_importSaveSinceMs{};
uint64_t s_importSaveSerial{};
uintmax_t s_importLastSize{};
int64_t s_importSizeSinceMs{};
int64_t s_importAppliedMs{};  // when the apply finished (the save request waits for the world to settle)
int s_importAttempts{};
TP_THIS_FUNCTION(TLoadImplHook, bool, void, const char*, int32_t, uint32_t, bool);
TLoadImplHook* RealLoadImpl{};

bool TP_MAKE_THISCALL(HookLoadImpl, void, const char* apName, int32_t aDevice, uint32_t aFlags, bool aCheck)
{
    s_importPhase = 0;
    s_importAwaitingName.store(false, std::memory_order_release);
    if (apName && *apName)
    {
        s_loadGeneration.fetch_add(1, std::memory_order_acq_rel);
        std::lock_guard lock(s_loadedSaveLock);
        s_loadedSave = apName;
        s_loadedSaveState = 1;
        s_loadedSaveAt = GetTickCount64();
    }
    const bool started = TiltedPhoques::ThisCall(RealLoadImpl, apThis, apName, aDevice, aFlags, aCheck);
    if (!started)
        OnSaveLoadHandled(false);
    return started;
}

TiltedPhoques::Initializer s_loadImplHook(
    []()
    {
        POINTER_SKYRIMSE(TLoadImplHook, s_loadImpl, 35728);
        RealLoadImpl = s_loadImpl.Get();
        TP_HOOK(&RealLoadImpl, HookLoadImpl);
    });

void OnSaveLoadHandled(bool aLoaded) noexcept
{
    std::lock_guard lock(s_loadedSaveLock);
    if (!aLoaded)
    {
        s_loadedSave.clear();
        s_loadedSaveState = 0;
        return;
    }
    spdlog::info("Save load confirmed (state {}, {})", s_loadedSaveState, s_loadedSave);
    if (s_loadedSaveState == 1)
        s_loadedSaveState = 2;
}

void WriteCharacterAfterLoad() noexcept
{
    std::string name;
    {
        std::lock_guard lock(s_loadedSaveLock);
        if (!s_loadedSaveState)
            return;
        if (GetTickCount64() - s_loadedSaveAt > 180000)
        {
            s_loadedSaveState = 0;
            return;
        }
        auto* pUI = UI::Get();
        const bool loading = pUI && pUI->GetMenuOpen(BSFixedString("Loading Menu"));
        if (s_loadedSaveState == 1)
            return;
        if (loading)
            return;
        auto* pPlayer = PlayerCharacter::Get();
        if (!pPlayer || !pPlayer->parentCell || !pPlayer->parentCell->IsAttached() || !pPlayer->GetNiNode() ||
            (pUI && pUI->GetMenuOpen(BSFixedString("Main Menu"))) || GetTickCount64() - s_loadedSaveAt < 1000)
            return;
        // The lobby ends on the frame after the load; wait for it rather than dropping the load.
        if (LobbyService::InLobbyWorld())
            return;
        s_loadedSaveState = 0;
        name = s_loadedSave;
    }
    auto save = SavesDirectory() / name;
    if (save.extension() != ".ess")
        save += ".ess";
    auto snap = std::filesystem::path(save).replace_extension(".snap");
    std::error_code error;
    if (!fs::exists(save, error))
        return;
    s_importPhase = 0;
    const auto inSharedSession = entt::locator<World>::has_value() && World::Get().GetPartyService().IsInParty() &&
        (World::Get().GetPartyService().GetSessionState() >= 1 ||
            World::Get().GetPartyService().GetPartyMembers().size() > 1);
    if (save.stem().string().rfind(kPrefix, 0) != 0 && !inSharedSession)
    {
        SaveHeader header;
        if (ReadSaveHeader(save, header, false))
        {
            const auto key = PlaythroughKey(save, header.Player);
            const auto carry = CarryHomePath(key);
            spdlog::info("After load: key {}, carry-home {} ({} ms) vs save {} ms", key, carry.filename().string(),
                FileTimeMs(carry), FileTimeMs(save));
            CharacterSnapshot brought;
            if (FileTimeMs(carry) > FileTimeMs(save) && CharacterSnapshots::ReadFile(carry, brought) &&
                std::string(brought.Name.c_str()) == header.Player)
            {
                s_importPath = carry;
                s_importLoadedSave = save;
                s_importKey = key;
                s_importPhase = 1;
                s_importAppliedMs = 0;
                s_importAttempts = 0;
                CharacterSnapshots::QueueApply(std::move(brought));
                spdlog::info("Carry-home {} queued after loading {}", carry.filename().string(), save.filename().string());
                return;
            }
        }
    }
    if (fs::exists(snap, error))
        return;
    spdlog::info("Loaded {} has no character file: writing one", save.filename().string());
    CharacterSnapshots::QueueCaptureTo(snap);
}

bool RequestNativeSave() noexcept
{
    auto* pManager = GetSaveLoadManager();
    if (!pManager)
        return false;
    using TQueueTask = void(void*, uint32_t);
    POINTER_SKYRIMSE(TQueueTask, s_queueTask, 35769);
    s_queueTask.Get()(pManager, 0xF0000080u);
    return true;
}

void PollCarryImport() noexcept
{
    if (!s_importPhase)
        return;
    if (entt::locator<World>::has_value())
    {
        const auto& party = World::Get().GetPartyService();
        if (party.IsInParty() && (party.GetSessionState() >= 1 || party.GetPartyMembers().size() > 1))
        {
            spdlog::warn("Carry-home import interrupted by a shared session; record remains for the next solo load");
            s_importPhase = 0;
            s_importAwaitingName.store(false, std::memory_order_release);
            return;
        }
    }
    if (s_importPhase == 1)
    {
        const auto status = CharacterSnapshots::ApplyStatus();
        if (status.rfind("failed", 0) == 0)
        {
            spdlog::error("Carry-home apply failed: {}", status);
            s_importPhase = 0;
            return;
        }
        if (status.rfind("done", 0) != 0)
            return;
        // A save requested right after a load is dropped by the engine without a word (2026-10-02 19:41): let the
        // world settle first, and ask again below if no save follows.
        if (!s_importAppliedMs)
            s_importAppliedMs = NowMs();
        if (NowMs() - s_importAppliedMs < 3000)
            return;
        auto* pManager = GetSaveLoadManager();
        if (!pManager)
            return;
        using TQueueTask = void(void*, uint32_t);
        POINTER_SKYRIMSE(TQueueTask, s_queueTask, 35769);
        s_importSaveSinceMs = NowMs();
        s_importSaveSerial = s_saveSerial.load(std::memory_order_acquire);
        s_importLastSize = 0;
        s_importSizeSinceMs = 0;
        {
            std::lock_guard lock(s_importSaveNameLock);
            s_importExpectedSave.clear();
        }
        s_importAwaitingName.store(true, std::memory_order_release);
        s_importPhase = 2;
        s_importAttempts = 1;
        s_queueTask.Get()(pManager, 0xF0000080u);
        spdlog::info("Carry-home applied; requested a new native save for {}", s_importKey);
        return;
    }
    if (NowMs() - s_importSaveSinceMs > 120000)
    {
        spdlog::error("Carry-home save timed out; {} remains for retry", s_importPath.filename().string());
        s_importPhase = 0;
        s_importAwaitingName.store(false, std::memory_order_release);
        return;
    }
    if (s_saveSerial.load(std::memory_order_acquire) <= s_importSaveSerial)
    {
        if (NowMs() - s_importSaveSinceMs > 10000 * s_importAttempts && s_importAttempts < 3)
        {
            if (auto* pManager = GetSaveLoadManager())
            {
                using TQueueTask = void(void*, uint32_t);
                POINTER_SKYRIMSE(TQueueTask, s_queueTask, 35769);
                ++s_importAttempts;
                s_importAwaitingName.store(true, std::memory_order_release);
                s_queueTask.Get()(pManager, 0xF0000080u);
                spdlog::warn("Carry-home: no save followed the request; asking again (attempt {})", s_importAttempts);
            }
        }
        return;
    }
    fs::path saved;
    {
        std::lock_guard lock(s_importSaveNameLock);
        saved = s_importExpectedSave;
    }
    if (saved.empty() || saved == s_importLoadedSave || FileTimeMs(saved) < s_importSaveSinceMs)
    {
        static int64_t s_lastWhy{};
        if (NowMs() - s_lastWhy > 5000)
        {
            s_lastWhy = NowMs();
            spdlog::info("Carry-home waits: expected '{}', file {} ms, since {} ms", saved.filename().string(),
                saved.empty() ? 0 : FileTimeMs(saved), s_importSaveSinceMs);
        }
        return;
    }
    std::error_code sizeError;
    const auto size = fs::file_size(saved, sizeError);
    if (sizeError || size == 0)
        return;
    if (size != s_importLastSize)
    {
        s_importLastSize = size;
        s_importSizeSinceMs = NowMs();
        return;
    }
    if (NowMs() - s_importSizeSinceMs < 500)
        return;
    SaveHeader header;
    if (!ReadSaveHeader(saved, header, false) || PlaythroughKey(saved, header.Player) != s_importKey)
    {
        spdlog::info("Carry-home: {} is not playthrough {} (player '{}')", saved.filename().string(), s_importKey, header.Player);
        return;
    }
    auto applied = s_importPath;
    applied.replace_extension(".applied.snap");
    if (!MoveFileExW(s_importPath.c_str(), applied.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
    {
        spdlog::error("Carry-home applied but {} was not retired (Windows error {})", s_importPath.string(), GetLastError());
        s_importPhase = 0;
        return;
    }
    using TShowHUDMessage = void(const char*, const char*, bool);
    POINTER_SKYRIMSE(TShowHUDMessage, showMessage, 52933);
    showMessage.Get()("Your journey with your companions has been carried home.", nullptr, true);
    spdlog::info("Carry-home {} committed as new save {}", applied.filename().string(), saved.filename().string());
    s_importPhase = 0;
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

static bool DrainQueuedQuit() noexcept
{
    if (!s_quitToMenuQueued.exchange(false, std::memory_order_acq_rel))
        return false;
    HookQuitToMainMenu();
    return true;
}

static void DrainDeferredSave() noexcept
{
    std::optional<DeferredSave> save;
    {
        std::lock_guard lock(s_deferredSaveLock);
        save.swap(s_deferredSave);
    }
    if (save)
    {
        const bool saved = HookSaveImpl(save->Manager, save->Kind, save->OutputStats,
            save->Name.empty() ? nullptr : save->Name.c_str());
        spdlog::info("Deferred save {} (kind {}) on the main thread: {}", save->Name, save->Kind, saved ? "saved" : "FAILED");
    }
}

void MarkMainThread() noexcept
{
    s_mainLoopThread.store(GetCurrentThreadId(), std::memory_order_release);
    if (s_carryWriteQueued.exchange(false, std::memory_order_acq_rel) &&
        s_carryQueuedGeneration.load(std::memory_order_acquire) == s_loadGeneration.load(std::memory_order_acquire))
        WriteCarryHome();
    if (!MT::CutEnabled(6))
    {
        DrainQueuedQuit();
        DrainRespawnOnMainFrame();
        DrainSaveWorkOnMainFrame();
        DrainTerminalOnMainFrame();
        DrainDeferredSave();
    }
}

void DrainRespawnOnMainFrame() noexcept
{
    if (MT::CutEnabled(6) && MT::CurrentSlot() != MT::Slot::S3_3)
    {
        MT::M6WrongSlot.fetch_add(1, std::memory_order_relaxed);
        MT::Report();
    }
    if (s_deathReloadBlocked.exchange(false, std::memory_order_acq_rel) &&
        entt::locator<World>::has_value() && World::Get().GetTransport().IsConnected())
    {
        if (auto* pPlayer = PlayerCharacter::Get(); pPlayer && pPlayer->actorState.IsDeadState())
        {
            if (auto* pCell = pPlayer->GetParentCellEx(); pCell && pCell->IsAttached())
            {
                pPlayer->RespawnPlayerAt(pCell, pPlayer->position);
                World::Get().GetTransport().Send(PlayerRespawnRequest());
                spdlog::info("Native death routed to co-op respawn");
            }
            else
                s_deathReloadBlocked.store(true, std::memory_order_release);
        }
    }
}

void DrainSaveWorkOnMainFrame() noexcept
{
    if (MT::CutEnabled(6) && MT::CurrentSlot() != MT::Slot::S5s)
    {
        MT::M6WrongSlot.fetch_add(1, std::memory_order_relaxed);
        MT::Report();
    }
    // Only changed forms are visited. The save loader rejects AddChange while loading.
    POINTER_SKYRIMSE(uint8_t, loadFlagForChanges, 403444);
    if (*loadFlagForChanges.Get() != 0)
    {
        std::lock_guard lock(s_saveChangesLock);
        s_saveChanges.clear();
        s_saveErases.clear();
    }
    else
    {
        std::unordered_map<uint32_t, uint32_t> changes;
        std::unordered_set<uint32_t> erases;
        {
            std::lock_guard lock(s_saveChangesLock);
            changes.swap(s_saveChanges);
            erases.swap(s_saveErases);
        }
        if (!erases.empty())
        {
            POINTER_SKYRIMSE(void*, saveLoadGame, 403330);
            using TEraseChange = void(void*, TESForm*);
            POINTER_SKYRIMSE(TEraseChange, eraseChange, 35583);
            if (auto* pGame = *saveLoadGame.Get())
                for (const auto formId : erases)
                    if (auto* pForm = TESForm::GetById(formId); pForm && (pForm->flags & 0x4000))
                        TiltedPhoques::ThisCall(eraseChange.Get(), pGame, pForm);
        }
        for (const auto& [formId, flags] : changes)
            if (auto* pForm = TESForm::GetById(formId))
            {
                auto* pActor = Cast<Actor>(pForm);
                if (!pActor || !pActor->GetExtension() || !pActor->GetExtension()->IsRemotePlayer())
                    pForm->MarkChanged(flags);
            }
    }
    // Every save made with the mod carries its character (<save>.snap), so the join picker never loads a save.
    if (s_snapshotAfterSave.exchange(false, std::memory_order_acq_rel))
    {
        const auto save = NewestSaveSince(s_lastSaveDoneMs.load(std::memory_order_acquire) - 10000);
        if (!save.empty())
            CharacterSnapshots::QueueCaptureTo(std::filesystem::path(save).replace_extension(".snap"));
    }
    WriteCharacterAfterLoad();
    PollCarryImport();
    if (MT::CutEnabled(6))
        DrainDeferredSave();
}

bool HasPendingTerminal() noexcept
{
    if (s_quitToMenuQueued.load(std::memory_order_acquire))
        return true;
    std::lock_guard lock(s_queuedLoadLock);
    return !s_queuedLoad.empty();
}

bool HasPendingQuit() noexcept
{
    return s_quitToMenuQueued.load(std::memory_order_acquire);
}

void DrainTerminalOnMainFrame() noexcept
{
    if (MT::CutEnabled(6) && MT::CurrentSlot() != MT::Slot::S7)
    {
        MT::M6WrongSlot.fetch_add(1, std::memory_order_relaxed);
        MT::Report();
    }
    if (DrainQueuedQuit())
        return;
    String load;
    {
        std::lock_guard lock(s_queuedLoadLock);
        load = std::exchange(s_queuedLoad, String{});
    }
    if (!load.empty())
    {
        // "file:<name>": any save in the folder by its file name (without .ess), else the checkpoint SSC_<id>.
        const std::string text = load.c_str();
        if (text.rfind("file:", 0) == 0)
            s_queuedLoadState = LoadFile(SavesDirectory() / (text.substr(5) + ".ess")) ? 1 : -1;
        else
            s_queuedLoadState = Load(load) ? 1 : -1;
    }
}

void QueueSaveChange(uint32_t aFormId, uint32_t aFlags) noexcept
{
    if (!aFormId || !aFlags)
        return;
    std::lock_guard lock(s_saveChangesLock);
    s_saveChanges[aFormId] |= aFlags;
}

void QueueSaveErase(uint32_t aFormId) noexcept
{
    if (!aFormId)
        return;
    std::lock_guard lock(s_saveChangesLock);
    s_saveErases.insert(aFormId);
}

bool Poll() noexcept
{
    if (s_pendingId.empty())
        return true;
    const auto now = NowMs();
    if (now < s_pendingNextScanMs)
        return false;
    s_pendingNextScanMs = now + 500;
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
    const bool follower = entt::locator<World>::has_value() && World::Get().GetPartyService().IsInParty() &&
        !World::Get().GetPartyService().IsLeader();
    auto snapshot = source;
    snapshot.replace_extension(".snap");
    if (follower && !fs::exists(snapshot) && NowMs() - s_pendingSinceMs < 30000)
        return false; // the next main frame still owes this save its character file
    const auto transfer = [&](const fs::path& acFrom, const fs::path& acTo)
    {
        if (follower)
        {
            if (!MoveFileExW(acFrom.c_str(), acTo.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
                error = std::error_code(static_cast<int>(GetLastError()), std::system_category());
        }
        else
            fs::copy_file(acFrom, acTo, fs::copy_options::overwrite_existing, error);
    };
    auto coSave = source;
    coSave.replace_extension(".skse");
    if (follower && fs::exists(coSave))
    {
        auto coTarget = target;
        coTarget.replace_extension(".skse");
        transfer(coSave, coTarget);
    }
    // The character written with the save travels with the checkpoint.
    if (follower && !error && fs::exists(snapshot))
    {
        auto snapshotTarget = target;
        snapshotTarget.replace_extension(".snap");
        transfer(snapshot, snapshotTarget);
    }
    if (!error)
        transfer(source, target);
    if (!follower && !error && fs::exists(coSave))
    {
        auto coTarget = target;
        coTarget.replace_extension(".skse");
        transfer(coSave, coTarget);
    }
    if (!follower && !error && fs::exists(snapshot))
    {
        auto snapshotTarget = target;
        snapshotTarget.replace_extension(".snap");
        transfer(snapshot, snapshotTarget);
    }
    if (error)
    {
        spdlog::error("Checkpoint save {}: copying {} failed: {}", target.filename().string(), source.filename().string(), error.message());
        return false;
    }
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
    const auto available = Available(1, false);
    return available.empty() ? String{} : available.front();
}

Vector<String> Available(size_t aMaximum, bool aRequireRoster) noexcept
{
    // The newest checkpoint whose save is still on this PC: a deleted one (the owner's purge of test checkpoints,
    // 2026-10-02) left Continue Together falling back to this PC's newest save of any kind.
    std::ifstream registry(RegistryPath());
    std::string line;
    Vector<String> available;
    std::error_code error;
    while (std::getline(registry, line))
    {
        if (line.empty())
            continue;
        const auto id = line.substr(0, line.find('\t'));
        // A party's own checkpoints only (<party>_<time>, digits): named test checkpoints (helgen_street2) are
        // registered too, and one was picked as the campaign to continue.
        const auto underscore = id.find('_');
        const bool partyCheckpoint = underscore != std::string::npos && underscore > 0 && underscore + 1 < id.size() &&
            id.find_first_not_of("0123456789abcdefABCDEF", 0) >= underscore &&
            id.find_first_not_of("0123456789", underscore + 1) == std::string::npos;
        auto roster = PathOf(String(id.c_str()));
        roster.replace_extension(".roster");
        if (partyCheckpoint && fs::exists(PathOf(String(id.c_str())), error) &&
            (!aRequireRoster || fs::exists(roster, error)))
        {
            const String saved(id.c_str());
            available.erase(std::remove(available.begin(), available.end(), saved), available.end());
            available.push_back(saved);
        }
    }
    std::reverse(available.begin(), available.end());
    if (available.size() > aMaximum) available.resize(aMaximum);
    return available;
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
bool ReadSaveHeader(const fs::path& acPath, SaveHeader& aHeader, bool aImage = true) noexcept
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
    if (!aImage)
        return true;
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
        if (entry.path().filename().string().ends_with(".applied.snap"))
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
        if (item.Snap.filename().string().rfind("carryhome_", 0) == 0)
            for (const auto& character : SavedCharacters(200))
                if (character.Snap == item.Snap)
                {
                    save = character.Save;
                    break;
                }
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

// Not a character to pick or delete: a join's drop-in copy (someone else's world), or a named test checkpoint
// (SSC_helgen_street, SSC_seed160: the scenario tests load them). Co-op saves are SSC_<digits>_<digits>.
bool IsTestOrDropIn(const fs::path& acPath) noexcept
{
    const auto name = acPath.filename().string();
    if (name.rfind(kPrefix, 0) != 0)
        return false;
    const auto stem = acPath.stem().string().substr(sizeof(kPrefix) - 1);
    return stem.empty() || stem.find_first_not_of("0123456789_") != std::string::npos;
}

// Ordinary saves use the 8 hex digits after the kind, or a legacy character name. SSC checkpoints have a separate key space.
std::string PlaythroughKey(const fs::path& acPath, const std::string& acPlayer) noexcept
{
    const auto stem = acPath.stem().string();
    if (stem.rfind(kPrefix, 0) == 0)
        return "ssc:name:" + acPlayer;
    {
        const auto first = stem.find('_');
        const auto second = first == std::string::npos ? std::string::npos : stem.find('_', first + 1);
        if (second != std::string::npos && second - first - 1 == 8)
        {
            auto field = stem.substr(first + 1, 8);
            if (field.find_first_not_of("0123456789abcdefABCDEF") == std::string::npos)
            {
                for (auto& c : field)
                    c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
                return field;
            }
        }
    }
    return "name:" + acPlayer;
}

std::vector<SavedCharacter> SavedCharacters(size_t aMaximum) noexcept
{
    std::map<std::string, SavedCharacter> byKey;
    std::map<std::string, std::pair<fs::path, int64_t>> snaps;
    std::error_code error;
    for (const auto& entry : fs::directory_iterator(SavesDirectory(), error))
    {
        const auto& path = entry.path();
        if (error || !entry.is_regular_file() || IsTestOrDropIn(path))
            continue;
        const auto ms = FileTimeMs(path);
        if (path.extension() == ".snap")
        {
            if (path.filename().string().rfind("carryhome_", 0) == 0)
                continue;
            CharacterSnapshot character;
            if (CharacterSnapshots::ReadFile(path, character) && !character.Name.empty())
            {
                auto& best = snaps[PlaythroughKey(path, character.Name.c_str())];
                if (ms > best.second)
                    best = {path, ms};
            }
            continue;
        }
        if (path.extension() != ".ess")
            continue;
        SaveHeader header;
        if (!ReadSaveHeader(path, header, false) || header.Player.empty())
            continue;
        const auto key = PlaythroughKey(path, header.Player);
        auto& item = byKey[key];
        if (ms <= item.SavedMs)
            continue;
        const uint32_t id = key.size() == 8 ? static_cast<uint32_t>(std::stoul(key, nullptr, 16)) : 0u;
        item = SavedCharacter{key, id, header.Player, header.Level, header.Location, path, {}, path.stem().string().rfind(kPrefix, 0) == 0, ms};
    }
    std::vector<SavedCharacter> result;
    for (auto& [key, item] : byKey)
    {
        auto own = item.Save;
        own.replace_extension(".snap");
        if (fs::exists(own, error))
            item.Snap = own;
        else if (const auto it = snaps.find(key); it != snaps.end())
            item.Snap = it->second.first;
        if (!item.Coop)
        {
            const auto carry = CarryHomePath(key);
            CharacterSnapshot brought;
            if (FileTimeMs(carry) > item.SavedMs && CharacterSnapshots::ReadFile(carry, brought) &&
                std::string(brought.Name.c_str()) == item.Name)
            {
                item.Snap = carry;
                item.Level = brought.Level;
            }
        }
        result.push_back(std::move(item));
    }
    std::sort(result.begin(), result.end(), [](const SavedCharacter& a, const SavedCharacter& b) { return a.SavedMs > b.SavedMs; });
    if (result.size() > aMaximum)
        result.resize(aMaximum);
    return result;
}

void SetCarryHomeSource(const std::string& acKey) noexcept
{
    {
        std::lock_guard lock(s_carrySourceLock);
        s_carryKey.clear();
        s_carryName.clear();
    }
    if (acKey.empty())
        return;
    for (const auto& character : SavedCharacters(200))
        if (character.Key == acKey && !character.Coop && !character.Save.empty())
        {
            {
                std::lock_guard lock(s_carrySourceLock);
                s_carryKey = character.Key;
                s_carryName = character.Name;
            }
            spdlog::info("Carry-home source: {} ({})", character.Name, character.Key);
            return;
        }
    spdlog::warn("Carry-home source {} has no ordinary playthrough", acKey);
}

void SetCarryHomeSourceFromSnapshot(const fs::path& acSnapshot) noexcept
{
    SetCarryHomeSource({});
    std::error_code error;
    const auto source = fs::weakly_canonical(acSnapshot, error);
    if (error)
        return;
    for (const auto& character : SavedCharacters(200))
        if (!character.Coop && !character.Snap.empty() && fs::equivalent(source, character.Snap, error) && !error)
        {
            SetCarryHomeSource(character.Key);
            return;
        }
    spdlog::warn("Drop-in character {} has no ordinary playthrough for carry-home", acSnapshot.string());
}

std::string CharacterKeyForSnapshot(const fs::path& acSnapshot) noexcept
{
    std::error_code error;
    const auto source = fs::weakly_canonical(acSnapshot, error);
    if (error) return {};
    for (const auto& character : SavedCharacters(200))
        if (!character.Coop && !character.Snap.empty() && fs::equivalent(source, character.Snap, error) && !error)
            return character.Key;
    return {};
}

std::string CurrentCharacterKey() noexcept
{
    {
        std::lock_guard lock(s_carrySourceLock);
        if (!s_carryKey.empty()) return s_carryKey;
    }
    // MakeSaveName (35749 / 140623DA0) formats the active manager ID at +D0 as %08X.
    // The newest file on disk can belong to a different character or have a changed display name.
    if (const auto* manager = GetSaveLoadManager())
    {
        const auto id = *reinterpret_cast<const uint32_t*>(static_cast<const uint8_t*>(manager) + 0xD0);
        if (id && id != UINT32_MAX) return fmt::format("{:08X}", id);
    }
    return {};
}

bool WriteRoster(const String& acId, const Vector<RosterPosition>& acEntries) noexcept
{
    if (!ValidId(acId) || acEntries.size() > 16) return false;
    auto path = PathOf(acId);
    path.replace_extension(".roster");
    auto temporary = path;
    temporary += ".tmp";
    std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out << "{\"version\":1,\"checkpoint\":\"" << JsonText(acId.c_str()) << "\",\"players\":[";
    for (size_t i = 0; i < acEntries.size(); ++i)
    {
        const auto& entry = acEntries[i];
        if (!entry.Valid()) return false;
        if (i) out << ',';
        out << fmt::format("{{\"steamId\":\"{}\",\"key\":\"{}\",\"name\":\"{}\","
            "\"world\":[{},{}],\"cell\":[{},{}],\"position\":[{:.6f},{:.6f},{:.6f}],"
            "\"heading\":{:.6f},\"downed\":{}}}", entry.SteamId, JsonText(entry.CharacterKey.c_str()),
            JsonText(entry.CharacterName.c_str()), entry.WorldSpaceId.ModId, entry.WorldSpaceId.BaseId,
            entry.CellId.ModId, entry.CellId.BaseId, entry.X, entry.Y, entry.Z, entry.Heading,
            entry.Downed ? "true" : "false");
    }
    out << "]}";
    out.close();
    if (!out) return false;
    if (!MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
    {
        spdlog::error("Checkpoint roster {} could not be replaced (Windows error {})", path.string(), GetLastError());
        return false;
    }
    return true;
}

std::string RosterJson(const String& acId) noexcept
{
    if (!ValidId(acId)) return "{\"players\":[]}";
    auto path = PathOf(acId);
    path.replace_extension(".roster");
    std::ifstream in(path, std::ios::binary);
    if (!in) return "{\"players\":[]}";
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

bool WriteCarryHome() noexcept
{
    std::string key, name;
    {
        std::lock_guard lock(s_carrySourceLock);
        key = s_carryKey;
        name = s_carryName;
    }
    if (key.empty() || LobbyService::InLobbyWorld())
        return false;
    if (GetCurrentThreadId() != s_mainLoopThread.load(std::memory_order_acquire))
    {
        s_carryQueuedGeneration = s_loadGeneration.load(std::memory_order_acquire);
        s_carryWriteQueued = true;
        return true;
    }
    CharacterSnapshot snapshot;
    std::string error;
    if (!CharacterSnapshots::Capture(snapshot, error))
    {
        spdlog::warn("Carry-home capture skipped: {}", error);
        return false;
    }
    if (std::string(snapshot.Name.c_str()) != name)
    {
        spdlog::error("Carry-home refused: live character {} is not chosen {}", snapshot.Name.c_str(), name);
        return false;
    }
    const auto path = CarryHomePath(key);
    if (!CharacterSnapshots::WriteFile(path, snapshot))
    {
        spdlog::error("Carry-home {} not written (Windows error {})", path.string(), GetLastError());
        return false;
    }
    spdlog::info("Carry-home written: {} ({} level {}, {} item stacks, {} quest items excluded)", path.string(),
        snapshot.Name.c_str(), snapshot.Level, snapshot.Items.Entries.size(), snapshot.ExcludedQuestItems.size());
    return true;
}

bool CarryHomeReady() noexcept
{
    std::string key;
    {
        std::lock_guard lock(s_carrySourceLock);
        key = s_carryKey;
    }
    if (key.empty()) return false;
    return WriteCarryHome();
}

std::string CarryHomeStatus() noexcept
{
    std::string key;
    if (LobbyService::InLobbyWorld())
        key = LobbyService::ChosenOwnPlaythroughKey();
    else
    {
        std::lock_guard lock(s_carrySourceLock);
        key = s_carryKey;
    }
    const auto path = key.empty() ? fs::path{} : CarryHomePath(key);
    CharacterSnapshot snapshot;
    const bool exists = !path.empty() && CharacterSnapshots::ReadFile(path, snapshot);
    return fmt::format("\"path\":\"{}\",\"timeMs\":{},\"character\":\"{}\",\"level\":{},\"items\":{},\"exists\":{}",
        JsonText(path.generic_string()), path.empty() ? 0 : FileTimeMs(path), exists ? JsonText(snapshot.Name.c_str()) : "",
        exists ? snapshot.Level : 0, exists ? snapshot.Items.Entries.size() : 0, exists ? "true" : "false");
}

size_t DeleteCharacterSaves(const std::string& acKey) noexcept
{
    if (acKey.empty())
        return 0;
    std::wstring list; // SHFileOperation's double-null-terminated list
    size_t saves = 0;
    std::error_code error;
    for (const auto& entry : fs::directory_iterator(SavesDirectory(), error))
    {
        const auto& path = entry.path();
        if (error || !entry.is_regular_file() || path.extension() != ".ess" || IsTestOrDropIn(path))
            continue;
        SaveHeader header;
        if (!ReadSaveHeader(path, header, false) || PlaythroughKey(path, header.Player) != acKey)
            continue;
        ++saves;
        for (const char* extension : {".ess", ".skse", ".snap"})
        {
            auto file = path;
            file.replace_extension(extension);
            if (fs::exists(file, error))
            {
                list += file.wstring();
                list.push_back(L'\0');
            }
        }
    }
    if (list.empty())
        return 0;
    auto appliedCarry = CarryHomePath(acKey);
    appliedCarry.replace_extension(".applied.snap");
    for (const auto& file : {CarryHomePath(acKey), appliedCarry})
        if (fs::exists(file, error))
        {
            list += file.wstring();
            list.push_back(L'\0');
        }
    list.push_back(L'\0');
    SHFILEOPSTRUCTW operation{};
    operation.wFunc = FO_DELETE;
    operation.pFrom = list.c_str();
    operation.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_NOERRORUI | FOF_SILENT;
    const int result = SHFileOperationW(&operation);
    spdlog::info("Deleted {} saves of playthrough {} to the Recycle Bin (result {}, aborted {})", saves, acKey, result, operation.fAnyOperationsAborted);
    return result == 0 ? saves : 0;
}

std::filesystem::path SavesFolder() noexcept
{
    return SavesDirectory();
}

bool LoadFile(const fs::path& acSave) noexcept
{
    auto* pManager = GetSaveLoadManager();
    std::error_code error;
    if (!pManager || acSave.extension() != ".ess" || !fs::exists(acSave, error))
    {
        spdlog::warn("Save {} is not loadable", acSave.string());
        return false;
    }
    const auto name = acSave.stem().string();
    // As Load: the console LoadGame command's steps (load flag 403444, tear-down 36604, Load_Impl 35728).
    POINTER_SKYRIMSE(uint8_t, s_loadFlag, 403444);
    using TPrepareLoad = void();
    POINTER_SKYRIMSE(TPrepareLoad, s_prepareLoad, 36604);
    TP_THIS_FUNCTION(TLoadImpl, bool, void, const char*, int32_t, uint32_t, bool);
    POINTER_SKYRIMSE(TLoadImpl, s_loadImpl, 35728);
    *s_loadFlag.Get() = 1;
    s_prepareLoad.Get()();
    const bool started = TiltedPhoques::ThisCall(s_loadImpl.Get(), pManager, name.c_str(), -1, 0u, true);
    spdlog::info("Save load {}: {}", name, started ? "started" : "FAILED");
    return started;
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
    // A different load order is the game's "Current" case (answered in advance, AchievementsWithMods.cpp): the save
    // loads with this game's plugins. Refusing here sent the party to each PC's newest save instead (2026-10-02: the
    // owners' cave-exit checkpoint, written before today's load order, was never loaded).
    if (const auto recorded = RecordedLoadOrder(acId); recorded && recorded != LoadOrderHash())
        spdlog::warn("Checkpoint {} was written under a different load order ({:x}, now {:x}); loading it with this one",
            name, recorded, LoadOrderHash());

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
        POINTER_SKYRIMSE(TQuitToMainMenu, quitToMainMenu, 55490);
        RealQuitToMainMenu = quitToMainMenu.Get();
        TP_HOOK(&RealQuitToMainMenu, HookQuitToMainMenu);
        POINTER_SKYRIMSE(TQueueSaveLoadTask, queueSaveLoadTask, 35769);
        RealQueueSaveLoadTask = queueSaveLoadTask.Get();
        TP_HOOK(&RealQueueSaveLoadTask, HookQueueSaveLoadTask);
    });
