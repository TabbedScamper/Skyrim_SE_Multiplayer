#include <Services/DropInService.h>

#include <World.h>
#include <Events/UpdateEvent.h>
#include <Messages/DropIn.h>
#include <Services/CharacterSnapshots.h>
#include <Services/CheckpointSaves.h>
#include <Services/PartyService.h>
#include <Services/TransportService.h>
#include <Services/SteamLobbyService.h>
#include <Services/DoorVoteService.h>
#include <Games/References.h>
#include <PlayerCharacter.h>
#include <Forms/TESObjectCELL.h>
#include <Forms/TESWorldSpace.h>
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

// No cell loading here. A roster never initiates individual travel through the party door barrier.
bool SameGrid(const NiPoint3& a, const NiPoint3& b) noexcept
{
    return std::floor(a.x / 4096.f) == std::floor(b.x / 4096.f) &&
        std::floor(a.y / 4096.f) == std::floor(b.y / 4096.f);
}

void PlaceInCurrentCell(PlayerCharacter* player, const NiPoint3& target, float heading) noexcept
{
    auto* cell = player->parentCell;
    // Actor virtual A9 (37309 / 1406770A0) updates both position and controller.
    // Reconcile as the Havok-moved path 19826 does, without MoveTo's queued player transition.
    player->SetPosition(target, true);
    using TReconcile = void(TESObjectREFR*, TESObjectCELL*, TESWorldSpace*);
    POINTER_SKYRIMSE(TReconcile, reconcile, 19799);
    reconcile.Get()(player, cell->worldspace ? nullptr : cell, cell->worldspace);
    player->SetRotation(player->rotation.x, player->rotation.y, heading);
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

bool DropInService::PreserveReturnPosition() const noexcept
{
    const auto epoch = m_preserveReturnEpoch.load();
    return epoch && epoch == m_world.GetPartyService().GetStartEpoch();
}

std::string DropInService::RosterPlaceStatus() const noexcept
{
    std::lock_guard lock(m_placeStatusLock);
    return m_placeStatus;
}

void DropInService::SetRosterPlaceStatus(const char* acStatus) noexcept
{
    std::lock_guard lock(m_placeStatusLock);
    m_placeStatus = acStatus;
}

void DropInService::SendStep(uint8_t aOp, const std::string& acText) noexcept
{
    RequestDropIn request;
    request.Attempt = m_attempt;
    request.Joiner = m_joiner;
    request.Op = static_cast<DropInOp>(aOp);
    request.Text = acText.substr(0, DropInData::MaxText).c_str();
    if (request.Op == DropInOp::Join)
    {
        request.SteamId = m_joinSteam;
        request.CharacterKey = m_joinKey.c_str();
    }
    m_transport.Send(request);
}

void DropInService::Fail(const std::string& acReason, bool aTellServer) noexcept
{
    m_preserveReturnEpoch = 0;
    {
        std::lock_guard lock(m_placementLock);
        m_placementPending = false;
        m_placementResult = 0;
    }
    spdlog::warn("Drop-in {:X}: {}", m_attempt, acReason);
    if (aTellServer && m_attempt)
        SendStep(static_cast<uint8_t>(DropInOp::Abort), acReason);
    if (m_phase == Phase::Apply || m_phase == Phase::Place || m_phase == Phase::WaitAdmit || m_phase == Phase::Load)
    {
        FadeOutGame(false, true, 1.f, true, 0.f);
        // Past the title screen the lobby panel that shows dropInStatus is gone: say it in the chat.
        m_world.GetOverlayService().SendSystemMessage(fmt::format("Joining failed: {}.", acReason));
    }
    m_phase = Phase::Idle;
    m_homeSnapshotPath.clear();
    m_file.clear();
    m_status = "failed: " + acReason;
}

bool DropInService::TryPlaceReturnPosition(const RosterPosition& saved) noexcept
{
    if (!saved.Valid() || !PlayerInWorld()) return false;
    auto* player = PlayerCharacter::Get();
    auto* cell = player ? player->parentCell : nullptr;
    if (!cell || !cell->IsAttached() || !player->GetNiNode()) return false;
    const auto& mods = m_world.GetModSystem();
    // Both interiors and exteriors restore coordinates inside the already loaded cell.
    // A different cell requires party travel, so it uses the leader fallback instead.
    if (mods.GetGameId(saved.CellId) != cell->formID) return false;
    if (saved.WorldSpaceId)
    {
        if (!cell->worldspace || mods.GetGameId(saved.WorldSpaceId) != cell->worldspace->formID)
            return false;
    }
    else if (cell->worldspace || mods.GetGameId(saved.CellId) != cell->formID)
        return false;
    NiPoint3 target = player->position;
    target.x = saved.X; target.y = saved.Y; target.z = saved.Z;
    if (cell->worldspace && !SameGrid(player->position, target)) return false;
    PlaceInCurrentCell(player, target, saved.Heading);
    SetRosterPlaceStatus("placed at roster position");
    spdlog::info("Drop-in roster: placed local player in cell {:X} at ({:.1f}, {:.1f}, {:.1f}), heading {:.2f}",
        cell->formID, saved.X, saved.Y, saved.Z, saved.Heading);
    return true;
}

void DropInService::QueuePlacement(bool aFallback) noexcept
{
    std::lock_guard lock(m_placementLock);
    m_placementPosition = m_returnPosition;
    m_placementFallback = aFallback;
    m_placementAttempt = m_attempt;
    m_placementEpoch = m_world.GetPartyService().GetStartEpoch();
    m_placementResult = 0;
    m_placementPending = true;
}

void DropInService::OnMainFrame() noexcept
{
    // One pending operation, no actor/cell scan. Result is consumed by the VM update before admission.
    std::lock_guard lock(m_placementLock);
    if (!m_placementPending) return;
    m_placementPending = false;
    const auto& party = m_world.GetPartyService();
    if (!m_transport.IsConnected() || !party.IsInParty() ||
        party.GetStartEpoch() != m_placementEpoch || !PlayerInWorld() ||
        !PlayerCharacter::Get()->parentCell->IsAttached() || m_world.GetDoorVoteService().HasPendingVote())
    {
        m_placementResult = -1;
        SetRosterPlaceStatus("placement cancelled: world or party changed");
        return;
    }
    // A drop-in is checked against the character it joined as (the server matched the roster on that key); after the
    // host's save loads, the engine's active save identity is the host's, never a co-op character's (2026-10-02 21:00).
    const auto expectedKey = m_joinKey.empty() ? CheckpointSaves::CurrentCharacterKey() : m_joinKey;
    if (!m_placementFallback && m_placementPosition &&
        (m_placementPosition->SteamId != m_world.GetSteamLobbyService().LocalSteamId() ||
         m_placementPosition->CharacterKey != expectedKey.c_str()))
    {
        m_placementResult = -1;
        SetRosterPlaceStatus("roster identity does not match the current character");
        return;
    }
    const bool placed = m_placementPosition && TryPlaceReturnPosition(*m_placementPosition);
    if (!placed && m_placementFallback)
    {
        auto* player = PlayerCharacter::Get();
        const float angle = static_cast<float>((m_placementAttempt >> 1) % 8) * 0.785398f;
        NiPoint3 aside = player->position;
        aside.x += 120.f * std::cos(angle);
        aside.y += 120.f * std::sin(angle);
        // At an exterior edge, retain the loaded host spot rather than cross the grid boundary.
        if (!player->parentCell->worldspace || SameGrid(player->position, aside))
            PlaceInCurrentCell(player, aside, player->rotation.z);
        SetRosterPlaceStatus("roster unavailable; placed beside leader");
        spdlog::info("Drop-in roster: using leader position (no match or different loaded cell)");
    }
    else if (!placed)
        SetRosterPlaceStatus("roster position unavailable in this loaded cell");
    if (m_placementFallback) m_preserveReturnEpoch = placed ? m_placementEpoch : 0;
    m_placementResult = placed ? 1 : 2;
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
    m_joinSteam = m_world.GetSteamLobbyService().LocalSteamId();
    m_joinKey = CheckpointSaves::CharacterKeyForSnapshot(acSnapshotPath);
    if (m_joinKey.empty()) m_joinKey = "name:" + std::string(m_character.Name.c_str());
    m_returnPosition.reset();
    m_preserveReturnEpoch = 0;
    SetRosterPlaceStatus("no roster position");
    CheckpointSaves::SetCarryHomeSource({});
    m_homeSnapshotPath = acSnapshotPath;
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
        CheckpointSaves::Begin(m_checkpoint.c_str(), false);
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
        if (acMessage.HasPosition && acMessage.Position.SteamId == m_joinSteam &&
            acMessage.Position.CharacterKey == m_joinKey.c_str())
            m_returnPosition = acMessage.Position;
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
        CheckpointSaves::SetCarryHomeSourceFromSnapshot(m_homeSnapshotPath);
        m_homeSnapshotPath.clear();
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
    if (m_placeQueued.exchange(false))
    {
        if (m_phase == Phase::Idle) QueuePlacement(false);
        else SetRosterPlaceStatus("join in progress; placement test deferred until admitted");
    }
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
        QueuePlacement(true);
        m_phase = Phase::Place;
        m_since = now;
        m_status = "placing your character";
        return;
    }
    case Phase::Place:
    {
        int result;
        {
            std::lock_guard lock(m_placementLock);
            result = m_placementResult;
        }
        if (result < 0 || (!result && now - m_since > 30000))
        {
            Fail("the loaded world changed before placement", true);
            return;
        }
        if (!result) return;
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
