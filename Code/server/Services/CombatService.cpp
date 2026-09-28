#include <Services/CombatService.h>
#include <Components.h>
#include <GameServer.h>
#include <World.h>

#include <Messages/ProjectileLaunchRequest.h>
#include <Messages/NotifyProjectileLaunch.h>
#include <Messages/RequestPlayerCombatState.h>
#include <Messages/NotifyPlayerCombatState.h>

namespace
{
struct PlayerCombatRelayClock
{
    struct Clock
    {
        uint64_t Epoch{};
        uint64_t Sequence{};
        uint32_t OwnerEpoch{};
        uint32_t Sender{};
    };
    std::array<Clock, 4> Streams;
};

void RelayPlayerCombat(World& aWorld, const PacketEvent<RequestPlayerCombatState>& aPacket)
{
    auto* sender = aPacket.pPlayer;
    const auto& state = aPacket.Packet.State;
    if (!sender || !state.IsValid())
        return;
    auto& parties = aWorld.GetPartyService();
    auto* party = parties.GetPlayerParty(sender);
    if (!party || party->SessionState < 2 || party->StartEpoch != state.Epoch)
        return;
    Player* leader = nullptr;
    for (auto* member : party->Members)
        if (member && member->GetId() == party->LeaderPlayerId)
            leader = member;
    if (!leader)
        return;
    const auto actor = static_cast<entt::entity>(state.ActorId);
    const auto* owner = aWorld.try_get<OwnerComponent>(actor);
    const auto* character = aWorld.try_get<CharacterComponent>(actor);
    if (!owner || !character || owner->OwnershipEpoch != state.OwnershipEpoch)
        return;
    Player* recipient = nullptr;
    if (state.Type == PlayerCombatState::Stealth)
    {
        if (sender == leader || sender->GetCharacter() != actor || !owner->IsCurrentOwner(sender, state.OwnershipEpoch))
            return;
        recipient = leader;
    }
    else if (state.Type == PlayerCombatState::Detection)
    {
        recipient = owner->GetOwner();
        if (sender != leader || !recipient || recipient == leader || recipient->GetCharacter() != actor)
            return;
    }
    else
    {
        const auto target = static_cast<entt::entity>(state.TargetId);
        const auto* targetOwner = aWorld.try_get<OwnerComponent>(target);
        const auto* targetCharacter = aWorld.try_get<CharacterComponent>(target);
        const auto* cell = aWorld.try_get<CellIdComponent>(actor);
        const auto* targetCell = aWorld.try_get<CellIdComponent>(target);
        if (!targetOwner || !targetCharacter || targetOwner->OwnershipEpoch != state.TargetOwnershipEpoch ||
            !cell || !targetCell || !cell->IsInRange(*targetCell, character->IsDragon()))
            return;
        if (state.Type == PlayerCombatState::Damage)
        {
            recipient = targetOwner->GetOwner();
            if (sender != leader || !owner->IsCurrentOwner(sender, state.OwnershipEpoch) || character->IsPlayer() ||
                !recipient || recipient == leader || recipient->GetCharacter() != target)
                return;
        }
        else
        {
            if (sender == leader || sender->GetCharacter() != actor || !owner->IsCurrentOwner(sender, state.OwnershipEpoch) ||
                !targetOwner->IsCurrentOwner(leader, state.TargetOwnershipEpoch) || targetCharacter->IsPlayer())
                return;
            recipient = leader;
        }
    }
    if (std::find(party->Members.begin(), party->Members.end(), recipient) == party->Members.end())
        return;
    auto& clock = aWorld.get_or_emplace<PlayerCombatRelayClock>(actor).Streams[state.Type];
    if (clock.Epoch == state.Epoch && clock.OwnerEpoch == state.OwnershipEpoch &&
        clock.Sender == sender->GetId() && state.Sequence <= clock.Sequence)
        return;
    clock = {state.Epoch, state.Sequence, state.OwnershipEpoch, sender->GetId()};
    NotifyPlayerCombatState notify;
    notify.State = state;
    recipient->Send(notify);
}
}

CombatService::CombatService(World& aWorld, entt::dispatcher& aDispatcher) noexcept
    : m_world(aWorld)
{
    m_projectileLaunchConnection = aDispatcher.sink<PacketEvent<ProjectileLaunchRequest>>().connect<&CombatService::OnProjectileLaunchRequest>(this);
    // World and dispatcher share lifetime; no process-global relay state survives a world reset.
    aDispatcher.sink<PacketEvent<RequestPlayerCombatState>>().connect<&RelayPlayerCombat>(aWorld);
}

void CombatService::OnProjectileLaunchRequest(const PacketEvent<ProjectileLaunchRequest>& acMessage) const noexcept
{
    auto& packet = acMessage.Packet;

    NotifyProjectileLaunch notify{};

    notify.ShooterID = packet.ShooterID;

    notify.OriginX = packet.OriginX;
    notify.OriginY = packet.OriginY;
    notify.OriginZ = packet.OriginZ;

    notify.ProjectileBaseID = packet.ProjectileBaseID;
    notify.WeaponID = packet.WeaponID;
    notify.AmmoID = packet.AmmoID;

    notify.ZAngle = packet.ZAngle;
    notify.XAngle = packet.XAngle;
    notify.YAngle = packet.YAngle;

    notify.ParentCellID = packet.ParentCellID;

    notify.SpellID = packet.SpellID;
    notify.CastingSource = packet.CastingSource;

    notify.Area = packet.Area;
    notify.Power = packet.Power;
    notify.Scale = packet.Scale;

    notify.AlwaysHit = packet.AlwaysHit;
    notify.NoDamageOutsideCombat = packet.NoDamageOutsideCombat;
    notify.AutoAim = packet.AutoAim;
    notify.DeferInitialization = packet.DeferInitialization;
    notify.ForceConeOfFire = packet.ForceConeOfFire;

    notify.UnkBool1 = packet.UnkBool1;
    notify.UnkBool2 = packet.UnkBool2;

    // Shooter 0: a world caster (non-actor temporary reference) launched by the leader; relay around its character.
    const auto senderCharacter = acMessage.pPlayer ? acMessage.pPlayer->GetCharacter() : std::nullopt;
    const auto cShooterEntity = packet.ShooterID == 0 && senderCharacter ? *senderCharacter : static_cast<entt::entity>(packet.ShooterID);
    if (!GameServer::Get()->SendToPlayersInRange(notify, cShooterEntity, acMessage.GetSender()))
        spdlog::error("{}: SendToPlayersInRange failed", __FUNCTION__);
}
