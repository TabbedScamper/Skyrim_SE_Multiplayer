#pragma once

#include <Messages/CorpseRagdollRequest.h>

struct World;
struct TransportService;
struct UpdateEvent;
struct DisconnectedEvent;
struct NotifyCorpseRagdoll;

// Settled corpses look the same on every PC. Each PC used to simulate its own ragdoll for a
// corpse (measured: 18 simulated bodies on both, different transforms, different bone poses).
// The corpse's owner sends every ragdoll body once the corpse has settled (and again every 5 s
// for late arrivals); the other PCs keyframe that corpse's whole ragdoll and place each body at
// the owner's transform, relative to the corpse's synced position.
class CorpseRagdollService
{
public:
    CorpseRagdollService(World& aWorld, entt::dispatcher& aDispatcher, TransportService& aTransport) noexcept;
    TP_NOCOPYMOVE(CorpseRagdollService);

private:
    struct OwnedCorpse
    {
        uint64_t SettledSinceMs{};
        uint64_t LastSentMs{};
    };
    struct RemoteCorpse
    {
        Vector<CorpseRagdollBody> Bodies;
        bool Keyframed{};
        uint64_t NextCheckMs{};
        uint32_t Applications{};
        bool CountMismatchLogged{};
    };

    void OnUpdate(const UpdateEvent&) noexcept;
    void OnDisconnected(const DisconnectedEvent&) noexcept;
    void OnCorpseRagdoll(const NotifyCorpseRagdoll&) noexcept;
    void CaptureOwned(uint64_t aNowMs) noexcept;
    void ApplyRemote(uint64_t aNowMs) noexcept;

    World& m_world;
    TransportService& m_transport;
    uint64_t m_nextTickMs{};
    Map<uint32_t, OwnedCorpse> m_owned;
    Map<uint32_t, RemoteCorpse> m_remote;
    entt::scoped_connection m_updateConnection;
    entt::scoped_connection m_disconnectConnection;
    entt::scoped_connection m_ragdollConnection;
};
