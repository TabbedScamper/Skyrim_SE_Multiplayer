#pragma once

#include <atomic>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>

struct World;
struct UpdateEvent;

// In-game updates (owner 2026-09-30: "easily update from within the game with patch notes in the options menu").
// Checks latest.json on the GitHub latest release (once at start, then every 6 hours, or on demand), shows the version
// and patch notes in Options > Updates, downloads the release zip in the background on request, verifies it and every
// file against the manifest's SHA-256, and stages it for the starter (Skyrim_SE_Multiplayer.exe) to apply at the next
// launch, before the game starts. Never blocks playing or co-op; offline is only a status.
struct UpdateService
{
    explicit UpdateService(World& aWorld, entt::dispatcher& aDispatcher) noexcept;
    ~UpdateService() noexcept;
    TP_NOCOPYMOVE(UpdateService);

    void CheckNow() noexcept;
    void Download() noexcept;
    // The state as JSON for the menu (current, latest, phase, notes, progress, error, lastChecked).
    [[nodiscard]] std::string StateJson() const noexcept;
    // This build's version: the release tag's version, or 0.0.0-dev+<commit> for a local build.
    [[nodiscard]] static std::string CurrentVersion() noexcept;
    // a < b by semantic version (pre-release parts compared as text; a dev build is older than any release).
    [[nodiscard]] static bool IsOlder(const std::string& a, const std::string& b) noexcept;
    // What to tell a player whose friend or server runs another build, given the other side's SSM_WIRE_VERSION:
    // who needs to update, pointing at Options > Updates.
    [[nodiscard]] static std::string MismatchText(const std::string& acTheirWireVersion) noexcept;

private:
    void OnUpdate(const UpdateEvent&) noexcept;
    void RunCheck() noexcept;
    void RunDownload() noexcept;
    void SetPhase(const char* acPhase, const std::string& acError = {}) noexcept;
    void Join() noexcept;

    World& m_world;
    entt::scoped_connection m_updateConnection;
    mutable std::mutex m_lock;
    std::thread m_worker;
    std::atomic<bool> m_busy{false};
    std::atomic<bool> m_stopping{false};
    // Serializes starting and joining the worker against shutdown.
    std::mutex m_workerLock;
    // The live WinHTTP session and unpacker process, closed or ended by the destructor on quit.
    std::atomic<void*> m_session{nullptr};
    std::atomic<void*> m_extractor{nullptr};
    uint64_t m_nextCheck{};
    std::string m_pushed;

    // Guarded by m_lock.
    std::string m_phase{"idle"};
    std::string m_latest;
    std::string m_notes;
    std::string m_publishedAt;
    std::string m_zipUrl;
    std::string m_zipSha;
    uint64_t m_zipSize{};
    std::string m_manifest;
    std::string m_error;
    uint64_t m_received{};
    uint64_t m_lastChecked{};
};
