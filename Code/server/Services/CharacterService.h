#pragma once

#include <Events/PacketEvent.h>
#include <Structs/ActorData.h>

struct UpdateEvent;
struct CharacterInteriorCellChangeEvent;
struct CharacterSpawnedEvent;
struct World;
struct AssignCharacterRequest;
struct AssignCharacterResponse;
struct CharacterSpawnRequest;
struct ClientReferencesMoveRequest;
struct CorpseRagdollRequest;
struct PlayerAppearanceRequest;
struct RequestFactionsChanges;
struct GridCellCoords;
struct RequestOwnershipTransfer;
struct CharacterRemoveEvent;
struct CharacterExteriorCellChangeEvent;
struct RequestOwnershipClaim;
struct OwnershipTransferEvent;
struct MountRequest;
struct NewPackageRequest;
struct RequestRespawn;
struct SyncExperienceRequest;
struct DialogueRequest;
struct SubtitleRequest;
struct Player;
struct RequestScriptedActorState;

/**
 * @brief Manages player and actor state.
 */
struct CharacterService
{
    CharacterService(World& aWorld, entt::dispatcher& aDispatcher) noexcept;
    ~CharacterService() noexcept = default;

    TP_NOCOPYMOVE(CharacterService);

    static void Serialize(World& aRegistry, entt::entity aEntity, CharacterSpawnRequest* apSpawnRequest) noexcept;
    void ReconcileCellOwnership(Player* apPlayer, bool aCellEntry = true) const noexcept;
    bool CanReplicateTo(Player* apPlayer, entt::entity aEntity) const noexcept;
    // Before a party wipe's checkpoint reload: removes the party's temporary (script-spawned, FF) actors, whose ids
    // change with every load and can never be rebound. Persistent actors keep their identity and owner, so the
    // leader's reloaded natives rebind to them exactly as on any Continue. Returns how many were removed.
    size_t DropPartyTemporaries(uint32_t aPartyId) const noexcept;
    // After a party reload: sends this player every character in its range (the other players included). Clients
    // forget their copies before the reload, and a reload that stays in the same area shifts no grid cell.
    void ReplayToPlayer(Player* apPlayer) const noexcept;

protected:
    enum class OwnershipTransferReason : uint8_t
    {
        LeaderAssignment,
        LeaderClaim,
        CellLease,
        Mount,
        Relinquish,
        OwnerUnavailable,
        CorpseCarry
    };

    void OnUpdate(const UpdateEvent& acEvent) const noexcept;
    void EnforceLeaderAuthority() const noexcept;
    void OnScriptedActorState(const PacketEvent<RequestScriptedActorState>& acMessage) const noexcept;
    void UpdateParkedActors() const noexcept;
    void ReleaseParkedActor(entt::entity aEntity) const noexcept;
    void OnCharacterExteriorCellChange(const CharacterExteriorCellChangeEvent& acEvent) const noexcept;
    void OnCharacterInteriorCellChange(const CharacterInteriorCellChangeEvent& acEvent) const noexcept;
    void OnAssignCharacterRequest(const PacketEvent<AssignCharacterRequest>& acMessage) const noexcept;
    void OnOwnershipTransferRequest(const PacketEvent<RequestOwnershipTransfer>& acMessage) const noexcept;
    void OnOwnershipTransferEvent(const OwnershipTransferEvent& acEvent) const noexcept;
    void OnOwnershipClaimRequest(const PacketEvent<RequestOwnershipClaim>& acMessage) const noexcept;
    void OnCharacterRemoveEvent(const CharacterRemoveEvent& acEvent) const noexcept;
    void OnCharacterSpawned(const CharacterSpawnedEvent& acEvent) const noexcept;
    void OnReferencesMoveRequest(const PacketEvent<ClientReferencesMoveRequest>& acMessage) const noexcept;
    void OnCorpseRagdoll(const PacketEvent<CorpseRagdollRequest>& acMessage) const noexcept;
    void OnPlayerAppearance(const PacketEvent<PlayerAppearanceRequest>& acMessage) const noexcept;
    void OnFactionsChanges(const PacketEvent<RequestFactionsChanges>& acMessage) const noexcept;
    void OnMountRequest(const PacketEvent<MountRequest>& acMessage) const noexcept;
    void OnNewPackageRequest(const PacketEvent<NewPackageRequest>& acMessage) const noexcept;
    void OnRequestRespawn(const PacketEvent<RequestRespawn>& acMessage) const noexcept;
    void OnSyncExperienceRequest(const PacketEvent<SyncExperienceRequest>& acMessage) const noexcept;
    void OnDialogueRequest(const PacketEvent<DialogueRequest>& acMessage) const noexcept;
    void OnSubtitleRequest(const PacketEvent<SubtitleRequest>& acMessage) const noexcept;

    void CreateCharacter(const PacketEvent<AssignCharacterRequest>& acMessage) const noexcept;
    void PopulateAssignmentResponse(entt::entity aEntity, AssignCharacterResponse& aResponse) const noexcept;
    static const char* GetOwnershipTransferReasonName(OwnershipTransferReason aReason) noexcept;
    bool CanClaimOwnership(Player* apPlayer, entt::entity aEntity, uint32_t aExpectedOwnershipEpoch, OwnershipTransferReason aReason) const noexcept;
    bool TransferOwnership(Player* apPlayer, entt::entity aEntity, OwnershipTransferReason aReason, bool aResetInvalidOwners = false) const noexcept;
    void TransferToNextOwner(entt::entity aEntity, OwnershipTransferReason aReason) const noexcept;
    void StampOwnership(entt::entity aEntity, Player* apPlayer) const noexcept;
    void ReconcileActorOwnership(Player* apPlayer, entt::entity aEntity, bool aCellEntry) const noexcept;
    ActorData BuildActorData(const entt::entity acEntity) const noexcept;

    void ProcessFactionsChanges() const noexcept;
    void ProcessMovementChanges() const noexcept;

private:
    World& m_world;
    mutable std::chrono::steady_clock::time_point m_nextOwnershipSweep{};

    entt::scoped_connection m_updateConnection;
    entt::scoped_connection m_scriptedActorStateConnection;
    entt::scoped_connection m_corpseRagdollConnection;
    entt::scoped_connection m_playerAppearanceConnection;
    entt::scoped_connection m_exteriorCellChangeEventConnection;
    entt::scoped_connection m_interiorCellChangeEventConnection;
    entt::scoped_connection m_characterAssignRequestConnection;
    entt::scoped_connection m_transferOwnershipConnection;
    entt::scoped_connection m_ownershipTransferEventConnection;
    entt::scoped_connection m_claimOwnershipConnection;
    entt::scoped_connection m_removeCharacterConnection;
    entt::scoped_connection m_characterSpawnedConnection;
    entt::scoped_connection m_referenceMovementSnapshotConnection;
    entt::scoped_connection m_factionsChangesConnection;
    entt::scoped_connection m_mountConnection;
    entt::scoped_connection m_newPackageConnection;
    entt::scoped_connection m_requestRespawnConnection;
    entt::scoped_connection m_syncExperienceConnection;
    entt::scoped_connection m_dialogueConnection;
    entt::scoped_connection m_subtitleConnection;
};
