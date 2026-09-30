#pragma once

#include <deque>
#include "Structs/Inventory.h"
#include "Structs/ActorData.h"
#include <atomic>
#include <unordered_map>
#include <Messages/NotifyScriptedActorState.h>
#include <Messages/NotifyOwnershipTransfer.h>

struct ActorAddedEvent;
struct ActorRemovedEvent;
struct UpdateEvent;
struct ConnectedEvent;
struct DisconnectedEvent;
struct EquipmentChangeEvent;
struct FormIdComponent;
struct ActionEvent;
struct AssignCharacterResponse;
struct CharacterSpawnRequest;
struct ServerReferencesMoveRequest;
struct NotifyInventoryChanges;
struct NotifyFactionsChanges;
struct NotifyRemoveCharacter;
struct NotifyDrawWeapon;
struct NotifyAnimObject;
struct NotifyOwnershipTransfer;
struct SpellCastEvent;
struct NotifySpellCast;
struct InterruptCastEvent;
struct NotifyInterruptCast;
struct AddTargetEvent;
struct NotifyAddTarget;
struct ProjectileLaunchedEvent;
struct NotifyProjectileLaunch;
struct MountEvent;
struct NotifyMount;
struct InitPackageEvent;
struct NotifyNewPackage;
struct NotifyRespawn;
struct NotifyPlayerAppearance;
struct BeastFormChangeEvent;
struct AddExperienceEvent;
struct NotifySyncExperience;
struct DialogueEvent;
struct NotifyDialogue;
struct SubtitleEvent;
struct NotifySubtitle;
struct NotifyActorTeleport;
struct PartyJoinedEvent;

struct Actor;
struct World;
struct TransportService;

/**
 * @brief Handles actors and players.
 */
struct CharacterService
{
    CharacterService(World& aWorld, entt::dispatcher& aDispatcher, TransportService& aTransport) noexcept;
    ~CharacterService() noexcept = default;

    TP_NOCOPYMOVE(CharacterService);

    static void DeleteTempActor(const uint32_t aFormId) noexcept;

    void SetPresentationDelayMs(uint32_t aDelayMs) noexcept;
    [[nodiscard]] uint32_t GetPresentationDelayMs() const noexcept;
    struct MountDiagnostic
    {
        uint32_t Pending{};
        uint64_t Notifications{};
        uint64_t WaitedFor3D{};
        uint64_t Applied{};
        uint64_t Seated{};
        uint64_t Rejected{};
        uint32_t LastRiderId{};
        uint32_t LastMountId{};
    };
    [[nodiscard]] MountDiagnostic GetMountDiagnostic() const noexcept;
    struct LocalPoseProductionDiagnostic
    {
        uint64_t Batches{};
        uint64_t Actors{};
        uint64_t TotalUs{};
        uint32_t MaxActorUs{};
        uint32_t LastBatchUs{};
        uint32_t LastBatchActors{};
    };
    [[nodiscard]] LocalPoseProductionDiagnostic GetLocalPoseProductionDiagnostic() const noexcept;
    struct MountRelationDiagnostic
    {
        uint32_t RiderId{};
        uint32_t MountId{};
        uint32_t RiderFormId{};
        uint32_t MountFormId{};
        uint32_t NativeMountFormId{};
        uint32_t NativeVehicleHandle{};
        bool HorseExtra{};
        uint32_t HorseHandle{};
        bool InteractionExtra{};
        bool InteractionPointerPresent{};
        uint32_t InteractionActorHandle{};
        uint32_t InteractionTargetHandle{};
        bool RiderHas3D{};
        bool MountHas3D{};
        bool WasSeated{};
        uint32_t Attempts{};
    };
    [[nodiscard]] Vector<MountRelationDiagnostic> GetPendingMountRelations() const noexcept;
    void SetVehicleTrialRiderId(uint32_t aRiderId) noexcept;
    [[nodiscard]] uint32_t GetVehicleTrialRiderId() const noexcept;
    [[nodiscard]] uint64_t GetVehicleTrialCalls() const noexcept;
    [[nodiscard]] uint64_t GetVehicleTrialImmediateSeats() const noexcept;
    [[nodiscard]] uint32_t GetVehicleTrialImmediateHandle() const noexcept;

    bool RequestOwnership(uint32_t aFormId, uint32_t aServerId, entt::entity aEntity) const noexcept;
    void ObserveDiscoveredActor(Actor* apActor) noexcept;
    bool IsActorDiscoverySuppressed(uint32_t aFormId) const noexcept;

    void OnActorAdded(const ActorAddedEvent& acEvent) noexcept;
    void OnActorRemoved(const ActorRemovedEvent& acEvent) noexcept;
    void OnUpdate(const UpdateEvent& acUpdateEvent) noexcept;
    void OnConnected(const ConnectedEvent& acConnectedEvent) const noexcept;
    void OnDisconnected(const DisconnectedEvent& acDisconnectedEvent) noexcept;
    // Before a party wipe's trip to the main menu: forget every tracked actor, as a fresh launch starts, without
    // releasing ownership (the server keeps the leader as owner, so its reloaded natives rebind). Entities kept across
    // the menu referenced unloaded forms: a load with them crashed (19032 on the follower, the host at 140639011), and
    // actors removed by the menu teardown relinquished the leader's NPCs to whichever player loaded first.
    void ReleaseForReload() noexcept;
    // Spawn requests that arrive while this PC reloads (after ReleaseForReload, until the loaded world is in) wait:
    // handled mid-load they resolved references the load was tearing down (host crash at 140639011, 2026-09-28).
    // TESLoadGameEvent (DiscoveryService): the reload finished; held spawns go out on the next update.
    void NoteGameLoaded() noexcept { m_holdSawLoading = true; }
    bool m_holdSpawns{};
    bool m_holdSawLoading{};
    uint64_t m_holdSince{};
    Vector<CharacterSpawnRequest> m_heldSpawns;
    // One new actor per frame: two Actor::Create calls in one frame were given the same temporary form id (the id is
    // taken at creation but enters the form table later), and the second spawn took over the first copy (a party
    // reload's replay: the other player's copy became a Thalmor soldier and was later deleted, 2026-09-29).
    bool m_createdThisFrame{};
    std::deque<CharacterSpawnRequest> m_nextFrameSpawns;
    void OnAssignCharacter(const AssignCharacterResponse& acMessage) noexcept;
    void OnCharacterSpawn(const CharacterSpawnRequest& acMessage) noexcept;
    void OnReferencesMoveRequest(const ServerReferencesMoveRequest& acMessage) const noexcept;
    void OnActionEvent(const ActionEvent& acActionEvent) const noexcept;
    void OnFactionsChanges(const NotifyFactionsChanges& acEvent) const noexcept;
    void OnOwnershipTransfer(const NotifyOwnershipTransfer& acMessage) noexcept;
    void OnRemoveCharacter(const NotifyRemoveCharacter& acMessage) noexcept;
    void OnNotifyDrawWeapon(const NotifyDrawWeapon& acMessage) noexcept;
    void OnNotifyAnimObject(const NotifyAnimObject& acMessage) noexcept;
    void RunAnimObjectUpdates() noexcept;

public:
    // Animation-graph prop events (AnimationObjects.cpp hooks, any thread): owned actors' events are relayed.
    static void QueueAnimObjectEvent(uint32_t aFormId, uint32_t aAnimObjectId, bool aDraw) noexcept;
    // Main thread (ObjectService::OnMainFrame): received props onto remote copies (Draw writes the scene node).
    static void ApplyAnimObjectsOnMainFrame() noexcept;
    // Actor moves requested off the main thread (network handlers), applied from the main loop.
    static void ApplyMovesOnMainFrame() noexcept;
    // A remote NPC copy was killed by sync (any thread): its body goes to the dead-body collision layer on the main loop.
    static void NoteRemoteDeath(uint32_t aFormId) noexcept;
    // Queued moves and dead-body layers of the old connection name nothing after a disconnect.
    static void ClearMainFrameWork() noexcept;
    void SendNameIfChanged() noexcept;
    std::string m_sentName;
    uint64_t m_nextNameCheckMs{};

private:
    uint64_t m_nextAnimObjectSnapshot{};
    uint64_t m_nextAnimObjectResend{};
    std::set<std::tuple<uint32_t, uint32_t, bool>> m_sentAnimObjects{};
    void OnMountEvent(const MountEvent& acEvent) const noexcept;
    void OnNotifyMount(const NotifyMount& acMessage) noexcept;
    void OnInitPackageEvent(const InitPackageEvent& acEvent) const noexcept;
    void OnNotifyNewPackage(const NotifyNewPackage& acMessage) const noexcept;
    void UpdateLeaderScriptedPackage() noexcept;
    void OnNotifyRespawn(const NotifyRespawn& acMessage) const noexcept;
    void OnNotifyPlayerAppearance(const NotifyPlayerAppearance& acMessage) noexcept;
    // Character creator together: this player's look sent live while editing (see CharacterService.cpp).
    void SendCreatorAppearance() noexcept;

public:
    // Where to show another player's character on this PC while they are in the creator: beside
    // this player instead of inside them (everyone stands on the same spot). Any thread.
    static bool GetCreatorDisplayOffset(uint32_t aFormId, NiPoint3& arOffset, float& arHeading) noexcept;

private:
    bool IsLeaderNativeActor(Actor* apActor) const noexcept;
    bool TryParkActor(entt::entity aEntity, Actor* apActor) noexcept;
    void OnScriptedActorState(const NotifyScriptedActorState& acMessage) noexcept;
    void RunScriptedActorUpdates() noexcept;
    struct ParkedActor
    {
        entt::entity Entity{entt::null};
        NotifyScriptedActorState Message{};
        bool DisabledByUs{};
        bool DisablePending{};
    };
    std::unordered_map<uint32_t, ScriptedActorState> m_loadedActorLocations;
    std::unordered_map<uint32_t, ParkedActor> m_parkedActors;
    std::unordered_map<uint32_t, NotifyOwnershipTransfer> m_restoredOwnershipGrants;
    entt::scoped_connection m_scriptedActorStateConnection;

    void OnBeastFormChange(const BeastFormChangeEvent& acEvent) const noexcept;
    void OnAddExperienceEvent(const AddExperienceEvent& acEvent) noexcept;
    void OnNotifySyncExperience(const NotifySyncExperience& acMessage) noexcept;
    void OnDialogueEvent(const DialogueEvent& acEvent) noexcept;
    void OnNotifyDialogue(const NotifyDialogue& acMessage) noexcept;
    void OnSubtitleEvent(const SubtitleEvent& acEvent) noexcept;
    void OnNotifySubtitle(const NotifySubtitle& acMessage) noexcept;
    void OnNotifyActorTeleport(const NotifyActorTeleport& acMessage) noexcept;
    void OnPartyJoinedEvent(const PartyJoinedEvent& acEvent) noexcept;

    void ProcessNewEntity(entt::entity aEntity) const noexcept;

private:
    // aEnableAfter: enable the actor once it stands at the destination (a parked actor resuming).
    // aCorpseCorrection: the settled corpse cell correction may move a dead copy (every other caller leaves dying,
    // dead and ragdolling copies to the owner's ragdoll stream).
    void MoveActor(Actor* apActor, const GameId& acWorldSpaceId, const GameId& acCellId, const Vector3_NetQuantize& acPosition,
        bool aEnableAfter = false, bool aCorpseCorrection = false) const noexcept;
    void MoveActorNow(Actor* apActor, const GameId& acWorldSpaceId, const GameId& acCellId, const Vector3_NetQuantize& acPosition,
        bool aCorpseCorrection) const noexcept;

    void RequestServerAssignment(entt::entity aEntity) const noexcept;
    // aFromDiscovery: the actor left the loaded world (discovery removal), the only case that may report a script removal.
    void CancelServerAssignment(entt::entity aEntity, uint32_t aFormId, bool aFromDiscovery = false) const noexcept;
    void DeleteRemoteEntityComponents(entt::entity aEntity) const noexcept;
    void DeclineOwnership(uint32_t aServerId, uint32_t aOwnershipEpoch) const noexcept;
    void ReconcileActorData(entt::entity aEntity, Actor* apActor, uint32_t aOwnershipEpoch, const ActorData& acActorData, bool aApplyInventory, bool aIsLocalOwner, bool aInitialNativeAssignment = false) noexcept;

    Actor* CreateCharacterForEntity(entt::entity aEntity) const noexcept;
    ActorData BuildActorData(Actor* apActor) const noexcept;
    void ApplyLeveledNpcPick(Actor* apActor, const GameId& acPickId) const noexcept;
    void ProcessLeveledConforms() noexcept;
    void ClearMountRelationsForServerId(uint32_t aServerId) noexcept;

    void RunLocalUpdates() const noexcept;
    void RunRemoteUpdates() noexcept;
    void RunPresentationEvents() noexcept;
    void RunFactionsUpdates() const noexcept;
    void RunSpawnUpdates() const noexcept;
    void RunPendingMounts() noexcept;
    void RunLocalMountUpdates() noexcept;
    void RunExperienceUpdates() noexcept;
    void ApplyCachedWeaponDraws(const UpdateEvent& acUpdateEvent) noexcept;

    World& m_world;
    entt::dispatcher& m_dispatcher;
    TransportService& m_transport;
    // 100 ms (was 300): max-sync; live-tunable via set_presentation_delay / sync_level.
    std::atomic<uint32_t> m_presentationDelayMs{100};
    uint32_t m_lastLeaderScriptedPackage{};
    // Selected actor Serialize includes pose capture and the ordinary movement fields.
    mutable std::atomic<uint64_t> m_localPoseBatches{};
    mutable std::atomic<uint64_t> m_localPoseActors{};
    mutable std::atomic<uint64_t> m_localPoseTotalUs{};
    mutable std::atomic<uint32_t> m_localPoseMaxActorUs{};
    mutable std::atomic<uint32_t> m_localPoseLastBatchUs{};
    mutable std::atomic<uint32_t> m_localPoseLastBatchActors{};

    float m_cachedExperience = 0.f;

    // TODO: revamp this, read the local anim var like vampire lord?
    struct WeaponDrawData
    {
        WeaponDrawData() = default;
        WeaponDrawData(bool aDrawWeapon)
            : m_drawWeapon(aDrawWeapon)
        {
        }

        double m_timer = 0.0;
        bool m_drawWeapon = false;
        bool m_isFirstPass = true;
    };

    Map<uint32_t, WeaponDrawData> m_weaponDrawUpdates{};

    // Actor form ID -> pick form ID. The active stage lives in ActorExtension.
    // Written from const message handlers, drained by ProcessLeveledConforms.
    mutable Map<uint32_t, uint32_t> m_pendingLeveledConforms{};
    uint64_t m_nextDeferredAssignmentRetryMs{};

    struct PendingVoice
    {
        uint32_t ServerId{};
        uint64_t Tick{};
        TiltedPhoques::String Filename{};
    };
    struct PendingSubtitle
    {
        uint32_t ServerId{};
        uint64_t Tick{};
        uint32_t TopicFormId{};
        TiltedPhoques::String Text{};
    };
    Vector<PendingVoice> m_pendingVoices{};
    Vector<PendingSubtitle> m_pendingSubtitles{};
    struct PendingMount
    {
        uint32_t MountId{};
        uint64_t NextAttemptMs{};
        uint32_t Attempts{};
        uint64_t StartedAtMs{};
        bool WasSeated{};
        bool VehicleTrialAttempted{};
    };
    std::unordered_map<uint32_t, PendingMount> m_pendingMounts{};
    struct LocalMountSendState
    {
        uint32_t OwnershipEpoch{};
        uint32_t MountId{};
        uint64_t LastSentMs{};
        uint64_t ZeroObservedAtMs{};
    };
    std::unordered_map<uint32_t, LocalMountSendState> m_localMountSent{};
    uint64_t m_lastLocalMountPollMs{};
    uint64_t m_mountNotifications{};
    uint64_t m_mountWaitedFor3D{};
    uint64_t m_mountApplied{};
    uint64_t m_mountSeated{};
    uint64_t m_mountRejected{};
    uint32_t m_lastMountRiderId{};
    uint32_t m_lastMountId{};
    std::atomic<uint32_t> m_vehicleTrialRiderId{};
    uint64_t m_vehicleTrialCalls{};
    uint64_t m_vehicleTrialImmediateSeats{};
    uint32_t m_vehicleTrialImmediateHandle{};

    entt::scoped_connection m_referenceAddedConnection;
    entt::scoped_connection m_referenceRemovedConnection;
    entt::scoped_connection m_drawWeaponConnection;
    entt::scoped_connection m_animObjectConnection;
    entt::scoped_connection m_updateConnection;
    entt::scoped_connection m_actionConnection;
    entt::scoped_connection m_factionsConnection;
    entt::scoped_connection m_ownershipTransferConnection;
    entt::scoped_connection m_removeCharacterConnection;
    entt::scoped_connection m_connectedConnection;
    entt::scoped_connection m_disconnectedConnection;
    entt::scoped_connection m_assignCharacterConnection;
    entt::scoped_connection m_characterSpawnConnection;
    entt::scoped_connection m_referenceMovementSnapshotConnection;
    entt::scoped_connection m_mountConnection;
    entt::scoped_connection m_notifyMountConnection;
    entt::scoped_connection m_initPackageConnection;
    entt::scoped_connection m_newPackageConnection;
    entt::scoped_connection m_notifyRespawnConnection;
    entt::scoped_connection m_notifyPlayerAppearanceConnection;
    uint64_t m_nextCreatorAppearanceMs{};
    uint64_t m_lastCreatorAppearanceHash{};
    bool m_creatorWasOpen{};
    entt::scoped_connection m_beastFormChangeConnection;
    entt::scoped_connection m_addExperienceEventConnection;
    entt::scoped_connection m_syncExperienceConnection;
    entt::scoped_connection m_dialogueEventConnection;
    entt::scoped_connection m_dialogueSyncConnection;
    entt::scoped_connection m_subtitleEventConnection;
    entt::scoped_connection m_subtitleSyncConnection;
    entt::scoped_connection m_actorTeleportConnection;
    entt::scoped_connection m_partyJoinedConnection;
};
