#include <Messages/PlayerAppearanceRequest.h>
#include <Messages/NotifyPlayerAppearance.h>
#include <Services/CharacterService.h>
#include <Components.h>
#include <GameServer.h>
#include <World.h>

#include <atomic>
#include <vector>

#include <Events/CharacterSpawnedEvent.h>
#include <Events/CharacterExteriorCellChangeEvent.h>
#include <Events/CharacterInteriorCellChangeEvent.h>
#include <Events/PlayerEnterWorldEvent.h>
#include <Events/UpdateEvent.h>
#include <Events/CharacterRemoveEvent.h>
#include <Events/OwnershipTransferEvent.h>

#include <Game/OwnerView.h>

#include <Messages/AssignCharacterRequest.h>
#include <Messages/AssignCharacterResponse.h>
#include <Messages/ServerReferencesMoveRequest.h>
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
#include <Messages/SyncExperienceRequest.h>
#include <Messages/NotifySyncExperience.h>
#include <Messages/DialogueRequest.h>
#include <Messages/NotifyDialogue.h>
#include <Messages/SubtitleRequest.h>
#include <Messages/NotifySubtitle.h>
#include <Messages/NotifyActorTeleport.h>
#include <Messages/CorpseRagdollRequest.h>
#include <Messages/NotifyCorpseRagdoll.h>

#include <Setting.h>
namespace
{
Console::Setting bEnableXpSync{"Gameplay:bEnableXpSync", "Syncs combat XP within the party", true};

// A temporary reference's FF form ID is local to one game process. Record
// which party members have already bound their native copy to this entity.
struct TemporaryActorProvenance
{
    uint64_t CreatedTick{};
    glm::vec3 CreationPosition{};
    std::vector<uint32_t> BoundPlayerIds;
};
}

CharacterService::CharacterService(World& aWorld, entt::dispatcher& aDispatcher) noexcept
    : m_world(aWorld)
    , m_updateConnection(aDispatcher.sink<UpdateEvent>().connect<&CharacterService::OnUpdate>(this))
    , m_interiorCellChangeEventConnection(aDispatcher.sink<CharacterInteriorCellChangeEvent>().connect<&CharacterService::OnCharacterInteriorCellChange>(this))
    , m_exteriorCellChangeEventConnection(aDispatcher.sink<CharacterExteriorCellChangeEvent>().connect<&CharacterService::OnCharacterExteriorCellChange>(this))
    , m_characterAssignRequestConnection(aDispatcher.sink<PacketEvent<AssignCharacterRequest>>().connect<&CharacterService::OnAssignCharacterRequest>(this))
    , m_transferOwnershipConnection(aDispatcher.sink<PacketEvent<RequestOwnershipTransfer>>().connect<&CharacterService::OnOwnershipTransferRequest>(this))
    , m_ownershipTransferEventConnection(aDispatcher.sink<OwnershipTransferEvent>().connect<&CharacterService::OnOwnershipTransferEvent>(this))
    , m_claimOwnershipConnection(aDispatcher.sink<PacketEvent<RequestOwnershipClaim>>().connect<&CharacterService::OnOwnershipClaimRequest>(this))
    , m_removeCharacterConnection(aDispatcher.sink<CharacterRemoveEvent>().connect<&CharacterService::OnCharacterRemoveEvent>(this))
    , m_characterSpawnedConnection(aDispatcher.sink<CharacterSpawnedEvent>().connect<&CharacterService::OnCharacterSpawned>(this))
    , m_referenceMovementSnapshotConnection(aDispatcher.sink<PacketEvent<ClientReferencesMoveRequest>>().connect<&CharacterService::OnReferencesMoveRequest>(this))
    , m_corpseRagdollConnection(aDispatcher.sink<PacketEvent<CorpseRagdollRequest>>().connect<&CharacterService::OnCorpseRagdoll>(this))
    , m_playerAppearanceConnection(aDispatcher.sink<PacketEvent<PlayerAppearanceRequest>>().connect<&CharacterService::OnPlayerAppearance>(this))
    , m_factionsChangesConnection(aDispatcher.sink<PacketEvent<RequestFactionsChanges>>().connect<&CharacterService::OnFactionsChanges>(this))
    , m_mountConnection(aDispatcher.sink<PacketEvent<MountRequest>>().connect<&CharacterService::OnMountRequest>(this))
    , m_newPackageConnection(aDispatcher.sink<PacketEvent<NewPackageRequest>>().connect<&CharacterService::OnNewPackageRequest>(this))
    , m_requestRespawnConnection(aDispatcher.sink<PacketEvent<RequestRespawn>>().connect<&CharacterService::OnRequestRespawn>(this))
    , m_syncExperienceConnection(aDispatcher.sink<PacketEvent<SyncExperienceRequest>>().connect<&CharacterService::OnSyncExperienceRequest>(this))
    , m_dialogueConnection(aDispatcher.sink<PacketEvent<DialogueRequest>>().connect<&CharacterService::OnDialogueRequest>(this))
    , m_subtitleConnection(aDispatcher.sink<PacketEvent<SubtitleRequest>>().connect<&CharacterService::OnSubtitleRequest>(this))
{
}

void CharacterService::Serialize(World& aRegistry, entt::entity aEntity, CharacterSpawnRequest* apSpawnRequest) noexcept
{
    const auto& characterComponent = aRegistry.get<CharacterComponent>(aEntity);

    apSpawnRequest->ServerId = World::ToInteger(aEntity);
    apSpawnRequest->AppearanceBuffer = characterComponent.SaveBuffer;
    apSpawnRequest->ChangeFlags = characterComponent.ChangeFlags;
    apSpawnRequest->FaceTints = characterComponent.FaceTints;
    apSpawnRequest->FactionsContent = characterComponent.FactionsContent;
    apSpawnRequest->IsDead = characterComponent.IsDead();
    apSpawnRequest->IsPlayer = characterComponent.IsPlayer();
    apSpawnRequest->IsWeaponDrawn = characterComponent.IsWeaponDrawn();
    apSpawnRequest->IsPlayerSummon = characterComponent.IsPlayerSummon();
    apSpawnRequest->PlayerId = characterComponent.PlayerId;
    apSpawnRequest->MountedOnServerId = characterComponent.MountedOnServerId;

    const auto* pOwnerComponent = aRegistry.try_get<OwnerComponent>(aEntity);
    if (pOwnerComponent)
    {
        apSpawnRequest->OwnershipEpoch = pOwnerComponent->OwnershipEpoch;
    }

    const auto* pFormIdComponent = aRegistry.try_get<FormIdComponent>(aEntity);
    if (pFormIdComponent)
    {
        apSpawnRequest->FormId = pFormIdComponent->Id;
    }

    const auto* pInventoryComponent = aRegistry.try_get<InventoryComponent>(aEntity);
    if (pInventoryComponent)
    {
        apSpawnRequest->InventoryContent = pInventoryComponent->Content;
    }

    const auto* pActorValuesComponent = aRegistry.try_get<ActorValuesComponent>(aEntity);
    if (pActorValuesComponent)
    {
        apSpawnRequest->InitialActorValues = pActorValuesComponent->CurrentActorValues;
    }

    if (characterComponent.BaseId)
    {
        apSpawnRequest->BaseId = characterComponent.BaseId.Id;
    }

    if (characterComponent.LeveledNpcPickId)
    {
        apSpawnRequest->LeveledNpcPickId = characterComponent.LeveledNpcPickId.Id;
    }

    const auto* pMovementComponent = aRegistry.try_get<MovementComponent>(aEntity);
    if (pMovementComponent)
    {
        apSpawnRequest->Position = pMovementComponent->Position;
        apSpawnRequest->Rotation.x = pMovementComponent->Rotation.x;
        apSpawnRequest->Rotation.y = pMovementComponent->Rotation.z;
    }

    const auto* pCellIdComponent = aRegistry.try_get<CellIdComponent>(aEntity);
    if (pCellIdComponent)
    {
        apSpawnRequest->CellId = pCellIdComponent->Cell;
    }

    auto& animationComponent = aRegistry.get<AnimationComponent>(aEntity);
    apSpawnRequest->ActionsToReplay = animationComponent.ActionsReplayCache.FormRefinedReplayChain();
}

void CharacterService::OnUpdate(const UpdateEvent&) const noexcept
{
    ProcessFactionsChanges();
    ProcessMovementChanges();
    EnforceLeaderAuthority();
}

// During a shared session the leader simulates every NPC it has in range; a follower only keeps
// NPCs the leader cannot reach. Assignment races at a new game start otherwise leave followers
// owning scene NPCs and vehicles (measured: the follower owned a cart horse, pulling against its
// own host-driven cart, so the leader's cart froze mid-road; and Lokir, stalling the cart scene).
void CharacterService::EnforceLeaderAuthority() const noexcept
{
    static auto s_next = std::chrono::steady_clock::time_point{};
    const auto now = std::chrono::steady_clock::now();
    if (now < s_next)
        return;
    s_next = now + std::chrono::seconds(1);

    auto& partyService = m_world.GetPartyService();
    const auto view = m_world.view<OwnerComponent, CharacterComponent, CellIdComponent>();
    std::vector<std::pair<Player*, entt::entity>> transfers;
    for (auto entity : view)
    {
        const auto& ownerComponent = view.get<OwnerComponent>(entity);
        const auto& characterComponent = view.get<CharacterComponent>(entity);
        Player* pOwner = ownerComponent.GetOwner();
        if (!pOwner || characterComponent.IsPlayer() || !partyService.IsPlayerInParty(pOwner) || partyService.IsPlayerLeader(pOwner))
            continue;
        auto* pParty = partyService.GetPlayerParty(pOwner);
        if (!pParty || pParty->SessionState < 1)
            continue;
        Player* pLeader = m_world.GetPlayerManager().GetById(pParty->LeaderPlayerId);
        if (!pLeader)
            continue;
        // A leader that relinquished this actor (not loaded there although its cell is in range)
        // keeps it relinquished: taking it back every second flipped the owner (and re-sent its
        // inventory) forever, actor 1F in the intro, epochs 2..94.
        if (std::find(ownerComponent.InvalidOwners.begin(), ownerComponent.InvalidOwners.end(), pLeader) !=
            ownerComponent.InvalidOwners.end())
            continue;
        const auto& cellIdComponent = view.get<CellIdComponent>(entity);
        if (cellIdComponent.Cell == GameId{} || pLeader->GetCellComponent().IsInRange(cellIdComponent, characterComponent.IsDragon()))
            transfers.emplace_back(pLeader, entity);
    }
    for (const auto& [pLeader, entity] : transfers)
    {
        spdlog::info("Leader authority: actor {:X} moves from a follower to the leader", World::ToInteger(entity));
        TransferOwnership(pLeader, entity, OwnershipTransferReason::LeaderAssignment);
    }
}

void CharacterService::OnCharacterExteriorCellChange(const CharacterExteriorCellChangeEvent& acEvent) const noexcept
{
    CharacterSpawnRequest spawnMessage;
    Serialize(m_world, acEvent.Entity, &spawnMessage);

    NotifyRemoveCharacter removeMessage;
    removeMessage.ServerId = World::ToInteger(acEvent.Entity);

    for (auto pPlayer : m_world.GetPlayerManager())
    {
        if (acEvent.Owner == pPlayer)
            continue;

        if (pPlayer->GetCellComponent().WorldSpaceId != acEvent.WorldSpaceId || pPlayer->GetCellComponent().WorldSpaceId == acEvent.WorldSpaceId && !GridCellCoords::IsCellInGridCell(acEvent.CurrentCoords, pPlayer->GetCellComponent().CenterCoords, false))
        {
            pPlayer->Send(removeMessage);
        }
        else if (pPlayer->GetCellComponent().WorldSpaceId == acEvent.WorldSpaceId && GridCellCoords::IsCellInGridCell(acEvent.CurrentCoords, pPlayer->GetCellComponent().CenterCoords, false))
        {
            pPlayer->Send(spawnMessage);
        }
    }
}

void CharacterService::OnCharacterInteriorCellChange(const CharacterInteriorCellChangeEvent& acEvent) const noexcept
{
    CharacterSpawnRequest spawnMessage;
    Serialize(m_world, acEvent.Entity, &spawnMessage);

    NotifyRemoveCharacter removeMessage;
    removeMessage.ServerId = World::ToInteger(acEvent.Entity);

    for (auto pPlayer : m_world.GetPlayerManager())
    {
        if (acEvent.Owner == pPlayer)
            continue;

        if (acEvent.NewCell == pPlayer->GetCellComponent().Cell)
            pPlayer->Send(spawnMessage);
        else
            pPlayer->Send(removeMessage);
    }
}

void CharacterService::OnAssignCharacterRequest(const PacketEvent<AssignCharacterRequest>& acMessage) const noexcept
{
    auto& message = acMessage.Packet;
    const auto& refId = message.ReferenceId;

    const auto isPlayer = (refId.ModId == 0 && refId.BaseId == 0x14);
    const auto isCustom = isPlayer || refId.ModId == std::numeric_limits<uint32_t>::max();

    // Script-spawned NPCs use different FF reference IDs on every client.
    // Reuse an existing party entity only when one placement is unambiguous;
    // the response binds the requester's own native, quest-bound reference.
    if (!isPlayer && isCustom && message.FormId != GameId{} &&
        !message.IsPlayerSummon && !message.IsMount &&
        m_world.GetPartyService().IsPlayerInParty(acMessage.pPlayer))
    {
        const auto requesterParty = acMessage.pPlayer->GetParty().JoinedPartyId;
        const auto now = GameServer::Get()->GetTick();
        const auto candidates = m_world.view<TemporaryActorProvenance, CharacterComponent,
            CellIdComponent, MovementComponent, OwnerComponent>();
        entt::entity match = entt::null;
        bool ambiguous = false;
        for (const auto candidate : candidates)
        {
            const auto& provenance = candidates.get<TemporaryActorProvenance>(candidate);
            const auto& character = candidates.get<CharacterComponent>(candidate);
            const auto& cell = candidates.get<CellIdComponent>(candidate);
            const auto& movement = candidates.get<MovementComponent>(candidate);
            const auto& owner = candidates.get<OwnerComponent>(candidate);
            if (!owner.GetOwner() || owner.GetOwner() == acMessage.pPlayer ||
                owner.GetOwner()->GetParty().JoinedPartyId != requesterParty ||
                std::find(provenance.BoundPlayerIds.begin(), provenance.BoundPlayerIds.end(),
                    acMessage.pPlayer->GetId()) != provenance.BoundPlayerIds.end() ||
                now < provenance.CreatedTick || now - provenance.CreatedTick > 10000 ||
                character.IsPlayer() || character.IsPlayerSummon() || character.IsMount() ||
                character.BaseId.Id != message.FormId ||
                character.LeveledNpcPickId.Id != message.LeveledNpcPickId ||
                cell.Cell != message.CellId || cell.WorldSpaceId != message.WorldSpaceId)
                continue;

            // Scene-driven natives can already have walked away by the time
            // the other machine discovers its copy.  Compare the original
            // placement as well as the live position.  This remains a
            // conservative heuristic: two plausible candidates are never
            // collapsed into one actor.
            const auto requestedPosition = static_cast<glm::vec3>(message.Position);
            const auto currentDelta = movement.Position - requestedPosition;
            const auto creationDelta = provenance.CreationPosition - requestedPosition;
            constexpr float kMaxPlacementDistanceSquared = 192.f * 192.f;
            if (glm::dot(currentDelta, currentDelta) > kMaxPlacementDistanceSquared &&
                glm::dot(creationDelta, creationDelta) > kMaxPlacementDistanceSquared)
                continue;
            if (match != entt::null)
            {
                ambiguous = true;
                break;
            }
            match = candidate;
        }
        if (match != entt::null && !ambiguous)
        {
            m_world.get<TemporaryActorProvenance>(match).BoundPlayerIds.push_back(
                acMessage.pPlayer->GetId());
            AssignCharacterResponse response{};
            response.Cookie = message.Cookie;
            response.Owner = false;
            PopulateAssignmentResponse(match, response);
            acMessage.pPlayer->Send(response);
            spdlog::info("Reconciled temporary actor {:X} from player {:X} to server {:X}",
                refId.BaseId, acMessage.pPlayer->GetId(), World::ToInteger(match));
            return;
        }
        if (ambiguous)
            spdlog::warn("Ambiguous temporary NPC placement for player {:X}, base {:X}:{:X}; keeping distinct actors",
                acMessage.pPlayer->GetId(), message.FormId.ModId, message.FormId.BaseId);
    }

    // Check if id is the player
    if (!isCustom)
    {
        // Look for the character
        auto view = m_world.view<FormIdComponent, ActorValuesComponent, CharacterComponent, MovementComponent, CellIdComponent, OwnerComponent, InventoryComponent>();

        const auto itor = std::find_if(
            std::begin(view), std::end(view),
            [view, refId](auto entity)
            {
                const auto& formIdComponent = view.get<FormIdComponent>(entity);

                return formIdComponent.Id == refId;
            });

        if (itor != std::end(view))
        {
            spdlog::debug("FormId: {:x}:{:x} is already managed", refId.ModId, refId.BaseId);

            auto& ownerComponent = view.get<OwnerComponent>(*itor);
            auto& characterComponent = view.get<CharacterComponent>(*itor);
            const bool isOwner = ownerComponent.GetOwner() == acMessage.pPlayer;
            const bool transferToDiscoverer = !isOwner && CanClaimOwnership(acMessage.pPlayer, *itor, ownerComponent.OwnershipEpoch, OwnershipTransferReason::LeaderAssignment);

            if (!characterComponent.LeveledNpcPickId && message.LeveledNpcPickId != GameId{})
            {
                characterComponent.LeveledNpcPickId = FormIdComponent(message.LeveledNpcPickId);
                spdlog::debug(
                    "Stored previously unknown leveled NPC pick {:x}:{:x} for FormId {:x}:{:x}",
                    message.LeveledNpcPickId.ModId,
                    message.LeveledNpcPickId.BaseId,
                    refId.ModId,
                    refId.BaseId);
            }

            AssignCharacterResponse response{};
            response.Cookie = message.Cookie;
            response.Owner = isOwner;
            PopulateAssignmentResponse(*itor, response);
            acMessage.pPlayer->Send(response);

            // The assignment response establishes a remote component before the grant arrives.
            if (transferToDiscoverer)
                TransferOwnership(acMessage.pPlayer, *itor,
                    m_world.GetPartyService().IsPlayerLeader(acMessage.pPlayer)
                        ? OwnershipTransferReason::LeaderAssignment
                        : OwnershipTransferReason::CellLease);

            return;
        }
    }

    // This entity has no owner create it
    CreateCharacter(acMessage);
}

void CharacterService::OnOwnershipTransferRequest(const PacketEvent<RequestOwnershipTransfer>& acMessage) const noexcept
{
    const auto& message = acMessage.Packet;

    const entt::entity cEntity = static_cast<entt::entity>(message.ServerId);
    const auto view = m_world.view<OwnerComponent, CharacterComponent, CellIdComponent, MovementComponent>();
    const auto it = view.find(cEntity);
    if (it == view.end())
    {
        spdlog::debug("Ignored ownership release from player {:X} for missing actor {:X}", acMessage.pPlayer->GetId(), message.ServerId);
        return;
    }

    auto& ownerComponent = view.get<OwnerComponent>(*it);
    if (ownerComponent.GetOwner() != acMessage.pPlayer || ownerComponent.OwnershipEpoch != message.OwnershipEpoch)
    {
        const uint32_t ownerId = ownerComponent.GetOwner() ? ownerComponent.GetOwner()->GetId() : 0;
        spdlog::debug(
            "Ignored ownership release from player {:X} for actor {:X}; current owner is {:X} and requested epoch {} does not match {}",
            acMessage.pPlayer->GetId(), message.ServerId, ownerId, message.OwnershipEpoch, ownerComponent.OwnershipEpoch);
        return;
    }

    if (message.Reason != OwnershipReleaseReason::Relinquish && message.Reason != OwnershipReleaseReason::DeclineGrant)
    {
        spdlog::warn("Ignored ownership release with invalid reason from player {:X} for actor {:X}", acMessage.pPlayer->GetId(), message.ServerId);
        return;
    }

    auto& characterComponent = view.get<CharacterComponent>(*it);
    if (characterComponent.IsPlayerSummon())
    {
        spdlog::info("Removing summon {:X} after player {:X} relinquished ownership", message.ServerId, acMessage.pPlayer->GetId());
        m_world.GetDispatcher().trigger(CharacterRemoveEvent(message.ServerId));
        return;
    }

    if (message.Reason == OwnershipReleaseReason::Relinquish && (message.WorldSpaceId || message.CellId))
    {
        const auto* pFormIdComponent = m_world.try_get<FormIdComponent>(cEntity);
        if (pFormIdComponent)
        {
            NotifyActorTeleport notify{};
            notify.FormId = pFormIdComponent->Id;
            notify.WorldSpaceId = message.WorldSpaceId;
            notify.CellId = message.CellId;
            notify.Position = message.Position;

            GameServer::Get()->SendToPlayers(notify, acMessage.pPlayer);
        }

        auto& cellIdComponent = view.get<CellIdComponent>(*it);
        cellIdComponent.WorldSpaceId = message.WorldSpaceId;
        cellIdComponent.Cell = message.CellId;
        cellIdComponent.CenterCoords = GridCellCoords::CalculateGridCellCoords(message.Position);

        auto& movementComponent = view.get<MovementComponent>(*it);
        movementComponent.Position = message.Position;
        movementComponent.Sent = true;
    }

    // A normal release starts a fresh search. A declined grant continues the current
    // search, retaining failed candidates so unloaded clients cannot bounce ownership.
    if (message.Reason == OwnershipReleaseReason::Relinquish)
        ownerComponent.InvalidOwners.clear();

    ownerComponent.InvalidOwners.push_back(acMessage.pPlayer);

    TransferToNextOwner(cEntity, OwnershipTransferReason::Relinquish);
}

void CharacterService::OnOwnershipTransferEvent(const OwnershipTransferEvent& acEvent) const noexcept
{
    // A disconnect starts a fresh search; previously unavailable clients may be ready now.
    const auto view = m_world.view<OwnerComponent>();
    if (const auto it = view.find(acEvent.Entity); it != view.end())
        view.get<OwnerComponent>(*it).InvalidOwners.clear();

    TransferToNextOwner(acEvent.Entity, OwnershipTransferReason::OwnerUnavailable);
}

void CharacterService::OnCharacterRemoveEvent(const CharacterRemoveEvent& acEvent) const noexcept
{
    const auto view = m_world.view<OwnerComponent>();
    const auto it = view.find(static_cast<entt::entity>(acEvent.ServerId));
    if (it == view.end())
        return;

    // Mount relations are persistent spawn state. A removed mount must not be
    // replayed to a late-joining client through another actor's snapshot.
    for (auto rider : m_world.view<CharacterComponent>())
    {
        auto& character = m_world.get<CharacterComponent>(rider);
        if (character.MountedOnServerId == acEvent.ServerId)
            character.MountedOnServerId = 0;
    }

    GameServer::Get()->GetWorld().GetScriptService().HandleCharacterDestoy(*it);

    NotifyRemoveCharacter response;
    response.ServerId = acEvent.ServerId;

    for (auto pPlayer : m_world.GetPlayerManager())
        pPlayer->Send(response);

    m_world.destroy(*it);
    spdlog::debug("Character destroyed {:X}", acEvent.ServerId);
}

void CharacterService::OnOwnershipClaimRequest(const PacketEvent<RequestOwnershipClaim>& acMessage) const noexcept
{
    const auto& message = acMessage.Packet;
    const entt::entity cEntity = static_cast<entt::entity>(message.ServerId);
    const auto reason = m_world.GetPartyService().IsPlayerLeader(acMessage.pPlayer)
        ? OwnershipTransferReason::LeaderClaim
        : OwnershipTransferReason::CellLease;

    if (!CanClaimOwnership(acMessage.pPlayer, cEntity, message.ExpectedOwnershipEpoch, reason))
        return;

    TransferOwnership(acMessage.pPlayer, cEntity, reason);
}

void CharacterService::OnCharacterSpawned(const CharacterSpawnedEvent& acEvent) const noexcept
{
    CharacterSpawnRequest message;
    Serialize(m_world, acEvent.Entity, &message);

    const auto& ownerComp = m_world.get<OwnerComponent>(acEvent.Entity);
    if (!GameServer::Get()->SendToPlayersInRange(message, acEvent.Entity, ownerComp.GetOwner()))
        spdlog::error("{}: SendToPlayersInRange failed", __FUNCTION__);

    GameServer::Get()->GetWorld().GetScriptService().HandleCharacterSpawn(acEvent.Entity);
}

void CharacterService::OnCorpseRagdoll(const PacketEvent<CorpseRagdollRequest>& acMessage) const noexcept
{
    // Only the corpse's owner defines its ragdoll; relay a complete, sane body set or nothing.
    OwnerView<CellIdComponent> view(m_world, acMessage.GetSender());
    const auto entity = static_cast<entt::entity>(acMessage.Packet.ServerId);
    if (view.find(entity) == std::end(view))
        return;
    const auto& bodies = acMessage.Packet.Bodies;
    if (bodies.empty() || bodies.size() > CorpseRagdollRequest::kMaxBodies)
        return;
    if (!std::all_of(std::begin(acMessage.Packet.Origin), std::end(acMessage.Packet.Origin),
            [](float v) { return std::isfinite(v) && std::abs(v) < 10'000'000.f; }))
        return;
    for (const auto& body : bodies)
    {
        float norm = 0.f;
        for (const float value : body.Rotation)
            norm += value * value;
        if (!std::all_of(std::begin(body.Position), std::end(body.Position), [](float v) { return std::isfinite(v) && std::abs(v) < 2000.f; }) ||
            !std::isfinite(norm) || std::abs(norm - 1.f) > 0.01f)
            return;
    }

    NotifyCorpseRagdoll notify{};
    notify.ServerId = acMessage.Packet.ServerId;
    notify.Tick = acMessage.Packet.Tick;
    std::copy(std::begin(acMessage.Packet.Origin), std::end(acMessage.Packet.Origin), std::begin(notify.Origin));
    notify.Bodies = bodies;
    GameServer::Get()->SendToPlayersInRange(notify, entity, acMessage.pPlayer);
}

void CharacterService::OnPlayerAppearance(const PacketEvent<PlayerAppearanceRequest>& acMessage) const noexcept
{
    // Only a player's own character, from its owner. Stored, so a later spawn shows the newest
    // look, and relayed to the other players (live while the owner is in the character creator).
    const auto& packet = acMessage.Packet;
    const auto entity = static_cast<entt::entity>(packet.ServerId);
    auto view = m_world.view<OwnerComponent, CharacterComponent>();
    const auto it = view.find(entity);
    if (it == view.end() || view.get<OwnerComponent>(entity).GetOwner() != acMessage.pPlayer)
        return;
    auto& characterComponent = view.get<CharacterComponent>(entity);
    if (!characterComponent.IsPlayer() || packet.AppearanceBuffer.empty() || packet.AppearanceBuffer.size() > 64 * 1024)
        return;
    characterComponent.SaveBuffer = packet.AppearanceBuffer;
    characterComponent.ChangeFlags = packet.ChangeFlags;
    characterComponent.FaceTints = packet.FaceTints;

    NotifyPlayerAppearance notify{};
    notify.ServerId = packet.ServerId;
    notify.ChangeFlags = packet.ChangeFlags;
    notify.AppearanceBuffer = packet.AppearanceBuffer;
    notify.FaceTints = packet.FaceTints;
    notify.InCreator = packet.InCreator;
    for (auto* pPlayer : m_world.GetPlayerManager())
    {
        if (pPlayer != acMessage.pPlayer)
            pPlayer->Send(notify);
    }
}

void CharacterService::OnReferencesMoveRequest(const PacketEvent<ClientReferencesMoveRequest>& acMessage) const noexcept
{
    OwnerView<AnimationComponent, MovementComponent, CellIdComponent> view(m_world, acMessage.GetSender());

    auto& message = acMessage.Packet;

    for (auto& entry : message.Updates)
    {
        const auto entity = static_cast<entt::entity>(entry.first);

        auto itor = view.find(entity);
        if (itor == std::end(view))
        {
            spdlog::debug("{:x} requested move of {:x} but does not exist", acMessage.pPlayer->GetConnectionId(), World::ToInteger(entity));
            continue;
        }

        auto& movementComponent = view.get<MovementComponent>(*itor);
        auto& cellIdComponent = view.get<CellIdComponent>(*itor);
        auto& animationComponent = view.get<AnimationComponent>(*itor);

        movementComponent.Tick = message.Tick;

        const auto movementCopy = movementComponent;

        auto& update = entry.second;
        auto& movement = update.UpdatedMovement;

        movementComponent.Position = movement.Position;
        movementComponent.Rotation = glm::vec3(movement.Rotation.x, 0.f, movement.Rotation.y);
        movementComponent.Variables = movement.Variables;
        movementComponent.Direction = movement.Direction;
        const auto requestedTargetId = update.CombatTargetServerId;
        if (requestedTargetId == 0 || requestedTargetId == 0xFFFFFFFFu)
            movementComponent.CombatTargetServerId = requestedTargetId;
        else
        {
            const auto targetEntity = static_cast<entt::entity>(requestedTargetId);
            movementComponent.CombatTargetServerId =
                m_world.valid(targetEntity) &&
                m_world.all_of<CharacterComponent>(targetEntity) ?
                    requestedTargetId : 0xFFFFFFFFu;
        }

        cellIdComponent.Cell = movement.CellId;
        cellIdComponent.WorldSpaceId = movement.WorldSpaceId;
        cellIdComponent.CenterCoords = GridCellCoords::CalculateGridCellCoords(movement.Position.x, movement.Position.y);

        for (auto& action : update.ActionEvents)
        {
            auto [canceled, reason] = GameServer::Get()->GetWorld().GetScriptService().HandleCharacterMove(entity);
            if (canceled)
                continue;

            animationComponent.CurrentAction = action;

            animationComponent.Actions.push_back(animationComponent.CurrentAction);
        }

        animationComponent.ActionsReplayCache.AppendAll(update.ActionEvents);

        // OwnerView already rejects a non-owner entity. Never relay a pose
        // until it has also passed the bounded transform validation.
        if (!update.EvaluatedPose.Bones.empty() && update.EvaluatedPose.IsValid())
        {
            animationComponent.EvaluatedPose = update.EvaluatedPose;
            animationComponent.EvaluatedPosePending = true;
        }
        if (!update.VisualBones.Bones.empty() && update.VisualBones.IsValid())
        {
            animationComponent.VisualBones = update.VisualBones;
            animationComponent.VisualBonesPending = true;
        }

        movementComponent.Sent = false;
    }
}

void CharacterService::OnFactionsChanges(const PacketEvent<RequestFactionsChanges>& acMessage) const noexcept
{
    OwnerView<CharacterComponent> view(m_world, acMessage.GetSender());

    auto& message = acMessage.Packet;

    for (auto& [id, factions] : message.Changes)
    {
        auto it = view.find(static_cast<entt::entity>(id));

        if (it == std::end(view) || view.get<OwnerComponent>(*it).GetOwner() != acMessage.pPlayer)
            continue;

        auto& characterComponent = view.get<CharacterComponent>(*it);
        characterComponent.FactionsContent = factions;
        characterComponent.SetDirtyFactions(true);
    }
}

void CharacterService::OnMountRequest(const PacketEvent<MountRequest>& acMessage) const noexcept
{
    const auto& message = acMessage.Packet;
    const entt::entity cRiderEntity = static_cast<entt::entity>(message.RiderId);
    const entt::entity cMountEntity = static_cast<entt::entity>(message.MountId);
    const auto view = m_world.view<OwnerComponent, CharacterComponent, CellIdComponent>();
    const auto riderIt = view.find(cRiderEntity);
    if (riderIt == view.end())
    {
        spdlog::debug("Rejected mount request from player {:X} because rider {:X} is invalid", acMessage.pPlayer->GetId(), message.RiderId);
        return;
    }

    const auto& riderOwner = view.get<OwnerComponent>(*riderIt);
    if (riderOwner.GetOwner() != acMessage.pPlayer ||
        riderOwner.OwnershipEpoch != message.RiderOwnershipEpoch)
        return;

    auto& rider = view.get<CharacterComponent>(*riderIt);
    if (message.MountId == 0)
    {
        if (!rider.MountedOnServerId)
            return;
        spdlog::info("Accepted mount relation cleared: rider {:X}, previous horse {:X}, source player {:X}, rider epoch {}",
            message.RiderId, rider.MountedOnServerId, acMessage.pPlayer->GetId(), message.RiderOwnershipEpoch);
        rider.MountedOnServerId = 0;
        NotifyMount notify{};
        notify.RiderId = message.RiderId;
        notify.MountId = 0;
        if (!GameServer::Get()->SendToPlayersInRange(notify, cRiderEntity, acMessage.GetSender()))
            spdlog::error("{}: dismount fan-out failed", __FUNCTION__);
        return;
    }

    const auto mountIt = view.find(cMountEntity);
    if (mountIt == view.end() || cRiderEntity == cMountEntity)
        return;

    if (!view.get<CharacterComponent>(*mountIt).IsMount())
    {
        spdlog::warn("Rejected mount request from player {:X} because actor {:X} is not a mount", acMessage.pPlayer->GetId(), message.MountId);
        return;
    }

    const auto& mountOwner = view.get<OwnerComponent>(*mountIt);
    if (mountOwner.OwnershipEpoch != message.MountOwnershipEpoch)
    {
        spdlog::debug(
            "Rejected stale mount request from player {:X} for rider {:X} at epoch {} and mount {:X} at epoch {}; current epochs are {} and {}",
            acMessage.pPlayer->GetId(), message.RiderId, message.RiderOwnershipEpoch, message.MountId, message.MountOwnershipEpoch,
            riderOwner.OwnershipEpoch, mountOwner.OwnershipEpoch);
        return;
    }

    const auto& mountCell = view.get<CellIdComponent>(*mountIt);
    if (!acMessage.pPlayer->GetCellComponent().IsInRange(mountCell, view.get<CharacterComponent>(*mountIt).IsDragon()))
    {
        spdlog::debug("Rejected mount request from player {:X} because mount {:X} is out of range", acMessage.pPlayer->GetId(), message.MountId);
        return;
    }

    if (rider.MountedOnServerId == message.MountId)
        return;

    if (!TransferOwnership(acMessage.pPlayer, *mountIt, OwnershipTransferReason::Mount))
        return;

    rider.MountedOnServerId = message.MountId;
    spdlog::info("Accepted mount relation set: rider {:X}, horse {:X}, source player {:X}, rider epoch {}, horse epoch {}",
        message.RiderId, message.MountId, acMessage.pPlayer->GetId(),
        message.RiderOwnershipEpoch, message.MountOwnershipEpoch);

    NotifyMount notify;
    notify.RiderId = message.RiderId;
    notify.MountId = message.MountId;

    if (!GameServer::Get()->SendToPlayersInRange(notify, cMountEntity, acMessage.GetSender()))
        spdlog::error("{}: SendToPlayersInRange failed", __FUNCTION__);
}

void CharacterService::OnNewPackageRequest(const PacketEvent<NewPackageRequest>& acMessage) const noexcept
{
    auto& message = acMessage.Packet;

    NotifyNewPackage notify;
    notify.ActorId = message.ActorId;
    notify.PackageId = message.PackageId;

    const entt::entity cEntity = static_cast<entt::entity>(message.ActorId);
    if (!GameServer::Get()->SendToPlayersInRange(notify, cEntity, acMessage.GetSender()))
        spdlog::error("{}: SendToPlayersInRange failed", __FUNCTION__);
}

void CharacterService::OnRequestRespawn(const PacketEvent<RequestRespawn>& acMessage) const noexcept
{
    auto view = m_world.view<OwnerComponent, CharacterComponent>();
    auto it = view.find(static_cast<entt::entity>(acMessage.Packet.ActorId));
    if (it == view.end())
    {
        spdlog::warn("No OwnerComponent found for actor id {:X}", acMessage.Packet.ActorId);
        return;
    }

    auto& ownerComponent = view.get<OwnerComponent>(*it);

    // Replay cache needs to be cleared when a character respawns
    if (auto* pAnimationComponent = m_world.try_get<AnimationComponent>(*it))
        pAnimationComponent->ActionsReplayCache.Clear();

    if (ownerComponent.GetOwner() == acMessage.pPlayer)
    {
        if (!acMessage.Packet.AppearanceBuffer.empty())
        {
            auto& characterComponent = view.get<CharacterComponent>(*it);
            characterComponent.SaveBuffer = acMessage.Packet.AppearanceBuffer;
            characterComponent.ChangeFlags = acMessage.Packet.ChangeFlags;
        }

        NotifyRespawn notify;
        notify.ActorId = acMessage.Packet.ActorId;

        if (!GameServer::Get()->SendToPlayersInRange(notify, *it, acMessage.GetSender()))
            spdlog::error("{}: SendToPlayersInRange failed", __FUNCTION__);
    }
    else
    {
        CharacterSpawnRequest message;
        Serialize(m_world, *it, &message);

        acMessage.GetSender()->Send(message);
    }
}

void CharacterService::OnSyncExperienceRequest(const PacketEvent<SyncExperienceRequest>& acMessage) const noexcept
{
    if (!bEnableXpSync)
        return;

    NotifySyncExperience notify;
    notify.Experience = acMessage.Packet.Experience;

    const auto& partyComponent = acMessage.pPlayer->GetParty();
    GameServer::Get()->SendToParty(notify, partyComponent, acMessage.GetSender());
}

void CharacterService::OnDialogueRequest(const PacketEvent<DialogueRequest>& acMessage) const noexcept
{
    auto& message = acMessage.Packet;

    NotifyDialogue notify{};
    notify.ServerId = message.ServerId;
    notify.Tick = message.Tick;
    notify.SoundFilename = message.SoundFilename;

    const entt::entity cEntity = static_cast<entt::entity>(message.ServerId);
    if (!GameServer::Get()->SendToPlayersInRange(notify, cEntity, acMessage.GetSender()))
        spdlog::error("{}: SendToPlayersInRange failed", __FUNCTION__);
}

void CharacterService::OnSubtitleRequest(const PacketEvent<SubtitleRequest>& acMessage) const noexcept
{
    auto& message = acMessage.Packet;

    NotifySubtitle notify{};
    notify.ServerId = message.ServerId;
    notify.Tick = message.Tick;
    notify.Text = message.Text;

    const entt::entity cEntity = static_cast<entt::entity>(message.ServerId);
    if (!GameServer::Get()->SendToPlayersInRange(notify, cEntity, acMessage.GetSender()))
        spdlog::error("{}: SendToPlayersInRange failed", __FUNCTION__);
}

void CharacterService::CreateCharacter(const PacketEvent<AssignCharacterRequest>& acMessage) const noexcept
{
    auto& message = acMessage.Packet;

    const auto gameId = message.ReferenceId;
    const auto baseId = message.FormId;

    const auto cEntity = m_world.create();
    const auto isTemporary = gameId.ModId == std::numeric_limits<uint32_t>::max();
    const auto isPlayer = (gameId.ModId == 0 && gameId.BaseId == 0x14);
    const auto isCustom = isPlayer || isTemporary;

    // For player characters and temporary forms
    if (!isCustom)
    {
        m_world.emplace<FormIdComponent>(cEntity, gameId.BaseId, gameId.ModId);
    }
    else if (baseId != GameId{} && !isTemporary)
    {
        m_world.destroy(cEntity);
        spdlog::warn("Unexpected NpcId, player {:x} might be forging packets", acMessage.pPlayer->GetConnectionId());
        return;
    }

    auto* const pServer = GameServer::Get();

    // The discoverer owns the initial epoch. Assigning a different player here
    // skips that player's spawn packet (owners are excluded from the broadcast),
    // so it would have no network entity on which to accept ownership. A nearby
    // leader receives the spawn, materializes the actor, then claims it at the
    // next epoch. A separated follower keeps the cell simulation lease.
    Player* const pOwner = acMessage.pPlayer;
    m_world.emplace<OwnerComponent>(cEntity, pOwner);

    auto& cellIdComponent = m_world.emplace<CellIdComponent>(cEntity, message.CellId);
    if (message.WorldSpaceId != GameId{})
    {
        cellIdComponent.WorldSpaceId = message.WorldSpaceId;
        cellIdComponent.CenterCoords = GridCellCoords::CalculateGridCellCoords(message.Position);
    }

    auto& characterComponent = m_world.emplace<CharacterComponent>(cEntity);
    characterComponent.ChangeFlags = message.ChangeFlags;
    characterComponent.SaveBuffer = std::move(message.AppearanceBuffer);
    characterComponent.BaseId = FormIdComponent(message.FormId);
    // Client-authoritative like BaseId; worst case a forged id changes which NPC identity renders.
    if (message.LeveledNpcPickId != GameId{})
        characterComponent.LeveledNpcPickId = FormIdComponent(message.LeveledNpcPickId);

    if (characterComponent.LeveledNpcPickId)
        spdlog::debug("Stored leveled NPC pick {:x}:{:x} for FormId {:x}:{:x}", message.LeveledNpcPickId.ModId, message.LeveledNpcPickId.BaseId, gameId.ModId, gameId.BaseId);
    characterComponent.FaceTints = message.FaceTints;
    characterComponent.FactionsContent = message.FactionsContent;
    characterComponent.SetDead(message.CurrentActorData.IsDead);
    characterComponent.SetPlayer(isPlayer);
    characterComponent.SetWeaponDrawn(message.CurrentActorData.IsWeaponDrawn);
    characterComponent.SetDragon(message.IsDragon);
    characterComponent.SetMount(message.IsMount);
    characterComponent.SetPlayerSummon(message.IsPlayerSummon);

    auto& inventoryComponent = m_world.emplace<InventoryComponent>(cEntity);
    inventoryComponent.Content = message.CurrentActorData.InitialInventory;

    auto& actorValuesComponent = m_world.emplace<ActorValuesComponent>(cEntity);
    actorValuesComponent.CurrentActorValues = message.CurrentActorData.InitialActorValues;

    spdlog::debug("FormId: {:x}:{:x} - NpcId: {:x}:{:x} assigned to {:x}", gameId.ModId, gameId.BaseId, baseId.ModId, baseId.BaseId, acMessage.pPlayer->GetConnectionId());

    auto& movementComponent = m_world.emplace<MovementComponent>(cEntity);
    movementComponent.Tick = pServer->GetTick();
    movementComponent.Position = message.Position;
    movementComponent.Rotation = {message.Rotation.x, 0.f, message.Rotation.y};
    movementComponent.Sent = false;

    if (isTemporary && !message.IsPlayerSummon && !message.IsMount && baseId != GameId{})
    {
        auto& provenance = m_world.emplace<TemporaryActorProvenance>(cEntity);
        provenance.CreatedTick = pServer->GetTick();
        provenance.CreationPosition = static_cast<glm::vec3>(message.Position);
        provenance.BoundPlayerIds.push_back(acMessage.pPlayer->GetId());
    }

    if (isTemporary)
    {
        static std::atomic<uint32_t> sProvenanceLogs{0};
        const auto logIndex = sProvenanceLogs.fetch_add(1, std::memory_order_relaxed);
        if (logIndex < 512)
            spdlog::info(
                "Temporary actor provenance: server {:X}, source player {:X}, source reference {:X}:{:X}, base {:X}:{:X}, cell {:X}:{:X}, worldspace {:X}:{:X}, position ({}, {}, {}), mount={}, summon={}, leveled pick {:X}:{:X}",
                static_cast<uint32_t>(cEntity), acMessage.pPlayer->GetId(), gameId.ModId,
                gameId.BaseId, baseId.ModId, baseId.BaseId, message.CellId.ModId,
                message.CellId.BaseId, message.WorldSpaceId.ModId,
                message.WorldSpaceId.BaseId, message.Position.x, message.Position.y,
                message.Position.z, message.IsMount, message.IsPlayerSummon,
                message.LeveledNpcPickId.ModId, message.LeveledNpcPickId.BaseId);
        else if (logIndex == 512)
            spdlog::info("Temporary actor provenance log capped at 512 creations for this server process");
    }

    m_world.emplace<AnimationComponent>(cEntity);

    // If this is a player character store a ref and trigger an event
    if (isPlayer)
    {
        const auto pPlayer = acMessage.pPlayer;

        pPlayer->SetCharacter(cEntity);
        pPlayer->GetQuestLogComponent().QuestContent = message.QuestContent;
        characterComponent.PlayerId = pPlayer->GetId();

        auto& dispatcher = m_world.GetDispatcher();
        dispatcher.trigger(PlayerEnterWorldEvent(pPlayer));
    }

    AssignCharacterResponse response{};
    response.Cookie = message.Cookie;
    response.Owner = pOwner == acMessage.pPlayer;
    PopulateAssignmentResponse(cEntity, response);

    pServer->Send(acMessage.pPlayer->GetConnectionId(), response);

    auto& dispatcher = m_world.GetDispatcher();
    dispatcher.trigger(CharacterSpawnedEvent(cEntity));
}

void CharacterService::PopulateAssignmentResponse(const entt::entity aEntity, AssignCharacterResponse& aResponse) const noexcept
{
    aResponse.ServerId = World::ToInteger(aEntity);

    if (const auto* pOwnerComponent = m_world.try_get<OwnerComponent>(aEntity))
        aResponse.OwnershipEpoch = pOwnerComponent->OwnershipEpoch;

    if (const auto* pActorValuesComponent = m_world.try_get<ActorValuesComponent>(aEntity))
        aResponse.AllActorValues = pActorValuesComponent->CurrentActorValues;

    if (const auto* pInventoryComponent = m_world.try_get<InventoryComponent>(aEntity))
    {
        aResponse.CurrentInventory = pInventoryComponent->Content;
        aResponse.InventoryAuthoritative = pInventoryComponent->HasAuthoritativeMutation;
    }

    if (const auto* pCharacterComponent = m_world.try_get<CharacterComponent>(aEntity))
    {
        aResponse.PlayerId = pCharacterComponent->PlayerId;
        aResponse.IsDead = pCharacterComponent->IsDead();
        aResponse.IsWeaponDrawn = pCharacterComponent->IsWeaponDrawn();
        aResponse.LeveledNpcPickId = pCharacterComponent->LeveledNpcPickId.Id;
        aResponse.MountedOnServerId = pCharacterComponent->MountedOnServerId;

        if (pCharacterComponent->LeveledNpcPickId)
        {
            spdlog::debug(
                "Including leveled NPC pick in assignment response for actor {:X}, pick: {:x}:{:x}, owner: {}, epoch: {}",
                aResponse.ServerId,
                aResponse.LeveledNpcPickId.ModId,
                aResponse.LeveledNpcPickId.BaseId,
                aResponse.Owner,
                aResponse.OwnershipEpoch);
        }
    }

    if (const auto* pMovementComponent = m_world.try_get<MovementComponent>(aEntity))
        aResponse.Position = pMovementComponent->Position;

    if (const auto* pCellIdComponent = m_world.try_get<CellIdComponent>(aEntity))
    {
        aResponse.CellId = pCellIdComponent->Cell;
        aResponse.WorldSpaceId = pCellIdComponent->WorldSpaceId;
    }

    if (auto* pAnimationComponent = m_world.try_get<AnimationComponent>(aEntity))
        aResponse.ActionsToReplay = pAnimationComponent->ActionsReplayCache.FormRefinedReplayChain();
}

const char* CharacterService::GetOwnershipTransferReasonName(const OwnershipTransferReason aReason) noexcept
{
    switch (aReason)
    {
    case OwnershipTransferReason::LeaderAssignment:
        return "party leader assignment";
    case OwnershipTransferReason::LeaderClaim:
        return "party leader claim";
    case OwnershipTransferReason::CellLease:
        return "separated-cell simulation lease";
    case OwnershipTransferReason::Mount:
        return "mounting";
    case OwnershipTransferReason::Relinquish:
        return "owner relinquished control";
    case OwnershipTransferReason::OwnerUnavailable:
        return "owner became unavailable";
    }

    return "unknown reason";
}

bool CharacterService::CanClaimOwnership(Player* apPlayer, const entt::entity aEntity, const uint32_t aExpectedOwnershipEpoch, const OwnershipTransferReason aReason) const noexcept
{
    const uint32_t serverId = World::ToInteger(aEntity);
    const char* pReasonName = GetOwnershipTransferReasonName(aReason);
    const auto view = m_world.view<OwnerComponent, CharacterComponent, CellIdComponent, FormIdComponent>();
    const auto it = view.find(aEntity);
    if (it == view.end())
    {
        spdlog::debug("Rejected {} from player {:X} because actor {:X} is unavailable or temporary", pReasonName, apPlayer->GetId(), serverId);
        return false;
    }

    const auto& ownerComponent = view.get<OwnerComponent>(*it);
    const auto& characterComponent = view.get<CharacterComponent>(*it);
    const auto& cellIdComponent = view.get<CellIdComponent>(*it);
    Player* const pCurrentOwner = ownerComponent.GetOwner();
    const uint32_t currentOwnerId = pCurrentOwner ? pCurrentOwner->GetId() : 0;

    const auto reject = [&](const char* apReason)
    {
        spdlog::debug(
            "Rejected {} from player {:X} for actor {:X}: {} (requested epoch {}, current owner {:X}, current epoch {})",
            pReasonName, apPlayer->GetId(), serverId, apReason, aExpectedOwnershipEpoch, currentOwnerId, ownerComponent.OwnershipEpoch);
        return false;
    };

    if (aExpectedOwnershipEpoch == 0 || ownerComponent.OwnershipEpoch != aExpectedOwnershipEpoch)
        return reject("the ownership epoch is stale");

    if (!pCurrentOwner || pCurrentOwner == apPlayer)
        return reject("the player already owns the actor");

    if (characterComponent.IsPlayer())
        return reject("a player actor cannot be claimed");

    // An actor whose stored cell is unknown was registered by a PC whose cell had not loaded yet
    // (a follower at the first frame of a new game: Lokir came in at cell 0, position 0,0,0). Nothing
    // is ever in range of that cell, so the leader's claim was rejected and the follower kept a
    // scene actor the leader's quest was waiting on (the intro cart scene stalled). The leader may
    // always claim such an actor.
    const bool unknownCell = cellIdComponent.Cell == GameId{};
    if (!apPlayer->GetCellComponent().IsInRange(cellIdComponent, characterComponent.IsDragon()) &&
        !(unknownCell && m_world.GetPartyService().IsPlayerLeader(apPlayer)))
        return reject("the actor is out of range");

    auto& partyService = m_world.GetPartyService();
    PartyService::Party* const pParty = partyService.GetPlayerParty(apPlayer);
    if (!pParty)
        return reject("the player is not in a party");
    if (characterComponent.IsMount() && !partyService.IsPlayerLeader(apPlayer))
        return reject("a follower cannot claim a mount from the leader's simulation");
    if (std::find(pParty->Members.begin(), pParty->Members.end(), pCurrentOwner) == pParty->Members.end())
        return reject("the current owner is not in the party");

    if (!partyService.IsPlayerLeader(apPlayer))
    {
        // EnforceLeaderAuthority gives every unknown-cell actor to the leader. Letting a follower
        // claim it back (nothing is ever "in range" of an unknown cell) flipped the owner every
        // second, re-sending its inventory each time (actor 20 in the intro, epochs 125..138).
        if (unknownCell)
            return reject("the actor's cell is unknown; the leader simulates it");
        auto* pLeader = m_world.GetPlayerManager().GetById(pParty->LeaderPlayerId);
        if (pLeader && pLeader->GetCellComponent().IsInRange(cellIdComponent, characterComponent.IsDragon()))
            return reject("the leader is in range and retains simulation authority");
        if (pCurrentOwner->GetCellComponent().IsInRange(cellIdComponent, characterComponent.IsDragon()))
            return reject("the current cell simulator is still in range");
    }

    return true;
}

bool CharacterService::TransferOwnership(Player* apPlayer, const entt::entity aEntity, const OwnershipTransferReason aReason, const bool aResetInvalidOwners) const noexcept
{
    const char* pReasonName = GetOwnershipTransferReasonName(aReason);
    const auto view = m_world.view<OwnerComponent, CharacterComponent, CellIdComponent>();
    const auto it = view.find(aEntity);
    if (!apPlayer || it == view.end())
    {
        spdlog::warn("Cannot transfer ownership of actor {:X} for {} because the target is invalid", World::ToInteger(aEntity), pReasonName);
        return false;
    }

    auto& ownerComponent = view.get<OwnerComponent>(*it);
    Player* const pOldOwner = ownerComponent.GetOwner();
    if (pOldOwner == apPlayer)
        return true;

    const uint32_t oldOwnerId = pOldOwner ? pOldOwner->GetId() : 0;
    const uint32_t oldEpoch = ownerComponent.OwnershipEpoch;
    uint32_t newEpoch = oldEpoch + 1;
    if (newEpoch == 0)
        newEpoch = 1;

    NotifyOwnershipTransfer notify{};
    notify.ServerId = World::ToInteger(aEntity);
    notify.OwnerPlayerId = apPlayer->GetId();
    notify.OwnershipEpoch = newEpoch;
    notify.CurrentActorData = BuildActorData(aEntity);
    notify.LeveledNpcPickId = view.get<CharacterComponent>(*it).LeveledNpcPickId.Id;

    ownerComponent.SetOwner(apPlayer);
    ownerComponent.OwnershipEpoch = newEpoch;
    if (auto* pAnimation = m_world.try_get<AnimationComponent>(aEntity))
    {
        pAnimation->EvaluatedPose = {};
        pAnimation->EvaluatedPosePending = false;
        pAnimation->VisualBones = {};
        pAnimation->VisualBonesPending = false;
    }
    if (aResetInvalidOwners)
        ownerComponent.InvalidOwners.clear();

    if (!GameServer::Get()->SendToPlayersInRange(notify, aEntity, pOldOwner))
        spdlog::error("Failed to broadcast ownership transfer for actor {:X}", notify.ServerId);

    // The former owner may already be out of range, so notify it directly as well.
    if (pOldOwner)
        pOldOwner->Send(notify);

    spdlog::info(
        "Transferred ownership of actor {:X} from player {:X} to player {:X} for {} (epoch {} to {})",
        notify.ServerId, oldOwnerId, notify.OwnerPlayerId, pReasonName, oldEpoch, newEpoch);

    return true;
}

void CharacterService::TransferToNextOwner(const entt::entity aEntity, const OwnershipTransferReason aReason) const noexcept
{
    const char* pReasonName = GetOwnershipTransferReasonName(aReason);
    const auto view = m_world.view<OwnerComponent, CharacterComponent, CellIdComponent>();
    const auto it = view.find(aEntity);
    if (it == view.end())
    {
        spdlog::warn("Cannot select a new owner for actor {:X} after {} because the actor is missing", World::ToInteger(aEntity), pReasonName);
        return;
    }

    auto& ownerComponent = view.get<OwnerComponent>(*it);
    const auto& characterComponent = view.get<CharacterComponent>(*it);
    const auto& cellIdComponent = view.get<CellIdComponent>(*it);

    auto& partyService = m_world.GetPartyService();
    // Prefer a leader in range, then grant a follower a temporary cell lease.
    // Ownership epochs still invalidate packets from the former simulator.
    for (int priority = 0; priority < 2; ++priority)
    for (Player* pPlayer : m_world.GetPlayerManager())
    {
        if (pPlayer == ownerComponent.GetOwner())
            continue;

        const bool follower = partyService.IsPlayerInParty(pPlayer) && !partyService.IsPlayerLeader(pPlayer);
        if (follower != (priority == 1))
            continue;

        if (std::find(ownerComponent.InvalidOwners.begin(), ownerComponent.InvalidOwners.end(), pPlayer) != ownerComponent.InvalidOwners.end())
            continue;

        if (!pPlayer->GetCellComponent().IsInRange(cellIdComponent, characterComponent.IsDragon()))
            continue;

        // Retain every owner that declined this handoff chain so the actor cannot bounce between unloaded clients.
        if (TransferOwnership(pPlayer, aEntity,
                follower ? OwnershipTransferReason::CellLease : aReason, false))
            return;
    }

    spdlog::info("Removing actor {:X} after {} because no eligible owner remains", World::ToInteger(aEntity), pReasonName);
    m_world.GetDispatcher().trigger(CharacterRemoveEvent(World::ToInteger(aEntity)));
}

ActorData CharacterService::BuildActorData(const entt::entity acEntity) const noexcept
{
    ActorData actorData{};

    const auto* pActorValuesComponent = m_world.try_get<ActorValuesComponent>(acEntity);
    if (pActorValuesComponent)
    {
        actorData.InitialActorValues = pActorValuesComponent->CurrentActorValues;
    }

    const auto* pInventoryComponent = m_world.try_get<InventoryComponent>(acEntity);
    if (pInventoryComponent)
    {
        actorData.InitialInventory = pInventoryComponent->Content;
    }

    actorData.IsDead = false;
    const auto* pCharacterComponent = m_world.try_get<CharacterComponent>(acEntity);
    if (pCharacterComponent)
    {
        actorData.IsDead = pCharacterComponent->IsDead();
        actorData.IsWeaponDrawn = pCharacterComponent->IsWeaponDrawn();
    }

    return actorData;
}

void CharacterService::ProcessFactionsChanges() const noexcept
{
    static std::chrono::steady_clock::time_point lastSendTimePoint;
    constexpr auto cDelayBetweenSnapshots = 2000ms;

    const auto now = std::chrono::steady_clock::now();
    if (now - lastSendTimePoint < cDelayBetweenSnapshots)
        return;

    lastSendTimePoint = now;

    const auto characterView = m_world.view<CellIdComponent, CharacterComponent, OwnerComponent>();

    TiltedPhoques::Map<Player*, NotifyFactionsChanges> messages;

    for (auto entity : characterView)
    {
        auto& characterComponent = characterView.get<CharacterComponent>(entity);
        auto& cellIdComponent = characterView.get<CellIdComponent>(entity);
        auto& ownerComponent = characterView.get<OwnerComponent>(entity);

        // If we have nothing new to send skip this
        if (!characterComponent.IsDirtyFactions())
            continue;

        for (auto pPlayer : m_world.GetPlayerManager())
        {
            if (pPlayer == ownerComponent.GetOwner())
                continue;

            if (!cellIdComponent.IsInRange(pPlayer->GetCellComponent(), characterComponent.IsDragon()))
                continue;

            auto& message = messages[pPlayer];
            auto& change = message.Changes[World::ToInteger(entity)];

            change = characterComponent.FactionsContent;
        }

        characterComponent.SetDirtyFactions(false);
    }

    for (auto [pPlayer, message] : messages)
    {
        if (!message.Changes.empty())
            pPlayer->Send(message);
    }
}

void CharacterService::ProcessMovementChanges() const noexcept
{
    static std::chrono::steady_clock::time_point lastSendTimePoint;
    constexpr auto cDelayBetweenSnapshots = 1000ms / 50;

    const auto now = std::chrono::steady_clock::now();
    if (now - lastSendTimePoint < cDelayBetweenSnapshots)
        return;

    lastSendTimePoint = now;

    const auto characterView = m_world.view<CharacterComponent, CellIdComponent, MovementComponent, AnimationComponent, OwnerComponent>();

    TiltedPhoques::Map<Player*, ServerReferencesMoveRequest> messages;

    for (auto pPlayer : m_world.GetPlayerManager())
    {
        auto& message = messages[pPlayer];

        message.Tick = GameServer::Get()->GetTick();
    }

    for (auto entity : characterView)
    {
        auto& characterComponent = characterView.get<CharacterComponent>(entity);
        auto& movementComponent = characterView.get<MovementComponent>(entity);
        auto& cellIdComponent = characterView.get<CellIdComponent>(entity);
        auto& ownerComponent = characterView.get<OwnerComponent>(entity);
        auto& animationComponent = characterView.get<AnimationComponent>(entity);

        // If we have nothing new to send skip this
        if (movementComponent.Sent == true)
            continue;

        for (auto pPlayer : m_world.GetPlayerManager())
        {
            if (pPlayer == ownerComponent.GetOwner())
                continue;

            if (!cellIdComponent.IsInRange(pPlayer->GetCellComponent(), characterComponent.IsDragon()))
                continue;

            auto& message = messages[pPlayer];
            auto& update = message.Updates[World::ToInteger(entity)];
            auto& movement = update.UpdatedMovement;

            movement.CellId = cellIdComponent.Cell;
            movement.WorldSpaceId = cellIdComponent.WorldSpaceId;
            movement.Position = movementComponent.Position;

            movement.Rotation.x = movementComponent.Rotation.x;
            movement.Rotation.y = movementComponent.Rotation.z;

            movement.Direction = movementComponent.Direction;
            movement.Variables = movementComponent.Variables;
            update.CombatTargetServerId = movementComponent.CombatTargetServerId;

            update.ActionEvents = animationComponent.Actions;
            if (animationComponent.EvaluatedPosePending)
                update.EvaluatedPose = animationComponent.EvaluatedPose;
            if (animationComponent.VisualBonesPending)
                update.VisualBones = animationComponent.VisualBones;
        }
    }

    m_world.view<AnimationComponent>().each([](AnimationComponent& animationComponent)
    {
        // Remove actions we've sent
        animationComponent.Actions.clear();
        animationComponent.EvaluatedPosePending = false;
        animationComponent.VisualBonesPending = false;
    });

    m_world.view<MovementComponent>().each([](MovementComponent& movementComponent) { movementComponent.Sent = true; });

    for (auto& [pPlayer, message] : messages)
    {
        if (!message.Updates.empty())
            pPlayer->Send(message);
    }
}
