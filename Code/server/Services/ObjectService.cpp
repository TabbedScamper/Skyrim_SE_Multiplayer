#include <Services/ObjectService.h>

#include <GameServer.h>
#include <World.h>
#include <Components.h>

#include <Events/PlayerLeaveCellEvent.h>

#include <Messages/ActivateRequest.h>
#include <Messages/NotifyActivate.h>
#include <Messages/LockChangeRequest.h>
#include <Messages/NotifyLockChange.h>
#include <Messages/AssignObjectsRequest.h>
#include <Messages/AssignObjectsResponse.h>
#include <Messages/ScriptAnimationRequest.h>
#include <Messages/NotifyScriptAnimation.h>
#include <Messages/PhysicsReferencesMoveRequest.h>
#include <Messages/NotifyPhysicsReferencesMove.h>
#include <Messages/PhysicsLeaseRequest.h>
#include <Messages/NotifyPhysicsLease.h>

ObjectService::ObjectService(World& aWorld, entt::dispatcher& aDispatcher)
    : m_world(aWorld)
{
    m_leaveCellConnection = aDispatcher.sink<PlayerLeaveCellEvent>().connect<&ObjectService::OnPlayerLeaveCellEvent>(this);
    m_assignObjectConnection = aDispatcher.sink<PacketEvent<AssignObjectsRequest>>().connect<&ObjectService::OnAssignObjectsRequest>(this);
    m_activateConnection = aDispatcher.sink<PacketEvent<ActivateRequest>>().connect<&ObjectService::OnActivate>(this);
    m_lockChangeConnection = aDispatcher.sink<PacketEvent<LockChangeRequest>>().connect<&ObjectService::OnLockChange>(this);
    m_scriptAnimationConnection = aDispatcher.sink<PacketEvent<ScriptAnimationRequest>>().connect<&ObjectService::OnScriptAnimationRequest>(this);
    m_physicsMoveConnection = aDispatcher.sink<PacketEvent<PhysicsReferencesMoveRequest>>().connect<&ObjectService::OnPhysicsReferencesMove>(this);
    m_physicsLeaseConnection = aDispatcher.sink<PacketEvent<PhysicsLeaseRequest>>().connect<&ObjectService::OnPhysicsLease>(this);
}

void ObjectService::BroadcastLease(const PartyService::Party& acParty, const GameId& acId, uint32_t aHolder) const noexcept
{
    NotifyPhysicsLease notify{};
    notify.Id = acId;
    notify.HolderId = aHolder;
    notify.Epoch = acParty.StartEpoch;
    for (auto* pMember : acParty.Members)
        pMember->Send(notify);
}

// Owner report (2026-09-29): a follower's hold-to-grab only jiggled the object. The leader streams every loose world
// object and followers steer them to its pose each physics step, so the follower's grab spring was pulled back. The
// grabbing player takes the object's stream while it carries it; everyone else, the leader included, follows it.
void ObjectService::OnPhysicsLease(const PacketEvent<PhysicsLeaseRequest>& acMessage) noexcept
{
    auto* pPlayer = acMessage.pPlayer;
    const auto& request = acMessage.Packet;
    auto* pParty = m_world.GetPartyService().GetPlayerParty(pPlayer);
    if (!request.IsValid() || !pParty || pParty->SessionState < 2 || request.Epoch != pParty->StartEpoch ||
        !pPlayer->GetParty().JoinedPartyId)
        return;
    const auto partyId = *pPlayer->GetParty().JoinedPartyId;
    auto& leases = m_leases[partyId];
    const auto now = GameServer::Get()->GetTick();
    auto it = leases.find(request.Id);
    if (it != leases.end() && (it->second.Epoch != pParty->StartEpoch || now - it->second.LastUpdate > 10000 ||
        !m_world.GetPlayerManager().GetById(it->second.Holder)))
    {
        leases.erase(it);
        it = leases.end();
    }
    if (request.Hold)
    {
        if (it != leases.end() && it->second.Holder != pPlayer->GetId())
        {
            // Someone else carries it: tell the asker who, so it keeps following that stream.
            NotifyPhysicsLease current{};
            current.Id = request.Id;
            current.HolderId = it->second.Holder;
            current.Epoch = pParty->StartEpoch;
            pPlayer->Send(current);
            return;
        }
        // The leader grabbing what nobody else carries is the normal case: it already streams it.
        if (m_world.GetPartyService().IsPlayerLeader(pPlayer))
        {
            if (it != leases.end())
            {
                leases.erase(it);
                BroadcastLease(*pParty, request.Id, 0);
            }
            return;
        }
        leases[request.Id] = {pPlayer->GetId(), pParty->StartEpoch, now};
        BroadcastLease(*pParty, request.Id, pPlayer->GetId());
        spdlog::info("Physics lease: player {} carries {:X}:{:X}", pPlayer->GetId(), request.Id.ModId, request.Id.BaseId);
        return;
    }
    if (it != leases.end() && it->second.Holder == pPlayer->GetId())
    {
        leases.erase(it);
        BroadcastLease(*pParty, request.Id, 0);
        spdlog::info("Physics lease: {:X}:{:X} back to the leader", request.Id.ModId, request.Id.BaseId);
    }
}

void ObjectService::OnPhysicsReferencesMove(const PacketEvent<PhysicsReferencesMoveRequest>& acMessage) noexcept
{
    auto& partyService = m_world.GetPartyService();
    auto* pParty = partyService.GetPlayerParty(acMessage.pPlayer);
    if (!pParty || pParty->SessionState < 2 || acMessage.Packet.Updates.empty() ||
        !acMessage.pPlayer->GetParty().JoinedPartyId)
        return;
    const bool leader = partyService.IsPlayerLeader(acMessage.pPlayer);
    const auto playerId = acMessage.pPlayer->GetId();
    const auto now = GameServer::Get()->GetTick();
    auto& leases = m_leases[*acMessage.pPlayer->GetParty().JoinedPartyId];
    // Stale leases (holder quiet for 10 s, gone, or a new campaign epoch) go back to the leader.
    for (auto it = leases.begin(); it != leases.end();)
    {
        if (it->second.Epoch != pParty->StartEpoch || now - it->second.LastUpdate > 10000 ||
            !m_world.GetPlayerManager().GetById(it->second.Holder))
        {
            BroadcastLease(*pParty, it->first, 0);
            it = leases.erase(it);
        }
        else
            ++it;
    }
    if (!leader && leases.empty())
    {
        spdlog::warn("Rejected physics snapshot from non-leader player {}", playerId);
        return;
    }

    NotifyPhysicsReferencesMove notify{};
    notify.Tick = acMessage.Packet.Tick;
    notify.AuthorityEpoch = pParty->StartEpoch;
    notify.Updates.reserve(acMessage.Packet.Updates.size());
    for (const auto& update : acMessage.Packet.Updates)
    {
        const auto lease = leases.find(update.Id);
        // The leader streams everything nobody carries; a carrier streams only what it carries.
        if (lease == leases.end() ? !leader : lease->second.Holder != playerId)
            continue;
        if (lease != leases.end())
            lease->second.LastUpdate = now;
        notify.Updates.push_back(update);
    }
    if (notify.Updates.empty())
        return;
    const auto& sourceCell = acMessage.pPlayer->GetCellComponent();
    for (auto* pMember : pParty->Members)
    {
        // A separated party member must never receive corrections for a cell
        // that their Skyrim instance has not loaded. The host remains the
        // source for the nearby region; distant-cell authority is a separate
        // concern and cannot be inferred from a missing local reference.
        if (pMember != acMessage.pPlayer && pMember->GetCellComponent().IsInRange(sourceCell, false))
            pMember->Send(notify);
    }
}

// TODO(cosideci): the cell handling of objects need to be revamped.
// We already store the location and worldspace of the mod through CellIdComponent.
// Clients need a message saying the entity was destroyed.
void ObjectService::OnPlayerLeaveCellEvent(const PlayerLeaveCellEvent& acEvent) noexcept
{
    for (Player* pPlayer : m_world.GetPlayerManager())
    {
        if (pPlayer->GetCellComponent().Cell == acEvent.OldCell)
            return;
    }

    auto objectView = m_world.view<ObjectComponent, CellIdComponent>();
    Vector<entt::entity> toDestroy;

    for (auto entity : objectView)
    {
        const auto& cellIdComponent = objectView.get<CellIdComponent>(entity);

        if (cellIdComponent.Cell != acEvent.OldCell)
            continue;

        toDestroy.push_back(entity);
    }

    for (auto& entity : toDestroy)
    {
        m_world.destroy(entity);
    }
}

// NOTE: this whole system kinda relies on all objects in a cell being static.
// This is fine for containers and doors, but if this system is expanded, think of temporaries.
void ObjectService::OnAssignObjectsRequest(const PacketEvent<AssignObjectsRequest>& acMessage) noexcept
{
    auto view = m_world.view<FormIdComponent, ObjectComponent, InventoryComponent>();

    AssignObjectsResponse response;

    for (const ObjectData& object : acMessage.Packet.Objects)
    {
        const auto iter = std::find_if(
            std::begin(view), std::end(view),
            [view, id = object.Id](auto entity)
            {
                const auto& formIdComponent = view.get<FormIdComponent>(entity);
                return formIdComponent.Id == id;
            });

        if (iter != std::end(view))
        {
            ObjectData objectData;
            objectData.ServerId = World::ToInteger(*iter);

            auto& formIdComponent = view.get<FormIdComponent>(*iter);
            objectData.Id = formIdComponent.Id;

            auto& objectComponent = view.get<ObjectComponent>(*iter);
            objectData.CurrentLockData = objectComponent.CurrentLockData;

            auto& inventoryComponent = view.get<InventoryComponent>(*iter);
            objectData.CurrentInventory = inventoryComponent.Content;

            objectData.IsSenderFirst = false;

            response.Objects.push_back(objectData);
        }
        else
        {
            const auto cEntity = m_world.create();

            m_world.emplace<FormIdComponent>(cEntity, object.Id);

            auto& objectComponent = m_world.emplace<ObjectComponent>(cEntity, acMessage.pPlayer);
            objectComponent.CurrentLockData = object.CurrentLockData;

            m_world.emplace<CellIdComponent>(cEntity, object.CellId, object.WorldSpaceId, object.CurrentCoords);
            auto& inventoryComp = m_world.emplace<InventoryComponent>(cEntity);
            inventoryComp.Content = object.CurrentInventory;

            ObjectData objectData;
            objectData.Id = object.Id;
            objectData.ServerId = World::ToInteger(cEntity);
            objectData.IsSenderFirst = true;

            response.Objects.push_back(objectData);
        }
    }

    if (!response.Objects.empty())
        acMessage.pPlayer->Send(response);
}

void ObjectService::OnActivate(const PacketEvent<ActivateRequest>& acMessage) const noexcept
{
    NotifyActivate notifyActivate;
    notifyActivate.Id = acMessage.Packet.Id;
    notifyActivate.ActivatorId = acMessage.Packet.ActivatorId;
    notifyActivate.PreActivationOpenState = acMessage.Packet.PreActivationOpenState;

    for (auto pPlayer : m_world.GetPlayerManager())
    {
        if (pPlayer != acMessage.pPlayer && pPlayer->GetCellComponent().Cell == acMessage.Packet.CellId)
        {
            pPlayer->Send(notifyActivate);
        }
    }
}

void ObjectService::OnLockChange(const PacketEvent<LockChangeRequest>& acMessage) const noexcept
{
    NotifyLockChange notifyLockChange;
    notifyLockChange.Id = acMessage.Packet.Id;
    notifyLockChange.IsLocked = acMessage.Packet.IsLocked;
    notifyLockChange.LockLevel = acMessage.Packet.LockLevel;

    auto objectView = m_world.view<FormIdComponent, ObjectComponent>();

    const auto iter = std::find_if(
        std::begin(objectView), std::end(objectView),
        [objectView, id = acMessage.Packet.Id](auto entity)
        {
            const auto& formIdComponent = objectView.get<FormIdComponent>(entity);
            return formIdComponent.Id == id;
        });

    if (iter != std::end(objectView))
    {
        auto& objectComponent = objectView.get<ObjectComponent>(*iter);
        objectComponent.CurrentLockData.IsLocked = acMessage.Packet.IsLocked;
        objectComponent.CurrentLockData.LockLevel = acMessage.Packet.LockLevel;
    }

    for (Player* pPlayer : m_world.GetPlayerManager())
    {
        if (pPlayer == acMessage.pPlayer)
            continue;

        if (pPlayer->GetCellComponent().Cell == acMessage.Packet.CellId)
            pPlayer->Send(notifyLockChange);
    }
}

void ObjectService::OnScriptAnimationRequest(const PacketEvent<ScriptAnimationRequest>& acMessage) noexcept
{
    auto& packet = acMessage.Packet;

    NotifyScriptAnimation message{};
    message.FormID = packet.FormID;
    message.Animation = packet.Animation;
    message.EventName = packet.EventName;

    for (Player* pPlayer : m_world.GetPlayerManager())
    {
        pPlayer->Send(message);
    }
}
