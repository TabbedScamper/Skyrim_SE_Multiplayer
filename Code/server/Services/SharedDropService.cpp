#include <Services/SharedDropService.h>
#include <World.h>
#include <Components.h>
#include <GameServer.h>
#include <Game/Player.h>
#include <Events/UpdateEvent.h>
#include <Messages/RequestSharedDrop.h>
#include <Messages/NotifySharedDrop.h>
#include <Messages/NotifyInventoryChanges.h>
#include <Setting.h>
#include <cmath>

namespace
{
Console::Setting bEnableItemDrops{"Gameplay:bEnableItemDrops", "Syncs shared dropped items with exclusive pickup", true};

bool SameItem(Inventory::Entry a, Inventory::Entry b)
{
    a.ExtraWorn = a.ExtraWornLeft = b.ExtraWorn = b.ExtraWornLeft = false;
    if (a.ExtraEnchantId.ModId == UINT32_MAX && b.ExtraEnchantId.ModId == UINT32_MAX)
        a.ExtraEnchantId = b.ExtraEnchantId;
    if (!a.CanBeMerged(b) || a.EnchantData.Effects.size() != b.EnchantData.Effects.size()) return false;
    for (size_t i = 0; i < a.EnchantData.Effects.size(); ++i)
    {
        const auto& x = a.EnchantData.Effects[i]; const auto& y = b.EnchantData.Effects[i];
        if (x.EffectId != y.EffectId || x.Magnitude != y.Magnitude || x.Area != y.Area ||
            x.Duration != y.Duration || x.RawCost != y.RawCost) return false;
    }
    return true;
}
}

SharedDropService::SharedDropService(World& aWorld, entt::dispatcher& aDispatcher)
    : m_world(aWorld)
    , m_requestConnection(aDispatcher.sink<PacketEvent<RequestSharedDrop>>().connect<&SharedDropService::OnRequest>(this))
    , m_updateConnection(aDispatcher.sink<UpdateEvent>().connect<&SharedDropService::OnUpdate>(this))
{
}

Player* SharedDropService::FindPlayer(uint32_t aId) const
{
    for (auto* player : m_world.GetPlayerManager()) if (player->GetId() == aId) return player;
    return nullptr;
}

bool SharedDropService::Near(const Player* aPlayer, const Drop& aDrop) const
{
    if (!aPlayer || !aPlayer->GetParty().JoinedPartyId || *aPlayer->GetParty().JoinedPartyId != aDrop.Party)
        return false;
    const CellIdComponent cell(aDrop.Data.Cell, aDrop.Data.WorldSpace,
        GridCellCoords::CalculateGridCellCoords(aDrop.Data.Physics.Position.x, aDrop.Data.Physics.Position.y));
    return aPlayer->GetCellComponent().WorldSpaceId == cell.WorldSpaceId && aPlayer->GetCellComponent().IsInRange(cell, false);
}

void SharedDropService::Send(Player* aPlayer, const Drop& aDrop, SharedDropAction aAction, uint64_t aToken) const
{
    if (!aPlayer) return;
    NotifySharedDrop reply;
    static_cast<SharedDropData&>(reply) = aDrop.Data;
    reply.Action = aAction;
    reply.Token = aToken;
    aPlayer->Send(reply);
}

void SharedDropService::Broadcast(const Drop& aDrop, SharedDropAction aAction) const
{
    for (auto* player : m_world.GetPlayerManager())
        if (aDrop.Seen.count(player->GetId())) Send(player, aDrop, aAction);
}

void SharedDropService::OnRequest(const PacketEvent<RequestSharedDrop>& aEvent) noexcept
{
    const auto& request = aEvent.Packet;
    auto* player = aEvent.pPlayer;
    auto* party = m_world.GetPartyService().GetPlayerParty(player);
    if (!request.IsValid() || !request.ValidPayload() || request.Action > SharedDropAction::Ready ||
        !party || party->StartEpoch != request.Epoch || party->SessionState < 2 ||
        !player->GetParty().JoinedPartyId || !player->GetCharacter() ||
        std::find(party->Members.begin(), party->Members.end(), player) == party->Members.end()) return;
    const auto actor = *player->GetCharacter();
    const auto* owner = m_world.try_get<OwnerComponent>(actor);
    const auto* character = m_world.try_get<CharacterComponent>(actor);
    const auto* movement = m_world.try_get<MovementComponent>(actor);
    auto* inventory = m_world.try_get<InventoryComponent>(actor);
    if (!owner || owner->GetOwner() != player || !character || !character->IsPlayer() || !movement || !inventory) return;

    if (request.Action == SharedDropAction::Create)
    {
        const auto key = std::make_pair(player->GetId(), request.Token);
        if (const auto it = m_receipts.find(key); it != m_receipts.end())
        {
            const auto* drop = m_world.try_get<Drop>(static_cast<entt::entity>(it->second));
            if (drop) Send(player, *drop, drop->Retired ? SharedDropAction::Local :
                drop->Claim.Taken ? SharedDropAction::Remove : SharedDropAction::Upsert, request.Token);
            return;
        }
        Drop drop;
        drop.Data = request;
        drop.Party = *player->GetParty().JoinedPartyId;
        const auto refuse = [&]()
        {
            drop.Data.OriginToken = request.Token;
            Send(player, drop, SharedDropAction::Local, request.Token);
        };
        const auto delta = request.Physics.Position - movement->Position;
        if (!Near(player, drop) || !std::isfinite(glm::dot(delta, delta)) || glm::dot(delta, delta) > 512.f * 512.f ||
            character->IsDead() || m_receipts.size() >= 16384) { refuse(); return; }
        int64_t available{};
        for (const auto& entry : inventory->Content.Entries)
            if (entry.BaseId == request.Item.BaseId && !entry.IsQuestItem && entry.Count > 0) available += entry.Count;
        if (available < request.Item.Count)
        {
            spdlog::warn("Shared drop: rejected unbacked item {:X} x{} from {}", request.Item.BaseId.BaseId,
                request.Item.Count, player->GetUsername());
            refuse();
            return;
        }
        // Debit the inventory once, from the authoritative entries (which may still be worn).
        int32_t remaining = request.Item.Count;
        auto entries = inventory->Content.Entries;
        // Charge/poison/tempering can change without an inventory event. The
        // drop owner's actual reference supplies those values; quantity still
        // must be backed by the server. Prefer an exact instance when possible.
        for (bool exact : {true, false})
        {
            for (auto& entry : entries)
            {
                if (!remaining || entry.Count <= 0 || entry.IsQuestItem || entry.BaseId != request.Item.BaseId ||
                    (exact && !SameItem(entry, request.Item))) continue;
                const auto count = (std::min)(remaining, entry.Count);
                auto debit = entry;
                debit.Count = -count;
                inventory->Content.AddOrRemoveEntry(debit);
                entry.Count -= count;
                remaining -= count;
            }
        }
        inventory->HasAuthoritativeMutation = true;
        NotifyInventoryChanges change;
        change.ServerId = World::ToInteger(actor); change.OwnershipEpoch = owner->OwnershipEpoch;
        change.Item = request.Item; change.Item.Count = -request.Item.Count; change.Drop = false;
        GameServer::Get()->SendToPlayersInRange(change, actor, aEvent.GetSender());

        const auto entity = m_world.create();
        drop.Data.Id = World::ToInteger(entity);
        drop.Data.Generation = 1; drop.Data.Owner = drop.Data.Creator = player->GetId();
        drop.Data.Replicas = 1;
        drop.Data.OriginToken = request.Token; drop.Data.Token = 0;
        drop.Data.Winner.clear();
        drop.Data.Physics.Id = {SharedDropData::PhysicsModId, drop.Data.Id};
        drop.Seen.insert(player->GetId()); drop.Ready.insert(player->GetId());
        drop.Retired = !bEnableItemDrops;
        m_receipts.emplace(key, drop.Data.Id);
        auto& stored = m_world.emplace<Drop>(entity, std::move(drop));
        Send(player, stored, stored.Retired ? SharedDropAction::Local : SharedDropAction::Upsert, request.Token);
        spdlog::info("Shared drop: {:X} x{} dropped by {} (id {})", stored.Data.Item.BaseId.BaseId,
            stored.Data.Item.Count, player->GetUsername(), stored.Data.Id);
        return;
    }

    if (request.Action == SharedDropAction::Snapshot)
    {
        for (auto entity : m_world.view<Drop>())
        {
            auto& drop = m_world.get<Drop>(entity);
            if (!drop.Retired && !drop.Claim.Taken && drop.Data.Epoch == request.Epoch && Near(player, drop))
            {
                drop.Seen.insert(player->GetId());
                Send(player, drop, SharedDropAction::Upsert);
            }
        }
        return;
    }
    const auto dropEntity = static_cast<entt::entity>(request.Id);
    if (!m_world.valid(dropEntity)) return;
    auto* drop = m_world.try_get<Drop>(dropEntity);
    if (!drop || drop->Data.Epoch != request.Epoch || drop->Party != *player->GetParty().JoinedPartyId) return;
    if (request.Action == SharedDropAction::Pickup)
    {
        const auto delta = drop->Data.Physics.Position - movement->Position;
        if (drop->Retired || request.Generation != drop->Data.Generation ||
            !drop->Claim.TryTake(player->GetId(), Near(player, *drop), !character->IsDead(), glm::dot(delta, delta)))
        {
            Send(player, *drop, SharedDropAction::Denied, request.Token);
            return;
        }
        // The terminal claim is committed before any grants or inventory notifications.
        inventory->Content.AddOrRemoveEntry(drop->Data.Item);
        inventory->HasAuthoritativeMutation = true;
        drop->Data.Winner = player->GetUsername().substr(0, SharedDropData::MaxName);
        Send(player, *drop, SharedDropAction::Granted, request.Token);
        Broadcast(*drop, SharedDropAction::Remove);
        NotifyInventoryChanges change;
        change.ServerId = World::ToInteger(actor); change.OwnershipEpoch = owner->OwnershipEpoch;
        change.Item = drop->Data.Item;
        GameServer::Get()->SendToPlayersInRange(change, actor, aEvent.GetSender());
        spdlog::info("Shared drop: {} picked up by {}", request.Id, player->GetUsername());
        return;
    }
    if (drop->Retired || drop->Claim.Taken || request.Generation != drop->Data.Generation || !Near(player, *drop)) return;
    if (request.Action == SharedDropAction::Ready)
    {
        if (drop->Seen.count(player->GetId()) && drop->Ready.insert(player->GetId()).second)
        {
            drop->Data.Replicas = static_cast<uint32_t>(drop->Ready.size());
            Broadcast(*drop, SharedDropAction::Upsert);
        }
        return;
    }
    if (request.Action != SharedDropAction::Move || !drop->Claim.CanMove(player->GetId(), drop->Data.Owner,
        request.Generation, drop->Data.Generation) || request.Physics.MotionType != 3 ||
        request.Physics.Id != GameId(SharedDropData::PhysicsModId, request.Id) || request.Tick <= drop->Data.Tick ||
        request.Tick > GameServer::Get()->GetTick() + 5000) return;
    const auto delta = request.Physics.Position - drop->Data.Physics.Position;
    const auto age = (std::min)(request.Tick - drop->Data.Tick, uint64_t{1000});
    const float maxStep = 512.f + static_cast<float>(age) * 8.f;
    if (glm::dot(delta, delta) > maxStep * maxStep || glm::length(request.Physics.LinearVelocity) > 200.f) return;
    drop->Data.Physics = request.Physics;
    drop->Data.Tick = request.Tick;
    for (auto* member : party->Members)
        if (member != player && Near(member, *drop) && drop->Ready.count(member->GetId()))
            Send(member, *drop, SharedDropAction::Move);
}

void SharedDropService::OnUpdate(const UpdateEvent&) noexcept
{
    const auto now = GameServer::Get()->GetTick();
    if (now < m_nextUpdate) return;
    m_nextUpdate = now + 250;
    for (auto entity : m_world.view<Drop>())
    {
        auto& drop = m_world.get<Drop>(entity);
        if (drop.Retired || drop.Claim.Taken) continue;
        const auto* party = m_world.GetPartyService().GetById(drop.Party);
        Player* next = nullptr;
        bool cellOccupied = false;
        if (party && party->StartEpoch == drop.Data.Epoch)
        {
            for (auto* player : party->Members)
            {
                if (!Near(player, drop)) continue;
                cellOccupied = true;
                if (drop.Seen.insert(player->GetId()).second) Send(player, drop, SharedDropAction::Upsert);
                if (drop.Ready.count(player->GetId()) && (!next || player->GetId() == party->LeaderPlayerId)) next = player;
            }
        }
        auto* owner = FindPlayer(drop.Data.Owner);
        if (party && party->StartEpoch == drop.Data.Epoch && Near(owner, drop)) continue;
        if (next)
        {
            drop.Data.Owner = next->GetId();
            ++drop.Data.Generation;
            // Clock samples from the old lease must not veto the new owner's first sample.
            drop.Data.Tick = 0;
            Broadcast(drop, SharedDropAction::Upsert);
            spdlog::info("Shared drop: {} ownership moved to {}", drop.Data.Id, next->GetUsername());
        }
        else if (!cellOccupied)
        {
            // Nobody has the cell: retain one normal native reference on the last owner's save.
            // Retain the tombstone so delayed movement/create requests cannot resurrect copies.
            drop.Retired = true;
            Broadcast(drop, SharedDropAction::Release);
        }
    }
}
