
#include <Services/TransportService.h>

#include <Events/ConnectedEvent.h>
#include <Events/ConnectionErrorEvent.h>
#include <Events/DisconnectedEvent.h>
#include <Events/UpdateEvent.h>

#include <Games/References.h>
#include <Games/TES.h>
#include <Forms/TESWorldSpace.h>
#include <Forms/TESObjectCELL.h>

#include <TimeManager.h>

#include <Forms/TESNPC.h>
#include <TiltedOnlinePCH.h>
#include <World.h>

#include <Messages/AuthenticationRequest.h>
#include <Messages/ServerMessageFactory.h>
#include <Messages/NotifySettingsChange.h>
#include <Packet.hpp>

#include <ScriptExtender.h>
#include <Services/DiscordService.h>

// #include <imgui_internal.h>

static constexpr wchar_t kMO2DllName[] = L"usvfs_x64.dll";

using TiltedPhoques::Packet;

static std::filesystem::path GetGameDataDirectory() noexcept
{
    std::array<wchar_t, 32768> executablePath{};
    const auto length = GetModuleFileNameW(nullptr, executablePath.data(), static_cast<DWORD>(executablePath.size()));
    if (length == 0 || length == executablePath.size())
        return std::filesystem::current_path() / L"Data";

    return std::filesystem::path(executablePath.data()).parent_path() / L"Data";
}

static std::filesystem::path GetDeploymentCachePath() noexcept
{
    std::array<wchar_t, 32768> localAppData{};
    const auto length = GetEnvironmentVariableW(L"LOCALAPPDATA", localAppData.data(), static_cast<DWORD>(localAppData.size()));
    const auto base = length > 0 && length < localAppData.size() ? std::filesystem::path(localAppData.data()) : std::filesystem::temp_directory_path();
    return base / L"SkyrimSEMultiplayer" / L"deployment-hash-cache-v2.tsv";
}

TransportService::TransportService(World& aWorld, entt::dispatcher& aDispatcher) noexcept
    : m_world(aWorld)
    , m_dispatcher(aDispatcher)
{
    m_updateConnection = m_dispatcher.sink<UpdateEvent>().connect<&TransportService::HandleUpdate>(this);
    m_settingsChangeConnection = m_dispatcher.sink<NotifySettingsChange>().connect<&TransportService::HandleNotifySettingsChange>(this);
    m_connectedConnection = m_dispatcher.sink<ConnectedEvent>().connect<&TransportService::HandleConnected>(this);
    m_disconnectedConnection = m_dispatcher.sink<DisconnectedEvent>().connect<&TransportService::HandleDisconnected>(this);

    m_connected = false;

    const auto dataDirectory = GetGameDataDirectory();
    const auto cachePath = GetDeploymentCachePath();
    m_deploymentScan = std::async(std::launch::async, [dataDirectory, cachePath] { return DeploymentScanner::Scan(dataDirectory, cachePath); });

    auto handlerGenerator = [this](auto& x)
    {
        using T = typename std::remove_reference_t<decltype(x)>::Type;

        m_messageHandlers[T::Opcode] = [this](UniquePtr<ServerMessage>& apMessage)
        {
            const auto pRealMessage = TiltedPhoques::CastUnique<T>(std::move(apMessage));
            m_dispatcher.trigger(*pRealMessage);
        };

        return false;
    };

    ServerMessageFactory::Visit(handlerGenerator);

    // Override authentication response
    m_messageHandlers[AuthenticationResponse::Opcode] = [this](UniquePtr<ServerMessage>& apMessage)
    {
        const auto pRealMessage = TiltedPhoques::CastUnique<AuthenticationResponse>(std::move(apMessage));
        HandleAuthenticationResponse(*pRealMessage);
    };
}

bool TransportService::Send(const ClientMessage& acMessage) const noexcept
{
    static thread_local ScratchAllocator s_allocator(1 << 18);

    struct ScopedReset
    {
        ~ScopedReset() { s_allocator.Reset(); }
    } allocatorGuard;

    if (IsConnected())
    {
        ScopedAllocator _{s_allocator};

        Buffer buffer(1 << 16);
        Buffer::Writer writer(&buffer);
        writer.WriteBits(0, 8); // Write first byte as packet needs it

        acMessage.Serialize(writer);
        TiltedPhoques::PacketView packet(reinterpret_cast<char*>(buffer.GetWriteData()), writer.Size());

        Client::Send(&packet);

        return true;
    }

    return false;
}

void TransportService::OnConsume(const void* apData, uint32_t aSize)
{
    ServerMessageFactory factory;
    TiltedPhoques::ViewBuffer buf((uint8_t*)apData, aSize);
    Buffer::Reader reader(&buf);

    auto pMessage = factory.Extract(reader);
    if (!pMessage)
    {
        spdlog::error("Couldn't parse packet from server");
        return;
    }

    m_messageHandlers[pMessage->GetOpcode()](pMessage);
}

void TransportService::OnConnected()
{
    m_authenticationPending = true;
    if (m_deploymentManifest)
    {
        m_authenticationPending = false;
        SendAuthenticationRequest();
        return;
    }
    if (m_deploymentScan.valid() && m_deploymentScan.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
    {
        spdlog::info("Connected; waiting for the effective Data deployment scan before authentication");
        return;
    }

    OnUpdate();
}

void TransportService::SendAuthenticationRequest()
{
    AuthenticationRequest request{};
    request.Version = BUILD_COMMIT;
    request.SKSEActive = IsScriptExtenderLoaded();
    request.MO2Active = GetModuleHandleW(kMO2DllName);

    request.Token = m_serverPassword;
    m_serverPassword = "";
    request.UserMods.Deployment = m_deploymentManifest.value_or(DeploymentManifest{});

    PlayerCharacter* pPlayer = PlayerCharacter::Get();

    // null if discord is not active
    // TODO: think about user opt out
    request.DiscordId = m_world.ctx().at<DiscordService>().GetUser().id;
    auto* pNpc = Cast<TESNPC>(pPlayer->baseForm);
    if (pNpc)
    {
        request.Username = pNpc->fullName.value.AsAscii();
    }
    else
    {
        request.Username = "Some dragon boi";
    }

    auto* const cpModManager = ModManager::Get();
    const auto dataDirectory = GetGameDataDirectory();

    for (auto* pMod : cpModManager->mods)
    {
        if (!pMod->IsLoaded())
            continue;

        auto& entry = request.UserMods.ModList.emplace_back();
        entry.Id = pMod->GetId();
        entry.IsLite = pMod->IsLite();
        entry.Filename = pMod->filename;

        const auto pluginPath = dataDirectory / pMod->filename;
        if (!Mods::FingerprintFile(pluginPath, entry))
            spdlog::error("Could not fingerprint loaded plugin {}", pluginPath.string());
    }

    auto& modSystem = m_world.GetModSystem();
    if (pPlayer->GetWorldSpace())
        modSystem.GetServerModId(pPlayer->GetWorldSpace()->formID, request.WorldSpaceId);

    modSystem.GetServerModId(pPlayer->parentCell->formID, request.CellId);

    request.Level = pPlayer->GetLevel();

    auto* pGameTime = TimeData::Get();
    request.PlayerTime.TimeScale = pGameTime->TimeScale->f;
    request.PlayerTime.Time = pGameTime->GameHour->f;
    request.PlayerTime.Year = pGameTime->GameYear->f;
    request.PlayerTime.Month = pGameTime->GameMonth->f;
    request.PlayerTime.Day = pGameTime->GameDay->f;

    Send(request);
}

void TransportService::OnDisconnected(EDisconnectReason aReason)
{
    m_connected = false;
    m_authenticationPending = false;

    spdlog::warn("Disconnected from server {}", aReason);

    m_dispatcher.trigger(DisconnectedEvent());
}

void TransportService::OnUpdate()
{
    if (!m_authenticationPending || !m_deploymentScan.valid() || m_deploymentScan.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
        return;

    const auto scan = m_deploymentScan.get();
    m_deploymentManifest = scan.Manifest;
    m_authenticationPending = false;
    spdlog::info(
        "Effective Data scan complete: {} files, {} hashed, {} cached, {} skipped, {} errors", scan.Manifest.AllFiles.FileCount, scan.HashedFiles, scan.CachedFiles,
        scan.SkippedFiles, scan.Errors.size());
    for (const auto& error : scan.Errors)
        spdlog::error("Effective Data scan: {}", error);
    SendAuthenticationRequest();
}

void TransportService::HandleUpdate(const UpdateEvent& acEvent) noexcept
{
    Update();
}

void TransportService::HandleConnected(const ConnectedEvent& acEvent) noexcept
{
    m_localPlayerId = acEvent.PlayerId;
}

void TransportService::HandleDisconnected(const DisconnectedEvent& acEvent) noexcept
{
    m_localPlayerId = NULL;
}

void TransportService::HandleAuthenticationResponse(const AuthenticationResponse& acMessage) noexcept
{
    using AR = AuthenticationResponse::ResponseType;
    if (acMessage.Type == AR::kAccepted)
    {
        m_connected = true;

        m_world.SetServerSettings(acMessage.Settings);
        m_campaignId = acMessage.CampaignId;
        m_campaignRevision = acMessage.CampaignRevision;
        m_authorityEpoch = acMessage.AuthorityEpoch;

        spdlog::info("Joined shared campaign {} at revision {} (authority epoch {})", m_campaignId.c_str(), m_campaignRevision, m_authorityEpoch);

        m_dispatcher.trigger(acMessage.UserMods);
        m_dispatcher.trigger(acMessage.Settings);
        m_dispatcher.trigger(ConnectedEvent(acMessage.PlayerId));
        return; // quit the function here.
    }

    // error finding

    TiltedPhoques::String ErrorInfo;

    ErrorInfo = "{";

    switch (acMessage.Type)
    {
    case AR::kWrongVersion:
        ErrorInfo += "\"error\": \"wrong_version\", \"data\": {";
        ErrorInfo += fmt::format("\"expectedVersion\": \"{}\", \"version\": \"{}\"", acMessage.Version, BUILD_COMMIT);
        ErrorInfo += "}";
        break;
    case AR::kModsMismatch:
    {
        ErrorInfo += "\"error\": \"mods_mismatch\", \"data\": {\"mods\": [";
        bool first = true;
        for (const auto& m : acMessage.UserMods.ModList)
        {
            if (!first)
                ErrorInfo += ",";
            ErrorInfo += fmt::format("[\"{}\",\"{}\",{}]", m.Filename.c_str(), m.Id, m.MismatchFlags);
            first = false;
        }
        ErrorInfo += "]}";
        break;
    }
    case AR::kClientModsDisallowed:
    {
        ErrorInfo += "\"error\": \"client_mods_disallowed\", \"data\": { \"mods\": [";
        if (acMessage.SKSEActive)
            ErrorInfo += "\"SKSE\"";
        if (acMessage.MO2Active)
            if (acMessage.SKSEActive)
                ErrorInfo += ",";
        ErrorInfo += "\"MO2\"";
        ErrorInfo += "]}";
        break;
    }
    case AR::kWrongPassword:
    {
        ErrorInfo += "\"error\": \"wrong_password\"";
        break;
    }
    case AR::kServerFull:
    {
        ErrorInfo += "\"error\": \"server_full\"";
        break;
    }
    default: ErrorInfo += "\"error\": \"no_reason\""; break;
    }

    ErrorInfo += "}";

    ConnectionErrorEvent errorEvent;
    if (!ErrorInfo.empty())
    {
        spdlog::error(ErrorInfo.c_str());
        errorEvent.ErrorDetail = std::move(ErrorInfo);
    }

    m_dispatcher.trigger(errorEvent);
}

void TransportService::HandleNotifySettingsChange(const NotifySettingsChange& acMessage) noexcept
{
    m_world.SetServerSettings(acMessage.Settings);
    m_dispatcher.trigger(acMessage.Settings);
}
