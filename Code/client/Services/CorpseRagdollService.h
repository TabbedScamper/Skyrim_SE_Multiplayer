#pragma once

#include <Messages/CorpseRagdollRequest.h>

#include <array>
#include <atomic>
#include <mutex>

struct World;
struct TransportService;
struct UpdateEvent;
struct DisconnectedEvent;
struct NotifyCorpseRagdoll;

// Ragdolls look the same on every PC, in real time: deaths, knockdowns, shouts, explosions.
// Each PC used to simulate its own ragdoll for an actor (measured: 18 simulated bodies on both,
// different transforms; the prisoner shot in the intro fell and spun differently on the follower).
// The actor's owner streams every ragdoll body (relative to the actor) at 20 Hz while physics owns
// the skeleton and the bodies move, and sends the settled pose afterwards (again every 5 s for
// late arrivals). The other PCs, whose copy is also ragdolling, place each body at the owner's pose
// on the shared presentation timeline (interpolated, like actor movement) and put the ragdoll to
// sleep once it has settled. A copy that is not ragdolling shows the owner's ragdoll pose through
// PoseCopyAuthority instead.
class CorpseRagdollService
{
public:
    CorpseRagdollService(World& aWorld, entt::dispatcher& aDispatcher, TransportService& aTransport) noexcept;
    TP_NOCOPYMOVE(CorpseRagdollService);
    // Called by the Main::Update hook on the main thread: the remote ragdolls are placed there,
    // before the frame's physics step, not from the update job running beside it.
    static void OnMainFrame() noexcept;
    // True while the owner's ragdoll stream for this actor drives it here (a sample within 10 s):
    // nothing else may move it (the corpse cell correction reloaded it with MoveTo: naked, then
    // teleported to the owner's final position). Any thread.
    static bool IsFollowingOwner(uint32_t aFormId) noexcept;
    // Testing ground: the actor's ragdoll bodies as JSON (world positions, game units), "[]" if none.
    static std::string DescribeRagdollBodies(Actor* apActor) noexcept;

private:
    struct OwnedRagdoll
    {
        uint64_t SettledSinceMs{};
        uint64_t LastSentMs{};
        bool SentSettled{};
    };
    struct Sample
    {
        uint64_t Tick{};
        float Origin[3]{};
        Vector<CorpseRagdollBody> Bodies;
    };
    struct RemoteRagdoll
    {
        std::array<Sample, 12> Ring{};
        uint32_t RingCount{};
        uint32_t RingNext{};
        bool Asleep{};
        // This copy was knocked into ragdoll to follow the owner's stream, and live placement logged.
        bool Knocked{};
        bool LiveLogged{};
        uint64_t NextCheckMs{};
        uint32_t Applications{};
        bool CountMismatchLogged{};
        const char* LastSkipReason{};
    };

    void OnUpdate(const UpdateEvent&) noexcept;
    void OnDisconnected(const DisconnectedEvent&) noexcept;
    void OnCorpseRagdoll(const NotifyCorpseRagdoll&) noexcept;
    void CaptureOwned(uint64_t aNowMs) noexcept;
    void ApplyRemote(uint64_t aNowMs) noexcept;

    World& m_world;
    TransportService& m_transport;
    uint64_t m_nextTickMs{};
    Map<uint32_t, OwnedRagdoll> m_owned;
    Map<uint32_t, RemoteRagdoll> m_remote;
    std::recursive_mutex m_remoteLock;
    std::atomic<bool> m_applyOnMainFrame{};
    entt::scoped_connection m_updateConnection;
    entt::scoped_connection m_disconnectConnection;
    entt::scoped_connection m_ragdollConnection;
};
