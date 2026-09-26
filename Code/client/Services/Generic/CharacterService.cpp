#include <Services/CreatorTogether.h>
#include <limits>
#include <Services/SmoothClock.h>
#include <Services/CorpseRagdollService.h>
#include "Forms/TESObjectCELL.h"
#include "Forms/TESWorldSpace.h"
#include "Services/PapyrusService.h"
#include <Services/PartyService.h>

#include <Services/CharacterService.h>
#include <Services/Generic/HeadTrackService.h>
#include <Services/QuestService.h>
#include <Services/TransportService.h>

#include <Games/References.h>
#include <Games/Misc/SubtitleManager.h>

#include <Forms/TESNPC.h>
#include <Interface/UI.h>
#include <Forms/TESQuest.h>

#include <BranchInfo.h>
#include <Components.h>

#include <Systems/InterpolationSystem.h>
#include <Systems/AnimationSystem.h>
#include <Systems/CacheSystem.h>
#include <Systems/FaceGenSystem.h>
#include <Systems/LeveledNpcSystem.h>

#include <Events/ActorAddedEvent.h>
#include <Events/ActorRemovedEvent.h>
#include <Events/UpdateEvent.h>
#include <Events/ConnectedEvent.h>
#include <Events/DisconnectedEvent.h>
#include <Events/MountEvent.h>
#include <Events/InitPackageEvent.h>
#include <Events/BeastFormChangeEvent.h>
#include <Events/AddExperienceEvent.h>
#include <Events/DialogueEvent.h>
#include <Events/SubtitleEvent.h>
#include <Events/MoveActorEvent.h>
#include <Events/PartyJoinedEvent.h>

#include <Structs/ActionEvent.h>
#include <Messages/AssignCharacterRequest.h>
#include <Messages/AssignCharacterResponse.h>
#include <Messages/ServerReferencesMoveRequest.h>
#include <Games/Skyrim/Havok/VisualPoseMailbox.h>
#include <Games/Skyrim/Havok/PoseCopyAuthority.h>
#include <Services/EngineFixes.h>
#include <Services/PlayerCollision.h>
#include <AI/AIProcess.h>
#include <Forms/TESPackage.h>
#include <Messages/ClientReferencesMoveRequest.h>
#include <Messages/CharacterSpawnRequest.h>
#include <Messages/RequestFactionsChanges.h>
#include <Messages/NotifyFactionsChanges.h>
#include <Messages/NotifyRemoveCharacter.h>
#include <Messages/RequestOwnershipTransfer.h>
#include <Messages/NotifyOwnershipTransfer.h>
#include <Messages/RequestOwnershipClaim.h>
#include <Messages/MountRequest.h>
#include <Messages/NotifyMount.h>
#include <Messages/NewPackageRequest.h>
#include <Messages/NotifyNewPackage.h>
#include <Messages/RequestRespawn.h>
#include <Messages/NotifyRespawn.h>
#include <Messages/PlayerAppearanceRequest.h>
#include <Messages/NotifyPlayerAppearance.h>
#include <Messages/SyncExperienceRequest.h>
#include <Messages/NotifySyncExperience.h>
#include <Messages/DialogueRequest.h>
#include <Messages/NotifyDialogue.h>
#include <Messages/SubtitleRequest.h>
#include <Messages/NotifySubtitle.h>
#include <Messages/NotifyActorTeleport.h>
#include <Messages/RequestScriptedActorState.h>

#include <World.h>
#include <Games/TES.h>
#include <Combat/CombatController.h>

namespace
{
// This binding has identity, but must never consume a follower's presentation.
struct LeaderNativeClaim
{
    uint32_t FormId{};
    bool Parked{};
    bool ResumeSent{};
};

bool IsLoadedActor(Actor* apActor) noexcept
{
    const auto* pCell = apActor ? apActor->GetParentCellEx() : nullptr;
    return apActor && !apActor->IsDisabled() && !apActor->IsDeleted() &&
        apActor->GetNiNode() && pCell && pCell->IsAttached();
}

bool IsLocationInPlayerRange(World& aWorld, const ScriptedActorState& acLocation, bool aIsDragon = false) noexcept
{
    auto* pPlayer = PlayerCharacter::Get();
    auto* pCell = pPlayer ? pPlayer->GetParentCellEx() : nullptr;
    if (!pCell || acLocation.CellId == GameId{})
        return false;
    auto* pWorld = pPlayer->GetWorldSpace();
    if (!pWorld)
        return !acLocation.WorldSpaceId && aWorld.GetModSystem().GetGameId(acLocation.CellId) == pCell->formID;
    auto* pTES = TES::Get();
    return pTES && aWorld.GetModSystem().GetGameId(acLocation.WorldSpaceId) == pWorld->formID &&
        GridCellCoords::IsCellInGridCell(GridCellCoords::CalculateGridCellCoords(acLocation.Position),
            GridCellCoords(pTES->centerGridX, pTES->centerGridY), aIsDragon);
}

ScriptedActorState GetActorLocation(World& aWorld, Actor* apActor) noexcept
{
    ScriptedActorState state;
    if (const auto* pCell = apActor->GetParentCellEx())
        aWorld.GetModSystem().GetServerModId(pCell->formID, state.CellId);
    if (const auto* pWorld = apActor->GetWorldSpace())
        aWorld.GetModSystem().GetServerModId(pWorld->formID, state.WorldSpaceId);
    state.Position = apActor->position;
    state.Disabled = apActor->IsDisabled();
    return state;
}
}

CharacterService::CharacterService(World& aWorld, entt::dispatcher& aDispatcher, TransportService& aTransport) noexcept
    : m_world(aWorld)
    , m_dispatcher(aDispatcher)
    , m_transport(aTransport)
{
    m_referenceAddedConnection = m_dispatcher.sink<ActorAddedEvent>().connect<&CharacterService::OnActorAdded>(this);
    m_scriptedActorStateConnection = m_dispatcher.sink<NotifyScriptedActorState>().connect<&CharacterService::OnScriptedActorState>(this);
    m_referenceRemovedConnection = m_dispatcher.sink<ActorRemovedEvent>().connect<&CharacterService::OnActorRemoved>(this);

    m_updateConnection = m_dispatcher.sink<UpdateEvent>().connect<&CharacterService::OnUpdate>(this);
    m_actionConnection = m_dispatcher.sink<ActionEvent>().connect<&CharacterService::OnActionEvent>(this);

    m_connectedConnection = m_dispatcher.sink<ConnectedEvent>().connect<&CharacterService::OnConnected>(this);
    m_disconnectedConnection = m_dispatcher.sink<DisconnectedEvent>().connect<&CharacterService::OnDisconnected>(this);

    m_assignCharacterConnection = m_dispatcher.sink<AssignCharacterResponse>().connect<&CharacterService::OnAssignCharacter>(this);
    m_characterSpawnConnection = m_dispatcher.sink<CharacterSpawnRequest>().connect<&CharacterService::OnCharacterSpawn>(this);
    m_referenceMovementSnapshotConnection = m_dispatcher.sink<ServerReferencesMoveRequest>().connect<&CharacterService::OnReferencesMoveRequest>(this);
    m_factionsConnection = m_dispatcher.sink<NotifyFactionsChanges>().connect<&CharacterService::OnFactionsChanges>(this);
    m_ownershipTransferConnection = m_dispatcher.sink<NotifyOwnershipTransfer>().connect<&CharacterService::OnOwnershipTransfer>(this);
    m_removeCharacterConnection = m_dispatcher.sink<NotifyRemoveCharacter>().connect<&CharacterService::OnRemoveCharacter>(this);

    m_mountConnection = m_dispatcher.sink<MountEvent>().connect<&CharacterService::OnMountEvent>(this);
    m_notifyMountConnection = m_dispatcher.sink<NotifyMount>().connect<&CharacterService::OnNotifyMount>(this);

    m_initPackageConnection = m_dispatcher.sink<InitPackageEvent>().connect<&CharacterService::OnInitPackageEvent>(this);
    m_newPackageConnection = m_dispatcher.sink<NotifyNewPackage>().connect<&CharacterService::OnNotifyNewPackage>(this);

    m_notifyRespawnConnection = m_dispatcher.sink<NotifyRespawn>().connect<&CharacterService::OnNotifyRespawn>(this);
    m_notifyPlayerAppearanceConnection = m_dispatcher.sink<NotifyPlayerAppearance>().connect<&CharacterService::OnNotifyPlayerAppearance>(this);
    m_beastFormChangeConnection = m_dispatcher.sink<BeastFormChangeEvent>().connect<&CharacterService::OnBeastFormChange>(this);

    m_addExperienceEventConnection = m_dispatcher.sink<AddExperienceEvent>().connect<&CharacterService::OnAddExperienceEvent>(this);
    m_syncExperienceConnection = m_dispatcher.sink<NotifySyncExperience>().connect<&CharacterService::OnNotifySyncExperience>(this);

    m_dialogueEventConnection = m_dispatcher.sink<DialogueEvent>().connect<&CharacterService::OnDialogueEvent>(this);
    m_dialogueSyncConnection = m_dispatcher.sink<NotifyDialogue>().connect<&CharacterService::OnNotifyDialogue>(this);

    m_subtitleEventConnection = m_dispatcher.sink<SubtitleEvent>().connect<&CharacterService::OnSubtitleEvent>(this);
    m_subtitleSyncConnection = m_dispatcher.sink<NotifySubtitle>().connect<&CharacterService::OnNotifySubtitle>(this);

    m_actorTeleportConnection = m_dispatcher.sink<NotifyActorTeleport>().connect<&CharacterService::OnNotifyActorTeleport>(this);

    m_partyJoinedConnection = aDispatcher.sink<PartyJoinedEvent>().connect<&CharacterService::OnPartyJoinedEvent>(this);
}

void CharacterService::SetPresentationDelayMs(uint32_t aDelayMs) noexcept
{
    m_presentationDelayMs.store(aDelayMs, std::memory_order_release);
}

bool CharacterService::IsLeaderNativeActor(Actor* apActor) const noexcept
{
    const auto& party = m_world.GetPartyService();
    return m_transport.IsOnline() && party.IsInParty() && party.IsLeader() && party.GetLeaderPlayerId() &&
        apActor && apActor->formID != 0x14 && apActor->formID < 0xFF000000 &&
        !apActor->IsTemporary() && !apActor->GetExtension()->IsRemotePlayer() && !apActor->IsPlayerSummon();
}

void CharacterService::ObserveDiscoveredActor(Actor* apActor) noexcept
{
    if (IsLeaderNativeActor(apActor) && IsLoadedActor(apActor))
        m_loadedActorLocations[apActor->formID] = GetActorLocation(m_world, apActor);
}

bool CharacterService::IsActorDiscoverySuppressed(const uint32_t aFormId) const noexcept
{
    return m_parkedActors.find(aFormId) != m_parkedActors.end();
}

bool CharacterService::TryParkActor(const entt::entity aEntity, Actor* apActor) noexcept
{
    if (!IsLeaderNativeActor(apActor) || apActor->IsDead() || apActor->IsDeleted())
        return false;
    const auto* pLocal = m_world.try_get<LocalComponent>(aEntity);
    const auto location = m_loadedActorLocations.find(apActor->formID);
    if (!pLocal || location == m_loadedActorLocations.end() || !IsLocationInPlayerRange(m_world, location->second, apActor->IsDragon()))
        return false;
    // Losing high process alone is not a scripted departure.
    const auto* pCell = apActor->GetParentCellEx();
    if (!apActor->IsDisabled() && pCell && pCell->IsAttached())
        return false;
    RequestScriptedActorState request;
    request.State = GetActorLocation(m_world, apActor);
    request.State.ServerId = pLocal->Id;
    request.State.OwnershipEpoch = pLocal->OwnershipEpoch;
    request.Anchor = location->second;
    if (!m_transport.Send(request))
        return false;
    auto& claim = m_world.get_or_emplace<LeaderNativeClaim>(aEntity);
    claim.FormId = apActor->formID;
    claim.Parked = true;
    claim.ResumeSent = false;
    m_world.remove<FormIdComponent, LocalAnimationComponent, CacheComponent, InterpolationComponent, RemoteAnimationComponent, WaitingFor3D>(aEntity);
    m_pendingLeveledConforms.erase(apActor->formID);
    m_weaponDrawUpdates.erase(apActor->formID);
    spdlog::info("Scripted departure actor {:X} server {:X} epoch {} cell {:X}:{:X} disabled={}",
        apActor->formID, request.State.ServerId, request.State.OwnershipEpoch,
        request.State.CellId.ModId, request.State.CellId.BaseId, request.State.Disabled);
    return true;
}

void CharacterService::OnScriptedActorState(const NotifyScriptedActorState& acMessage) noexcept
{
    const auto& state = acMessage.State;
    const auto& party = m_world.GetPartyService();
    const uint32_t formId = m_world.GetModSystem().GetGameId(acMessage.FormId);
    if (!state.OwnershipEpoch || !formId || formId == 0x14 || formId >= 0xFF000000 ||
        (state.Phase != ScriptedActorPhase::Park && state.Phase != ScriptedActorPhase::Resume &&
            state.Phase != ScriptedActorPhase::Release && state.Phase != ScriptedActorPhase::Bind))
        return;
    auto entity = Utils::FindEntityByServerId(state.ServerId);
    if (entity)
    {
        const auto* pLocal = m_world.try_get<LocalComponent>(*entity);
        const auto* pRemote = m_world.try_get<RemoteComponent>(*entity);
        const auto epoch = pLocal ? pLocal->OwnershipEpoch : pRemote ? pRemote->OwnershipEpoch : 0;
        if (epoch > state.OwnershipEpoch)
            return;
    }
    if (state.Phase == ScriptedActorPhase::Bind)
    {
        auto* pActor = Cast<Actor>(TESForm::GetById(formId));
        if (!IsLeaderNativeActor(pActor) || party.GetLeaderPlayerId() != acMessage.LeaderPlayerId ||
            acMessage.LeaderPlayerId != m_transport.GetLocalPlayerId())
            return;
        if (entity && m_world.all_of<LocalComponent>(*entity))
            return;
        if (!entity)
        {
            auto forms = m_world.view<FormIdComponent>();
            const auto it = std::find_if(forms.begin(), forms.end(), [forms, formId](auto e) { return forms.get<FormIdComponent>(e).Id == formId; });
            entity = it == forms.end() ? m_world.create() : *it;
        }
        DeleteRemoteEntityComponents(*entity);
        m_world.remove<FormIdComponent, WaitingForAssignmentComponent>(*entity);
        m_world.emplace_or_replace<RemoteComponent>(*entity, state.ServerId, formId, state.OwnershipEpoch);
        m_world.emplace_or_replace<LeaderNativeClaim>(*entity).FormId = formId;
        m_loadedActorLocations.try_emplace(formId, state);
        pActor->GetExtension()->SetRemote(false);
        spdlog::info("Bound native leader identity actor {:X} server {:X} epoch {}", formId, state.ServerId, state.OwnershipEpoch);
        return;
    }
    if (entity && acMessage.LeaderPlayerId == m_transport.GetLocalPlayerId())
    {
        if (auto* pClaim = m_world.try_get<LeaderNativeClaim>(*entity))
        {
            if (state.Phase == ScriptedActorPhase::Park)
            {
                pClaim->Parked = true;
                m_world.remove<FormIdComponent, LocalAnimationComponent, CacheComponent>(*entity);
            }
            else if (state.Phase == ScriptedActorPhase::Resume)
            {
                pClaim->Parked = false;
                pClaim->ResumeSent = false;
            }
            else if (state.Phase == ScriptedActorPhase::Release)
            {
                m_loadedActorLocations.erase(formId);
                m_world.remove<LeaderNativeClaim>(*entity);
                m_world.emplace_or_replace<FormIdComponent>(*entity, formId);
                OnActorRemoved(ActorRemovedEvent(formId));
                if (IsLoadedActor(Cast<Actor>(TESForm::GetById(formId))))
                    OnActorAdded(ActorAddedEvent(formId));
            }
        }
        return;
    }
    auto parked = m_parkedActors.find(formId);
    if (state.Phase != ScriptedActorPhase::Park)
    {
        if (parked != m_parkedActors.end() && parked->second.Message.State.ServerId == state.ServerId &&
            parked->second.Message.State.OwnershipEpoch <= state.OwnershipEpoch)
            parked->second.Message = acMessage;
        return;
    }
    if (!party.IsInParty() || party.IsLeader() || party.GetLeaderPlayerId() != acMessage.LeaderPlayerId)
        return;
    if (parked != m_parkedActors.end())
    {
        if (parked->second.Message.State.OwnershipEpoch > state.OwnershipEpoch)
            return;
        parked->second.Message = acMessage;
        return;
    }
    if (!entity)
    {
        auto forms = m_world.view<FormIdComponent>();
        const auto it = std::find_if(forms.begin(), forms.end(), [forms, formId](auto e) { return forms.get<FormIdComponent>(e).Id == formId; });
        entity = it == forms.end() ? m_world.create() : *it;
    }
    DeleteRemoteEntityComponents(*entity);
    m_world.remove<LocalComponent, LocalAnimationComponent, LeaderNativeClaim, DeferredAssignmentComponent>(*entity);
    // Keep network identity, but exclude the parked actor from every service's
    // form-based simulation and remote replay views until it is restored.
    m_world.remove<FormIdComponent>(*entity);
    m_world.emplace_or_replace<RemoteComponent>(*entity, state.ServerId, formId, state.OwnershipEpoch);
    m_parkedActors[formId].Entity = *entity;
    m_parkedActors[formId].Message = acMessage;
    m_pendingLeveledConforms.erase(formId);
    m_weaponDrawUpdates.erase(formId);
    if (auto* pActor = Cast<Actor>(TESForm::GetById(formId)))
    {
        pActor->GetExtension()->SetRemote(true);
        pActor->GetExtension()->Reconciliation = ActorExtension::ReconciliationStage::None;
        VisualPoseMailbox::Clear(&pActor->animationGraphHolder);
    }
    spdlog::info("Follower parked actor {:X} server {:X} epoch {}", formId, state.ServerId, state.OwnershipEpoch);
}

void CharacterService::RunScriptedActorUpdates() noexcept
{
    const auto& party = m_world.GetPartyService();
    for (auto it = m_parkedActors.begin(); it != m_parkedActors.end();)
    {
        auto& parked = it->second;
        auto& state = parked.Message.State;
        if (!m_transport.IsOnline() || !party.IsInParty() || party.GetLeaderPlayerId() != parked.Message.LeaderPlayerId || party.IsLeader())
            state.Phase = ScriptedActorPhase::Release;
        auto* pActor = Cast<Actor>(TESForm::GetById(it->first));
        if (pActor && !pActor->IsDeleted())
        {
            // Actor::Disable (ID 37267, VA 1406754E0) queues ID 36978.
            // Wait for that task before undoing our disable on resume/cleanup.
            if (parked.DisablePending && pActor->IsDisabled() && !pActor->GetNiNode())
                parked.DisablePending = false;
            if (state.Phase == ScriptedActorPhase::Park && !pActor->IsDisabled() && !parked.DisablePending)
            {
                parked.DisabledByUs = true;
                parked.DisablePending = true;
                pActor->DisableImpl();
            }
            if (state.Phase == ScriptedActorPhase::Park || parked.DisablePending)
            {
                ++it;
                continue;
            }
            if (state.Phase == ScriptedActorPhase::Resume)
                MoveActor(pActor, state.WorldSpaceId, state.CellId, state.Position);
            if ((parked.DisabledByUs || state.Phase == ScriptedActorPhase::Resume) && pActor->IsDisabled())
                pActor->EnableImpl();
        }
        else if (state.Phase == ScriptedActorPhase::Park)
        {
            ++it;
            continue;
        }
        const uint32_t formId = it->first;
        const uint32_t serverId = state.ServerId;
        const auto parkedEntity = parked.Entity;
        const bool resume = state.Phase == ScriptedActorPhase::Resume;
        it = m_parkedActors.erase(it);
        if (pActor && !m_transport.IsOnline())
            pActor->GetExtension()->SetRemote(false);
        if (m_world.valid(parkedEntity))
        {
            m_world.remove<WaitingForAssignmentComponent>(parkedEntity);
            m_world.emplace_or_replace<FormIdComponent>(parkedEntity, formId);
            if (m_restoredOwnershipGrants.find(serverId) == m_restoredOwnershipGrants.end())
            {
                if (resume && m_transport.IsOnline())
                {
                    // Retain remote identity during restoration, including for
                    // disconnect cleanup. Refresh the snapshot once 3D exists.
                    if (IsLoadedActor(pActor))
                    {
                        CacheSystem::Setup(m_world, parkedEntity, pActor);
                        RequestServerAssignment(parkedEntity);
                    }
                }
                else
                {
                    DeleteRemoteEntityComponents(parkedEntity);
                    if (pActor)
                        pActor->GetExtension()->SetRemote(false);
                    if (m_transport.IsOnline() && IsLoadedActor(pActor))
                        ProcessNewEntity(parkedEntity);
                }
            }
        }
        spdlog::info("Follower parking cleared actor {:X} server {:X} resume={}", formId, serverId, resume);
    }

    for (auto it = m_restoredOwnershipGrants.begin(); it != m_restoredOwnershipGrants.end();)
    {
        const auto entity = Utils::FindEntityByServerId(it->first);
        const auto* pRemote = entity ? m_world.try_get<RemoteComponent>(*entity) : nullptr;
        auto* pActor = pRemote ? Cast<Actor>(TESForm::GetById(pRemote->CachedRefId)) : nullptr;
        const auto* pCell = pActor ? pActor->GetParentCellEx() : nullptr;
        if (pRemote && (IsActorDiscoverySuppressed(pRemote->CachedRefId) ||
            (it->second.OwnerPlayerId == m_transport.GetLocalPlayerId() && pActor &&
                !pActor->IsDeleted() && !pActor->IsDisabled() && pCell && pCell->IsAttached() && !pActor->GetNiNode())))
        {
            ++it;
            continue;
        }
        const auto grant = it->second;
        it = m_restoredOwnershipGrants.erase(it);
        OnOwnershipTransfer(grant);
    }

    auto claims = m_world.view<LeaderNativeClaim>();
    Vector<entt::entity> entities(claims.begin(), claims.end());
    for (const auto entity : entities)
    {
        const auto formId = m_world.get<LeaderNativeClaim>(entity).FormId;
        auto* pActor = Cast<Actor>(TESForm::GetById(formId));
        if (!IsLeaderNativeActor(pActor))
        {
            m_world.remove<LeaderNativeClaim>(entity);
            m_world.emplace_or_replace<FormIdComponent>(entity, formId);
            CancelServerAssignment(entity, formId);
            if (IsLoadedActor(pActor))
                ProcessNewEntity(entity);
            continue;
        }
        auto* pLocal = m_world.try_get<LocalComponent>(entity);
        if (!pLocal)
            continue;
        auto& claim = m_world.get<LeaderNativeClaim>(entity);
        if (IsLoadedActor(pActor))
        {
            if (claim.Parked)
            {
                if (!claim.ResumeSent)
                {
                    RequestScriptedActorState request;
                    request.State = GetActorLocation(m_world, pActor);
                    request.State.ServerId = pLocal->Id;
                    request.State.OwnershipEpoch = pLocal->OwnershipEpoch;
                    request.State.Phase = ScriptedActorPhase::Resume;
                    if (request.State.CellId == GameId{})
                        continue;
                    claim.ResumeSent = m_transport.Send(request);
                    spdlog::info("Leader return actor {:X} server {:X} epoch {}", pActor->formID, pLocal->Id, pLocal->OwnershipEpoch);
                }
                continue;
            }
            if (!m_world.all_of<LocalAnimationComponent>(entity))
            {
                m_world.emplace_or_replace<FormIdComponent>(entity, formId);
                CacheSystem::Setup(m_world, entity, pActor);
                m_world.emplace<LocalAnimationComponent>(entity);
                spdlog::info("Pending leader actor ready {:X} server {:X} epoch {}", pActor->formID, pLocal->Id, pLocal->OwnershipEpoch);
            }
        }
        else if (!claim.Parked)
        {
            const auto location = m_loadedActorLocations.find(formId);
            if (pActor->IsDead() || pActor->IsDeleted() || (location != m_loadedActorLocations.end() &&
                location->second.CellId != GameId{} && !IsLocationInPlayerRange(m_world, location->second, pActor->IsDragon())))
            {
                m_world.remove<LeaderNativeClaim>(entity);
                m_world.emplace_or_replace<FormIdComponent>(entity, formId);
                OnActorRemoved(ActorRemovedEvent(formId));
            }
            else
                TryParkActor(entity, pActor);
        }
    }
}

uint32_t CharacterService::GetPresentationDelayMs() const noexcept
{
    return m_presentationDelayMs.load(std::memory_order_acquire);
}

void CharacterService::SetVehicleTrialRiderId(uint32_t aRiderId) noexcept
{
    m_vehicleTrialRiderId.store(aRiderId, std::memory_order_release);
}

uint32_t CharacterService::GetVehicleTrialRiderId() const noexcept
{
    return m_vehicleTrialRiderId.load(std::memory_order_acquire);
}

uint64_t CharacterService::GetVehicleTrialCalls() const noexcept
{
    return m_vehicleTrialCalls;
}

uint64_t CharacterService::GetVehicleTrialImmediateSeats() const noexcept
{
    return m_vehicleTrialImmediateSeats;
}

uint32_t CharacterService::GetVehicleTrialImmediateHandle() const noexcept
{
    return m_vehicleTrialImmediateHandle;
}

CharacterService::MountDiagnostic CharacterService::GetMountDiagnostic() const noexcept
{
    return {static_cast<uint32_t>(m_pendingMounts.size()),
        m_mountNotifications, m_mountWaitedFor3D, m_mountApplied,
        m_mountSeated, m_mountRejected, m_lastMountRiderId, m_lastMountId};
}

CharacterService::LocalPoseProductionDiagnostic
CharacterService::GetLocalPoseProductionDiagnostic() const noexcept
{
    return {m_localPoseBatches.load(std::memory_order_relaxed),
        m_localPoseActors.load(std::memory_order_relaxed),
        m_localPoseTotalUs.load(std::memory_order_relaxed),
        m_localPoseMaxActorUs.load(std::memory_order_relaxed),
        m_localPoseLastBatchUs.load(std::memory_order_relaxed),
        m_localPoseLastBatchActors.load(std::memory_order_relaxed)};
}

Vector<CharacterService::MountRelationDiagnostic>
CharacterService::GetPendingMountRelations() const noexcept
{
    Vector<MountRelationDiagnostic> relations;
    relations.reserve(m_pendingMounts.size());
    for (const auto& [riderId, pending] : m_pendingMounts)
    {
        auto* pRider = Utils::GetByServerId<Actor>(riderId);
        auto* pMount = Utils::GetByServerId<Actor>(pending.MountId);
        const auto native = pRider ? pRider->GetNativeMountState() :
            Actor::NativeMountState{};
        relations.push_back({riderId, pending.MountId,
            pRider ? pRider->formID : 0,
            pMount ? pMount->formID : 0,
            pRider ? pRider->GetNativeMountFormId() : 0,
            pRider ? pRider->someRefrHandle : 0,
            native.HorseExtra, native.HorseHandle,
            native.InteractionExtra, native.InteractionPointerPresent,
            native.InteractionActorHandle, native.InteractionTargetHandle,
            pRider && pRider->GetNiNode(),
            pMount && pMount->GetNiNode(),
            pending.WasSeated, pending.Attempts});
    }
    return relations;
}

void CharacterService::ClearMountRelationsForServerId(uint32_t aServerId) noexcept
{
    for (auto it = m_pendingMounts.begin(); it != m_pendingMounts.end();)
    {
        if (it->first == aServerId || it->second.MountId == aServerId)
            it = m_pendingMounts.erase(it);
        else
            ++it;
    }
}

void CharacterService::DeleteRemoteEntityComponents(entt::entity aEntity) const noexcept
{
    m_world.remove<FaceGenComponent, InterpolationComponent, RemoteAnimationComponent, RemoteComponent, CacheComponent, WaitingFor3D, PlayerComponent>(aEntity);
}

void CharacterService::DeclineOwnership(const uint32_t aServerId, const uint32_t aOwnershipEpoch) const noexcept
{
    RequestOwnershipTransfer request{};
    request.ServerId = aServerId;
    request.OwnershipEpoch = aOwnershipEpoch;
    request.Reason = OwnershipReleaseReason::DeclineGrant;
    m_transport.Send(request);
}

void CharacterService::ReconcileActorData(
    const entt::entity aEntity, Actor* apActor, const uint32_t aOwnershipEpoch, const ActorData& acActorData, const bool aApplyInventory, const bool aIsLocalOwner, const bool aInitialNativeAssignment) noexcept
{
    if (auto* pWaitingFor3D = m_world.try_get<WaitingFor3D>(aEntity))
    {
        pWaitingFor3D->SpawnRequest.InitialActorValues = acActorData.InitialActorValues;
        pWaitingFor3D->SpawnRequest.InventoryContent = acActorData.InitialInventory;
        pWaitingFor3D->SpawnRequest.IsDead = acActorData.IsDead;
        pWaitingFor3D->SpawnRequest.IsWeaponDrawn = acActorData.IsWeaponDrawn;
        pWaitingFor3D->SpawnRequest.OwnershipEpoch = aOwnershipEpoch;
    }

    if (!apActor)
        return;

    apActor->SetActorValues(acActorData.InitialActorValues);

    if (aApplyInventory)
    {
        const Inventory currentInventory = apActor->GetActorInventory();
        // A native actor can equip its default outfit after the discovery
        // snapshot was sent.  An empty assignment snapshot is not evidence
        // that the host deliberately stripped that actor: RemoveAllItems()
        // here would erase the outfit on both the owner and its peers.
        const bool incompleteSnapshot = aInitialNativeAssignment && !acActorData.IsDead &&
            acActorData.InitialInventory.Entries.empty() &&
            !currentInventory.Entries.empty();
        if (incompleteSnapshot)
        {
            spdlog::warn("Preserved native inventory for actor {:X}: empty assignment snapshot, {} current entries",
                apActor->formID, currentInventory.Entries.size());
            if (currentInventory.CurrentMagicEquipment != acActorData.InitialInventory.CurrentMagicEquipment)
                apActor->SetMagicEquipment(acActorData.InitialInventory.CurrentMagicEquipment);
        }
        else if (currentInventory.Entries != acActorData.InitialInventory.Entries || currentInventory.CurrentMagicEquipment != acActorData.InitialInventory.CurrentMagicEquipment)
            apActor->SetActorInventory(acActorData.InitialInventory);
    }

    if (apActor->IsDead() != acActorData.IsDead)
        acActorData.IsDead ? apActor->Kill() : apActor->Respawn();

    if (aIsLocalOwner)
    {
        // A remote draw correction may still be queued when an ownership grant arrives.
        m_weaponDrawUpdates.erase(apActor->formID);

        if (apActor->actorState.IsWeaponDrawn() != acActorData.IsWeaponDrawn)
            apActor->SetWeaponDrawnEx(acActorData.IsWeaponDrawn);
    }
    else
        m_weaponDrawUpdates[apActor->formID] = {acActorData.IsWeaponDrawn};
}

bool CharacterService::RequestOwnership(const uint32_t aFormId, const uint32_t aServerId, const entt::entity aEntity) const noexcept
{
    if (IsActorDiscoverySuppressed(aFormId))
        return false;
    Actor* pActor = Cast<Actor>(TESForm::GetById(aFormId));
    if (!pActor)
    {
        spdlog::warn("Cannot request ownership of actor {:X} because its form {:X} is unavailable", aServerId, aFormId);
        return false;
    }

    ActorExtension* pExtension = pActor->GetExtension();
    if (pExtension->IsRemotePlayer())
    {
        spdlog::warn("Cannot request ownership of remote player actor {:X}", aServerId);
        return false;
    }

    if (pActor->IsPlayerSummon())
    {
        spdlog::warn("Cannot request ownership of remote player summon {:X}", aServerId);
        return false;
    }

    const auto* pRemoteComponent = m_world.try_get<RemoteComponent>(aEntity);
    if (!pRemoteComponent || pRemoteComponent->Id != aServerId || pRemoteComponent->OwnershipEpoch == 0)
        return false;

    RequestOwnershipClaim request;
    request.ServerId = aServerId;
    request.ExpectedOwnershipEpoch = pRemoteComponent->OwnershipEpoch;

    if (!m_transport.Send(request))
        return false;

    return true;
}

void CharacterService::DeleteTempActor(const uint32_t aFormId) noexcept
{
    Actor* pActor = Cast<Actor>(TESForm::GetById(aFormId));
    if (pActor && ((pActor->formID & 0xFF000000) == 0xFF000000))
    {
        pActor->Delete();
        spdlog::info("\tDeleted actor {:X}", aFormId);
    }
}

void CharacterService::OnActorAdded(const ActorAddedEvent& acEvent) noexcept
{
    if (IsActorDiscoverySuppressed(acEvent.FormId))
        return;
    Actor* pActor = Cast<Actor>(TESForm::GetById(acEvent.FormId));
    if (!pActor)
        return;

    for (const auto entity : m_world.view<LeaderNativeClaim>())
        if (m_world.get<LeaderNativeClaim>(entity).FormId == acEvent.FormId)
            return;

    // Parked and pending native bindings retain their form across discovery.
    auto forms = m_world.view<FormIdComponent>();
    const auto existing = std::find_if(forms.begin(), forms.end(), [forms, &acEvent](auto e) { return forms.get<FormIdComponent>(e).Id == acEvent.FormId; });
    if (existing != forms.end())
    {
        if (const auto* pRemote = m_world.try_get<RemoteComponent>(*existing); pRemote &&
            !m_world.any_of<InterpolationComponent, WaitingForAssignmentComponent>(*existing))
        {
            if (m_restoredOwnershipGrants.find(pRemote->Id) == m_restoredOwnershipGrants.end())
            {
                CacheSystem::Setup(m_world, *existing, pActor);
                RequestServerAssignment(*existing);
            }
            return;
        }
        ProcessNewEntity(*existing);
        return;
    }

    if (acEvent.FormId == 0x14)
    {
        pActor->GetExtension()->SetPlayer(true);
    }

    entt::entity entity;

    const auto view = m_world.view<RemoteComponent>();
    const auto it = std::find_if(
        std::begin(view), std::end(view),
        [&acEvent, view](entt::entity entity)
        {
            auto& remoteComponent = view.get<RemoteComponent>(entity);
            return remoteComponent.CachedRefId == acEvent.FormId;
        });

    if (it != std::end(view))
    {
        Actor* pActor = Cast<Actor>(TESForm::GetById(acEvent.FormId));
        pActor->GetExtension()->SetRemote(!m_world.all_of<LeaderNativeClaim>(*it));

        entity = *it;
    }
    else
        entity = m_world.create();

    m_world.emplace_or_replace<FormIdComponent>(entity, acEvent.FormId);
    m_world.emplace_or_replace<EarlyAnimationBufferComponent>(entity);

    ProcessNewEntity(entity);
}

void CharacterService::OnActorRemoved(const ActorRemovedEvent& acEvent) noexcept
{
    if (IsActorDiscoverySuppressed(acEvent.FormId))
        return;
    if (auto* pActor = Cast<Actor>(TESForm::GetById(acEvent.FormId)))
        pActor->GetExtension()->Reconciliation = ActorExtension::ReconciliationStage::None;

    m_pendingLeveledConforms.erase(acEvent.FormId);

    auto view = m_world.view<FormIdComponent>();
    const auto entityIt = std::find_if(view.begin(), view.end(), [view, formId = acEvent.FormId](auto aEntity) { return view.get<FormIdComponent>(aEntity).Id == formId; });

    if (entityIt == view.end())
    {
        for (const auto entity : m_world.view<LeaderNativeClaim>())
            if (m_world.get<LeaderNativeClaim>(entity).FormId == acEvent.FormId)
                return;
        spdlog::error("Actor to remove not found in form ids map {:X}", acEvent.FormId);
        return;
    }

    const auto cId = *entityIt;

    if (auto* pActor = Cast<Actor>(TESForm::GetById(acEvent.FormId)))
    {
        const auto* pClaim = m_world.try_get<LeaderNativeClaim>(cId);
        if ((pClaim && (pClaim->Parked || !m_world.all_of<LocalComponent>(cId))) || TryParkActor(cId, pActor))
            return;
        const auto location = m_loadedActorLocations.find(acEvent.FormId);
        if (m_world.all_of<WaitingForAssignmentComponent>(cId) && IsLeaderNativeActor(pActor) &&
            !pActor->IsDead() && !pActor->IsDeleted() && location != m_loadedActorLocations.end() &&
            IsLocationInPlayerRange(m_world, location->second, pActor->IsDragon()))
        {
            m_world.emplace_or_replace<LeaderNativeClaim>(cId).FormId = acEvent.FormId;
            return;
        }
    }
    m_loadedActorLocations.erase(acEvent.FormId);
    m_world.remove<LeaderNativeClaim>(cId);

    auto& formIdComponent = view.get<FormIdComponent>(cId);
    CancelServerAssignment(*entityIt, formIdComponent.Id);

    m_world.remove<EarlyAnimationBufferComponent, DeferredAssignmentComponent>(cId);

    if (m_world.all_of<FormIdComponent>(cId))
        m_world.remove<FormIdComponent>(cId);

    if (m_world.orphan(cId))
        m_world.destroy(cId);

    spdlog::info("Actor removed, form id: {:X}", acEvent.FormId);
}

void CharacterService::OnUpdate(const UpdateEvent& acUpdateEvent) noexcept
{
    RunScriptedActorUpdates();
    EngineFixes::OnFrame();
    static uint64_t s_nextScriptedPackageMs = 0;
    if (const auto packageNow = GetTickCount64(); packageNow >= s_nextScriptedPackageMs)
    {
        s_nextScriptedPackageMs = packageNow + 100;
        UpdateLeaderScriptedPackage();
    }
    SendCreatorAppearance();
    PoseCopyAuthority::SetCurrentTick(SmoothClock::NowTick() ? SmoothClock::NowTick() : m_transport.GetClock().GetCurrentTick());
    static uint64_t s_nextPoseRegistryMs = 0;
    if (const auto registryNow = GetTickCount64(); registryNow >= s_nextPoseRegistryMs)
    {
        s_nextPoseRegistryMs = registryNow + 250;
        PoseCopyAuthority::RefreshRegistry(m_world);
        PlayerCollision::Update(m_world);
    }

    // Discovery emits ActorAddedEvent only once per high-process lifetime.
    // New Game can expose the player (and scene actors) before their cell is
    // attached, so an assignment deferred then otherwise never retries.
    // Revisit only still-unassigned actors, at a bounded cadence.
    const auto nowMs = GetTickCount64();
    if (m_transport.IsOnline() && nowMs >= m_nextDeferredAssignmentRetryMs)
    {
        m_nextDeferredAssignmentRetryMs = nowMs + 1000;
        auto unassigned = m_world.view<FormIdComponent, DeferredAssignmentComponent>(entt::exclude<
            ObjectComponent, RemoteComponent, LocalComponent,
            WaitingForAssignmentComponent>);
        Vector<entt::entity> deferred(unassigned.begin(), unassigned.end());
        for (const auto entity : deferred)
        {
            const auto formId = m_world.get<FormIdComponent>(entity).Id;
            auto* pActor = Cast<Actor>(TESForm::GetById(formId));
            if (pActor && pActor->parentCell && !pActor->IsDeleted())
            {
                m_world.remove<DeferredAssignmentComponent>(entity);
                spdlog::info("Retrying deferred cell assignment for actor {:X}", formId);
                ProcessNewEntity(entity);
            }
        }
    }
    RunSpawnUpdates();
    RunLocalMountUpdates();
    RunPendingMounts();
    RunLocalUpdates();
    RunFactionsUpdates();
    RunRemoteUpdates();
    RunPresentationEvents();
    RunExperienceUpdates();
    ApplyCachedWeaponDraws(acUpdateEvent);
    ProcessLeveledConforms();
}

void CharacterService::OnConnected(const ConnectedEvent& acConnectedEvent) const noexcept
{
    // Go through all the forms that were previously detected
    auto view = m_world.view<FormIdComponent>(entt::exclude<ObjectComponent>);
    Vector<entt::entity> entities(view.begin(), view.end());

    for (auto entity : entities)
    {
        auto& formIdComponent = m_world.get<FormIdComponent>(entity);
        // Delete all temporary actors on connect
        if (formIdComponent.Id > 0xFF000000)
        {
            Actor* pActor = Cast<Actor>(TESForm::GetById(formIdComponent.Id));
            if (pActor)
                pActor->Delete();

            continue;
        }

        ProcessNewEntity(entity);
    }
}

void CharacterService::OnDisconnected(const DisconnectedEvent& acDisconnectedEvent) noexcept
{
    m_loadedActorLocations.clear();
    m_restoredOwnershipGrants.clear();
    for (const auto entity : m_world.view<LeaderNativeClaim>())
        m_world.emplace_or_replace<FormIdComponent>(entity, m_world.get<LeaderNativeClaim>(entity).FormId);
    m_world.clear<LeaderNativeClaim>();
    for (auto& [formId, parked] : m_parkedActors)
        parked.Message.State.Phase = ScriptedActorPhase::Release;
    m_nextDeferredAssignmentRetryMs = 0;
    VisualPoseMailbox::SetPresentationTick(0);
    VisualPoseMailbox::SetApplyEnabled(false);
    VisualPoseMailbox::SetApplyFormId(0);
    m_pendingVoices.clear();
    m_pendingSubtitles.clear();
    m_pendingMounts.clear();
    m_vehicleTrialRiderId.store(0, std::memory_order_release);
    m_localMountSent.clear();
    auto remoteView = m_world.view<FormIdComponent, RemoteComponent>();
    for (auto entity : remoteView)
    {
        auto& formIdComponent = remoteView.get<FormIdComponent>(entity);

        auto pActor = Cast<Actor>(TESForm::GetById(formIdComponent.Id));
        if (!pActor)
            continue;

        VisualPoseMailbox::Clear(&pActor->animationGraphHolder);

        if (pActor->GetExtension()->IsRemotePlayer())
            pActor->Delete();
        else
            pActor->GetExtension()->SetRemote(false);
    }

    m_world.clear<WaitingForAssignmentComponent, DeferredAssignmentComponent,
        LocalComponent, RemoteComponent>();

    for (const auto& [formId, pickFormId] : m_pendingLeveledConforms)
    {
        if (auto* pActor = Cast<Actor>(TESForm::GetById(formId)))
        {
            auto& stage = pActor->GetExtension()->Reconciliation;
            // Don't leave the actor disabled if we disconnect before re-enabling it.
            if (stage == ActorExtension::ReconciliationStage::WaitingForDisable && !pActor->IsDeleted())
                pActor->EnableImpl();

            stage = ActorExtension::ReconciliationStage::None;
        }
    }

    m_pendingLeveledConforms.clear();
}

void CharacterService::OnAssignCharacter(const AssignCharacterResponse& acMessage) noexcept
{
    spdlog::info("Received for cookie {:X}, server id {:X}", acMessage.Cookie, acMessage.ServerId);

    auto view = m_world.view<WaitingForAssignmentComponent>();
    const auto itor = std::find_if(std::begin(view), std::end(view), [view, cookie = acMessage.Cookie](auto entity) { return view.get<WaitingForAssignmentComponent>(entity).Cookie == cookie; });

    if (itor == std::end(view))
    {
        spdlog::warn("Never found requested cookie: {}", acMessage.Cookie);
        return;
    }

    const auto cEntity = *itor;
    const bool isCancelled = view.get<WaitingForAssignmentComponent>(cEntity).Cancelled;

    m_world.remove<WaitingForAssignmentComponent>(cEntity);
#if (!IS_MASTER)
    m_world.remove<ReplayedActionsDebugComponent>(cEntity);
#endif

    if (const auto* pRemote = m_world.try_get<RemoteComponent>(cEntity); pRemote && IsActorDiscoverySuppressed(pRemote->CachedRefId))
        return;
    if (isCancelled)
    {
        if (acMessage.Owner)
            DeclineOwnership(acMessage.ServerId, acMessage.OwnershipEpoch);

        if (m_world.valid(cEntity))
            m_world.destroy(cEntity);

        return;
    }

    if (acMessage.OwnershipEpoch == 0)
    {
        spdlog::warn("Ignored assignment for actor {:X} because the server returned an invalid ownership epoch", acMessage.ServerId);
        return;
    }

    const auto formIdComponent = m_world.try_get<FormIdComponent>(cEntity);
    if (!formIdComponent)
    {
        if (acMessage.Owner)
            DeclineOwnership(acMessage.ServerId, acMessage.OwnershipEpoch);

        if (m_world.valid(cEntity))
            m_world.destroy(cEntity);

        spdlog::warn("Discarded assignment for actor {:X} because the local entity no longer has a form", acMessage.ServerId);
        return;
    }

    Actor* pActor = Cast<Actor>(TESForm::GetById(formIdComponent->Id));
    if (!pActor)
    {
        if (acMessage.Owner)
            DeclineOwnership(acMessage.ServerId, acMessage.OwnershipEpoch);

        spdlog::warn("Discarded assignment for actor {:X} because form {:X} is unavailable", acMessage.ServerId, formIdComponent->Id);
        m_world.destroy(cEntity);
        return;
    }

    // TODO: how could this possibly trigger?
    // it's kinda interfering with my WaitingFor3D code
    if (acMessage.PlayerId != 0)
        m_world.emplace_or_replace<PlayerComponent>(cEntity, acMessage.PlayerId);

    ActorData actorData{};
    actorData.InitialActorValues = acMessage.AllActorValues;
    actorData.InitialInventory = acMessage.CurrentInventory;
    actorData.IsDead = acMessage.IsDead;
    actorData.IsWeaponDrawn = acMessage.IsWeaponDrawn;

    if (IsActorDiscoverySuppressed(pActor->formID))
    {
        m_world.emplace_or_replace<RemoteComponent>(cEntity, acMessage.ServerId, pActor->formID, acMessage.OwnershipEpoch);
        return;
    }

    if (!acMessage.Owner && IsLeaderNativeActor(pActor))
    {
        ScriptedActorState anchor;
        anchor.CellId = acMessage.CellId;
        anchor.WorldSpaceId = acMessage.WorldSpaceId;
        anchor.Position = acMessage.Position;
        if (IsLocationInPlayerRange(m_world, anchor, pActor->IsDragon()) || acMessage.CellId == GameId{})
        {
            m_loadedActorLocations.try_emplace(pActor->formID, anchor);
            m_world.emplace_or_replace<RemoteComponent>(cEntity, acMessage.ServerId, pActor->formID, acMessage.OwnershipEpoch);
            m_world.emplace_or_replace<LeaderNativeClaim>(cEntity).FormId = pActor->formID;
            m_world.remove<InterpolationComponent, RemoteAnimationComponent, WaitingFor3D>(cEntity);
            m_world.remove<FormIdComponent>(cEntity);
            pActor->GetExtension()->SetRemote(false);
            RequestOwnership(pActor->formID, acMessage.ServerId, cEntity);
            spdlog::info("Pending leader assignment actor {:X} server {:X} epoch {}", pActor->formID, acMessage.ServerId, acMessage.OwnershipEpoch);
            return;
        }
    }

    if (acMessage.Owner)
    {
        spdlog::info("Received local actor, form id: {:X}", pActor->formID);

        // The owner's assignment echoes an earlier snapshot of this very actor, which the live native
        // actor is newer than: apply none of it, and never mark it remote for the moment (its native
        // processing, actions and moves would be suppressed). A death the server knows of still applies.
        m_weaponDrawUpdates.erase(pActor->formID);
        if (!IsLeaderNativeActor(pActor) && acMessage.IsDead && !pActor->IsDead())
            pActor->Kill();

        auto& localAnimationComponent = m_world.emplace_or_replace<LocalAnimationComponent>(cEntity);

        if (auto* pEarlyAnimComponent = m_world.try_get<EarlyAnimationBufferComponent>(cEntity))
        {
            for (const auto& action : pEarlyAnimComponent->Actions)
            {
                localAnimationComponent.Append(action);
            }
        }
        m_world.remove<EarlyAnimationBufferComponent>(cEntity);

        auto& localComponent = m_world.emplace_or_replace<LocalComponent>(cEntity, acMessage.ServerId, acMessage.OwnershipEpoch);
        localComponent.IsDead = acMessage.IsDead;
        localComponent.IsWeaponDrawn = acMessage.IsWeaponDrawn;
        if (IsLeaderNativeActor(pActor))
        {
            ScriptedActorState anchor;
            anchor.CellId = acMessage.CellId;
            anchor.WorldSpaceId = acMessage.WorldSpaceId;
            anchor.Position = acMessage.Position;
            m_loadedActorLocations.try_emplace(pActor->formID, anchor);
            m_world.emplace_or_replace<LeaderNativeClaim>(cEntity).FormId = pActor->formID;
            localComponent.IsDead = pActor->IsDead();
            localComponent.IsWeaponDrawn = pActor->actorState.IsWeaponDrawn();
            if (!IsLoadedActor(pActor))
            {
                m_world.remove<LocalAnimationComponent>(cEntity);
                TryParkActor(cEntity, pActor);
                m_world.remove<FormIdComponent>(cEntity);
            }
        }
    }
    else
    {
        spdlog::info("Received remote actor, form id: {:X}, isweapondrawn: {}", pActor->formID, acMessage.IsWeaponDrawn);

        // A server spawn may beat the reply to this native temporary actor's
        // assignment. Keep the native reference (and its quest alias/scripts)
        // and retire only the synthetic proxy for the same server entity.
        if (pActor->IsTemporary())
        {
            // The synthetic proxy may still be waiting for ActorAddedEvent,
            // so it can have only RemoteComponent::CachedRefId at this point.
            // Requiring FormIdComponent missed that race and left two actors
            // bound to one server ID on the follower.
            const auto remotes = m_world.view<RemoteComponent>();
            Vector<entt::entity> duplicateProxies;
            for (const auto candidate : remotes)
            {
                if (candidate != cEntity && remotes.get<RemoteComponent>(candidate).Id == acMessage.ServerId)
                    duplicateProxies.push_back(candidate);
            }
            for (const auto proxy : duplicateProxies)
            {
                const auto* pProxyForm = m_world.try_get<FormIdComponent>(proxy);
                const uint32_t proxyFormId = pProxyForm ? pProxyForm->Id :
                    m_world.get<RemoteComponent>(proxy).CachedRefId;
                auto* pProxy = Cast<Actor>(TESForm::GetById(proxyFormId));
                if (!pProxy || !pProxy->IsTemporary() || proxyFormId == pActor->formID)
                    continue;
                DeleteRemoteEntityComponents(proxy);
                VisualPoseMailbox::Clear(&pProxy->animationGraphHolder);
                DeleteTempActor(proxyFormId);
                if (!pProxyForm && m_world.valid(proxy) && m_world.orphan(proxy))
                    m_world.destroy(proxy);
                spdlog::info("Retired early temporary proxy {:X} for native {:X}, server {:X}",
                    proxyFormId, pActor->formID, acMessage.ServerId);
            }
        }

        const auto mountBefore = pActor->GetNativeMountState();
        const bool traceMountedAssignment = mountBefore.InteractionExtra ||
            mountBefore.HorseExtra;
        if (traceMountedAssignment)
            spdlog::info("Mounted actor assignment before reconcile {:X}: nativeMount={:X} interaction={} horse={} at=({}, {}, {}) target=({}, {}, {})",
                pActor->formID, pActor->GetNativeMountFormId(),
                mountBefore.InteractionExtra, mountBefore.HorseExtra,
                pActor->position.x, pActor->position.y, pActor->position.z,
                acMessage.Position.x, acMessage.Position.y, acMessage.Position.z);

        m_world.emplace_or_replace<RemoteComponent>(cEntity, acMessage.ServerId, formIdComponent->Id, acMessage.OwnershipEpoch);

        pActor->GetExtension()->SetRemote(true);

        m_world.remove<EarlyAnimationBufferComponent>(cEntity);
        InterpolationSystem::Setup(m_world, cEntity);
        AnimationSystem::Setup(m_world, cEntity);
        AnimationSystem::AddActionsForReplay(m_world.get<RemoteAnimationComponent>(cEntity), acMessage.ActionsToReplay);

#if (!IS_MASTER)
        m_world.emplace_or_replace<ReplayedActionsDebugComponent>(cEntity, acMessage.ActionsToReplay);
#endif

        ReconcileActorData(cEntity, pActor, acMessage.OwnershipEpoch, actorData, true, false,
            !acMessage.InventoryAuthoritative);

        if (traceMountedAssignment)
            spdlog::info("Mounted actor assignment after reconcile {:X}: nativeMount={:X} interaction={}",
                pActor->formID, pActor->GetNativeMountFormId(),
                pActor->GetNativeMountState().InteractionExtra);

        MoveActor(pActor, acMessage.WorldSpaceId, acMessage.CellId, acMessage.Position);

        if (traceMountedAssignment)
            spdlog::info("Mounted actor assignment after move {:X}: nativeMount={:X} interaction={}",
                pActor->formID, pActor->GetNativeMountFormId(),
                pActor->GetNativeMountState().InteractionExtra);

        // The owner's leveled pick rides the assignment response for actors we discovered ourselves
        ApplyLeveledNpcPick(pActor, acMessage.LeveledNpcPickId);
    }
    if (!acMessage.Owner && acMessage.MountedOnServerId)
        m_pendingMounts[acMessage.ServerId] = {acMessage.MountedOnServerId, 0, 0};
}

void CharacterService::OnCharacterSpawn(const CharacterSpawnRequest& acMessage) noexcept
{
    if (acMessage.FormId != GameId{} && IsActorDiscoverySuppressed(m_world.GetModSystem().GetGameId(acMessage.FormId)))
        return;
    if (const auto existing = Utils::FindEntityByServerId(acMessage.ServerId); existing && m_world.all_of<LocalComponent>(*existing))
        return;
    if (acMessage.OwnershipEpoch == 0)
    {
        spdlog::warn("Ignored spawn for actor {:X} because the ownership epoch is invalid", acMessage.ServerId);
        return;
    }

    auto remoteView = m_world.view<RemoteComponent>();
    const auto remoteItor = std::find_if(std::begin(remoteView), std::end(remoteView), [remoteView, Id = acMessage.ServerId](auto entity) { return remoteView.get<RemoteComponent>(entity).Id == Id; });

    if (remoteItor != std::end(remoteView))
    {
        spdlog::warn("Character with remote id {:X} is already spawned.", acMessage.ServerId);
        return;
    }

    Actor* pActor = nullptr;

    std::optional<entt::entity> entity;

    // Custom forms
    if (acMessage.FormId == GameId{})
    {
        TESNPC* pNpc = nullptr;

        entity = m_world.create();

        if (acMessage.BaseId != GameId{})
        {
            // Prefer the owner's resolved leveled pick over the lossy template base
            if (acMessage.LeveledNpcPickId != GameId{})
                pNpc = Cast<TESNPC>(TESForm::GetById(m_world.GetModSystem().GetGameId(acMessage.LeveledNpcPickId)));

            if (!pNpc)
                pNpc = Cast<TESNPC>(TESForm::GetById(m_world.GetModSystem().GetGameId(acMessage.BaseId)));

            if (!pNpc)
            {
                spdlog::error("Failed to retrieve NPC, it will not be spawned, possibly missing mod, base: {:X}:{:X}, form: {:X}:{:X}", acMessage.BaseId.BaseId, acMessage.BaseId.ModId, acMessage.FormId.BaseId, acMessage.FormId.ModId);
                return;
            }

            pNpc->Deserialize(acMessage.AppearanceBuffer, acMessage.ChangeFlags);
        }
        else
        {
            // Players and npcs with temporary ref ids and base ids (usually random events)
            pNpc = TESNPC::Create(acMessage.AppearanceBuffer, acMessage.ChangeFlags);
            FaceGenSystem::Setup(m_world, *entity, acMessage.FaceTints);
        }

        pActor = Actor::Create(pNpc);
    }
    else
    {
        const uint32_t cActorId = World::Get().GetModSystem().GetGameId(acMessage.FormId);

        auto waitingView = m_world.view<FormIdComponent, WaitingForAssignmentComponent>();
        const auto waitingItor = std::find_if(std::begin(waitingView), std::end(waitingView), [waitingView, cActorId](auto entity) { return waitingView.get<FormIdComponent>(entity).Id == cActorId; });

        if (waitingItor != std::end(waitingView))
        {
            spdlog::info("Character with form id {:X} already has a spawn request in progress.", cActorId);
            return;
        }

        auto* const pForm = TESForm::GetById(cActorId);
        pActor = Cast<Actor>(pForm);

        if (!pActor)
        {
            spdlog::error("Failed to retrieve Actor {:X}, it will not be spawned, possibly missing mod", cActorId);
            spdlog::error("\tForm : {:X}", pForm ? pForm->formID : 0);
            return;
        }

        const auto view = m_world.view<FormIdComponent>();
        const auto itor = std::find_if(std::begin(view), std::end(view), [cActorId, view](entt::entity entity) { return view.get<FormIdComponent>(entity).Id == cActorId; });

        if (itor != std::end(view))
            entity = *itor;
        else
            entity = m_world.create();
    }

    if (!pActor)
    {
        spdlog::error("Actor object {:X} could not be created.", acMessage.ServerId);
        return;
    }

    spdlog::info("CharacterSpawnRequest, server id: {:X}, form id: {:X}", acMessage.ServerId, pActor->formID);

    ScriptedActorState anchor;
    anchor.CellId = acMessage.CellId;
    anchor.Position = acMessage.Position;
    if (auto* pCell = Cast<TESObjectCELL>(TESForm::GetById(m_world.GetModSystem().GetGameId(acMessage.CellId))); pCell && pCell->worldspace)
        m_world.GetModSystem().GetServerModId(pCell->worldspace->formID, anchor.WorldSpaceId);
    if (!acMessage.IsPlayer && IsLeaderNativeActor(pActor) &&
        (IsLocationInPlayerRange(m_world, anchor, pActor->IsDragon()) || acMessage.CellId == GameId{}))
    {
        m_loadedActorLocations.try_emplace(pActor->formID, anchor);
        DeleteRemoteEntityComponents(*entity);
        m_world.remove<FormIdComponent>(*entity);
        m_world.emplace_or_replace<RemoteComponent>(*entity, acMessage.ServerId, pActor->formID, acMessage.OwnershipEpoch);
        m_world.emplace_or_replace<LeaderNativeClaim>(*entity).FormId = pActor->formID;
        pActor->GetExtension()->SetRemote(false);
        RequestOwnership(pActor->formID, acMessage.ServerId, *entity);
        spdlog::info("Pending leader spawn actor {:X} server {:X} epoch {} has3D={}",
            pActor->formID, acMessage.ServerId, acMessage.OwnershipEpoch, pActor->GetNiNode() != nullptr);
        return;
    }

    // Pending reconciliation re-enables the actor after applying the owner's pick.
    if (pActor->IsDisabled() && pActor->GetExtension()->Reconciliation != ActorExtension::ReconciliationStage::WaitingForDisable)
    {
        spdlog::warn("Disabled actor is being re-enabled: {:X}", pActor->formID);
        pActor->EnableImpl();
    }

    pActor->GetExtension()->SetRemote(true);
    pActor->rotation.x = acMessage.Rotation.x;
    pActor->rotation.z = acMessage.Rotation.y;
    pActor->MoveTo(PlayerCharacter::Get()->parentCell, acMessage.Position);
    pActor->SetActorValues(acMessage.InitialActorValues);

    pActor->GetExtension()->SetPlayer(acMessage.IsPlayer);
    if (acMessage.IsPlayer)
    {
        pActor->SetIgnoreFriendlyHit(true);
        pActor->SetPlayerRespawnMode();
        m_world.emplace_or_replace<PlayerComponent>(*entity, acMessage.PlayerId);
    }

    if (pActor->IsDead() != acMessage.IsDead)
        acMessage.IsDead ? pActor->Kill() : pActor->Respawn();

    spdlog::info("Spawn Request Is summon {}", acMessage.IsPlayerSummon);

    if (acMessage.IsPlayerSummon)
    {
        // Prevents remote summons agroing other players.
        pActor->SetCommandingActor(PlayerCharacter::Get()->GetHandle());
    }

    // Static references arrive with their own locally rolled leveled pick; conform to the owner's.
    if (acMessage.FormId != GameId{})
        ApplyLeveledNpcPick(pActor, acMessage.LeveledNpcPickId);

    m_world.emplace_or_replace<RemoteComponent>(*entity, acMessage.ServerId, pActor->formID, acMessage.OwnershipEpoch);

    auto& interpolationComponent = InterpolationSystem::Setup(m_world, *entity);
    interpolationComponent.Position = acMessage.Position;

    AnimationSystem::Setup(m_world, *entity);

    m_world.emplace_or_replace<WaitingFor3D>(*entity, acMessage);

    auto& remoteAnimationComponent = m_world.get<RemoteAnimationComponent>(*entity);

    AnimationSystem::AddActionsForReplay(remoteAnimationComponent, acMessage.ActionsToReplay);

    if (acMessage.MountedOnServerId)
        m_pendingMounts[acMessage.ServerId] = {acMessage.MountedOnServerId, 0, 0};

#if (!IS_MASTER)
    m_world.emplace_or_replace<ReplayedActionsDebugComponent>(*entity, acMessage.ActionsToReplay);
#endif
}

void CharacterService::OnReferencesMoveRequest(const ServerReferencesMoveRequest& acMessage) const noexcept
{
    auto view = m_world.view<RemoteComponent, InterpolationComponent, RemoteAnimationComponent>();

    for (const auto& [serverId, update] : acMessage.Updates)
    {
        auto itor = std::find_if(std::begin(view), std::end(view), [serverId = serverId, view](entt::entity entity) { return view.get<RemoteComponent>(entity).Id == serverId; });

        if (itor == std::end(view))
            continue;

        auto& interpolationComponent = view.get<InterpolationComponent>(*itor);
        auto& animationComponent = view.get<RemoteAnimationComponent>(*itor);
        const auto& movement = update.UpdatedMovement;

        if (acMessage.Tick >= interpolationComponent.AuthorityTick)
        {
            const auto positionStep = glm::vec3(movement.Position) -
                interpolationComponent.AuthorityPosition;
            if (!interpolationComponent.AuthorityTick ||
                glm::dot(positionStep, positionStep) > 64.f)
                interpolationComponent.AuthorityStableSinceTick = acMessage.Tick;
            interpolationComponent.AuthorityTick = acMessage.Tick;
            interpolationComponent.AuthorityCellId = movement.CellId;
            interpolationComponent.AuthorityWorldSpaceId = movement.WorldSpaceId;
            interpolationComponent.AuthorityPosition = movement.Position;
        }

        InterpolationComponent::TimePoint point;
        point.Tick = acMessage.Tick;
        point.Position = movement.Position;
        point.Rotation = {movement.Rotation.x, 0.f, movement.Rotation.y};
        point.Variables = movement.Variables;
        point.Direction = movement.Direction;

        InterpolationSystem::AddPoint(interpolationComponent, point);
        if (acMessage.Tick >= animationComponent.LastReceivedCombatTargetTick)
        {
            animationComponent.LastReceivedCombatTargetTick = acMessage.Tick;
            auto& targets = animationComponent.CombatTargetTimePoints;
            const uint32_t lastTargetId = targets.empty() ?
                animationComponent.DesiredCombatTargetServerId :
                targets.back().ServerId;
            if (lastTargetId != update.CombatTargetServerId ||
                (!animationComponent.DesiredCombatTargetTick && targets.empty()))
            {
                targets.push_back({acMessage.Tick, update.CombatTargetServerId});
                if (targets.size() > 32)
                    targets.pop_front();
            }
        }

        for (const auto& action : update.ActionEvents)
        {
            animationComponent.TimePoints.push_back(action);
            animationComponent.LastReceivedAction = action;
        }

        if (!update.EvaluatedPose.Bones.empty() &&
            acMessage.Tick >= animationComponent.EvaluatedPoseTick)
        {
            animationComponent.EvaluatedPose = update.EvaluatedPose;
            animationComponent.EvaluatedPoseTick = acMessage.Tick;
            if (const auto* pFormId = m_world.try_get<FormIdComponent>(*itor))
                PoseCopyAuthority::PushOwnerSample(pFormId->Id, update.EvaluatedPose,
                    update.EvaluatedPose.SourceTick ? update.EvaluatedPose.SourceTick : acMessage.Tick);
        }
        if (!update.VisualBones.Bones.empty() &&
            acMessage.Tick >= animationComponent.VisualBonesTick)
        {
            animationComponent.VisualBones = update.VisualBones;
            animationComponent.VisualBonesTick = acMessage.Tick;
            if (const auto* pFormId = m_world.try_get<FormIdComponent>(*itor))
            {
                if (auto* pActor = Cast<Actor>(TESForm::GetById(pFormId->Id)))
                {
                    const auto* pExtension = pActor->GetExtension();
                    VisualPoseMailbox::Publish(&pActor->animationGraphHolder,
                        pActor, pActor->formID,
                        view.get<RemoteComponent>(*itor).OwnershipEpoch,
                        pExtension ? pExtension->GraphDescriptorHash : 0,
                        update.VisualBones);
                }
            }
        }
    }
}

void CharacterService::OnActionEvent(const ActionEvent& acActionEvent) const noexcept
{
    auto* pActionActor = Cast<Actor>(TESForm::GetById(acActionEvent.ActorId));
    auto* pActionExtension = pActionActor ? pActionActor->GetExtension() : nullptr;
    auto view = m_world.view<LocalAnimationComponent, FormIdComponent>();
    const auto itor = std::find_if(std::begin(view), std::end(view), [id = acActionEvent.ActorId, view](entt::entity entity) { return view.get<FormIdComponent>(entity).Id == id; });

    if (itor != std::end(view))
    {
        auto& localComponent = view.get<LocalAnimationComponent>(*itor);

        localComponent.Append(acActionEvent);
        if (pActionExtension)
            pActionExtension->LatestAnimationDispatch = 1;
    }
    else if (m_transport.IsOnline())
    {
        // A `LocalAnimationComponent` is not attached yet, but the actor already exists and is running animations

        auto view = m_world.view<FormIdComponent, EarlyAnimationBufferComponent>();
        const auto itor = std::find_if(std::begin(view), std::end(view), [id = acActionEvent.ActorId, view](entt::entity entity) { return view.get<FormIdComponent>(entity).Id == id; });

        if (itor != std::end(view))
        {
            view.get<EarlyAnimationBufferComponent>(*itor).Actions.push_back(acActionEvent);
            if (pActionExtension)
                pActionExtension->LatestAnimationDispatch = 2;
        }
        else if (pActionExtension)
            pActionExtension->LatestAnimationDispatch = 3;
    }
    else if (pActionExtension)
        pActionExtension->LatestAnimationDispatch = 3;
}

void CharacterService::OnFactionsChanges(const NotifyFactionsChanges& acEvent) const noexcept
{
    auto view = m_world.view<RemoteComponent, FormIdComponent, CacheComponent>();

    for (const auto& [id, factions] : acEvent.Changes)
    {
        const auto itor = std::find_if(std::begin(view), std::end(view), [id = id, view](entt::entity entity) { return view.get<RemoteComponent>(entity).Id == id; });

        if (itor != std::end(view))
        {
            auto& formIdComponent = view.get<FormIdComponent>(*itor);

            auto* const pActor = Cast<Actor>(TESForm::GetById(formIdComponent.Id));
            if (!pActor)
                return;

            auto& cacheComponent = view.get<CacheComponent>(*itor);
            cacheComponent.FactionsContent = factions;

            pActor->SetFactions(cacheComponent.FactionsContent);
        }
    }
}

void CharacterService::OnOwnershipTransfer(const NotifyOwnershipTransfer& acMessage) noexcept
{
    if (acMessage.OwnershipEpoch == 0)
    {
        spdlog::warn("Ignored ownership update for actor {:X} because the epoch is invalid", acMessage.ServerId);
        return;
    }

    auto entity = Utils::FindEntityByServerId(acMessage.ServerId);
    if (entity && !m_world.any_of<LocalComponent, RemoteComponent>(*entity))
        entity.reset();

    uint32_t currentEpoch = 0;
    if (entity)
    {
        if (const auto* pLocalComponent = m_world.try_get<LocalComponent>(*entity))
            currentEpoch = pLocalComponent->OwnershipEpoch;
        else if (const auto* pRemoteComponent = m_world.try_get<RemoteComponent>(*entity))
            currentEpoch = pRemoteComponent->OwnershipEpoch;
    }

    if (currentEpoch != 0 && acMessage.OwnershipEpoch <= currentEpoch)
    {
        spdlog::debug("Ignored stale ownership update for actor {:X} at epoch {}; current epoch is {}", acMessage.ServerId, acMessage.OwnershipEpoch, currentEpoch);
        return;
    }

    for (auto& [formId, parked] : m_parkedActors)
    {
        if (parked.Message.State.ServerId != acMessage.ServerId)
            continue;
        auto& grant = m_restoredOwnershipGrants[acMessage.ServerId];
        if (grant.OwnershipEpoch < acMessage.OwnershipEpoch)
            grant = acMessage;
        parked.Message.State.Phase = ScriptedActorPhase::Release;
        spdlog::info("Deferred restored actor grant {:X} epoch {} until local disable completes", acMessage.ServerId, acMessage.OwnershipEpoch);
        return;
    }

    const bool isLocalOwner = acMessage.OwnerPlayerId == m_transport.GetLocalPlayerId();
    if (!entity)
    {
        // A transfer does not contain enough form data to recreate an unknown actor. Decline so the server can try another loaded client.
        if (isLocalOwner)
            DeclineOwnership(acMessage.ServerId, acMessage.OwnershipEpoch);
        else
            spdlog::debug("Ignored ownership update for unknown actor {:X} at epoch {}", acMessage.ServerId, acMessage.OwnershipEpoch);
        return;
    }

    const entt::entity cEntity = *entity;
    if (const auto* pOldForm = m_world.try_get<FormIdComponent>(cEntity))
    {
        if (auto* pOldActor = Cast<Actor>(TESForm::GetById(pOldForm->Id)))
            VisualPoseMailbox::Clear(&pOldActor->animationGraphHolder);
    }
    if (auto* pRemoteAnimation = m_world.try_get<RemoteAnimationComponent>(cEntity))
    {
        pRemoteAnimation->EvaluatedPose = {};
        pRemoteAnimation->EvaluatedPoseTick = 0;
        pRemoteAnimation->VisualBones = {};
        pRemoteAnimation->VisualBonesTick = 0;
    }
    const auto* pFormIdComponent = m_world.try_get<FormIdComponent>(cEntity);
    Actor* pActor = pFormIdComponent ? Cast<Actor>(TESForm::GetById(pFormIdComponent->Id)) : nullptr;
    if (!pActor)
        if (const auto* pClaim = m_world.try_get<LeaderNativeClaim>(cEntity))
            pActor = Cast<Actor>(TESForm::GetById(pClaim->FormId));
    if (!pActor)
        if (const auto* pRemote = m_world.try_get<RemoteComponent>(cEntity))
            pActor = Cast<Actor>(TESForm::GetById(pRemote->CachedRefId));

    // Preserve the accepted epoch's pick for actors that still need to be created.
    if (auto* pWaitingFor3D = m_world.try_get<WaitingFor3D>(cEntity))
        pWaitingFor3D->SpawnRequest.LeveledNpcPickId = acMessage.LeveledNpcPickId;

    if (isLocalOwner)
    {
        if (IsLeaderNativeActor(pActor))
        {
            DeleteRemoteEntityComponents(cEntity);
            m_world.emplace_or_replace<LeaderNativeClaim>(cEntity).FormId = pActor->formID;
            // Owned here: the local systems (actor values, capture) read its form id.
            m_world.emplace_or_replace<FormIdComponent>(cEntity, pActor->formID);
            auto& local = m_world.emplace_or_replace<LocalComponent>(cEntity, acMessage.ServerId, acMessage.OwnershipEpoch);
            local.IsDead = pActor->IsDead();
            local.IsWeaponDrawn = pActor->actorState.IsWeaponDrawn();
            pActor->GetExtension()->SetRemote(false);
            m_weaponDrawUpdates.erase(pActor->formID);
            m_pendingLeveledConforms.erase(pActor->formID);
            if (IsLoadedActor(pActor))
            {
                m_world.emplace_or_replace<FormIdComponent>(cEntity, pActor->formID);
                CacheSystem::Setup(m_world, cEntity, pActor);
                m_world.emplace_or_replace<LocalAnimationComponent>(cEntity);
            }
            else
            {
                m_world.remove<LocalAnimationComponent>(cEntity);
                TryParkActor(cEntity, pActor);
                m_world.remove<FormIdComponent>(cEntity);
            }
            spdlog::info("Accepted native leader grant actor {:X} server {:X} epoch {} pending3D={}",
                pActor->formID, acMessage.ServerId, acMessage.OwnershipEpoch, !IsLoadedActor(pActor));
            return;
        }
        if (!pFormIdComponent || !pActor || !pActor->GetNiNode())
        {
            uint32_t cachedRefId = pFormIdComponent ? pFormIdComponent->Id : 0;
            if (const auto* pRemoteComponent = m_world.try_get<RemoteComponent>(cEntity))
                cachedRefId = pRemoteComponent->CachedRefId;

            if (pActor)
                pActor->GetExtension()->SetRemote(true);

            m_world.remove<LocalAnimationComponent, LocalComponent>(cEntity);
            if (m_world.all_of<RemoteComponent>(cEntity))
                m_world.get<RemoteComponent>(cEntity).OwnershipEpoch = acMessage.OwnershipEpoch;
            else if (cachedRefId != 0)
                m_world.emplace<RemoteComponent>(cEntity, acMessage.ServerId, cachedRefId, acMessage.OwnershipEpoch);

            spdlog::warn("Declined ownership of actor {:X} at epoch {} because the actor is not ready", acMessage.ServerId, acMessage.OwnershipEpoch);
            DeclineOwnership(acMessage.ServerId, acMessage.OwnershipEpoch);
            return;
        }

        // Reconcile while hooks still treat the actor as remote/non-authoritative.
        pActor->GetExtension()->SetRemote(true);
        m_world.remove<LocalAnimationComponent, LocalComponent>(cEntity);
        m_world.emplace_or_replace<RemoteComponent>(cEntity, acMessage.ServerId, pFormIdComponent->Id, acMessage.OwnershipEpoch);

        ReconcileActorData(cEntity, pActor, acMessage.OwnershipEpoch, acMessage.CurrentActorData, true, true);
        ApplyLeveledNpcPick(pActor, acMessage.LeveledNpcPickId);

        DeleteRemoteEntityComponents(cEntity);
        CacheSystem::Setup(m_world, cEntity, pActor);
        m_world.emplace_or_replace<LocalAnimationComponent>(cEntity);
        auto& localComponent = m_world.emplace_or_replace<LocalComponent>(cEntity, acMessage.ServerId, acMessage.OwnershipEpoch);
        localComponent.IsDead = acMessage.CurrentActorData.IsDead;
        localComponent.IsWeaponDrawn = acMessage.CurrentActorData.IsWeaponDrawn;

        // LocalComponent is installed only after canonical reconciliation is complete.
        pActor->GetExtension()->SetRemote(false);
        spdlog::info("Gained ownership of actor {:X} at epoch {}", acMessage.ServerId, acMessage.OwnershipEpoch);
        return;
    }

    if (pActor)
        pActor->GetExtension()->SetRemote(true);

    m_world.remove<LeaderNativeClaim>(cEntity);
    m_world.remove<LocalAnimationComponent, LocalComponent>(cEntity);
    if (!pFormIdComponent && pActor)
        pFormIdComponent = &m_world.emplace_or_replace<FormIdComponent>(cEntity, pActor->formID);

    if (pFormIdComponent)
    {
        m_world.emplace_or_replace<RemoteComponent>(cEntity, acMessage.ServerId, pFormIdComponent->Id, acMessage.OwnershipEpoch);

        if (!m_world.all_of<InterpolationComponent>(cEntity))
            InterpolationSystem::Setup(m_world, cEntity);
        if (!m_world.all_of<RemoteAnimationComponent>(cEntity))
            AnimationSystem::Setup(m_world, cEntity);
    }
    else if (auto* pRemoteComponent = m_world.try_get<RemoteComponent>(cEntity))
    {
        pRemoteComponent->OwnershipEpoch = acMessage.OwnershipEpoch;
    }

    ReconcileActorData(cEntity, pActor, acMessage.OwnershipEpoch, acMessage.CurrentActorData, pActor && pActor->GetNiNode(), false);
    if (pActor)
        ApplyLeveledNpcPick(pActor, acMessage.LeveledNpcPickId);

    spdlog::info("Actor {:X} is now owned by player {:X} at epoch {}", acMessage.ServerId, acMessage.OwnerPlayerId, acMessage.OwnershipEpoch);
}

void CharacterService::OnRemoveCharacter(const NotifyRemoveCharacter& acMessage) noexcept
{
    for (const auto& [formId, parked] : m_parkedActors)
        if (parked.Message.State.ServerId == acMessage.ServerId && parked.Message.State.Phase == ScriptedActorPhase::Park)
            return;
    m_restoredOwnershipGrants.erase(acMessage.ServerId);
    if (const auto entity = Utils::FindEntityByServerId(acMessage.ServerId); entity && m_world.all_of<LeaderNativeClaim>(*entity))
    {
        const auto formId = m_world.get<LeaderNativeClaim>(*entity).FormId;
        m_loadedActorLocations.erase(formId);
        m_world.remove<LeaderNativeClaim, LocalComponent, LocalAnimationComponent>(*entity);
        m_world.emplace_or_replace<FormIdComponent>(*entity, formId);
        DeleteRemoteEntityComponents(*entity);
        if (IsLoadedActor(Cast<Actor>(TESForm::GetById(formId))))
            ProcessNewEntity(*entity);
        return;
    }
    ClearMountRelationsForServerId(acMessage.ServerId);
    m_localMountSent.erase(acMessage.ServerId);
    auto view = m_world.view<RemoteComponent>();

    const auto itor = std::find_if(std::begin(view), std::end(view), [id = acMessage.ServerId, view](entt::entity entity) { return view.get<RemoteComponent>(entity).Id == id; });

    if (itor != std::end(view))
    {
        if (auto* pFormIdComponent = m_world.try_get<FormIdComponent>(*itor))
        {
            Actor* pActor = Cast<Actor>(TESForm::GetById(pFormIdComponent->Id));
            if (pActor)
                VisualPoseMailbox::Clear(&pActor->animationGraphHolder);
            if (pActor && pActor->IsTemporary())
                CharacterService::DeleteTempActor(pFormIdComponent->Id);
            else if (pActor)
                pActor->GetExtension()->SetRemote(false);
        }

        DeleteRemoteEntityComponents(*itor);
    }
}

namespace
{
// Other players in the character creator, by form id: the slot they are shown in beside this
// player (1, 2, ...), and when their last look was applied here.
struct CreatorPlayer
{
    uint32_t Slot{};
    uint64_t AppliedAtMs{};
};
std::mutex s_creatorPlayersLock;
std::unordered_map<uint32_t, CreatorPlayer> s_creatorPlayers;
constexpr float kCreatorSpacing = 110.f;

uint64_t CreatorNowMs() noexcept
{
    return GetTickCount64();
}
} // namespace

bool CharacterService::GetCreatorDisplayOffset(const uint32_t aFormId, NiPoint3& arOffset, float& arHeading) noexcept
{
    std::lock_guard lock(s_creatorPlayersLock);
    const auto it = s_creatorPlayers.find(aFormId);
    auto* pPlayer = PlayerCharacter::Get();
    if (it == s_creatorPlayers.end() || !pPlayer)
        return false;
    // In front of this player, side by side, facing it: a player waiting for the creator to finish
    // watches the others edit without turning.
    const float heading = pPlayer->rotation.z;
    const float count = static_cast<float>(s_creatorPlayers.size());
    const float side = (static_cast<float>(it->second.Slot) - (count + 1.f) * 0.5f) * kCreatorSpacing;
    const float forward = 160.f;
    arOffset.x = std::sin(heading) * forward + std::cos(heading) * side;
    arOffset.y = std::cos(heading) * forward - std::sin(heading) * side;
    arOffset.z = 0.f;
    arHeading = heading + static_cast<float>(TiltedPhoques::Pi);
    return true;
}

// While the creator (RaceSex Menu) is open: this player's look, once a second when it changed,
// and the final look when the creator closes. Receivers apply it to this player's character.
void CharacterService::SendCreatorAppearance() noexcept
{
    auto* pUI = UI::Get();
    const bool creatorOpen = pUI && pUI->GetMenuOpen(BSFixedString("RaceSex Menu"));
    const bool closedNow = m_creatorWasOpen && !creatorOpen;
    m_creatorWasOpen = creatorOpen;
    if (!creatorOpen && !closedNow)
        return;
    const auto now = CreatorNowMs();
    if (!closedNow && now < m_nextCreatorAppearanceMs)
        return;
    m_nextCreatorAppearanceMs = now + 1000;

    auto* pPlayer = PlayerCharacter::Get();
    auto* pNpc = pPlayer ? Cast<TESNPC>(pPlayer->baseForm) : nullptr;
    if (!pNpc)
        return;
    auto view = m_world.view<FormIdComponent>();
    const auto it = std::find_if(view.begin(), view.end(), [view](auto entity) { return view.get<FormIdComponent>(entity).Id == 0x14; });
    if (it == view.end())
        return;
    const auto serverId = Utils::GetServerId(*it);
    if (!serverId)
        return;

    PlayerAppearanceRequest request;
    request.ServerId = *serverId;
    pNpc->MarkChanged(0x2000800);
    request.ChangeFlags = pNpc->GetChangeFlags();
    pNpc->Serialize(&request.AppearanceBuffer);
    const auto& tints = pPlayer->GetTints();
    request.FaceTints.Entries.resize(tints.length);
    for (auto i = 0u; i < tints.length; ++i)
    {
        request.FaceTints.Entries[i].Alpha = tints[i]->alpha;
        request.FaceTints.Entries[i].Color = tints[i]->color;
        request.FaceTints.Entries[i].Type = tints[i]->type;
        if (tints[i]->texture)
            request.FaceTints.Entries[i].Name = tints[i]->texture->name.AsAscii();
    }
    // Still editing until Done (the creator stays open while the others finish; CreatorTogether).
    request.InCreator = creatorOpen && !CreatorTogether::IsDone();

    uint64_t hash = 14695981039346656037ULL;
    for (const char c : request.AppearanceBuffer)
        hash = (hash ^ static_cast<uint8_t>(c)) * 1099511628211ULL;
    for (const auto& entry : request.FaceTints.Entries)
    {
        hash = (hash ^ entry.Color ^ (static_cast<uint64_t>(entry.Alpha * 1000.f) << 32) ^ entry.Type) * 1099511628211ULL;
        for (const char c : entry.Name)
            hash = (hash ^ static_cast<uint8_t>(c)) * 1099511628211ULL;
    }
    hash = (hash ^ static_cast<uint64_t>(request.InCreator)) * 1099511628211ULL; // Done counts as a change
    if (!closedNow && hash == m_lastCreatorAppearanceHash)
        return;
    m_lastCreatorAppearanceHash = hash;
    m_transport.Send(request);
    if (closedNow)
        spdlog::info("Character creator closed: sent the final look");
}

// Another player's look (live while they edit): coalesced and applied to their character on the
// main thread, before the creator sets its preview visibility.
void CharacterService::OnNotifyPlayerAppearance(const NotifyPlayerAppearance& acMessage) noexcept
{
    auto view = m_world.view<FormIdComponent, RemoteComponent>();
    const auto entityIt = std::find_if(view.begin(), view.end(),
        [view, id = acMessage.ServerId](auto aEntity) { return view.get<RemoteComponent>(aEntity).Id == id; });
    if (entityIt == view.end())
        return;
    auto* pActor = Cast<Actor>(TESForm::GetById(view.get<FormIdComponent>(*entityIt).Id));
    auto* pNpc = pActor ? Cast<TESNPC>(pActor->baseForm) : nullptr;
    if (!pNpc || !pActor->GetExtension() || !pActor->GetExtension()->IsPlayer())
        return;

    {
        std::lock_guard lock(s_creatorPlayersLock);
        if (acMessage.InCreator)
        {
            auto& creatorPlayer = s_creatorPlayers[pActor->formID];
            if (!creatorPlayer.Slot)
                creatorPlayer.Slot = static_cast<uint32_t>(s_creatorPlayers.size());
            const auto now = CreatorNowMs();
            creatorPlayer.AppliedAtMs = now;
        }
        else
            s_creatorPlayers.erase(pActor->formID);
    }

    CreatorTogether::SetRemoteReady(pActor->formID, !acMessage.InCreator);
    CreatorTogether::QueueAppearance(pActor->formID, acMessage);
}

void CharacterService::OnNotifyRespawn(const NotifyRespawn& acMessage) const noexcept
{
    auto view = m_world.view<FormIdComponent, RemoteComponent>();
    const auto entityIt = std::find_if(view.begin(), view.end(), [view, id = acMessage.ActorId](auto aEntity) { return view.get<RemoteComponent>(aEntity).Id == id; });

    if (entityIt == view.end())
    {
        spdlog::error("Actor to respawn not found in: {:X}", acMessage.ActorId);
        return;
    }

    const auto cId = *entityIt;

    auto& formIdComponent = view.get<FormIdComponent>(cId);
    CancelServerAssignment(*entityIt, formIdComponent.Id);

    m_world.remove<EarlyAnimationBufferComponent>(cId);

    if (m_world.all_of<FormIdComponent>(cId))
        m_world.remove<FormIdComponent>(cId);

    if (m_world.orphan(cId))
        m_world.destroy(cId);

    RequestRespawn request;
    request.ActorId = acMessage.ActorId;

    m_transport.Send(request);
}

void CharacterService::OnBeastFormChange(const BeastFormChangeEvent& acEvent) const noexcept
{
    auto view = m_world.view<FormIdComponent>();

    const auto it = std::find_if(view.begin(), view.end(), [view](auto entity) { return view.get<FormIdComponent>(entity).Id == 0x14; });

    std::optional<uint32_t> serverIdRes = Utils::GetServerId(*it);
    if (!serverIdRes.has_value())
    {
        spdlog::error("{}: failed to find server id", __FUNCTION__);
        return;
    }

    uint32_t serverId = serverIdRes.value();

    RequestRespawn request;
    request.ActorId = serverId;

    Actor* pActor = Utils::GetByServerId<Actor>(serverId);
    if (!pActor)
    {
        spdlog::warn(__FUNCTION__ ": could not find actor for server id {:X}", serverId);
        return;
    }

    TESNPC* pNpc = Cast<TESNPC>(pActor->baseForm);
    if (!pNpc)
    {
        spdlog::warn(__FUNCTION__ ": could not find actor baseform for server id {:X}", serverId);
        return;
    }

    pNpc->Serialize(&request.AppearanceBuffer);
    request.ChangeFlags = pNpc->GetChangeFlags();

    m_transport.Send(request);
}

void CharacterService::OnMountEvent(const MountEvent& acEvent) const noexcept
{
    auto view = m_world.view<FormIdComponent>();

    const auto riderIt = std::find_if(std::begin(view), std::end(view), [id = acEvent.RiderID, view](auto entity) { return view.get<FormIdComponent>(entity).Id == id; });

    if (riderIt == std::end(view))
    {
        spdlog::warn("Rider not found, form id: {:X}", acEvent.RiderID);
        return;
    }

    const entt::entity cRiderEntity = *riderIt;
    const auto* pRiderLocalComponent = m_world.try_get<LocalComponent>(cRiderEntity);
    if (!pRiderLocalComponent)
        return;

    const auto mountIt = std::find_if(std::begin(view), std::end(view), [id = acEvent.MountID, view](auto entity) { return view.get<FormIdComponent>(entity).Id == id; });

    if (mountIt == std::end(view))
    {
        spdlog::warn("Mount not found, form id: {:X}", acEvent.MountID);
        return;
    }

    const entt::entity cMountEntity = *mountIt;

    uint32_t mountServerId = 0;
    uint32_t mountOwnershipEpoch = 0;
    if (const auto* pMountLocalComponent = m_world.try_get<LocalComponent>(cMountEntity))
    {
        mountServerId = pMountLocalComponent->Id;
        mountOwnershipEpoch = pMountLocalComponent->OwnershipEpoch;
    }
    else if (const auto* pMountRemoteComponent = m_world.try_get<RemoteComponent>(cMountEntity))
    {
        mountServerId = pMountRemoteComponent->Id;
        mountOwnershipEpoch = pMountRemoteComponent->OwnershipEpoch;
    }
    else
        return;

    MountRequest request{};
    request.RiderId = pRiderLocalComponent->Id;
    request.RiderOwnershipEpoch = pRiderLocalComponent->OwnershipEpoch;
    request.MountId = mountServerId;
    request.MountOwnershipEpoch = mountOwnershipEpoch;

    auto* pRider = Cast<Actor>(TESForm::GetById(acEvent.RiderID));
    const auto native = pRider ? pRider->GetNativeMountState() : Actor::NativeMountState{};
    spdlog::info(
        "Native mount package event: rider {:X}, server {:X}, horse {:X}, horse server {:X}, observed native horse {:X}, interaction={}, horseExtra={}",
        acEvent.RiderID, request.RiderId, acEvent.MountID, request.MountId,
        pRider ? pRider->GetNativeMountFormId() : 0,
        native.InteractionExtra, native.HorseExtra);

    m_transport.Send(request);
}

void CharacterService::OnNotifyMount(const NotifyMount& acMessage) noexcept
{
    ++m_mountNotifications;
    m_lastMountRiderId = acMessage.RiderId;
    m_lastMountId = acMessage.MountId;
    spdlog::info("Received mount relation: rider {:X}, horse {:X}, tick {}",
        acMessage.RiderId, acMessage.MountId, GetTickCount64());
    if (!acMessage.RiderId)
        return;
    if (!acMessage.MountId)
    {
        m_pendingMounts.erase(acMessage.RiderId);
        return;
    }
    if (acMessage.RiderId == acMessage.MountId)
        return;
    // Actor ownership assignment and 3D creation may lag this one-shot
    // network event. Keep the latest relation until both scene nodes exist.
    m_pendingMounts[acMessage.RiderId] = {acMessage.MountId, 0, 0, 0, false};
}

void CharacterService::RunPendingMounts() noexcept
{
    const auto now = GetTickCount64();
    for (auto it = m_pendingMounts.begin(); it != m_pendingMounts.end();)
    {
        auto& pending = it->second;
        if (now < pending.NextAttemptMs)
        {
            ++it;
            continue;
        }
        auto* pRider = Utils::GetByServerId<Actor>(it->first);
        auto* pMount = Utils::GetByServerId<Actor>(pending.MountId);
        if (!pRider || !pMount || !pRider->GetNiNode() ||
            !pMount->GetNiNode())
        {
            ++m_mountWaitedFor3D;
            pending.NextAttemptMs = now + 100;
            ++it;
            continue;
        }
        if (pRider->GetNativeMountFormId() == pMount->formID)
        {
            // The engine must continue processing a remotely owned rider's
            // mounted state; stopping at the first seated frame can undo it.
            if (!pending.WasSeated)
                ++m_mountSeated;
            pending.WasSeated = true;
            pending.NextAttemptMs = now + 100;
            ++it;
            continue;
        }
        if (it->first == m_vehicleTrialRiderId.load(std::memory_order_acquire) &&
            !pending.VehicleTrialAttempted &&
            pRider->GetExtension()->IsRemote())
        {
            if (pRider->SetNativeVehicle(pMount))
            {
                pending.VehicleTrialAttempted = true;
                ++m_vehicleTrialCalls;
                m_vehicleTrialImmediateHandle = pRider->someRefrHandle;
                if (pRider->GetNativeMountFormId() == pMount->formID)
                    ++m_vehicleTrialImmediateSeats;
                pending.NextAttemptMs = now + 1000;
                ++it;
                continue;
            }
        }
        // A successful start merely queues the engine package. Give native
        // processing time to seat the rider before re-initiating it.
        if (pending.StartedAtMs && now - pending.StartedAtMs < 5000)
        {
            pending.NextAttemptMs = now + 100;
            ++it;
            continue;
        }
        const bool started = pRider->InitiateMountPackage(pMount);
        if (pending.Attempts < 8)
            spdlog::info("Deferred native mount rider {:X} horse {:X}: started={} riderProcess={} horseProcess={} nativeMount={:X} interaction={} attempt={}",
                pRider->formID, pMount->formID, started,
                pRider->currentProcess != nullptr,
                pMount->currentProcess != nullptr,
                pRider->GetNativeMountFormId(),
                pRider->GetNativeMountState().InteractionExtra,
                pending.Attempts + 1);
        if (started)
        {
            ++m_mountApplied;
            pending.StartedAtMs = now;
            pending.NextAttemptMs = now + 100;
            spdlog::info("Started deferred mount rider {:X} horse {:X} after {} attempts",
                it->first, pending.MountId, pending.Attempts + 1);
            ++it;
            continue;
        }
        ++pending.Attempts;
        ++m_mountRejected;
        pending.NextAttemptMs = now + 500;
        ++it;
    }
}

void CharacterService::RunLocalMountUpdates() noexcept
{
    if (!m_transport.IsConnected())
        return;

    const auto now = GetTickCount64();
    if (now - m_lastLocalMountPollMs < 200)
        return;
    m_lastLocalMountPollMs = now;

    const auto view = m_world.view<LocalComponent, FormIdComponent>();
    for (const auto entity : view)
    {
        const auto& owner = view.get<LocalComponent>(entity);
        const auto& form = view.get<FormIdComponent>(entity);
        auto* pRider = Cast<Actor>(TESForm::GetById(form.Id));
        if (!pRider || !pRider->GetNiNode())
            continue;

        auto& state = m_localMountSent[owner.Id];
        if (state.OwnershipEpoch != owner.OwnershipEpoch)
        {
            state = {};
            state.OwnershipEpoch = owner.OwnershipEpoch;
        }

        const uint32_t nativeMountFormId = pRider->GetNativeMountFormId();
        uint32_t mountId = 0;
        uint32_t mountEpoch = 0;
        if (nativeMountFormId)
        {
            state.ZeroObservedAtMs = 0;
            auto token = Utils::GetLocalOwnershipToken(nativeMountFormId);
            if (!token)
                token = Utils::GetRemoteOwnershipToken(nativeMountFormId);
            if (!token)
                continue; // The horse may not have a server identity yet.
            mountId = token->ServerId;
            mountEpoch = token->OwnershipEpoch;
        }
        else
        {
            if (!state.MountId)
                continue;
            if (!state.ZeroObservedAtMs)
                state.ZeroObservedAtMs = now;
            if (now - state.ZeroObservedAtMs < 300)
                continue;
        }

        if (mountId == state.MountId && now - state.LastSentMs < 2000)
            continue;

        if (mountId != state.MountId)
        {
            const auto native = pRider->GetNativeMountState();
            spdlog::info(
                "Network-resolved native mount transition: rider {:X}, server {:X}, epoch {}, previous horse {:X}, observed horse {:X}, interaction={}, horseExtra={}, zeroForMs={}",
                pRider->formID, owner.Id, owner.OwnershipEpoch, state.MountId,
                mountId, native.InteractionExtra, native.HorseExtra,
                mountId || !state.ZeroObservedAtMs ? 0 : now - state.ZeroObservedAtMs);
        }

        MountRequest request{};
        request.RiderId = owner.Id;
        request.RiderOwnershipEpoch = owner.OwnershipEpoch;
        request.MountId = mountId;
        request.MountOwnershipEpoch = mountEpoch;
        m_transport.Send(request);
        state.MountId = mountId;
        state.LastSentMs = now;
    }
}

void CharacterService::OnInitPackageEvent(const InitPackageEvent& acEvent) const noexcept
{
    if (!m_transport.IsConnected())
        return;

    auto view = m_world.view<FormIdComponent>();

    const auto actorIt = std::find_if(std::begin(view), std::end(view), [id = acEvent.ActorId, view](auto entity) { return view.get<FormIdComponent>(entity).Id == id; });

    if (actorIt == std::end(view))
        return;

    const entt::entity cActorEntity = *actorIt;

    std::optional<uint32_t> actorServerIdRes = Utils::GetServerId(cActorEntity);
    if (!actorServerIdRes.has_value())
    {
        spdlog::error("{}: failed to find server id", __FUNCTION__);
        return;
    }

    NewPackageRequest request;
    request.ActorId = actorServerIdRes.value();
    if (!m_world.GetModSystem().GetServerModId(acEvent.PackageId, request.PackageId.ModId, request.PackageId.BaseId))
        return;

    m_transport.Send(request);
}

namespace
{
// PlayerCharacter::SetAIDriven (ID 40586; Game.SetPlayerAIDriven). The flag is bit 3 of +0xBEA.
void SetPlayerAIDriven(PlayerCharacter* apPlayer, bool aDriven) noexcept
{
    TP_THIS_FUNCTION(TSetAIDriven, void, PlayerCharacter, bool);
    POINTER_SKYRIMSE(TSetAIDriven, s_setAIDriven, 40586);
    TiltedPhoques::ThisCall(s_setAIDriven, apPlayer, aDriven);
}

bool IsPlayerAIDriven(const PlayerCharacter* apPlayer) noexcept
{
    return (*(reinterpret_cast<const uint8_t*>(apPlayer) + 0xBEA) & 0x8) != 0;
}

// Whether this follower's player is currently mirroring the leader's scripted package.
bool s_mirroringLeaderPackage = false;
} // namespace

// Scripted player movement (a quest or scene makes the player AI-driven and gives it a package:
// the walk out of the Helgen cart, escorts, "follow me" scenes, mod cutscenes) runs from the
// leader's scene or fragment, which the follower's copy may never start. The leader reports its
// own player's scripted package as a package notification for its player; a follower applies the
// same package to its own player, which then walks the same path to the same markers.
void CharacterService::UpdateLeaderScriptedPackage() noexcept
{
    const auto& party = m_world.GetPartyService();
    auto* pPlayer = PlayerCharacter::Get();
    if (!party.IsInParty() || !party.IsLeader() || party.GetSessionState() < 2 || !pPlayer)
        return;
    const auto* pProcess = pPlayer->currentProcess;
    const uint32_t packageId = IsPlayerAIDriven(pPlayer) && pProcess && pProcess->package ? pProcess->package->formID : 0;
    if (packageId == m_lastLeaderScriptedPackage)
        return;
    m_lastLeaderScriptedPackage = packageId;

    auto view = m_world.view<FormIdComponent, LocalComponent>();
    for (auto entity : view)
    {
        if (view.get<FormIdComponent>(entity).Id != 0x14)
            continue;
        NewPackageRequest request;
        request.ActorId = view.get<LocalComponent>(entity).Id;
        if (packageId && !m_world.GetModSystem().GetServerModId(packageId, request.PackageId.ModId, request.PackageId.BaseId))
            return;
        m_transport.Send(request);
        spdlog::info("Leader player scripted package {:X} ({})", packageId, packageId ? "AI-driven" : "released");
        return;
    }
}

void CharacterService::OnNotifyNewPackage(const NotifyNewPackage& acMessage) const noexcept
{
    auto remoteView = m_world.view<RemoteComponent, FormIdComponent>();
    const auto remoteIt = std::find_if(std::begin(remoteView), std::end(remoteView), [remoteView, Id = acMessage.ActorId](auto entity) { return remoteView.get<RemoteComponent>(entity).Id == Id; });

    if (remoteIt == std::end(remoteView))
    {
        spdlog::warn("Actor for package with remote id {:X} not found.", acMessage.ActorId);
        return;
    }

    auto formIdComponent = remoteView.get<FormIdComponent>(*remoteIt);

    const TESForm* pForm = TESForm::GetById(formIdComponent.Id);
    Actor* pActor = Cast<Actor>(pForm);

    const auto& party = m_world.GetPartyService();
    if (pActor && pActor->GetExtension()->IsRemotePlayer() && party.IsInParty() && !party.IsLeader())
    {
        auto* pPlayer = PlayerCharacter::Get();
        const uint32_t packageId = acMessage.PackageId.BaseId || acMessage.PackageId.ModId ?
            World::Get().GetModSystem().GetGameId(acMessage.PackageId) : 0;
        auto* pPackage = packageId ? Cast<TESPackage>(TESForm::GetById(packageId)) : nullptr;
        if (pPlayer && pPackage)
        {
            SetPlayerAIDriven(pPlayer, true);
            pPlayer->SetPackage(pPackage);
            s_mirroringLeaderPackage = true;
            PlayerCollision::SetMirroringScript(true);
            spdlog::info("Follower player follows the leader's scripted package {:X}", packageId);
        }
        else if (pPlayer && s_mirroringLeaderPackage)
        {
            SetPlayerAIDriven(pPlayer, false);
            s_mirroringLeaderPackage = false;
            PlayerCollision::SetMirroringScript(false);
            spdlog::info("Follower player released from the leader's scripted package");
        }
        return;
    }

    const uint32_t cPackageFormId = World::Get().GetModSystem().GetGameId(acMessage.PackageId);
    const TESForm* pPackageForm = TESForm::GetById(cPackageFormId);
    if (!pPackageForm)
    {
        spdlog::warn("Actor package not found, base id: {:X}, mod id: {:X}", acMessage.PackageId.BaseId, acMessage.PackageId.ModId);
        return;
    }

    TESPackage* pPackage = Cast<TESPackage>(pPackageForm);

    pActor->SetPackage(pPackage);
}

void CharacterService::OnAddExperienceEvent(const AddExperienceEvent& acEvent) noexcept
{
    m_cachedExperience += acEvent.Experience;
}

void CharacterService::OnNotifySyncExperience(const NotifySyncExperience& acMessage) noexcept
{
    PlayerCharacter* pPlayer = PlayerCharacter::Get();

    if (PlayerCharacter::LastUsedCombatSkill == -1)
        return;

    pPlayer->AddSkillExperience(PlayerCharacter::LastUsedCombatSkill, acMessage.Experience);
}

void CharacterService::OnDialogueEvent(const DialogueEvent& acEvent) noexcept
{
    if (!m_transport.IsConnected())
        return;

    auto view = m_world.view<FormIdComponent>(entt::exclude<ObjectComponent>);
    auto entityIt = std::find_if(view.begin(), view.end(), [view, formId = acEvent.ActorID](auto entity) { return view.get<FormIdComponent>(entity).Id == formId; });

    if (entityIt == view.end())
        return;

    auto serverIdRes = Utils::GetServerId(*entityIt);
    if (!serverIdRes)
    {
        spdlog::error("{}: server id not found for form id {:X}", __FUNCTION__, acEvent.ActorID);
        return;
    }

    DialogueRequest request{};
    request.ServerId = serverIdRes.value();
    request.Tick = m_transport.GetClock().GetCurrentTick();
    request.SoundFilename = acEvent.VoiceFile;

    m_transport.Send(request);
}

void CharacterService::OnNotifyDialogue(const NotifyDialogue& acMessage) noexcept
{
    // Voice and remote actor animation share a presentation clock. Playing
    // immediately on packet arrival makes the voice lead the buffered pose.
    if (m_pendingVoices.size() >= 64)
        m_pendingVoices.erase(m_pendingVoices.begin());
    m_pendingVoices.push_back({acMessage.ServerId, acMessage.Tick, acMessage.SoundFilename});
}

void CharacterService::OnSubtitleEvent(const SubtitleEvent& acEvent) noexcept
{
    if (!m_transport.IsConnected())
        return;

    auto view = m_world.view<FormIdComponent>(entt::exclude<ObjectComponent>);
    auto entityIt = std::find_if(view.begin(), view.end(), [view, formId = acEvent.SpeakerID](auto entity) { return view.get<FormIdComponent>(entity).Id == formId; });

    if (entityIt == view.end())
        return;

    auto serverIdRes = Utils::GetServerId(*entityIt);
    if (!serverIdRes)
    {
        spdlog::error("{}: server id not found for form id {:X}", __FUNCTION__, acEvent.SpeakerID);
        return;
    }

    SubtitleRequest request{};
    request.ServerId = serverIdRes.value();
    request.Tick = m_transport.GetClock().GetCurrentTick();
    request.Text = acEvent.Text;
    request.TopicFormId = acEvent.TopicFormID;

    m_transport.Send(request);
}

void CharacterService::OnNotifySubtitle(const NotifySubtitle& acMessage) noexcept
{
    if (m_pendingSubtitles.size() >= 64)
        m_pendingSubtitles.erase(m_pendingSubtitles.begin());
    m_pendingSubtitles.push_back({acMessage.ServerId, acMessage.Tick, acMessage.TopicFormId, acMessage.Text});
}

void CharacterService::RunPresentationEvents() noexcept
{
    const uint64_t presentationDelayMs = GetPresentationDelayMs();
    const auto now = m_transport.GetClock().GetCurrentTick();
    const auto presentationTick = now > presentationDelayMs ? now - presentationDelayMs : 0;

    for (auto it = m_pendingVoices.begin(); it != m_pendingVoices.end();)
    {
        if (it->Tick > presentationTick)
        {
            ++it;
            continue;
        }

        if (Actor* pActor = Utils::GetByServerId<Actor>(it->ServerId))
        {
            static uint32_t replayProbeCount = 0;
            if (replayProbeCount++ < 96)
                spdlog::info("Network voice replay actor {:X} hostTick={} presentationTick={} file={}",
                    pActor->formID, it->Tick, presentationTick, it->Filename.c_str());

            // Preserve a remote NPC's silent local scene handle and native
            // completion semantics; only its owner replay is audible.
            if (!pActor->GetExtension()->IsRemote())
                pActor->StopCurrentDialogue(true);
            pActor->SpeakSound(it->Filename.c_str());
        }
        it = m_pendingVoices.erase(it);
    }

    for (auto it = m_pendingSubtitles.begin(); it != m_pendingSubtitles.end();)
    {
        if (it->Tick > presentationTick)
        {
            ++it;
            continue;
        }

        if (Actor* pActor = Utils::GetByServerId<Actor>(it->ServerId))
        {
            auto* pInfo = Cast<TESTopicInfo>(TESForm::GetById(it->TopicFormId));
            SubtitleManager::Get()->ShowSubtitle(pActor, it->Text.c_str(), pInfo);
        }
        it = m_pendingSubtitles.erase(it);
    }
}

void CharacterService::OnNotifyActorTeleport(const NotifyActorTeleport& acMessage) noexcept
{
    auto& modSystem = m_world.GetModSystem();

    const uint32_t cActorId = World::Get().GetModSystem().GetGameId(acMessage.FormId);
    if (IsActorDiscoverySuppressed(cActorId))
        return;
    Actor* pActor = Cast<Actor>(TESForm::GetById(cActorId));
    if (!pActor)
    {
        spdlog::error(__FUNCTION__ ": failed to retrieve actor to teleport.");
        return;
    }

    MoveActor(pActor, acMessage.WorldSpaceId, acMessage.CellId, acMessage.Position);

    spdlog::info("Successfully teleported actor, form id: {:X}, world space: {:X}, cell: {:X}, position: ({}, {}, {})", pActor->formID, acMessage.WorldSpaceId.BaseId, acMessage.CellId.BaseId, acMessage.Position.x, acMessage.Position.y, acMessage.Position.z);
}

void CharacterService::OnPartyJoinedEvent(const PartyJoinedEvent& acEvent) noexcept
{
    // Takes ownership of all actors
    if (acEvent.IsLeader)
    {
        auto view = m_world.view<FormIdComponent>(entt::exclude<ObjectComponent>);
        Vector<entt::entity> entities(view.begin(), view.end());

        for (auto entity : entities)
            ProcessNewEntity(entity);
    }
}

void CharacterService::MoveActor(Actor* apActor, const GameId& acWorldSpaceId, const GameId& acCellId, const Vector3_NetQuantize& acPosition) const noexcept
{
    // Never a remote actor that is dying, dead, knocked down or ragdolling (ActorState1 lifeState
    // bits 21-24, knockState 25-27): MoveTo disables and re-enables it, reloading its 3D, which
    // showed as the falling intro prisoner going naked and landing at the owner's final spot. The
    // owner's ragdoll stream places its body (CorpseRagdollService).
    if (apActor && apActor->GetExtension() && apActor->GetExtension()->IsRemote())
    {
        const uint32_t flags1 = apActor->actorState.flags1;
        if (((flags1 >> 21) & 0xF) != 0 || ((flags1 >> 25) & 0x7) != 0 || apActor->IsDead())
        {
            spdlog::info("Left {:X} where it is: dying or ragdolling, its owner's ragdoll places it", apActor->formID);
            return;
        }
    }
    TESObjectCELL* pCell = nullptr;
    if (!acWorldSpaceId)
    {
        const uint32_t cCellId = m_world.GetModSystem().GetGameId(acCellId);
        pCell = Cast<TESObjectCELL>(TESForm::GetById(cCellId));
    }
    // In case of lazy-loading of exterior cells
    else
    {
        const uint32_t cWorldSpaceId = m_world.GetModSystem().GetGameId(acWorldSpaceId);
        TESWorldSpace* const pWorldSpace = Cast<TESWorldSpace>(TESForm::GetById(cWorldSpaceId));
        if (pWorldSpace)
        {
            GridCellCoords coordinates = GridCellCoords::CalculateGridCellCoords(acPosition);
            pCell = pWorldSpace->LoadCell(coordinates.X, coordinates.Y);
        }
    }

    if (!pCell)
    {
        spdlog::error(__FUNCTION__ ": failed to fetch cell to teleport, actor: {:X}, worldspace: {:X}, cell: {:X}, position: {}, {}, {}", apActor->formID, acWorldSpaceId.BaseId, acCellId.BaseId, acPosition.x, acPosition.y, acPosition.z);
        return;
    }

    // Moving either participant of an active native interaction through
    // MoveTo tears down both RefrInteraction links, even in the same cell.
    // Keep the pair intact; ordinary host position updates use interpolation.
    if (apActor->GetParentCellEx() == pCell &&
        apActor->GetNativeMountState().InteractionExtra)
        return;

    apActor->MoveTo(pCell, acPosition);
}

void CharacterService::ProcessNewEntity(entt::entity aEntity) const noexcept
{
    if (!m_transport.IsOnline())
        return;

    auto& formIdComponent = m_world.get<FormIdComponent>(aEntity);
    if (IsActorDiscoverySuppressed(formIdComponent.Id) || m_world.all_of<LeaderNativeClaim, LocalComponent>(aEntity))
        return;

    Actor* const pActor = Cast<Actor>(TESForm::GetById(formIdComponent.Id));
    if (!pActor)
    {
        spdlog::warn(__FUNCTION__ ": actor for new entity not found, form id: {:X}", formIdComponent.Id);
        return;
    }

    if (auto* pRemoteComponent = m_world.try_get<RemoteComponent>(aEntity); pRemoteComponent)
    {
        // TODO(cosideci): don't just take all actors (i.e. from other parties),
        // maybe check it server side, add a variable to the request.
        const auto& party = m_world.GetPartyService();
        if (party.IsInParty() && !pActor->IsTemporary() &&
            (!pActor->IsMount() || party.IsLeader()))
        {
            // Rider and mount must share a simulator before the first native
            // mount event. A leader in range may claim mounts; followers still
            // receive cell leases only when the leader is unavailable.
            spdlog::info("Requesting cell-validated ownership for actor {:X} with server id {:X}", pActor->formID, pRemoteComponent->Id);

            RequestOwnership(pActor->formID, pRemoteComponent->Id, aEntity);
        }
        else
            spdlog::info("New entity remotely managed, form id: {:X}, server id: {:X}", pActor->formID, pRemoteComponent->Id);

        return;
    }

    if (m_world.any_of<RemoteComponent, LocalComponent, WaitingForAssignmentComponent>(aEntity))
        return;

    CacheSystem::Setup(World::Get(), aEntity, pActor);

    RequestServerAssignment(aEntity);
}

void CharacterService::RequestServerAssignment(const entt::entity aEntity) const noexcept
{
    if (!m_transport.IsOnline())
        return;

    static uint32_t sCookieSeed = 0;

    const auto& formIdComponent = m_world.get<FormIdComponent>(aEntity);

    auto* pActor = Cast<Actor>(TESForm::GetById(formIdComponent.Id));
    if (!pActor)
        return;

    TESNPC* pNpc = Cast<TESNPC>(pActor->baseForm);
    if (!pNpc)
        return;

    AssignCharacterRequest message{};

    message.Cookie = sCookieSeed;

    if (!m_world.GetModSystem().GetServerModId(formIdComponent.Id, message.ReferenceId))
    {
        spdlog::error("Server reference id not found for form id {:X}", formIdComponent.Id);
        return;
    }

    // Actors discovered during the New Game transition can exist before the
    // engine has attached them to a cell. They will be visited again once the
    // cell is ready, so defer assignment instead of dereferencing null.
    const auto* pParentCell = pActor->parentCell;
    if (!pParentCell)
    {
        m_world.emplace_or_replace<DeferredAssignmentComponent>(aEntity);
        spdlog::debug("Deferring server assignment for actor {:X}: no parent cell yet", pActor->formID);
        return;
    }

    if (!m_world.GetModSystem().GetServerModId(pParentCell->formID, message.CellId))
    {
        spdlog::error("Server cell id not found for cell id {:X}", pParentCell->formID);
        return;
    }

    if (const auto pWorldSpace = pActor->GetWorldSpace())
    {
        if (!m_world.GetModSystem().GetServerModId(pWorldSpace->formID, message.WorldSpaceId))
            return;
    }

    message.Position = pActor->position;
    message.Rotation.x = pActor->rotation.x;
    message.Rotation.y = pActor->rotation.z;

    // Serialize the base form
    const auto isPlayer = (formIdComponent.Id == 0x14);
    const auto isTemporary = pActor->formID >= 0xFF000000;

    if (isPlayer)
    {
        pNpc->MarkChanged(0x2000800);
    }

    const auto changeFlags = pNpc->GetChangeFlags();

    if (isPlayer || changeFlags != 0)
    {
        message.ChangeFlags = changeFlags;
        pNpc->Serialize(&message.AppearanceBuffer);
    }

    if (isPlayer)
    {
        auto& entries = message.FaceTints.Entries;

        const auto& tints = PlayerCharacter::Get()->GetTints();

        entries.resize(tints.length);

        for (auto i = 0u; i < tints.length; ++i)
        {
            entries[i].Alpha = tints[i]->alpha;
            entries[i].Color = tints[i]->color;
            entries[i].Type = tints[i]->type;

            if (tints[i]->texture)
                entries[i].Name = tints[i]->texture->name.AsAscii();
        }
    }

    if (isPlayer)
    {
        auto& questLog = message.QuestContent.Entries;
        auto& modSystem = m_world.GetModSystem();

        for (const auto& objective : PlayerCharacter::Get()->objectives)
        {
            auto* pQuest = objective.instance->quest;
            if (!pQuest)
                continue;

            if (!QuestService::IsNonSyncableQuest(pQuest))
            {
                GameId id{};

                if (modSystem.GetServerModId(pQuest->formID, id))
                {
                    auto& entry = questLog.emplace_back();
                    entry.Stage = pQuest->currentStage;
                    entry.Id = id;
                }
            }
        }

        // remove duplicates
        const auto ip = std::unique(questLog.begin(), questLog.end());
        questLog.resize(std::distance(questLog.begin(), ip));
    }

    message.CurrentActorData = BuildActorData(pActor);

    message.FactionsContent = pActor->GetFactions();
    message.IsDragon = pActor->IsDragon();
    message.IsMount = pActor->IsMount();
    message.IsPlayerSummon = pActor->GetCommandingActor() && pActor->GetCommandingActor()->formID == 0x14;

    if (const TESNPC* pPick = pActor->GetLeveledPick())
    {
        const uint32_t pickFormId = pPick->formID;
        if (m_world.GetModSystem().GetServerModId(pickFormId, message.LeveledNpcPickId))
            spdlog::info("Captured leveled NPC pick {:X} for actor {:X} (base {:X})", pickFormId, pActor->formID, pNpc->formID);
        else
            spdlog::warn("Leveled NPC pick {:X} has no server id, identity sync skipped", pickFormId);
    }
    else if (pNpc->IsTemporary())
    {
        spdlog::info("No leveled pick recoverable for temp base {:X} (actor {:X}), identity sync unavailable", pNpc->formID, pActor->formID);
    }

    if (pNpc->IsTemporary())
        pNpc = pNpc->GetTemplateBase();

    if (isTemporary)
    {
        if (pNpc && !m_world.GetModSystem().GetServerModId(pNpc->formID, message.FormId))
        {
            spdlog::error("Server NPC form id not found for form id {:X}", pNpc->formID);
            return;
        }
    }

    // Serialize actions
    auto* const pExtension = pActor->GetExtension();

    message.LatestAction = pExtension->LatestAnimation;
    pActor->SaveAnimationVariables(message.LatestAction.Variables);

    spdlog::info("Request id: {:X}, cookie: {:X}, entity: {:X}", formIdComponent.Id, sCookieSeed, to_integral(aEntity));

    if (m_transport.Send(message))
    {
        m_world.emplace<WaitingForAssignmentComponent>(aEntity, sCookieSeed);

        sCookieSeed++;
    }
}

void CharacterService::CancelServerAssignment(const entt::entity aEntity, const uint32_t aFormId) const noexcept
{
    if (m_world.all_of<RemoteComponent>(aEntity))
    {
        Actor* pActor = Cast<Actor>(TESForm::GetById(aFormId));

        if (pActor)
        {
            if (pActor->IsTemporary())
            {
                spdlog::info("Temporary Remote Deleted {:X}", aFormId);
                pActor->Delete();
            }
            else
            {
                pActor->GetExtension()->SetRemote(false);
            }
        }

        DeleteRemoteEntityComponents(aEntity);

        return;
    }

    // Keep the cookie until the server response arrives so awarded ownership can be relinquished.
    if (m_world.all_of<WaitingForAssignmentComponent>(aEntity))
    {
        auto& waitingComponent = m_world.get<WaitingForAssignmentComponent>(aEntity);
        waitingComponent.Cancelled = true;
        return;
    }

    if (m_world.all_of<LocalComponent>(aEntity))
    {
        auto& localComponent = m_world.get<LocalComponent>(aEntity);

        RequestOwnershipTransfer request{};
        request.ServerId = localComponent.Id;
        request.OwnershipEpoch = localComponent.OwnershipEpoch;
        request.Reason = OwnershipReleaseReason::Relinquish;

        if (Actor* pActor = Cast<Actor>(TESForm::GetById(aFormId)))
        {
            if (!pActor->IsTemporary())
            {
                auto& modSystem = m_world.GetModSystem();

                if (TESWorldSpace* pWorldSpace = pActor->GetWorldSpace())
                {
                    if (!modSystem.GetServerModId(pWorldSpace->formID, request.WorldSpaceId))
                        spdlog::error("World space id not found, despite having a world space, {:X}", pWorldSpace->formID);
                }

                if (TESObjectCELL* pCell = pActor->GetParentCellEx())
                {
                    if (!modSystem.GetServerModId(pCell->formID, request.CellId))
                        spdlog::error("Cell id not found, despite having a cell, {:X}", pCell->formID);
                }

                request.Position = pActor->position;
            }
        }

        spdlog::info(
            "Transferring ownership of local actor, server id: {:X}, epoch: {}, worldspace: {:X}, cell: {:X}, position: "
            "({}, {}, {})",
            request.ServerId, request.OwnershipEpoch, request.WorldSpaceId.BaseId, request.CellId.BaseId, request.Position.x, request.Position.y, request.Position.z);

        m_transport.Send(request);

        m_world.remove<LocalAnimationComponent, LocalComponent>(aEntity);
    }
}

Actor* CharacterService::CreateCharacterForEntity(entt::entity aEntity) const noexcept
{
    auto* pWaitingFor3D = m_world.try_get<WaitingFor3D>(aEntity);
    auto* pInterpolationComponent = m_world.try_get<InterpolationComponent>(aEntity);

    if (!pWaitingFor3D || !pInterpolationComponent)
    {
        spdlog::error(__FUNCTION__ ": could not find WaitingFor3D or InterpolationComponent");
        return nullptr;
    }

    auto& acMessage = pWaitingFor3D->SpawnRequest;

    Actor* pActor = nullptr;

    // Custom forms
    if (acMessage.FormId == GameId{})
    {
        TESNPC* pNpc = nullptr;

        if (acMessage.BaseId != GameId{})
        {
            // Prefer the owner's resolved leveled pick over the lossy template base
            if (acMessage.LeveledNpcPickId != GameId{})
                pNpc = Cast<TESNPC>(TESForm::GetById(m_world.GetModSystem().GetGameId(acMessage.LeveledNpcPickId)));

            if (!pNpc)
                pNpc = Cast<TESNPC>(TESForm::GetById(m_world.GetModSystem().GetGameId(acMessage.BaseId)));

            if (!pNpc)
            {
                spdlog::error("Failed to retrieve NPC, it will not be spawned, possibly missing mod");
                return nullptr;
            }

            pNpc->Deserialize(acMessage.AppearanceBuffer, acMessage.ChangeFlags);
        }
        else
        {
            pNpc = TESNPC::Create(acMessage.AppearanceBuffer, acMessage.ChangeFlags);
            FaceGenSystem::Setup(m_world, aEntity, acMessage.FaceTints);
        }

        pActor = Actor::Create(pNpc);
    }

    auto& remoteComponent = m_world.get<RemoteComponent>(aEntity);

    if (!pActor)
    {
        spdlog::error(__FUNCTION__ ": could not spawn actor for remote server id {:X}.", remoteComponent.Id);
        return nullptr;
    }

    pActor->GetExtension()->SetRemote(true);
    pActor->rotation.x = acMessage.Rotation.x;
    pActor->rotation.z = acMessage.Rotation.y;
    pActor->MoveTo(PlayerCharacter::Get()->parentCell, pInterpolationComponent->Position);
    pActor->SetActorValues(acMessage.InitialActorValues);

    pActor->GetExtension()->SetPlayer(acMessage.IsPlayer);
    if (acMessage.IsPlayer)
    {
        pActor->SetIgnoreFriendlyHit(true);
        pActor->SetPlayerRespawnMode();
        m_world.emplace_or_replace<PlayerComponent>(aEntity, acMessage.PlayerId);
    }

    if (pActor->IsDead() != acMessage.IsDead)
        acMessage.IsDead ? pActor->Kill() : pActor->Respawn();

    spdlog::info("Spawned character for entity, server id: {:X}", remoteComponent.Id);

    return pActor;
}

ActorData CharacterService::BuildActorData(Actor* apActor) const noexcept
{
    ActorData actorData{};
    actorData.InitialActorValues = apActor->GetEssentialActorValues();
    actorData.InitialInventory = apActor->GetActorInventory();
    actorData.IsDead = apActor->IsDead();
    actorData.IsWeaponDrawn = apActor->actorState.IsWeaponFullyDrawn();

    return actorData;
}

void CharacterService::ApplyLeveledNpcPick(Actor* apActor, const GameId& acPickId) const noexcept
{
    if (acPickId == GameId{})
        return;

    TESNPC* pBase = Cast<TESNPC>(apActor->baseForm);
    if (!pBase)
        return;

    if (!LeveledNpcSystem::GetOriginalBase(apActor))
        {
        spdlog::warn("Leveled pick {:x}:{:x} received for actor {:X} without an original leveled base, keeping local base", acPickId.ModId, acPickId.BaseId, apActor->formID);
            return;
        }

    const uint32_t cPickId = World::Get().GetModSystem().GetGameId(acPickId);
    if (cPickId == 0)
    {
        spdlog::warn("Leveled NPC pick {:X}:{:X} not resolvable, possibly missing mod, keeping local pick", acPickId.ModId, acPickId.BaseId);
        return;
    }

    TESNPC* pPick = Cast<TESNPC>(TESForm::GetById(cPickId));
    if (!pPick)
    {
        spdlog::warn("Leveled NPC pick {:X} is not an NPC, keeping local pick", cPickId);
        return;
    }

    const TESNPC* pLocalPick = apActor->GetLeveledPick();
    const uint32_t localPickId = pLocalPick ? pLocalPick->formID : 0;

    // Even a pick matching the current base must supersede pending work.
    if (pBase->IsTemporary() && localPickId == cPickId && m_pendingLeveledConforms.find(apActor->formID) == m_pendingLeveledConforms.end())
    {
        spdlog::info("Leveled actor {:X} already matches owner's pick {:X}", apActor->formID, cPickId);
        return;
    }

    spdlog::info("Queued leveled NPC reconciliation for actor {:X}, base: {:X}, local pick: {:X}, owner's pick: {:X}",
        apActor->formID, pBase->formID, localPickId, cPickId);

    // Defer reference changes to the service update: cell attach may still own the actor here.
    // Queueing to the runner from a drained task would re-lock its drain mutex.
    // Preserve the stage when a newer pick arrives during a disable or rebuild.
    m_pendingLeveledConforms[apActor->formID] = cPickId;
}

void CharacterService::ProcessLeveledConforms() noexcept
{
    using ReconciliationStage = ActorExtension::ReconciliationStage;

    if (m_pendingLeveledConforms.empty())
        return;

    // Never touch references while the loading screen is up - the cell attach
    // owns them and mutating mid-stream crashes the loader
    UI* pUI = UI::Get();
    if (pUI && pUI->GetMenuOpen(BSFixedString("Loading Menu")))
        return;

    for (auto it = m_pendingLeveledConforms.begin(); it != m_pendingLeveledConforms.end();)
    {
        const uint32_t cPickFormId = it->second;

        Actor* pActor = Cast<Actor>(TESForm::GetById(it->first));
        TESNPC* pPick = Cast<TESNPC>(TESForm::GetById(cPickFormId));
        if (!pActor || pActor->IsDeleted() || !pPick)
        {
            if (pActor)
                pActor->GetExtension()->Reconciliation = ReconciliationStage::None;

            it = m_pendingLeveledConforms.erase(it);
            continue;
        }

        auto& stage = pActor->GetExtension()->Reconciliation;
        if (stage == ReconciliationStage::WaitingFor3D)
        {
            const auto* pCell = pActor->GetParentCellEx();
            if (!pCell || !pCell->IsAttached())
            {
                spdlog::info("Abandoning leveled NPC reconciliation for actor {:X} because its cell is not attached, pick: {:X}, cell state: {}, disabled: {}",
                    it->first, cPickFormId, pCell ? static_cast<int>(pCell->cellState) : -1, pActor->IsDisabled());
                stage = ReconciliationStage::None;
                it = m_pendingLeveledConforms.erase(it);
                continue;
            }

            if (pActor->IsDisabled() || !pActor->GetNiNode())
            {
                ++it;
                continue;
            }

            if (pActor->baseForm && pActor->baseForm->IsTemporary() && pActor->GetLeveledPick() == pPick)
            {
                spdlog::info("Completed leveled NPC reconciliation for actor {:X}, base: {:X}, pick: {:X}", it->first, pActor->baseForm->formID, cPickFormId);
                stage = ReconciliationStage::None;
                it = m_pendingLeveledConforms.erase(it);
                continue;
            }

            // A newer pick arrived during the rebuild; start its disable now.
            stage = ReconciliationStage::None;
        }

        if (stage == ReconciliationStage::WaitingForDisable)
        {
            if (!pActor->IsDisabled() || pActor->GetNiNode())
            {
                spdlog::debug("Waiting for leveled actor {:X} to finish disabling before applying pick {:X}, disabled: {}, has 3D: {}",
                    it->first, cPickFormId, pActor->IsDisabled(), pActor->GetNiNode() != nullptr);
                ++it;
                continue;
            }

            if (!LeveledNpcSystem::ApplyPick(pActor, pPick))
            {
                spdlog::warn("Could not rebuild leveled actor {:X} from its original base and pick {:X}, keeping local base", it->first, cPickFormId);
            pActor->EnableImpl();
                stage = ReconciliationStage::None;
                it = m_pendingLeveledConforms.erase(it);
                continue;
            }

            // Recompute the graph descriptor after changing picks; stale variable indices can cause out-of-bounds writes.
            pActor->GetExtension()->GraphDescriptorHash = 0;

            // Enable can return before the rebuilt 3D is available to discovery.
            stage = ReconciliationStage::WaitingFor3D;
            pActor->EnableImpl();
            spdlog::info("Re-enabled conformed leveled actor {:X}, base: {:X}, pick: {:X}, waiting for 3D",
                it->first, pActor->baseForm->formID, cPickFormId);
            ++it;
            continue;
        }

        if (!pActor->loadedState && !LeveledNpcSystem::IsLeveledNpcBase(Cast<TESNPC>(pActor->baseForm)))
        {
            // Wait for distant actors to load 3D; newer picks replace pending work and disconnects clear it.
            // Unresolved shells bypass this wait because they need a pick before they can load a model.
            ++it;
            continue;
        }

        // DisableImpl() is asynchronous: it only queues a request to disable this actor.
        // Wait for the disabled flag and old 3D removal before changing the base.
        pActor->DisableImpl();
        stage = ReconciliationStage::WaitingForDisable;
        ++it;
    }
}

void CharacterService::RunLocalUpdates() const noexcept
{
    static std::chrono::steady_clock::time_point lastSendTimePoint;
    constexpr auto cDelayBetweenSnapshots = 50ms;

    const auto now = std::chrono::steady_clock::now();
    if (now - lastSendTimePoint < cDelayBetweenSnapshots)
        return;

    lastSendTimePoint = now;

    ClientReferencesMoveRequest message;
    message.Tick = m_transport.GetClock().GetCurrentTick();

    auto animatedLocalView = m_world.view<LocalComponent, LocalAnimationComponent, FormIdComponent>();

    // Keep the nearby 20 Hz budget, then service overdue distance tiers fairly. The
    // old four-slot rotation spent slots on actors already in the nearby set and
    // included actors in unrelated interiors. Interest includes every party player.
    constexpr size_t cNearPoseActors = 24;
    constexpr size_t cTierPoseActors = 8;
    static std::unordered_map<uint32_t, std::chrono::steady_clock::time_point> lastPoseAttempt;
    Set<entt::entity> selectedPoseActors;
    {
        struct Candidate
        {
            float Distance;
            entt::entity Entity;
            uint32_t FormId;
            double Overdue{};
        };
        std::vector<Candidate> distances;
        auto* pLocalPlayer = PlayerCharacter::Get();
        std::vector<Actor*> playerActors;
        if (pLocalPlayer && pLocalPlayer->parentCell)
            playerActors.push_back(pLocalPlayer);
        auto players = m_world.view<FormIdComponent, PlayerComponent>();
        for (auto player : players)
        {
            const auto formId = players.get<FormIdComponent>(player).Id;
            if (formId == 0x14)
                continue;
            if (auto* pActor = Cast<Actor>(TESForm::GetById(formId)); pActor && IsLoadedActor(pActor))
                playerActors.push_back(pActor);
        }
        for (auto entity : animatedLocalView)
        {
            auto* pActor = Cast<Actor>(TESForm::GetById(animatedLocalView.get<FormIdComponent>(entity).Id));
            if (!pActor || !IsLoadedActor(pActor) || !pActor->parentCell || pActor == pLocalPlayer)
                continue;
            float nearest = (std::numeric_limits<float>::max)();
            for (const auto* playerActor : playerActors)
            {
                const auto* cell = pActor->parentCell;
                if (!playerActor->parentCell || (cell != playerActor->parentCell &&
                    (!cell->worldspace || cell->worldspace != playerActor->parentCell->worldspace)))
                    continue;
                const auto d = pActor->position - playerActor->position;
                nearest = (std::min)(nearest, d.x * d.x + d.y * d.y + d.z * d.z);
            }
            if (nearest < (std::numeric_limits<float>::max)())
                distances.push_back({nearest, entity, pActor->formID});
        }
        std::sort(distances.begin(), distances.end(), [](const auto& a, const auto& b) {
            return a.Distance == b.Distance ? a.FormId < b.FormId : a.Distance < b.Distance;
        });
        size_t nearby = 0;
        while (nearby < distances.size() && nearby < cNearPoseActors &&
            distances[nearby].Distance <= 4096.f * 4096.f)
        {
            selectedPoseActors.insert(distances[nearby].Entity);
            lastPoseAttempt[distances[nearby].FormId] = now;
            ++nearby;
        }
        // 10 Hz to 16384 units, 5 Hz to 32768, 1 Hz beyond. These are target
        // rates within eight slots per snapshot, not promises under crowd overload.
        for (size_t i = nearby; i < distances.size(); ++i)
        {
            auto& candidate = distances[i];
            const double interval = candidate.Distance <= 16384.f * 16384.f ? 100.0 :
                candidate.Distance <= 32768.f * 32768.f ? 200.0 : 1000.0;
            const auto it = lastPoseAttempt.try_emplace(candidate.FormId, now - 1s).first;
            candidate.Overdue = std::chrono::duration<double, std::milli>(now - it->second).count() / interval;
        }
        std::sort(distances.begin() + nearby, distances.end(), [](const auto& a, const auto& b) {
            return a.Overdue == b.Overdue ? a.FormId < b.FormId : a.Overdue > b.Overdue;
        });
        for (size_t i = nearby; i < distances.size() && i < nearby + cTierPoseActors; ++i)
        {
            if (distances[i].Overdue < 1.0)
                break;
            selectedPoseActors.insert(distances[i].Entity);
            lastPoseAttempt[distances[i].FormId] = now;
        }
        // Never retain scheduling state for actors that unloaded or changed authority.
        Set<uint32_t> live;
        for (const auto& candidate : distances)
            live.insert(candidate.FormId);
        for (auto it = lastPoseAttempt.begin(); it != lastPoseAttempt.end();)
            it = live.contains(it->first) ? std::next(it) : lastPoseAttempt.erase(it);
    }
    uint64_t selectedSerializeUs = 0;
    uint32_t selectedActors = 0;

    for (auto entity : animatedLocalView)
    {
        auto& localComponent = animatedLocalView.get<LocalComponent>(entity);
        auto& animationComponent = animatedLocalView.get<LocalAnimationComponent>(entity);
        auto& formIdComponent = animatedLocalView.get<FormIdComponent>(entity);
        auto* pActor = Cast<Actor>(TESForm::GetById(formIdComponent.Id));
        if (IsLeaderNativeActor(pActor) && !IsLoadedActor(pActor))
            continue;

        // Bound selection to 24 nearby actors, this player, and eight distance-tier slots.
        // Selection is an attempt, not proof of a fresh capture: culled actors send movement
        // and actions while the other PC animates their missing pose locally.
        const bool capturePose = selectedPoseActors.contains(entity) || formIdComponent.Id == 0x14;
        const auto serializeStarted = capturePose ?
            std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
        AnimationSystem::Serialize(m_world, message, localComponent,
            animationComponent, formIdComponent, capturePose);
        if (formIdComponent.Id == 0x14)
        {
            const auto update = message.Updates.find(localComponent.Id);
            if (update != message.Updates.end())
                HeadTrackService::FillLocalMovement(update.value().UpdatedMovement);
        }
        if (capturePose)
        {
            // Measures selected serialization/request work. Delayed captures may
            // occur in other calls; native graph work is outside this timer.
            const auto elapsedUs = static_cast<uint32_t>((std::min)(
                int64_t{UINT32_MAX}, std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now() - serializeStarted).count()));
            selectedSerializeUs += elapsedUs;
            ++selectedActors;
            auto previousMax = m_localPoseMaxActorUs.load(std::memory_order_relaxed);
            while (elapsedUs > previousMax &&
                !m_localPoseMaxActorUs.compare_exchange_weak(previousMax, elapsedUs,
                    std::memory_order_relaxed)) {}
        }
    }

    m_localPoseBatches.fetch_add(1, std::memory_order_relaxed);
    m_localPoseActors.fetch_add(selectedActors, std::memory_order_relaxed);
    m_localPoseTotalUs.fetch_add(selectedSerializeUs, std::memory_order_relaxed);
    const auto batchUs = static_cast<uint32_t>((std::min)(
        uint64_t{UINT32_MAX}, selectedSerializeUs));
    m_localPoseLastBatchUs.store(batchUs, std::memory_order_relaxed);
    m_localPoseLastBatchActors.store(selectedActors, std::memory_order_relaxed);

    m_transport.Send(message);
}

void CharacterService::RunRemoteUpdates() noexcept
{
    // Keep actor, visual-pose, voice, and subtitle playback on one timeline.
    const auto smoothNow = SmoothClock::NowTick();
    const auto now = smoothNow ? smoothNow : m_transport.GetClock().GetCurrentTick();
    const auto delay = static_cast<uint64_t>(GetPresentationDelayMs());
    const auto tick = now > delay ? now - delay : 0;
    PoseCopyAuthority::SetPresentationDelayMs(static_cast<uint32_t>(delay));
    VisualPoseMailbox::SetPresentationTick(tick);
    PoseCopyAuthority::SetPresentationTick(tick);

    // Interpolation has to keep running even if the actor is not in view, otherwise we will never know if we need to spawn it
    auto interpolatedEntities = m_world.view<RemoteComponent, InterpolationComponent>();
    static uint64_t s_lastCorpseCorrectionMs = 0;
    if (now < s_lastCorpseCorrectionMs)
        s_lastCorpseCorrectionMs = 0;
    struct PendingCorpseCorrection
    {
        uint32_t ActorId{};
        uint32_t PriorCellId{};
        uint32_t TargetCellId{};
        uint32_t Attempt{};
        float Error{};
        GameId WorldSpaceId{};
        GameId CellId{};
        Vector3_NetQuantize Position{};
    };
    std::optional<PendingCorpseCorrection> pendingCorpseCorrection;

    for (auto entity : interpolatedEntities)
    {
        auto* pFormIdComponent = m_world.try_get<FormIdComponent>(entity);
        auto& interpolationComponent = interpolatedEntities.get<InterpolationComponent>(entity);

        Actor* pActor = nullptr;
        if (pFormIdComponent)
        {
            auto* pForm = TESForm::GetById(pFormIdComponent->Id);
            pActor = Cast<Actor>(pForm);
        }

        InterpolationSystem::Update(pActor, interpolationComponent, tick);

        // Dead remote actors no longer receive per-frame ForcePosition: that
        // fought their ragdolls. Reconcile only a settled owner corpse that
        // ended in a different loaded cell or drifted far from the owner.
        if (pendingCorpseCorrection || !pActor ||
            !pActor->actorState.IsDeadState() ||
            CorpseRagdollService::IsFollowingOwner(pActor->formID) ||
            !pActor->GetNiNode() || !interpolationComponent.AuthorityCellId ||
            !interpolationComponent.AuthorityTick ||
            now < interpolationComponent.AuthorityStableSinceTick + 1000 ||
            now < s_lastCorpseCorrectionMs + 250 ||
            now < interpolationComponent.AuthorityTick ||
            now - interpolationComponent.AuthorityTick > 2000 ||
            pActor->GetNativeMountState().InteractionExtra)
            continue;

        auto* pExtension = pActor->GetExtension();
        if (pExtension && pExtension->IsPlayer())
            continue;

        auto* pPlayer = PlayerCharacter::Get();
        if (!pPlayer)
            continue;
        const uint32_t targetCellId = m_world.GetModSystem().GetGameId(
            interpolationComponent.AuthorityCellId);
        if (!targetCellId)
            continue;
        if (interpolationComponent.AuthorityWorldSpaceId)
        {
            const uint32_t targetWorldId = m_world.GetModSystem().GetGameId(
                interpolationComponent.AuthorityWorldSpaceId);
            auto* pWorldSpace = pPlayer->GetWorldSpace();
            if (!targetWorldId || !pWorldSpace ||
                pWorldSpace->formID != targetWorldId)
                continue;
            const auto corpseCoords = GridCellCoords::CalculateGridCellCoords(
                interpolationComponent.AuthorityPosition.x,
                interpolationComponent.AuthorityPosition.y);
            const auto* pTES = TES::Get();
            if (!pTES || !GridCellCoords::IsCellInGridCell(corpseCoords,
                    {pTES->centerGridX, pTES->centerGridY}, false))
                continue;
        }
        else
        {
            auto* pPlayerCell = pPlayer->GetParentCellEx();
            if (!pPlayerCell || pPlayerCell->formID != targetCellId)
                continue;
        }

        auto* pCurrentCell = pActor->GetParentCellEx();
        const auto positionError = pActor->position - NiPoint3{
            interpolationComponent.AuthorityPosition};
        const float errorSquared = positionError.x * positionError.x +
            positionError.y * positionError.y + positionError.z * positionError.z;
        // Exterior parent-cell IDs can lag ragdoll movement across a grid
        // boundary. Exterior corrections follow the owner's position, not
        // that potentially stale parent-cell field.
        const bool cellMismatch = !pCurrentCell ||
            pCurrentCell->formID != targetCellId;
        if (errorSquared <= 128.f * 128.f &&
            (interpolationComponent.AuthorityWorldSpaceId || !cellMismatch))
            continue;
        const auto correctionStep = interpolationComponent.AuthorityPosition -
            interpolationComponent.LastCorpseCorrectionPosition;
        const bool sameTarget =
            interpolationComponent.LastCorpseCorrectionCellId == targetCellId &&
            glm::dot(correctionStep, correctionStep) <= 16.f * 16.f;
        if (!sameTarget)
            interpolationComponent.CorpseCorrectionAttemptsForTarget = 0;
        if (interpolationComponent.CorpseCorrectionAttemptsForTarget >= 2 ||
            (sameTarget && now <
                interpolationComponent.LastCorpseCorrectionTick + 5000))
            continue;

        const uint32_t actorId = pActor->formID;
        const uint32_t priorCellId = pCurrentCell ? pCurrentCell->formID : 0;
        interpolationComponent.LastCorpseCorrectionTick = now;
        interpolationComponent.LastCorpseCorrectionCellId = targetCellId;
        interpolationComponent.LastCorpseCorrectionPosition =
            interpolationComponent.AuthorityPosition;
        ++interpolationComponent.CorpseCorrectionAttempts;
        ++interpolationComponent.CorpseCorrectionAttemptsForTarget;
        s_lastCorpseCorrectionMs = now;
        Vector3_NetQuantize targetPosition;
        targetPosition = interpolationComponent.AuthorityPosition;
        pendingCorpseCorrection = PendingCorpseCorrection{actorId, priorCellId,
            targetCellId, interpolationComponent.CorpseCorrectionAttempts,
            std::sqrt(errorSquared),
            interpolationComponent.AuthorityWorldSpaceId,
            interpolationComponent.AuthorityCellId, targetPosition};
    }

    if (pendingCorpseCorrection)
    {
        const auto& correction = *pendingCorpseCorrection;
        auto* pActor = Cast<Actor>(TESForm::GetById(correction.ActorId));
        if (pActor && pActor->GetExtension()->IsRemote() &&
            pActor->actorState.IsDeadState())
        {
            MoveActor(pActor, correction.WorldSpaceId, correction.CellId,
                correction.Position);
            spdlog::info("Corpse cell correction actor={:X} sourceCell={:X} hostCell={:X} error={} attempt={}",
                correction.ActorId, correction.PriorCellId,
                correction.TargetCellId, correction.Error, correction.Attempt);
        }
    }

    auto animatedView = m_world.view<RemoteComponent, RemoteAnimationComponent, FormIdComponent>();

    for (auto entity : animatedView)
    {
        auto& animationComponent = animatedView.get<RemoteAnimationComponent>(entity);
        auto& formIdComponent = animatedView.get<FormIdComponent>(entity);

        auto* pForm = TESForm::GetById(formIdComponent.Id);
        auto* pActor = Cast<Actor>(pForm);
        if (!pActor)
            continue;

        auto& targetPoints = animationComponent.CombatTargetTimePoints;
        while (!targetPoints.empty() && targetPoints.front().Tick <= tick)
        {
            animationComponent.DesiredCombatTargetTick = targetPoints.front().Tick;
            animationComponent.DesiredCombatTargetServerId =
                targetPoints.front().ServerId;
            targetPoints.pop_front();
        }
        if (animationComponent.DesiredCombatTargetServerId == 0xFFFFFFFFu)
        {
            if (auto* pExtension = pActor->GetExtension())
                pExtension->PresentedCombatTargetFormId.store(
                    0xFFFFFFFFu, std::memory_order_release);
        }

        // A target choice is a small authoritative input to combat and gaze.
        // Keep it on the movement presentation timeline. An unresolved owner
        // target must not be mistaken for "none" on the follower.
        if (animationComponent.DesiredCombatTargetTick &&
            animationComponent.DesiredCombatTargetTick <= tick &&
            animationComponent.DesiredCombatTargetServerId != 0xFFFFFFFFu &&
            pActor->pCombatController &&
            (animationComponent.LastCombatTargetApplyTick == 0 ||
                now - animationComponent.LastCombatTargetApplyTick >= 100))
        {
            Actor* pDesiredTarget = animationComponent.DesiredCombatTargetServerId ?
                Utils::GetByServerId<Actor>(
                    animationComponent.DesiredCombatTargetServerId) : nullptr;
            if (!animationComponent.DesiredCombatTargetServerId || pDesiredTarget)
            {
                if (auto* pExtension = pActor->GetExtension();
                    pExtension && !pExtension->IsPlayer())
                    pExtension->PresentedCombatTargetFormId.store(
                        pDesiredTarget ? pDesiredTarget->formID : 0,
                        std::memory_order_release);
                auto* pNativeTarget = Cast<Actor>(TESObjectREFR::GetByHandle(
                    pActor->pCombatController->targetHandle));
                if (pNativeTarget != pDesiredTarget)
                {
                    animationComponent.LastCombatTargetApplyDesiredFormId =
                        pDesiredTarget ? pDesiredTarget->formID : 0;
                    animationComponent.LastCombatTargetApplyBeforeFormId =
                        pNativeTarget ? pNativeTarget->formID : 0;
                    pActor->SetCombatTargetEx(pDesiredTarget);
                    auto* pAfterTarget = Cast<Actor>(TESObjectREFR::GetByHandle(
                        pActor->pCombatController->targetHandle));
                    animationComponent.LastCombatTargetApplyAfterFormId =
                        pAfterTarget ? pAfterTarget->formID : 0;
                    animationComponent.LastCombatTargetApplyTick = now;
                }
            }
        }

        AnimationSystem::Update(m_world, pActor, animationComponent, tick);
        // The opt-in presenter writes only after the native graph update.
        // A second write here ran before later native animation jobs and
        // caused host/local pose oscillation and severe per-frame work.
    }

    auto facegenView = m_world.view<FormIdComponent, FaceGenComponent>();

    for (auto entity : facegenView)
    {
        auto& formIdComponent = facegenView.get<FormIdComponent>(entity);
        auto& faceGenComponent = facegenView.get<FaceGenComponent>(entity);

        const auto* pForm = TESForm::GetById(formIdComponent.Id);
        auto* pActor = Cast<Actor>(pForm);
        if (!pActor)
            continue;

        FaceGenSystem::Update(m_world, pActor, faceGenComponent);
    }

    auto waitingView = m_world.view<FormIdComponent, WaitingFor3D>();

    Vector<entt::entity> readyEntities;
    for (auto entity : waitingView)
    {
        auto& formIdComponent = waitingView.get<FormIdComponent>(entity);
        auto& waitingFor3D = waitingView.get<WaitingFor3D>(entity);

        Actor* pActor = Cast<Actor>(TESForm::GetById(formIdComponent.Id));
        if (!pActor || !pActor->GetNiNode())
            continue;

        // By now, the actor has materialized in the world and is ready for further setup

        pActor->SetActorInventory(waitingFor3D.SpawnRequest.InventoryContent);
        pActor->SetFactions(waitingFor3D.SpawnRequest.FactionsContent);
        if (!waitingFor3D.SpawnRequest.ActionsToReplay.Actions.empty())
            pActor->LoadAnimationVariables(waitingFor3D.SpawnRequest.ActionsToReplay.Actions[0].Variables);
        m_weaponDrawUpdates[pActor->formID] = {waitingFor3D.SpawnRequest.IsWeaponDrawn};

        if (pActor->IsDead() != waitingFor3D.SpawnRequest.IsDead)
            waitingFor3D.SpawnRequest.IsDead ? pActor->Kill() : pActor->Respawn();

        if (pActor->IsVampireLord())
            pActor->FixVampireLordModel();

        readyEntities.push_back(entity);

        spdlog::info("Applied 3D for actor, form id: {:X}", pActor->formID);
    }

    for (auto entity : readyEntities)
    {
        m_world.remove<WaitingFor3D>(entity);

        // Reprocess the remote actor now that an ownership grant can be accepted without immediately declining it.
        ProcessNewEntity(entity);
    }
}

void CharacterService::RunFactionsUpdates() const noexcept
{
    static std::chrono::steady_clock::time_point lastSendTimePoint;
    constexpr auto cDelayBetweenSnapshots = 2000ms;

    const auto now = std::chrono::steady_clock::now();
    if (now - lastSendTimePoint < cDelayBetweenSnapshots)
        return;

    lastSendTimePoint = now;

    RequestFactionsChanges message;

    auto factionedActors = m_world.view<LocalComponent, CacheComponent, FormIdComponent>();
    for (auto entity : factionedActors)
    {
        auto& formIdComponent = factionedActors.get<FormIdComponent>(entity);
        auto& localComponent = factionedActors.get<LocalComponent>(entity);
        auto& cacheComponent = factionedActors.get<CacheComponent>(entity);

        const auto* pForm = TESForm::GetById(formIdComponent.Id);
        const auto* pActor = Cast<Actor>(pForm);
        if (!pActor)
            continue;

        // Check if cached factions and current factions are identical
        auto factions = pActor->GetFactions();

        if (cacheComponent.FactionsContent == factions)
            continue;

        cacheComponent.FactionsContent = factions;

        // If not send the current factions and replace the cached factions
        message.Changes[localComponent.Id] = factions;
    }

    if (!message.Changes.empty())
        m_transport.Send(message);
}

void CharacterService::RunSpawnUpdates() const noexcept
{
    auto invisibleView = m_world.view<RemoteComponent, InterpolationComponent, RemoteAnimationComponent, WaitingFor3D>(entt::exclude<FormIdComponent>);
    Vector<entt::entity> entities(invisibleView.begin(), invisibleView.end());

    for (const auto entity : entities)
    {
        auto& remoteComponent = m_world.get<RemoteComponent>(entity);
        auto& interpolationComponent = m_world.get<InterpolationComponent>(entity);

        if (const auto pWorldSpace = PlayerCharacter::Get()->GetWorldSpace())
        {
            float characterX = interpolationComponent.Position.x;
            float characterY = interpolationComponent.Position.y;
            const auto characterCoords = GridCellCoords::CalculateGridCellCoords(characterX, characterY);
            const TES* pTES = TES::Get();
            const auto playerCoords = GridCellCoords(pTES->centerGridX, pTES->centerGridY);

            // TODO(cosideci): IsDragon probably shouldn't be straight up false here.
            if (GridCellCoords::IsCellInGridCell(characterCoords, playerCoords, false))
            {
                auto* pActor = Cast<Actor>(TESForm::GetById(remoteComponent.CachedRefId));
                if (!pActor)
                {
                    pActor = CreateCharacterForEntity(entity);
                    if (!pActor)
                        continue;

                    remoteComponent.CachedRefId = pActor->formID;
                }

                pActor->MoveTo(PlayerCharacter::Get()->parentCell, interpolationComponent.Position);
            }
        }
    }
}

void CharacterService::RunExperienceUpdates() noexcept
{
    static std::chrono::steady_clock::time_point lastSendTimePoint;
    constexpr auto cDelayBetweenSnapshots = 1000ms;

    const auto now = std::chrono::steady_clock::now();
    if (now - lastSendTimePoint < cDelayBetweenSnapshots)
        return;

    lastSendTimePoint = now;

    if (m_cachedExperience == 0.f)
        return;

    if (!World::Get().GetPartyService().IsInParty())
        return;

    SyncExperienceRequest message;
    message.Experience = m_cachedExperience;

    m_cachedExperience = 0.f;

    m_transport.Send(message);

    spdlog::debug("Sending over experience {}", message.Experience);
}

void CharacterService::ApplyCachedWeaponDraws(const UpdateEvent& acUpdateEvent) noexcept
{
    std::vector<uint32_t> toRemove{};

    for (auto& [cId, _] : m_weaponDrawUpdates)
    {
        auto& data = m_weaponDrawUpdates[cId];

        data.m_timer += acUpdateEvent.Delta;

        // Remote actors get 2 passes because Skyrim's weapon drawing is the most finnicky thing in existence.
        double maxTime = data.m_isFirstPass ? 0.5 : 2.0;
        if (data.m_timer <= maxTime)
            continue;

        Actor* pActor = Cast<Actor>(TESForm::GetById(cId));
        if (!pActor || !pActor->GetExtension()->IsRemote())
        {
            toRemove.push_back(cId);
            continue;
        }

        pActor->SetWeaponDrawnEx(data.m_drawWeapon);

        if (!data.m_isFirstPass)
            toRemove.push_back(cId);

        data.m_isFirstPass = false;
    }

    for (uint32_t id : toRemove)
        m_weaponDrawUpdates.erase(id);
}
