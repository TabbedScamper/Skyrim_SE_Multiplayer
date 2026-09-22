#pragma once

#include <condition_variable>
#include <deque>
#include <memory>
#include <thread>
#include <atomic>

struct World;

inline constexpr UINT cGameTestWakeMessage = WM_APP + 0x51B;

// Local-only automation bridge. The pipe thread never touches game state;
// requests are executed on Skyrim's window thread through cGameTestWakeMessage.
struct GameTestService
{
    explicit GameTestService(World& aWorld) noexcept;
    ~GameTestService() noexcept;

    TP_NOCOPYMOVE(GameTestService);

    void OnWindowThread() noexcept;
    void OnGameThread() noexcept;
    [[nodiscard]] std::string GetCachedGameSnapshot() const noexcept;

private:
    struct Request
    {
        std::string Line;
        std::string Response;
        bool Complete{false};
        std::mutex Mutex;
        std::condition_variable Completed;
    };

    void PipeMain() noexcept;
    std::string Execute(const std::string& acLine) noexcept;
    void WakeWindowThread() noexcept;

    World& m_world;
    std::atomic_bool m_stopping{false};
    std::mutex m_queueMutex;
    std::deque<std::shared_ptr<Request>> m_requests;
    std::thread m_pipeThread;
    mutable std::mutex m_snapshotMutex;
    std::string m_gameSnapshot{"null"};
    uint64_t m_gameSnapshotTimeMs{};
    uint64_t m_nextGameSnapshotTimeMs{};
    Set<std::string> m_watchedQuests{"MQ101"};
};
