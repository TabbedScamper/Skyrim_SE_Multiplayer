#include <Services/DropInService.h>

#include <World.h>
#include <Events/UpdateEvent.h>
#include <Messages/DropIn.h>
#include <Services/CharacterSnapshots.h>
#include <Services/CheckpointSaves.h>
#include <Services/PartyService.h>
#include <Services/TransportService.h>
#include <Games/References.h>
#include <PlayerCharacter.h>
#include <Forms/TESObjectCELL.h>
#include <Interface/UI.h>
#include <Services/OverlayService.h>
#include <OverlayApp.hpp>

#include <fstream>
#include <random>

namespace
{
uint64_t Fnv(const std::string& acBytes) noexcept
{
    uint64_t hash = 14695981039346656037ULL;
    for (const char c : acBytes)
        hash = (hash ^ static_cast<uint8_t>(c)) * 1099511628211ULL;
    return hash;
}

// 16 chunks of 16 KiB per update: a 3 MB save crosses in about 12 frames over the reliable channel.
constexpr int kChunksPerUpdate = 16;

bool PlayerInWorld() noexcept
{
    auto* pPlayer = PlayerCharacter::Get();
    auto* pUi = UI::Get();
    return pPlayer && pPlayer->parentCell && pPlayer->GetNiNode() && pUi &&
           !pUi->GetMenuOpen(BSFixedString("Loading Menu")) && !pUi->GetMenuOpen(BSFixedString("Main Menu"));
}
} // namespace

DropInService::DropInService(World& aWorld, entt::dispatcher& aDispatcher, TransportService& aTransport) noexcept
    : m_world(aWorld)
    , m_transport(aTransport)
{
    m_updateConnection = aDispatcher.sink<UpdateEvent>().connect<&DropInService::OnUpdate>(this);
    m_messageConnection = aDispatcher.sink<NotifyDropIn>().connect<&DropInService::OnMessage>(this);
}

std::string DropInService::Status() const noexcept
{
    return m_status;
}

void DropInService::SendStep(uint8_t aOp, const std::string& acText) noexcept
{
    RequestDropIn request;
    request.Attempt = m_attempt;
    request.Joiner = m_joiner;
    request.Op = static_cast<DropInOp>(aOp);
    request.Text = acText.substr(0, DropInData::MaxText).c_str();
    m_transport.Send(request);
}

void DropInService::Fail(const std::string& acReason, bool aTellServer) noexcept
{
    spdlog::warn("Drop-in {:X}: {}", m_attempt, acReason);
    if (aTellServer && m_attempt)
        SendStep(static_cast<uint8_t>(DropInOp::Abort), acReason);
    if (m_phase == Phase::Apply || m_phase == Phase::WaitAdmit || m_phase == Phase::Load)
    {
        FadeOutGame(false, true, 1.f, true, 0.f);
        // Past the title screen the lobby panel that shows dropInStatus is gone: say it in the chat.
        m_world.GetOverlayService().SendSystemMessage(fmt::format("Joining failed: {}.", acReason));
    }
    m_phase = Phase::Idle;
    m_file.clear();
    m_status = "failed: " + acReason;
}

std::string DropInService::StartJoin(const std::string& acSnapshotPath) noexcept
{
    const auto& party = m_world.GetPartyService();
    if (!m_transport.IsConnected() || !party.IsInParty())
        return "not connected to a party";
    if (party.IsLeader())
        return "the host is already playing";
    if (party.GetSessionState() != 3)
        return "the session is not running yet";
    if (m_phase != Phase::Idle)
        return "already joining";
    if (!CharacterSnapshots::ReadFile(acSnapshotPath, m_character))
        return "that character could not be read";
    std::random_device random;
    m_attempt = (static_cast<uint64_t>(random()) << 32) | random() | 1;
    m_joiner = 0;
    m_file.clear();
    m_phase = Phase::WaitSave;
    m_since = GetTickCount64();
    m_status = "waiting for the host's save";
    SendStep(static_cast<uint8_t>(DropInOp::Join), m_character.Name.c_str());
    spdlog::info("Drop-in {:X}: joining as {} (level {})", m_attempt, m_character.Name.c_str(), m_character.Level);
    return {};
}

void DropInService::OnMessage(const NotifyDropIn& acMessage) noexcept
{
    if (acMessage.Op == DropInOp::Capture)
    {
        // Leader: a fresh save of this world for the joiner, through the checkpoint path (never Save_Impl from here).
        if (m_phase != Phase::Idle && m_phase != Phase::Capture && m_phase != Phase::Stream)
            return;
        m_attempt = acMessage.Attempt;
        m_joiner = acMessage.Joiner;
        m_checkpoint = fmt::format("dropin_{:X}", static_cast<uint32_t>(m_attempt));
        m_phase = Phase::Capture;
        m_since = GetTickCount64();
        m_status = "saving for a joining player";
        CheckpointSaves::Begin(m_checkpoint.c_str());
        spdlog::info("Drop-in {:X}: {} joins; saving {}", m_attempt, acMessage.Text.c_str(), m_checkpoint);
        return;
    }
    if (acMessage.Attempt != m_attempt)
        return;
    switch (acMessage.Op)
    {
    case DropInOp::Chunk:
        if (m_phase != Phase::WaitSave && m_phase != Phase::Receive)
            return;
        if (acMessage.Offset != m_file.size())
        {
            Fail("a piece of the save arrived out of order", true);
            return;
        }
        m_phase = Phase::Receive;
        m_file.append(acMessage.Bytes.data(), acMessage.Bytes.size());
        return;
    case DropInOp::Done:
    {
        if (m_phase != Phase::Receive && m_phase != Phase::WaitSave)
            return;
        if (m_file.size() != acMessage.Total || Fnv(m_file) != acMessage.Hash)
        {
            Fail("the host's save arrived damaged", true);
            return;
        }
        // Named here from our own attempt id, never from the message text (a hostile server could send "..\").
        m_checkpoint = fmt::format("dropin_{:X}", static_cast<uint32_t>(m_attempt));
        if (!CheckpointSaves::WriteReceived(m_checkpoint.c_str(), m_file))
        {
            Fail("the host's save could not be written", true);
            return;
        }
        m_file.clear();
        m_phase = Phase::Load;
        m_since = GetTickCount64();
        m_status = "loading";
        CheckpointSaves::QueueLoad(m_checkpoint.c_str());
        spdlog::info("Drop-in {:X}: received the host's save ({} bytes), loading", m_attempt, acMessage.Total);
        return;
    }
    case DropInOp::Admit:
        if (m_phase != Phase::WaitAdmit)
            return;
        FadeOutGame(false, true, 1.f, true, 0.f);
        m_phase = Phase::Idle;
        m_status = "joined";
        spdlog::info("Drop-in {:X}: admitted", m_attempt);
        return;
    case DropInOp::Abort:
        if (m_phase != Phase::Idle)
            Fail(acMessage.Text.empty() ? "cancelled" : acMessage.Text.c_str(), false);
        return;
    default:
        return;
    }
}

void DropInService::OnUpdate(const UpdateEvent&) noexcept
{
    // The lobby shows the join's progress.
    if (m_status != m_pushedStatus)
    {
        m_pushedStatus = m_status;
        auto arguments = CefListValue::Create();
        arguments->SetString(0, m_status);
        if (auto* pApp = m_world.GetOverlayService().GetOverlayApp())
            pApp->ExecuteAsync("dropInStatus", arguments);
    }
    if (m_phase == Phase::Idle)
        return;
    const auto now = GetTickCount64();
    switch (m_phase)
    {
    case Phase::Capture:
    {
        if (now - m_since > 90000)
        {
            Fail("the host's save did not finish", true);
            return;
        }
        const auto path = CheckpointSaves::PathOf(m_checkpoint.c_str());
        std::error_code error;
        if (CheckpointSaves::IsPending() || !std::filesystem::exists(path, error))
            return;
        std::ifstream file(path, std::ios::binary);
        m_file.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
        if (m_file.empty() || m_file.size() > DropInData::MaxFile)
        {
            Fail("the save could not be read", true);
            return;
        }
        m_sent = 0;
        m_phase = Phase::Stream;
        m_status = "sending the world to a joining player";
        return;
    }
    case Phase::Stream:
    {
        for (int i = 0; i < kChunksPerUpdate && m_sent < m_file.size(); ++i)
        {
            RequestDropIn chunk;
            chunk.Attempt = m_attempt;
            chunk.Joiner = m_joiner;
            chunk.Op = DropInOp::Chunk;
            chunk.Offset = m_sent;
            const auto size = (std::min)(DropInData::MaxChunk, m_file.size() - static_cast<size_t>(m_sent));
            chunk.Bytes.assign(m_file.data() + m_sent, size);
            m_transport.Send(chunk);
            m_sent += size;
        }
        if (m_sent < m_file.size())
            return;
        RequestDropIn done;
        done.Attempt = m_attempt;
        done.Joiner = m_joiner;
        done.Op = DropInOp::Done;
        done.Text = m_checkpoint.c_str();
        done.Total = m_file.size();
        done.Hash = Fnv(m_file);
        m_transport.Send(done);
        spdlog::info("Drop-in {:X}: sent {} bytes of {} to player {}", m_attempt, m_file.size(), m_checkpoint, m_joiner);
        m_file.clear();
        m_phase = Phase::Idle;
        m_status = "idle";
        return;
    }
    case Phase::WaitSave:
    case Phase::Receive:
        if (now - m_since > 90000)
            Fail("the host's save did not arrive", true);
        else if (m_phase == Phase::Receive)
            m_status = fmt::format("receiving the host's world ({} KB)", m_file.size() / 1024);
        return;
    case Phase::Load:
        if (CheckpointSaves::QueuedLoadState() < 0)
        {
            Fail("the host's world could not be loaded on this PC (see the log)", true);
            return;
        }
        if (now - m_since > 120000)
        {
            Fail("the host's world did not load", true);
            return;
        }
        // Black from the first loaded frame (the save shows the host's character until ours goes in), then a moment
        // to settle before the character is applied.
        if (!PlayerInWorld())
            return;
        FadeOutGame(true, true, 0.f, true, 0.f);
        if (!m_loadedAt)
            m_loadedAt = now;
        if (now - m_loadedAt < 3000)
            return;
        m_loadedAt = 0;
        CharacterSnapshots::QueueApply(m_character);
        m_phase = Phase::Apply;
        m_since = now;
        m_status = "applying your character";
        return;
    case Phase::Apply:
    {
        FadeOutGame(true, true, 0.f, true, 0.f);
        const auto status = CharacterSnapshots::ApplyStatus();
        if (status.rfind("failed", 0) == 0)
        {
            Fail("your character could not be applied (" + status + ")", true);
            return;
        }
        if (status.rfind("done", 0) != 0)
        {
            if (now - m_since > 30000)
                Fail("applying your character took too long", true);
            return;
        }
        // The save puts this player where the host stood: step aside so the two do not overlap. Each attempt picks one
        // of eight directions, so several joiners do not all land on the same spot.
        if (auto* pPlayer = PlayerCharacter::Get(); pPlayer && pPlayer->parentCell)
        {
            const float angle = static_cast<float>((m_attempt >> 1) % 8) * 0.785398f;
            NiPoint3 aside = pPlayer->position;
            aside.x += 120.f * std::cos(angle);
            aside.y += 120.f * std::sin(angle);
            pPlayer->MoveTo(pPlayer->parentCell, aside);
        }
        SendStep(static_cast<uint8_t>(DropInOp::Loaded));
        m_phase = Phase::WaitAdmit;
        m_since = now;
        m_status = "joining";
        return;
    }
    case Phase::WaitAdmit:
        FadeOutGame(true, true, 0.f, true, 0.f);
        if (now - m_since > 30000)
            Fail("the server did not admit this player", true);
        return;
    default:
        return;
    }
}
