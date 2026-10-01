#include "Events/CharacterInteriorCellChangeEvent.h"
#include "Events/CharacterExteriorCellChangeEvent.h"
#include "Events/PlayerLeaveCellEvent.h"

#include <Services/PlayerService.h>
#include <Services/CharacterService.h>
#include <GameServer.h>

#include <Messages/ShiftGridCellRequest.h>
#include <Messages/EnterExteriorCellRequest.h>
#include <Messages/EnterInteriorCellRequest.h>
#include <Messages/CharacterSpawnRequest.h>
#include <Messages/PlayerRespawnRequest.h>
#include <Messages/NotifyInventoryChanges.h>
#include <Messages/NotifyPlayerRespawn.h>
#include <Messages/NotifyRespawn.h>
#include <Messages/PlayerLevelRequest.h>
#include <Messages/NotifyPlayerLevel.h>
#include <Messages/NotifyPlayerCellChanged.h>

#include <Setting.h>
namespace
{
Console::Setting fGoldLossFactor{"Gameplay:fGoldLossFactor", "Factor of the amount of gold lost on death", 0.0f};

bool SnapshotEligible(World& aWorld, Player* apPlayer, entt::entity aEntity)
{
    const bool allowed = aWorld.GetCharacterService().CanReplicateTo(apPlayer, aEntity);
    const auto& actor = aWorld.get<CharacterComponent>(aEntity);
    if (!actor.IsPlayer() && aWorld.GetPartyService().IsPlayerLeader(apPlayer))
    {
        const auto& owner = aWorld.get<OwnerComponent>(aEntity);
        const auto* simulator = owner.GetOwner();
        const auto& cell = aWorld.get<CellIdComponent>(aEntity);
        spdlog::info("Orphan snapshot: actor {:X} leader={} owner={} released={} actorEpoch={} cell={:X}:{:X} packetAgeMs={} replicate={}",
            World::ToInteger(aEntity), apPlayer->GetId(), simulator ? simulator->GetId() : 0,
            owner.Released, owner.PartyEpoch, cell.Cell.ModId, cell.Cell.BaseId,
            simulator ? GameServer::Get()->GetTick() - simulator->LastPacketTick : 0, allowed);
    }
    return allowed;
}
}

PlayerService::PlayerService(World& aWorld, entt::dispatcher& aDispatcher) noexcept
    : m_world(aWorld)
    , m_interiorCellEnterConnection(aDispatcher.sink<PacketEvent<EnterInteriorCellRequest>>().connect<&PlayerService::HandleInteriorCellEnter>(this))
    , m_gridCellShiftConnection(aDispatcher.sink<PacketEvent<ShiftGridCellRequest>>().connect<&PlayerService::HandleGridCellShift>(this))
    , m_exteriorCellEnterConnection(aDispatcher.sink<PacketEvent<EnterExteriorCellRequest>>().connect<&PlayerService::HandleExteriorCellEnter>(this))
    , m_playerRespawnConnection(aDispatcher.sink<PacketEvent<PlayerRespawnRequest>>().connect<&PlayerService::OnPlayerRespawnRequest>(this))
    , m_playerLevelConnection(aDispatcher.sink<PacketEvent<PlayerLevelRequest>>().connect<&PlayerService::OnPlayerLevelRequest>(this))
{
}

void SendPlayerCellChanged(const Player* apPlayer) noexcept
{
    auto& cellComponent = apPlayer->GetCellComponent();

    NotifyPlayerCellChanged notify{};
    notify.PlayerId = apPlayer->GetId();
    notify.WorldSpaceId = cellComponent.WorldSpaceId;
    notify.CellId = cellComponent.Cell;

    GameServer::Get()->SendToPlayers(notify, apPlayer);
}

void PlayerService::HandleGridCellShift(const PacketEvent<ShiftGridCellRequest>& acMessage) const noexcept
{
    auto* pPlayer = acMessage.pPlayer;

    auto& message = acMessage.Packet;

    const GameId oldCell = pPlayer->GetCellComponent().Cell;

    CellIdComponent cell = CellIdComponent{message.PlayerCell, message.WorldSpaceId, message.CenterCoords};
    pPlayer->SetCellComponent(cell);

    m_world.GetDispatcher().trigger(PlayerLeaveCellEvent(oldCell));
    m_world.GetCharacterService().ReconcileCellOwnership(pPlayer);

    auto characterView = m_world.view<CellIdComponent, CharacterComponent, OwnerComponent>();
    for (auto character : characterView)
    {
        const auto& characterCellComponent = characterView.get<CellIdComponent>(character);

        const auto& characterComponent = characterView.get<CharacterComponent>(character);
        // An actor's parent cell can be temporary and absent from the client's cell list.
        // Use the same worldspace/position range as movement broadcasts, including dragon range.
        const bool isInRange = cell.IsInRange(characterCellComponent, characterComponent.IsDragon());

        if (!isInRange)
        {
            continue;
        }

        if (!SnapshotEligible(m_world, pPlayer, character))
            continue;

        CharacterSpawnRequest spawnMessage;
        CharacterService::Serialize(m_world, character, &spawnMessage);

        pPlayer->Send(spawnMessage);
    }
}

void PlayerService::HandleExteriorCellEnter(const PacketEvent<EnterExteriorCellRequest>& acMessage) const noexcept
{
    auto& message = acMessage.Packet;
    auto* pPlayer = acMessage.pPlayer;

    if (message.Heartbeat)
    {
        const auto& recorded = pPlayer->GetCellComponent();
        if (recorded.WorldSpaceId == message.WorldSpaceId && recorded.Cell == message.CellId)
            return;
        spdlog::warn("Cell record healed for player {}: recorded {:X}:{:X} world {:X}:{:X}, actually {:X}:{:X} world {:X}:{:X}",
            pPlayer->GetId(), recorded.Cell.ModId, recorded.Cell.BaseId, recorded.WorldSpaceId.ModId,
            recorded.WorldSpaceId.BaseId, message.CellId.ModId, message.CellId.BaseId, message.WorldSpaceId.ModId,
            message.WorldSpaceId.BaseId);
    }

    if (pPlayer->GetCharacter())
    {
        auto entity = *pPlayer->GetCharacter();

        auto cell = CellIdComponent{message.CellId, message.WorldSpaceId, message.CurrentCoords};

        if (pPlayer->GetCellComponent())
        {
            m_world.GetDispatcher().trigger(CharacterExteriorCellChangeEvent{pPlayer, entity, message.WorldSpaceId, message.CurrentCoords});
        }

        pPlayer->SetCellComponent(cell);
        m_world.GetCharacterService().ReconcileCellOwnership(pPlayer);

        SendPlayerCellChanged(pPlayer);
    }
}

void PlayerService::HandleInteriorCellEnter(const PacketEvent<EnterInteriorCellRequest>& acMessage) const noexcept
{
    auto* pPlayer = acMessage.pPlayer;

    auto& message = acMessage.Packet;

    const auto oldCell = pPlayer->GetCellComponent().Cell;
    // The client re-reports its cell every few seconds. A matching record needs nothing; a stale one is healed by the
    // ordinary enter below (the follower stayed "outside" here after entering the Helgen Keep with the host, so it got
    // no host copy, no revive and Helgen's actors to simulate, 2026-09-30 19:25).
    if (message.Heartbeat)
    {
        if (oldCell == message.CellId && !pPlayer->GetCellComponent().WorldSpaceId)
            return;
        spdlog::warn("Cell record healed for player {}: recorded {:X}:{:X} world {:X}:{:X}, actually interior {:X}:{:X}",
            pPlayer->GetId(), oldCell.ModId, oldCell.BaseId, pPlayer->GetCellComponent().WorldSpaceId.ModId,
            pPlayer->GetCellComponent().WorldSpaceId.BaseId, message.CellId.ModId, message.CellId.BaseId);
    }

    auto cell = CellIdComponent{message.CellId, {}, {}};
    pPlayer->SetCellComponent(cell);

    m_world.GetDispatcher().trigger(PlayerLeaveCellEvent(oldCell));
    m_world.GetCharacterService().ReconcileCellOwnership(pPlayer);

    if (pPlayer->GetCharacter())
    {
        auto entity = *pPlayer->GetCharacter();

        if (auto pCellIdComponent = m_world.try_get<CellIdComponent>(entity); pCellIdComponent)
        {
            m_world.GetDispatcher().trigger(CharacterInteriorCellChangeEvent{pPlayer, entity, message.CellId});
        }
    }

    // Other players already recorded in this interior, by their player cell: a player character's own cell
    // component follows its movement later. Two players entering the Helgen Keep together each missed the other:
    // the second one's character still read the exterior, and the first one's arrival sent a remove (owner,
    // 2026-09-30 19:25: the host was invisible to the follower for the rest of the Keep).
    std::vector<entt::entity> sentPlayers;
    for (auto* pOther : m_world.GetPlayerManager())
    {
        if (pOther == pPlayer || !pOther->GetCharacter() || !m_world.valid(*pOther->GetCharacter()) ||
            pOther->GetCellComponent().Cell != message.CellId)
            continue;
        const auto character = *pOther->GetCharacter();
        if (!m_world.all_of<CharacterComponent, OwnerComponent>(character) || !SnapshotEligible(m_world, pPlayer, character))
            continue;
        CharacterSpawnRequest spawnMessage;
        CharacterService::Serialize(m_world, character, &spawnMessage);
        pPlayer->Send(spawnMessage);
        sentPlayers.push_back(character);
    }

    auto characterView = m_world.view<CellIdComponent, CharacterComponent, OwnerComponent>();
    for (auto character : characterView)
    {

        if (message.CellId != characterView.get<CellIdComponent>(character).Cell)
            continue;

        if (std::find(sentPlayers.begin(), sentPlayers.end(), character) != sentPlayers.end())
            continue;

        if (!SnapshotEligible(m_world, pPlayer, character))
            continue;

        CharacterSpawnRequest spawnMessage;
        CharacterService::Serialize(m_world, character, &spawnMessage);

        pPlayer->Send(spawnMessage);
    }

    SendPlayerCellChanged(pPlayer);
}

void PlayerService::OnPlayerRespawnRequest(const PacketEvent<PlayerRespawnRequest>& acMessage) const noexcept
{
    float goldLossFactor = fGoldLossFactor.as_float();

    auto character = acMessage.pPlayer->GetCharacter();
    if (!character)
        return;

    auto view = m_world.view<InventoryComponent>();

    const auto it = view.find(static_cast<entt::entity>(*character));

    if (it != view.end())
    {
        if (goldLossFactor != 0.0)
        {
            auto& inventoryComponent = view.get<InventoryComponent>(*it);

            GameId goldId(0, 0xF);
            int32_t goldCount = inventoryComponent.Content.GetEntryCountById(goldId);
            int32_t goldToRemove = static_cast<int32_t>(goldCount * goldLossFactor);

            Inventory::Entry entry{};
            entry.BaseId = goldId;
            entry.Count = -goldToRemove;

            inventoryComponent.Content.AddOrRemoveEntry(entry);

            NotifyInventoryChanges notifyInventoryChanges{};
            notifyInventoryChanges.ServerId = World::ToInteger(*character);
            if (const auto* pOwnerComponent = m_world.try_get<OwnerComponent>(*character))
                notifyInventoryChanges.OwnershipEpoch = pOwnerComponent->OwnershipEpoch;
            notifyInventoryChanges.Item = entry;
            notifyInventoryChanges.Drop = false;

            // Exclude respawned player from inventory changes notification...
            if (!GameServer::Get()->SendToPlayersInRange(notifyInventoryChanges, *character, acMessage.GetSender()))
                spdlog::error("{}: SendToPlayersInRange failed", __FUNCTION__);

            // ...and instead, send NotifyPlayerRespawn so that the client can print a message.
            NotifyPlayerRespawn notifyPlayerRespawn{};
            notifyPlayerRespawn.GoldLost = goldToRemove;

            acMessage.pPlayer->Send(notifyPlayerRespawn);
        }

        // Let all other players in cell respawn this player, since the body state seems to be bugged otherwise
        NotifyRespawn notifyRespawn{};
        notifyRespawn.ActorId = World::ToInteger(*character);

        if (!GameServer::Get()->SendToPlayersInRange(notifyRespawn, *character, acMessage.GetSender()))
            spdlog::error("{}: SendToPlayersInRange failed", __FUNCTION__);
    }
}

void PlayerService::OnPlayerLevelRequest(const PacketEvent<PlayerLevelRequest>& acMessage) const noexcept
{
    acMessage.pPlayer->SetLevel(acMessage.Packet.NewLevel);

    NotifyPlayerLevel notify{};
    notify.PlayerId = acMessage.pPlayer->GetId();
    notify.NewLevel = acMessage.Packet.NewLevel;

    GameServer::Get()->SendToPlayers(notify, acMessage.pPlayer);
}
