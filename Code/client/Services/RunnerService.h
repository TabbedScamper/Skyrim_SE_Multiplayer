#pragma once

#include <TiltedCore/TaskQueue.hpp>
#include <atomic>

struct UpdateEvent;

/**
 * @brief Dispatches events.
 */
struct RunnerService
{
    RunnerService(entt::dispatcher& aDispatcher) noexcept;
    ~RunnerService() noexcept = default;

    TP_NOCOPYMOVE(RunnerService);

    /**
     * @brief Executes all queued events every frame.
     */
    void OnUpdate(const UpdateEvent& acUpdateEvent) noexcept;

    /**
     * @brief Queues an event for OnUpdate() to execute.
     */
    template <class T> void Trigger(T acEvent)
    {
        // Events carry raw engine pointers (SpellCastEvent's caster) and run a frame later: one queued before a game
        // load is dropped after it (an in-game reload crashed the host in MagicService::OnSpellCastEvent on a freed
        // caster, 2026-09-29).
        m_runner.Add([event = std::move(acEvent), this, generation = s_loadGeneration.load(std::memory_order_acquire)]()
        {
            if (generation == s_loadGeneration.load(std::memory_order_acquire))
                m_dispatcher.trigger(std::move(event));
        });
    }

    // TESLoadGameEvent: every event queued before it is stale.
    static void NoteGameLoaded() noexcept { s_loadGeneration.fetch_add(1, std::memory_order_acq_rel); }

    /**
     * @brief Queues a lambda for OnUpdate() to execute.
     */
    void Queue(std::function<void()> aFunctor) noexcept;

private:
    inline static std::atomic<uint64_t> s_loadGeneration{};
    entt::dispatcher& m_dispatcher;
    TiltedPhoques::TaskQueue m_runner;
};
