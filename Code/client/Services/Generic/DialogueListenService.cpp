#include <TiltedOnlinePCH.h>
#include <Services/Generic/DialogueListenService.h>
#include <World.h>
#include <Components.h>
#include <Events/UpdateEvent.h>
#include <Events/DisconnectedEvent.h>
#include <Events/SubtitleEvent.h>
#include <Messages/RequestDialogueListen.h>
#include <Messages/NotifyDialogueListen.h>
#include <Games/Misc/MenuTopicManager.h>
#include <PlayerCharacter.h>
#include <Interface/UI.h>
#include <cmath>

namespace DialogueListen
{
void Begin(uint32_t aNpcFormId, uint32_t aSpeakerPlayerId) noexcept
{
    World::Get().GetRunner().Queue([=] {
        World::Get().GetDialogueListenService().Begin(aNpcFormId, aSpeakerPlayerId);
    });
}
void End() noexcept
{
    World::Get().GetRunner().Queue([] { World::Get().GetDialogueListenService().End(); });
}
}

DialogueListenService::DialogueListenService(World& aWorld, entt::dispatcher& aDispatcher,
    TransportService& aTransport) noexcept
    : m_world(aWorld), m_transport(aTransport)
    , m_updateConnection(aDispatcher.sink<UpdateEvent>().connect<&DialogueListenService::OnUpdate>(this))
    , m_disconnectConnection(aDispatcher.sink<DisconnectedEvent>().connect<&DialogueListenService::OnDisconnected>(this))
    , m_stateConnection(aDispatcher.sink<NotifyDialogueListen>().connect<&DialogueListenService::OnState>(this))
    , m_subtitleConnection(aDispatcher.sink<SubtitleEvent>().connect<&DialogueListenService::OnSubtitle>(this))
{
}

uint32_t DialogueListenService::ServerId(uint32_t aFormId) const noexcept
{
    for (auto [entity, form] : m_world.view<FormIdComponent>().each())
        if (form.Id == aFormId)
        {
            if (auto* local = m_world.try_get<LocalComponent>(entity)) return local->Id;
            if (auto* remote = m_world.try_get<RemoteComponent>(entity)) return remote->Id;
        }
    return DialogueListenState::None;
}

bool DialogueListenService::PartyMember(uint32_t aPlayerId) const noexcept
{
    const auto& party = m_world.GetPartyService();
    const auto& members = party.GetPartyMembers();
    return party.IsInParty() && std::find(members.begin(), members.end(), aPlayerId) != members.end();
}

bool DialogueListenService::Near(uint32_t aFormId) const noexcept
{
    auto* player = PlayerCharacter::Get();
    auto* npc = Cast<Actor>(TESForm::GetById(aFormId));
    if (!player || !npc || !player->parentCell || !npc->parentCell || !npc->GetNiNode() ||
        player->IsInCombat() || npc->IsInCombat() || player->IsDead() || npc->IsDead()) return false;
    if (player->GetWorldSpace() != npc->GetWorldSpace() ||
        (!player->GetWorldSpace() && player->parentCell != npc->parentCell)) return false;
    const auto dx = player->position.x - npc->position.x;
    const auto dy = player->position.y - npc->position.y;
    const auto dz = player->position.z - npc->position.z;
    return dx * dx + dy * dy + dz * dz <= 400.f * 400.f;
}

void DialogueListenService::Begin(uint32_t aNpcFormId, uint32_t aSpeakerPlayerId) noexcept
{
    if (m_listenForm == aNpcFormId && m_speaker == aSpeakerPlayerId) return;
    End("another conversation");
    if (!m_transport.IsConnected() || !PartyMember(aSpeakerPlayerId) ||
        aSpeakerPlayerId == m_transport.GetLocalPlayerId() || !Near(aNpcFormId) ||
        m_world.GetPartyService().IsFollowerCinematicInputGated() ||
        m_world.GetOverlayService().GetActive()) return;
    if (auto* manager = MenuTopicManager::Get(); manager && manager->menuOpen) return;
    const auto id = ServerId(aNpcFormId);
    if (id == DialogueListenState::None) return;
    m_listenForm = aNpcFormId;
    m_speaker = aSpeakerPlayerId;
    m_epoch = m_world.GetPartyService().GetStartEpoch();
    m_waitUntil = GetTickCount64() + 3000;
    RequestDialogueListen request;
    request.Query = true;
    request.Speaker = aSpeakerPlayerId;
    request.State.Epoch = m_epoch;
    request.State.NpcServerId = id;
    m_transport.Send(request);
}

void DialogueListenService::End(const char* aReason) noexcept
{
    if (!m_listenForm) return;
    m_presentation.End();
    m_listenForm = 0;
    m_listening = false;
    m_waitUntil = 0;
    m_received = {};
    m_world.GetOverlayService().PushDialogueListen(nullptr, 0);
    spdlog::info("Dialogue listen: ended ({})", aReason);
}

void DialogueListenService::OnDisconnected(const DisconnectedEvent&) noexcept
{
    End("disconnect");
    m_sent = {};
    m_subtitle.clear();
    m_subtitleForm = 0;
    DialogueListenNative::Reset();
}

void DialogueListenService::OnSubtitle(const SubtitleEvent& aEvent) noexcept
{
    auto* actor = Cast<Actor>(TESForm::GetById(aEvent.SpeakerID));
    if (!actor || !MenuTopicManager::IsPlayerDialogueSpeaker(actor)) return;
    m_subtitleForm = aEvent.SpeakerID;
    m_subtitle = aEvent.Text.substr(0, 4096);
    // Actor::UpdateVoice 37544 / 14068E150 fills fVoiceTimer from the sound's
    // duration, or native silent-line timing, before SubtitleManager::Show.
    const auto seconds = actor->fVoiceTimer;
    m_duration = std::isfinite(seconds) && seconds > 0 ?
        static_cast<uint32_t>((std::min)(seconds * 1000.f, 300000.f)) : 0;
    m_lineTick = m_world.GetTick();
    ++m_lineSerial;
    m_hideSerial = DialogueListenNative::SubtitleHideSerial(m_subtitleForm);
}

void DialogueListenService::PushUI() noexcept
{
    auto state = m_received;
    const auto now = m_world.GetTick();
    if (state.DurationMs && now > state.LineStartedTick)
    {
        const auto elapsed = now - state.LineStartedTick;
        state.DurationMs = elapsed >= state.DurationMs ? 0 : state.DurationMs - static_cast<uint32_t>(elapsed);
        if (!state.DurationMs) state.Subtitle.clear();
    }
    m_world.GetOverlayService().PushDialogueListen(&state, m_speaker);
}

void DialogueListenService::OnState(const NotifyDialogueListen& aMessage) noexcept
{
    if (!aMessage.IsValid() || !aMessage.State.Valid() || !m_listenForm ||
        aMessage.Speaker != m_speaker || aMessage.State.Epoch != m_epoch ||
        !PartyMember(m_speaker)) return;
    const auto& state = aMessage.State;
    if (m_received.Session && (state.Session < m_received.Session ||
        (state.Session == m_received.Session && state.Revision < m_received.Revision))) return;
    if (state.NpcServerId != ServerId(m_listenForm))
    {
        if (m_listening) End("speaker changed target");
        return;
    }
    if (!state.Active) { End("speaker closed dialogue"); return; }
    if (m_received.Session && state.Session != m_received.Session) { End("conversation replaced"); return; }
    if (!Near(m_listenForm)) { End("distance or combat"); return; }
    if (!m_listening)
    {
        if (!m_presentation.Begin(Cast<Actor>(TESForm::GetById(m_listenForm))))
        { End("camera unavailable"); return; }
        m_listening = true;
        m_waitUntil = 0;
        spdlog::info("Dialogue listen: {} listening to {} with {:X}",
            m_transport.GetLocalPlayerId(), m_speaker, m_listenForm);
    }
    m_received = state;
    PushUI();
}

void DialogueListenService::OnUpdate(const UpdateEvent&) noexcept
{
    DialogueListenNative::ObserveMenu();
    const auto& party = m_world.GetPartyService();
    if (!m_transport.IsConnected() || !party.IsInParty())
    { End("disconnected or party left"); m_sent = {}; return; }
    auto observation = DialogueListenNative::Read();
    auto& state = observation.State;
    state.Epoch = party.GetStartEpoch();
    auto* player = PlayerCharacter::Get();
    auto* npc = Cast<Actor>(TESForm::GetById(observation.NpcFormId));
    state.Active = state.Active && player && npc && !player->IsInCombat() && !npc->IsInCombat() &&
        !player->IsDead() && !npc->IsDead();
    if (state.Active)
    {
        state.NpcServerId = ServerId(observation.NpcFormId);
        m_world.GetModSystem().GetServerModId(observation.NpcFormId, state.Npc);
        state.Active = state.NpcServerId != DialogueListenState::None;
    }
    if (state.Active)
    {
        if (!m_sent.Active || state.NpcServerId != m_sent.NpcServerId || state.Epoch != m_sent.Epoch)
            ++m_session;
        state.Session = m_session;
        if (m_subtitleForm == observation.NpcFormId)
        {
            const auto hidden = DialogueListenNative::SubtitleHideSerial(m_subtitleForm);
            if (hidden != m_hideSerial) { m_subtitle.clear(); m_duration = 0; m_hideSerial = hidden; }
            state.Subtitle = m_subtitle;
            state.DurationMs = m_duration;
            state.LineSerial = m_lineSerial;
            state.LineStartedTick = m_lineTick;
        }
        state.Revision = m_sent.Revision;
        if (!(state == m_sent) && state.Valid())
        {
            state.Revision = ++m_revision;
            RequestDialogueListen request;
            request.State = state;
            if (m_transport.Send(request))
            {
                m_sent = state;
                spdlog::info("Dialogue listen: state sent ({} topics)", state.Topics.size());
            }
        }
    }
    else if (m_sent.Active)
    {
        RequestDialogueListen request;
        request.State = m_sent;
        request.State.Active = false;
        request.State.Revision = ++m_revision;
        m_transport.Send(request);
        m_sent = {};
        m_subtitle.clear();
        m_subtitleForm = 0;
    }
    if (!m_listenForm) return;
    if (party.GetStartEpoch() != m_epoch || !PartyMember(m_speaker)) { End("party changed"); return; }
    if (party.IsFollowerCinematicInputGated() || m_world.GetOverlayService().GetActive())
    { End("cinematic controls or overlay"); return; }
    if (!Near(m_listenForm)) { End("distance, unload, death or combat"); return; }
    if (DialogueListenNative::Presentation::ExitPressed()) { End("Tab/B"); return; }
    if (auto* ui = UI::Get())
    {
        static BSFixedString loading("Loading Menu"), dialogue("Dialogue Menu"), creator("RaceSex Menu");
        if (ui->GetMenuOpen(loading) || ui->GetMenuOpen(dialogue) || ui->GetMenuOpen(creator))
        { End("local menu"); return; }
    }
    if (m_waitUntil && GetTickCount64() >= m_waitUntil) { End("no active conversation"); return; }
    if (m_listening && !m_presentation.Update(Cast<Actor>(TESForm::GetById(m_listenForm)), m_received.Fov))
        End("camera or controls changed");
}
