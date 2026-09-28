#include <Services/Generic/StealthService.h>
#include <World.h>
#include <Components.h>
#include <Games/ActorExtension.h>
#include <Actor.h>
#include <PlayerCharacter.h>
#include <AI/AIProcess.h>
#include <Misc/ActorValueOwner.h>
#include <Combat/CombatController.h>
#include <Events/UpdateEvent.h>
#include <Events/DisconnectedEvent.h>
#include <Messages/RequestPlayerCombatState.h>
#include <Messages/NotifyPlayerCombatState.h>

StealthService::StealthService(World& aWorld, entt::dispatcher& aDispatcher, TransportService& aTransport)
    : m_world(aWorld), m_transport(aTransport)
    , m_update(aDispatcher.sink<UpdateEvent>().connect<&StealthService::OnUpdate>(this))
    , m_disconnect(aDispatcher.sink<DisconnectedEvent>().connect<&StealthService::OnDisconnected>(this))
    , m_notify(aDispatcher.sink<NotifyPlayerCombatState>().connect<&StealthService::OnNotify>(this))
    , m_impact(aDispatcher.sink<PlayerCombat::Impact>().connect<&StealthService::OnImpact>(this))
{
}

StealthService::~StealthService() { PlayerCombat::Clear(); }

bool StealthService::Active() const
{
    const auto& party = m_world.GetPartyService();
    return m_transport.IsConnected() && party.IsInParty() && party.GetStartEpoch() && party.GetSessionState() >= 2;
}

void StealthService::Reset()
{
    for (auto& [id, track] : m_tracks)
    {
        if (auto* actor = Cast<Actor>(TESObjectREFR::GetByHandle(track.Handle)); actor && actor->GetExtension()->IsRemotePlayer())
            PlayerCombat::SetTeammate(actor, track.WasTeammate);
    }
    m_tracks.clear();
    m_received.clear();
    PlayerCombat::Clear();
    m_epoch = 0;
    m_leader = 0;
    m_nextUpdate = 0;
}

void StealthService::OnDisconnected(const DisconnectedEvent&) { Reset(); }

void StealthService::Send(PlayerCombatState aState)
{
    aState.Epoch = m_world.GetPartyService().GetStartEpoch();
    aState.Sequence = ++m_sequence;
    if (!aState.IsValid())
        return;
    RequestPlayerCombatState request;
    request.State = std::move(aState);
    m_transport.Send(request);
}

void StealthService::Apply(Actor* apActor, const PlayerCombatState& aState)
{
    // Change only the native sneak bit; movement/life/weapon state belongs to other systems.
    constexpr uint32_t sneakBit = 0x200;
    apActor->actorState.flags1 = (apActor->actorState.flags1 & ~sneakBit) | (aState.Sneaking ? sneakBit : 0);
    static BSFixedString isSneaking("isSneaking");
    apActor->animationGraphHolder.SetVariableBool(&isSneaking, aState.Sneaking);
    for (size_t i = 0; i < aState.Values.size(); ++i)
    {
        const auto value = PlayerCombatState::ActorValues[i];
        if (apActor->GetActorValue(value) != aState.Values[i])
            apActor->ForceActorValue(ActorValueOwner::ForceMode::DAMAGE, value, aState.Values[i]);
    }
    PlayerCombat::Publish(apActor, aState);
}

void StealthService::OnUpdate(const UpdateEvent&)
{
    if (!Active())
    {
        if (m_epoch)
            Reset();
        return;
    }
    const auto& party = m_world.GetPartyService();
    if (m_epoch != party.GetStartEpoch() || m_leader != party.GetLeaderPlayerId())
    {
        Reset();
        m_epoch = party.GetStartEpoch();
        m_leader = party.GetLeaderPlayerId();
    }
    const auto now = GetTickCount64();
    if (now < m_nextUpdate)
        return;
    m_nextUpdate = now + 200;

    if (!party.IsLeader())
    {
        auto* player = PlayerCharacter::Get();
        if (player && player->GetNiNode())
        {
            const auto token = Utils::GetLocalOwnershipTokenOnRunner(player->formID);
            if (token)
            {
                PlayerCombatState state;
                state.ActorId = token->ServerId;
                state.OwnershipEpoch = token->OwnershipEpoch;
                if (PlayerCombat::Capture(player, state))
                    Send(std::move(state));
            }
        }
        return;
    }

    // One observer snapshot per host tick; preserve track and native query order.
    std::optional<PlayerCombat::ObserverSnapshot> observers;
    for (auto it = m_tracks.begin(); it != m_tracks.end();)
    {
        auto& track = it->second;
        auto* actor = Utils::GetByServerIdOnRunner<Actor>(it->first);
        const auto token = actor ? Utils::GetRemoteOwnershipTokenOnRunner(actor->formID) : std::nullopt;
        if (!actor || !actor->GetExtension()->IsRemotePlayer() || !token ||
            token->OwnershipEpoch != track.State.OwnershipEpoch || now - track.ReceivedAt >= 2000)
        {
            if (auto* old = Cast<Actor>(TESObjectREFR::GetByHandle(track.Handle)); old && old->GetExtension()->IsRemotePlayer())
                PlayerCombat::SetTeammate(old, track.WasTeammate);
            PlayerCombat::Forget(track.FormId);
            it = m_tracks.erase(it);
            continue;
        }
        if (actor->currentProcess && actor->GetNiNode())
        {
            const auto handle = actor->GetHandle().handle.iBits;
            if (track.Handle != handle)
            {
                if (auto* old = Cast<Actor>(TESObjectREFR::GetByHandle(track.Handle)); old && old->GetExtension()->IsRemotePlayer())
                    PlayerCombat::SetTeammate(old, track.WasTeammate);
                PlayerCombat::Forget(track.FormId);
                track.FormId = actor->formID;
                track.Handle = handle;
                track.WasTeammate = (actor->flags1 & (1u << 26)) != 0;
                PlayerCombat::SetTeammate(actor, true);
                spdlog::info("Player combat registered copy={:X} server={} epoch={}", actor->formID, it->first, track.State.OwnershipEpoch);
            }
            Apply(actor, track.State);
            PlayerCombatState result;
            result.Type = PlayerCombatState::Detection;
            result.ActorId = it->first;
            result.OwnershipEpoch = token->OwnershipEpoch;
            if (!observers)
                observers.emplace();
            PlayerCombat::GetAwareness(actor, result.DetectionLevel, result.LOSCount, observers->Get());
            result.MeterLevel = PlayerCombat::GetMeterLevel(actor, result.DetectionLevel);
            if (now >= track.NextDiagnostic)
            {
                track.NextDiagnostic = now + 5000;
                spdlog::info("Player stealth server={} copy={:X} sneak={} moving={} running={} light={} skill={} noise={} invisible={} entries={} detected={} eye={}",
                    it->first, actor->formID, track.State.Sneaking, track.State.Moving, track.State.Running,
                    track.State.Light, track.State.Values[0], track.State.Values[2], track.State.Values[1],
                    track.State.Perks.size(), result.DetectionLevel, result.MeterLevel);
            }
            Send(std::move(result));
        }
        ++it;
    }
}

void StealthService::OnNotify(const NotifyPlayerCombatState& aMessage)
{
    const auto& state = aMessage.State;
    if (!Active() || !state.IsValid() || state.Epoch != m_world.GetPartyService().GetStartEpoch())
        return;
    // A notification can precede the next UpdateEvent after a session/leader change.
    if (m_epoch != state.Epoch || m_leader != m_world.GetPartyService().GetLeaderPlayerId())
    {
        Reset();
        m_epoch = state.Epoch;
        m_leader = m_world.GetPartyService().GetLeaderPlayerId();
    }
    const uint64_t key = (uint64_t{state.ActorId} << 2) | state.Type;
    auto& received = m_received[key];
    if (received.OwnershipEpoch == state.OwnershipEpoch && state.Sequence <= received.Sequence)
        return;
    received = {state.OwnershipEpoch, state.Sequence};
    const bool leader = m_world.GetPartyService().IsLeader();
    if (state.Type == PlayerCombatState::Stealth && leader)
    {
        auto& track = m_tracks[state.ActorId];
        track.State = state;
        track.ReceivedAt = GetTickCount64();
    }
    else if (state.Type == PlayerCombatState::Detection && !leader)
    {
        const auto token = Utils::GetLocalOwnershipTokenOnRunner(0x14);
        if (token && token->ServerId == state.ActorId && token->OwnershipEpoch == state.OwnershipEpoch)
            PlayerCombat::SetMeter(state.MeterLevel, state.LOSCount);
    }
    else if (state.Type == PlayerCombatState::Damage && !leader)
    {
        const auto token = Utils::GetLocalOwnershipTokenOnRunner(0x14);
        if (!token || token->ServerId != state.TargetId || token->OwnershipEpoch != state.TargetOwnershipEpoch)
            return;
        auto* attacker = Utils::GetByServerIdOnRunner<Actor>(state.ActorId);
        const auto sourceEntity = Utils::FindEntityByServerIdOnRunner(state.ActorId);
        const auto* source = sourceEntity ? m_world.try_get<RemoteComponent>(*sourceEntity) : nullptr;
        if (!source || source->OwnershipEpoch != state.OwnershipEpoch)
            return;
        PlayerCombat::ApplyDamage(PlayerCharacter::Get(), attacker, state.RawDamage, state.KillMove);
        spdlog::debug("Player combat damage seq={} attacker={} victim={} raw={}", state.Sequence, state.ActorId, state.TargetId, state.RawDamage);
    }
    else if (state.Type == PlayerCombatState::Threat && leader)
    {
        auto* attacker = Utils::GetByServerIdOnRunner<Actor>(state.ActorId);
        auto* victim = Utils::GetByServerIdOnRunner<Actor>(state.TargetId);
        const auto victimToken = victim ? Utils::GetLocalOwnershipTokenOnRunner(victim->formID) : std::nullopt;
        const auto attackerToken = attacker ? Utils::GetRemoteOwnershipTokenOnRunner(attacker->formID) : std::nullopt;
        if (attacker && victim && attackerToken && victimToken && attacker->GetExtension()->IsRemotePlayer() &&
            !victim->GetExtension()->IsPlayer() && !victim->IsDead() &&
            attackerToken->OwnershipEpoch == state.OwnershipEpoch && victimToken->OwnershipEpoch == state.TargetOwnershipEpoch)
        {
            // StartCombat adds the aggressor through native combat/group bookkeeping. Do not stop
            // existing combat or overwrite targetHandle: the engine chooses among all opponents.
            victim->StartCombat(attacker);
            if (victim->pCombatController)
            {
                // ID 33224 is the bookkeeping called by native DoDamage before its health write.
                using NotifyDamage = void(CombatController*, Actor*, float);
                POINTER_SKYRIMSE(NotifyDamage, notifyDamage, 33224);
                notifyDamage(victim->pCombatController, attacker, state.RawDamage);
            }
            spdlog::debug("Player combat threat seq={} attacker={} victim={}", state.Sequence, state.ActorId, state.TargetId);
        }
    }
}

void StealthService::OnImpact(const PlayerCombat::Impact& aImpact)
{
    if (!Active() || aImpact.SessionEpoch != m_world.GetPartyService().GetStartEpoch())
        return;
    auto* attacker = Cast<Actor>(TESForm::GetById(aImpact.AttackerFormId));
    auto* victim = Cast<Actor>(TESForm::GetById(aImpact.VictimFormId));
    if (!attacker || !victim)
        return;
    const auto source = Utils::GetLocalOwnershipTokenOnRunner(attacker->formID);
    const auto target = Utils::GetRemoteOwnershipTokenOnRunner(victim->formID);
    if (!source || !target)
        return;
    const bool leader = m_world.GetPartyService().IsLeader();
    if (aImpact.ThreatOnly ? (leader || !attacker->GetExtension()->IsLocalPlayer() || victim->GetExtension()->IsPlayer()) :
        (!leader || attacker->GetExtension()->IsPlayer() || !victim->GetExtension()->IsRemotePlayer()))
        return;
    PlayerCombatState state;
    state.Type = aImpact.ThreatOnly ? PlayerCombatState::Threat : PlayerCombatState::Damage;
    state.ActorId = source->ServerId;
    state.OwnershipEpoch = source->OwnershipEpoch;
    state.TargetId = target->ServerId;
    state.TargetOwnershipEpoch = target->OwnershipEpoch;
    state.RawDamage = aImpact.Damage;
    state.KillMove = aImpact.KillMove;
    Send(std::move(state));
}
