#include <TiltedOnlinePCH.h>

#include <Services/GameTestService.h>
#include <Services/GameSettingsService.h>
#include <Services/OverlayService.h>
#include <World.h>

#include <Games/Skyrim/BSGraphics/BSGraphicsRenderer.h>
#include <Games/ActorExtension.h>
#include <Games/Misc/MenuTopicManager.h>
#include <Games/Skyrim/Interface/UI.h>
#include <Games/Skyrim/PlayerCharacter.h>
#include <Games/Skyrim/Forms/ActorValueInfo.h>
#include <Games/Skyrim/Forms/TESQuest.h>
#include <Games/Skyrim/Forms/TESObjectCELL.h>
#include <Games/Skyrim/Forms/TESWorldSpace.h>
#include <Games/Skyrim/Forms/TESPackage.h>
#include <Games/Skyrim/AI/Movement/PlayerControls.h>
#include <Games/Skyrim/Camera/PlayerCamera.h>
#include <Games/Skyrim/AI/AIProcess.h>
#include <Games/TES.h>
#include <Services/PartyService.h>
#include <Services/TransportService.h>
#include <Services/QuestService.h>
#include <Components.h>
#include <OverlayApp.hpp>
#include <OverlayRenderHandler.hpp>

namespace
{
constexpr wchar_t cTestPipeName[] = LR"(\\.\pipe\SkyrimSEMultiplayer.Test)";
constexpr DWORD cPipeRejectRemoteClients = 0x00000008;

std::string EscapeJson(const std::string& acValue)
{
    std::string result;
    result.reserve(acValue.size() + 16);
    for (const char value : acValue)
    {
        switch (value)
        {
        case '\\': result += "\\\\"; break;
        case '"': result += "\\\""; break;
        case '\n': result += "\\n"; break;
        case '\r': result += "\\r"; break;
        case '\t': result += "\\t"; break;
        default: result += value; break;
        }
    }
    return result;
}

std::string GetJsonString(const std::string& acJson, const char* acName)
{
    const std::string key = std::string("\"") + acName + "\"";
    auto position = acJson.find(key);
    if (position == std::string::npos)
        return {};
    position = acJson.find(':', position + key.size());
    if (position == std::string::npos)
        return {};
    position = acJson.find('"', position + 1);
    if (position == std::string::npos)
        return {};
    std::string result;
    for (++position; position < acJson.size(); ++position)
    {
        const char value = acJson[position];
        if (value == '"')
            break;
        if (value == '\\' && position + 1 < acJson.size())
        {
            const char escaped = acJson[++position];
            if (escaped == 'n') result += '\n';
            else if (escaped == 'r') result += '\r';
            else if (escaped == 't') result += '\t';
            else result += escaped;
        }
        else
            result += value;
    }
    return result;
}

uint64_t GetJsonId(const std::string& acJson)
{
    const auto key = acJson.find("\"id\"");
    if (key == std::string::npos)
        return 0;
    const auto colon = acJson.find(':', key + 4);
    if (colon == std::string::npos)
        return 0;
    return std::strtoull(acJson.c_str() + colon + 1, nullptr, 10);
}

std::string Result(uint64_t aId, const std::string& acPayload)
{
    return fmt::format("{{\"id\":{},\"ok\":true,{}}}", aId, acPayload);
}

std::string Error(uint64_t aId, const std::string& acMessage)
{
    return fmt::format("{{\"id\":{},\"ok\":false,\"error\":\"{}\"}}", aId, EscapeJson(acMessage));
}

const char* JsonBool(bool aValue)
{
    return aValue ? "true" : "false";
}

bool HandlerEnabled(const PlayerInputHandler* apHandler)
{
    return apHandler && apHandler->isEnabled;
}
}

GameTestService::GameTestService(World& aWorld) noexcept
    : m_world(aWorld)
    , m_pipeThread([this]() { PipeMain(); })
{
    spdlog::info("In-game test bridge starting at \\\\.\\pipe\\SkyrimSEMultiplayer.Test");
}

GameTestService::~GameTestService() noexcept
{
    m_stopping = true;
    if (m_pipeThread.joinable())
        CancelSynchronousIo(static_cast<HANDLE>(m_pipeThread.native_handle()));
    // Wake a blocking ConnectNamedPipe during orderly shutdown.
    if (const HANDLE pipe = CreateFileW(cTestPipeName, GENERIC_READ | GENERIC_WRITE, 0, nullptr,
            OPEN_EXISTING, 0, nullptr); pipe != INVALID_HANDLE_VALUE)
        CloseHandle(pipe);
    if (m_pipeThread.joinable())
        m_pipeThread.join();
}

void GameTestService::WakeWindowThread() noexcept
{
    if (auto* pWindow = BSGraphics::GetMainWindow(); pWindow && pWindow->hWnd)
        PostMessageW(pWindow->hWnd, cGameTestWakeMessage, 0, 0);
}

void GameTestService::PipeMain() noexcept
{
    while (!m_stopping)
    {
        const HANDLE pipe = CreateNamedPipeW(cTestPipeName, PIPE_ACCESS_DUPLEX,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | cPipeRejectRemoteClients,
            1, 64 * 1024, 64 * 1024, 0, nullptr);
        if (pipe == INVALID_HANDLE_VALUE)
        {
            Sleep(1000);
            continue;
        }

        const bool connected = ConnectNamedPipe(pipe, nullptr) != FALSE || GetLastError() == ERROR_PIPE_CONNECTED;
        if (!connected || m_stopping)
        {
            CloseHandle(pipe);
            continue;
        }

        std::string buffered;
        char chunk[4096];
        DWORD bytesRead = 0;
        bool requestServed = false;
        while (!m_stopping && !requestServed && ReadFile(pipe, chunk, sizeof(chunk), &bytesRead, nullptr) && bytesRead)
        {
            buffered.append(chunk, bytesRead);
            for (auto newline = buffered.find('\n'); newline != std::string::npos; newline = buffered.find('\n'))
            {
                auto line = buffered.substr(0, newline);
                buffered.erase(0, newline + 1);
                if (!line.empty() && line.back() == '\r')
                    line.pop_back();
                if (line.empty())
                    continue;

                auto request = std::make_shared<Request>();
                request->Line = std::move(line);
                {
                    std::scoped_lock lock(m_queueMutex);
                    m_requests.push_back(request);
                }
                WakeWindowThread();

                std::unique_lock lock(request->Mutex);
                if (!request->Completed.wait_for(lock, 15s, [&]() { return request->Complete || m_stopping; }))
                    request->Response = Error(GetJsonId(request->Line), "window-thread timeout");
                request->Response += '\n';
                DWORD written = 0;
                if (!WriteFile(pipe, request->Response.data(), static_cast<DWORD>(request->Response.size()), &written, nullptr))
                    break;
                // The client is blocked reading this response, so flushing here
                // guarantees delivery before the one-shot server disconnects.
                FlushFileBuffers(pipe);
                // One request per connection keeps a disconnected or idle
                // diagnostic client from monopolizing the sole local pipe.
                requestServed = true;
                break;
            }
        }
        DisconnectNamedPipe(pipe);
        CloseHandle(pipe);
    }
}

void GameTestService::OnWindowThread() noexcept
{
    std::deque<std::shared_ptr<Request>> requests;
    {
        std::scoped_lock lock(m_queueMutex);
        requests.swap(m_requests);
    }
    for (const auto& request : requests)
    {
        const auto response = Execute(request->Line);
        {
            std::scoped_lock lock(request->Mutex);
            request->Response = response;
            request->Complete = true;
        }
        request->Completed.notify_one();
    }
}

void GameTestService::OnGameThread() noexcept
{
    const auto now = GetTickCount64();
    if (now < m_nextGameSnapshotTimeMs)
        return;
    m_nextGameSnapshotTimeMs = now + 100;

    try
    {
        std::string snapshot = "{";
        snapshot += fmt::format("\"sampleTimeMs\":{},\"worldTick\":{}", now, m_world.GetTick());

        const auto& party = m_world.GetPartyService();
        const auto& transport = m_world.GetTransport();
        snapshot += fmt::format(
            ",\"session\":{{\"online\":{},\"localPlayerId\":{},\"inParty\":{},\"leader\":{},"
            "\"leaderPlayerId\":{},\"memberCount\":{},\"campaignId\":\"{}\","
            "\"campaignRevision\":{},\"authorityEpoch\":{}}}",
            JsonBool(transport.IsOnline()), transport.GetLocalPlayerId(), JsonBool(party.IsInParty()),
            JsonBool(party.IsLeader()), party.GetLeaderPlayerId(), party.GetPartyMembers().size(),
            EscapeJson(transport.GetCampaignId().c_str()), transport.GetCampaignRevision(),
            transport.GetAuthorityEpoch());

        if (auto* pPlayer = PlayerCharacter::Get())
        {
            auto* pCell = pPlayer->GetParentCellEx();
            auto* pWorldspace = pPlayer->GetWorldSpace();
            auto* pPackage = pPlayer->currentProcess ? pPlayer->currentProcess->package : nullptr;
            snapshot += fmt::format(
                ",\"player\":{{\"present\":true,\"formId\":{},\"cellId\":{},\"worldspaceId\":{},"
                "\"position\":[{},{},{}],\"rotation\":[{},{},{}],\"dead\":{},\"bleedingOut\":{},"
                "\"inCombat\":{},\"weaponDrawn\":{},\"weaponFullyDrawn\":{},\"dialogueHandle\":{},"
                "\"combatHandle\":{},\"killerHandle\":{},\"packageFormId\":{},\"movementType\":{},"
                "\"health\":{},\"magicka\":{},\"stamina\":{},\"speed\":{},"
                "\"actorStateFlags1\":{},\"actorStateFlags2\":{}}}",
                pPlayer->formID, pCell ? pCell->formID : 0, pWorldspace ? pWorldspace->formID : 0,
                pPlayer->position.x, pPlayer->position.y, pPlayer->position.z,
                pPlayer->rotation.x, pPlayer->rotation.y, pPlayer->rotation.z,
                JsonBool(pPlayer->IsDead()), JsonBool(pPlayer->actorState.IsBleedingOut()),
                JsonBool(pPlayer->IsInCombat()), JsonBool(pPlayer->actorState.IsWeaponDrawn()),
                JsonBool(pPlayer->actorState.IsWeaponFullyDrawn()), pPlayer->dialogueHandle,
                pPlayer->combatHandle, pPlayer->killerHandle, pPackage ? pPackage->formID : 0,
                pPlayer->currentProcess ? pPlayer->currentProcess->movementType : -1,
                pPlayer->GetActorValue(ActorValueInfo::kHealth),
                pPlayer->GetActorValue(ActorValueInfo::kMagicka),
                pPlayer->GetActorValue(ActorValueInfo::kStamina), pPlayer->GetSpeed(),
                pPlayer->actorState.flags1, pPlayer->actorState.flags2);

            if (const auto* pExtension = pPlayer->GetExtension())
            {
                const auto& action = pExtension->LatestAnimation;
                snapshot += fmt::format(
                    ",\"playerAnimation\":{{\"graphReady\":{},\"graphDescriptor\":{},"
                    "\"reconciliationStage\":{},\"tick\":{},\"actionId\":{},\"targetId\":{},"
                    "\"idleId\":{},\"type\":{},\"state1\":{},\"state2\":{},\"event\":\"{}\"}}",
                    JsonBool(pPlayer->animationGraphHolder.IsReady()), pExtension->GraphDescriptorHash,
                    static_cast<uint32_t>(pExtension->Reconciliation), action.Tick, action.ActionId,
                    action.TargetId, action.IdleId, action.Type, action.State1, action.State2,
                    EscapeJson(action.EventName.c_str()));
            }
        }
        else
            snapshot += ",\"player\":{\"present\":false}";

        if (auto* pControls = PlayerControls::GetInstance())
        {
            snapshot += fmt::format(
                ",\"controls\":{{\"present\":true,\"blocked\":{},\"movement\":{},\"look\":{},"
                "\"sprint\":{},\"readyWeapon\":{},\"activate\":{},\"jump\":{},\"shout\":{},"
                "\"attackBlock\":{},\"sneak\":{},\"togglePov\":{},\"autoMove\":{},"
                "\"running\":{},\"povScriptMode\":{},\"remapMode\":{},"
                "\"moveInput\":[{},{}],\"lookInput\":[{},{}]}}",
                JsonBool(pControls->bBlockPlayerInput), JsonBool(HandlerEnabled(pControls->pMovementHandler)),
                JsonBool(HandlerEnabled(pControls->pLookHandler)), JsonBool(HandlerEnabled(pControls->pSprintHandler)),
                JsonBool(HandlerEnabled(pControls->pReadyWeaponHandler)), JsonBool(HandlerEnabled(pControls->pActivateHandler)),
                JsonBool(HandlerEnabled(pControls->pJumpHandler)), JsonBool(HandlerEnabled(pControls->shoutHandler)),
                JsonBool(HandlerEnabled(pControls->attackBlockHandler)), JsonBool(HandlerEnabled(pControls->sneakHandler)),
                JsonBool(HandlerEnabled(pControls->togglePOVHandler)), JsonBool(pControls->Data.bAutoMove),
                JsonBool(pControls->Data.bRunning), JsonBool(pControls->Data.povScriptMode),
                JsonBool(pControls->Data.remapMode), pControls->Data.MoveInputVec.x,
                pControls->Data.MoveInputVec.y, pControls->Data.LookInputVec.x,
                pControls->Data.LookInputVec.y);
        }
        else
            snapshot += ",\"controls\":{\"present\":false}";

        if (auto* pCamera = PlayerCamera::Get())
        {
            snapshot += fmt::format(
                ",\"camera\":{{\"present\":true,\"firstPerson\":{},\"hasState\":{},"
                "\"position\":[{},{},{}],\"rotation\":[{},{}],\"zoom\":{}}}",
                JsonBool(pCamera->IsFirstPerson()), JsonBool(pCamera->state != nullptr),
                pCamera->pos.x, pCamera->pos.y, pCamera->pos.z, pCamera->rotX, pCamera->rotZ, pCamera->zoom);
        }
        else
            snapshot += ",\"camera\":{\"present\":false}";

        snapshot += ",\"menus\":[";
        if (auto* pUI = UI::Get())
        {
            bool firstMenu = true;
            for (auto* pMenu : pUI->menuStack)
            {
                if (!pMenu)
                    continue;
                auto* pName = pUI->LookupMenuNameByInstance(pMenu);
                if (!pName)
                    continue;
                if (!firstMenu)
                    snapshot += ',';
                snapshot += fmt::format("\"{}\"", EscapeJson(pName->AsAscii()));
                firstMenu = false;
            }
        }
        snapshot += ']';

        if (auto* pUI = UI::Get())
        {
            snapshot += fmt::format(
                ",\"ui\":{{\"loading\":{},\"dialogue\":{},\"pausesGame\":{},\"allowSaving\":{},"
                "\"disablePauseMenu\":{},\"modal\":{},\"visible\":{},\"closingAllMenus\":{},"
                "\"dontHideCursor\":{}}}",
                JsonBool(pUI->GetMenuOpen(BSFixedString("Loading Menu"))),
                JsonBool(pUI->GetMenuOpen(BSFixedString("Dialogue Menu"))), pUI->numPausesGame,
                pUI->numAllowSaving, pUI->numDisablePauseMenu, JsonBool(pUI->modal),
                JsonBool(pUI->menuSystemVisible), JsonBool(pUI->closingAllMenus),
                pUI->numDontHideCursorWhenTopmost);
        }
        else
            snapshot += ",\"ui\":{\"present\":false}";

        if (auto* pDialogue = MenuTopicManager::Get())
        {
            const auto* pSpeaker = TESObjectREFR::GetByHandle(pDialogue->speaker.handle.iBits);
            snapshot += fmt::format(
                ",\"dialogue\":{{\"menuOpen\":{},\"speakerHandle\":{},\"speakerFormId\":{},"
                "\"hasOptions\":{}}}",
                JsonBool(pDialogue->menuOpen), pDialogue->speaker.handle.iBits,
                pSpeaker ? pSpeaker->formID : 0,
                JsonBool(pDialogue->pOptions != nullptr));
        }
        else
            snapshot += ",\"dialogue\":{\"present\":false}";

        if (auto* pTes = TES::Get())
        {
            snapshot += fmt::format(
                ",\"world\":{{\"centerGrid\":[{},{}],\"currentGrid\":[{},{}],"
                "\"interiorCellId\":{}}}",
                pTes->centerGridX, pTes->centerGridY, pTes->currentGridX, pTes->currentGridY,
                pTes->interiorCell ? pTes->interiorCell->formID : 0);
        }
        if (auto* pProcesses = ProcessLists::Get())
        {
            snapshot += fmt::format(
                ",\"actorProcessing\":{{\"highCount\":{},\"highHandles\":{},"
                "\"middleHighHandles\":{},\"middleLowHandles\":{},\"lowHandles\":{}}}",
                pProcesses->numberHighActors, pProcesses->highActorHandleArray.length,
                pProcesses->middleHighActorHandleArray.length, pProcesses->middleLowActorHandleArray.length,
                pProcesses->lowActorHandleArray.length);
        }

        if (const auto* pWindow = BSGraphics::GetMainWindow(); pWindow && pWindow->hWnd)
        {
            RECT client{};
            RECT window{};
            RECT clip{};
            GetClientRect(pWindow->hWnd, &client);
            GetWindowRect(pWindow->hWnd, &window);
            const bool cursorClipped = GetClipCursor(&clip) != FALSE &&
                (clip.left != 0 || clip.top != 0 || clip.right != GetSystemMetrics(SM_CXSCREEN) ||
                    clip.bottom != GetSystemMetrics(SM_CYSCREEN));
            snapshot += fmt::format(
                ",\"window\":{{\"foreground\":{},\"minimized\":{},\"visible\":{},"
                "\"client\":[{},{}],\"rect\":[{},{},{},{}],\"cursorClipped\":{}}}",
                JsonBool(GetForegroundWindow() == pWindow->hWnd), JsonBool(IsIconic(pWindow->hWnd) != FALSE),
                JsonBool(IsWindowVisible(pWindow->hWnd) != FALSE), client.right - client.left,
                client.bottom - client.top, window.left, window.top, window.right, window.bottom,
                JsonBool(cursorClipped));
        }

        snapshot += ",\"networkEntities\":[";
        bool firstEntity = true;
        size_t emittedEntities = 0;
        const auto entityView = m_world.view<FormIdComponent>();
        for (const auto entity : entityView)
        {
            if (emittedEntities++ >= 256)
                break;
            const auto& form = entityView.get<FormIdComponent>(entity);
            const auto* pLocal = m_world.try_get<LocalComponent>(entity);
            const auto* pRemote = m_world.try_get<RemoteComponent>(entity);
            const auto* pNetworkPlayer = m_world.try_get<PlayerComponent>(entity);
            const auto* pLocalAnimation = m_world.try_get<LocalAnimationComponent>(entity);
            const auto* pRemoteAnimation = m_world.try_get<RemoteAnimationComponent>(entity);
            if (!firstEntity)
                snapshot += ',';
            firstEntity = false;
            snapshot += fmt::format(
                "{{\"formId\":{},\"playerId\":{},\"authority\":\"{}\",\"networkId\":{},"
                "\"ownershipEpoch\":{},\"waitingFor3D\":{},\"waitingForAssignment\":{},"
                "\"animationQueued\":{},\"animationReplayQueued\":{}}}",
                form.Id, pNetworkPlayer ? pNetworkPlayer->Id : 0,
                pLocal ? "local" : (pRemote ? "remote" : "unassigned"),
                pLocal ? pLocal->Id : (pRemote ? pRemote->Id : 0),
                pLocal ? pLocal->OwnershipEpoch : (pRemote ? pRemote->OwnershipEpoch : 0),
                JsonBool(m_world.all_of<WaitingFor3D>(entity)),
                JsonBool(m_world.all_of<WaitingForAssignmentComponent>(entity)),
                pLocalAnimation ? pLocalAnimation->Actions.size() : 0,
                pRemoteAnimation ? pRemoteAnimation->TimePoints.size() : 0);
        }
        snapshot += ']';

        Set<std::string> watchedQuests;
        {
            std::scoped_lock lock(m_snapshotMutex);
            watchedQuests = m_watchedQuests;
        }
        snapshot += ",\"quests\":[";
        bool firstQuest = true;
        if (auto* pModManager = ModManager::Get())
        {
            for (auto* pQuest : pModManager->quests)
            {
                if (!pQuest || !watchedQuests.contains(pQuest->idName.AsAscii()))
                    continue;
                if (!firstQuest)
                    snapshot += ',';
                firstQuest = false;
                snapshot += fmt::format(
                    "{{\"editorId\":\"{}\",\"formId\":{},\"currentStage\":{},\"flags\":{},"
                    "\"state\":{},\"enabled\":{},\"active\":{},\"stopped\":{},\"doneStages\":[",
                    EscapeJson(pQuest->idName.AsAscii()), pQuest->formID, pQuest->currentStage, pQuest->flags,
                    static_cast<uint8_t>(pQuest->getState()), JsonBool(pQuest->IsEnabled()),
                    JsonBool(pQuest->IsActive()), JsonBool(pQuest->IsStopped()));
                bool firstStage = true;
                for (auto* pStage : pQuest->stages)
                {
                    if (!pStage || !pStage->IsDone())
                        continue;
                    if (!firstStage)
                        snapshot += ',';
                    snapshot += fmt::format("{}", pStage->stageIndex);
                    firstStage = false;
                }
                snapshot += "]}";
            }
        }
        snapshot += ']';

        snapshot += ",\"questEvents\":[";
        bool firstEvent = true;
        for (const auto& event : m_world.GetQuestService().GetRecentDebugEvents())
        {
            if (!firstEvent)
                snapshot += ',';
            snapshot += fmt::format(
                "{{\"sequence\":{},\"timeMs\":{},\"kind\":\"{}\",\"formId\":{},\"stage\":{},"
                "\"scopedOverride\":{},\"inParty\":{},\"leader\":{}}}",
                event.Sequence, event.TimeMs, EscapeJson(event.Kind.c_str()), event.FormId, event.Stage,
                JsonBool(event.ScopedOverride), JsonBool(event.InParty), JsonBool(event.Leader));
            firstEvent = false;
        }
        snapshot += "]}";

        std::scoped_lock lock(m_snapshotMutex);
        m_gameSnapshot = std::move(snapshot);
        m_gameSnapshotTimeMs = now;
    }
    catch (const std::exception& exception)
    {
        spdlog::warn("Game test snapshot failed: {}", exception.what());
    }
    catch (...)
    {
        spdlog::warn("Game test snapshot failed with native exception");
    }
}

std::string GameTestService::GetCachedGameSnapshot() const noexcept
{
    std::scoped_lock lock(m_snapshotMutex);
    return m_gameSnapshot;
}

std::string GameTestService::Execute(const std::string& acLine) noexcept
{
    const uint64_t id = GetJsonId(acLine);
    const auto command = GetJsonString(acLine, "command");
    try
    {
        if (command == "ping")
            return Result(id, fmt::format("\"pid\":{},\"protocol\":2", GetCurrentProcessId()));

        if (command == "capabilities")
            return Result(id, "\"protocol\":2,\"commands\":[\"ping\",\"capabilities\",\"snapshot\","
                "\"game_snapshot\",\"watch_quest\",\"capture_bundle\",\"screenshot\",\"open_options\","
                "\"close_options\",\"controller\",\"toggle_window\",\"confirm_display\",\"setting\"]");

        if (command == "watch_quest")
        {
            const auto editorId = GetJsonString(acLine, "editorId");
            if (editorId.empty())
                return Error(id, "editorId is required");
            std::scoped_lock lock(m_snapshotMutex);
            m_watchedQuests.insert(editorId);
            return Result(id, fmt::format("\"editorId\":\"{}\",\"watched\":true", EscapeJson(editorId)));
        }

        if (command == "game_snapshot")
        {
            std::scoped_lock lock(m_snapshotMutex);
            const auto age = m_gameSnapshotTimeMs ? GetTickCount64() - m_gameSnapshotTimeMs : 0;
            return Result(id, fmt::format("\"ageMs\":{},\"game\":{}", age, m_gameSnapshot));
        }

        if (command == "capture_bundle")
        {
            const auto path = m_world.GetGameSettingsService().CaptureTestScreenshot();
            if (path.empty())
                return Error(id, "screenshot failed");
            const auto statePath = path.parent_path() / (path.stem().string() + ".game.json");
            std::ofstream state(statePath, std::ios::binary);
            state << GetCachedGameSnapshot();
            if (!state)
                return Error(id, "game snapshot write failed");
            return Result(id, fmt::format("\"screenshotPath\":\"{}\",\"gameStatePath\":\"{}\"",
                EscapeJson(path.string()), EscapeJson(statePath.string())));
        }

        if (command == "open_options")
        {
            auto& overlay = m_world.GetOverlayService();
            overlay.SetActive(true);
            if (auto* pApp = overlay.GetOverlayApp())
                pApp->ExecuteAsync("showTitleOptions");
            m_world.GetGameSettingsService().RequestSettings();
            return Result(id, "\"action\":\"open_options\"");
        }
        if (command == "close_options")
        {
            m_world.GetOverlayService().SetActive(false);
            return Result(id, "\"action\":\"close_options\"");
        }
        if (command == "controller")
        {
            const auto button = GetJsonString(acLine, "button");
            if (!m_world.GetOverlayService().InjectTestControllerButton(button))
                return Error(id, "unknown controller button");
            return Result(id, fmt::format("\"button\":\"{}\"", EscapeJson(button)));
        }
        if (command == "toggle_window")
        {
            m_world.GetGameSettingsService().ToggleWindowMode();
            return Result(id, "\"action\":\"toggle_window\"");
        }
        if (command == "confirm_display")
        {
            m_world.GetGameSettingsService().ConfirmDisplaySettings();
            return Result(id, "\"action\":\"confirm_display\"");
        }
        if (command == "setting")
        {
            const auto name = GetJsonString(acLine, "name");
            const auto value = GetJsonString(acLine, "value");
            if (name.empty())
                return Error(id, "setting name is required");
            m_world.GetGameSettingsService().PreviewSetting(name.c_str(), value.c_str());
            return Result(id, fmt::format("\"name\":\"{}\",\"value\":\"{}\"",
                EscapeJson(name), EscapeJson(value)));
        }
        if (command == "screenshot")
        {
            const auto path = m_world.GetGameSettingsService().CaptureTestScreenshot();
            if (path.empty())
                return Error(id, "screenshot failed");
            return Result(id, fmt::format("\"path\":\"{}\"", EscapeJson(path.string())));
        }
        if (command == "snapshot")
        {
            auto* pWindow = BSGraphics::GetMainWindow();
            auto* pRenderer = BSGraphics::GetRendererData();
            if (!pWindow || !pWindow->hWnd || !pRenderer)
                return Error(id, "renderer is not initialized");

            RECT client{}, outer{};
            GetClientRect(pWindow->hWnd, &client);
            GetWindowRect(pWindow->hWnd, &outer);
            CURSORINFO cursor{sizeof(cursor)};
            GetCursorInfo(&cursor);
            DXGI_SWAP_CHAIN_DESC swap{};
            const bool haveSwap = pWindow->pSwapChain && SUCCEEDED(pWindow->pSwapChain->GetDesc(&swap));

            uint32_t overlayWidth = 0, overlayHeight = 0;
            uint16_t cursorX = 0, cursorY = 0;
            bool cefCursor = false;
            if (auto* pApp = m_world.GetOverlayService().GetOverlayApp(); pApp && pApp->GetClient())
                if (auto handler = pApp->GetClient()->GetOverlayRenderHandler())
                {
                    std::tie(overlayWidth, overlayHeight) = handler->GetRenderSize();
                    std::tie(cursorX, cursorY) = handler->GetCursorLocation();
                    cefCursor = handler->IsCursorVisible();
                }

            bool mainMenu = false;
            if (auto* pUI = UI::Get())
                mainMenu = pUI->GetMenuOpen(BSFixedString("Main Menu"));

            const auto style = static_cast<uint64_t>(GetWindowLongPtrW(pWindow->hWnd, GWL_STYLE));
            return Result(id, fmt::format(
                "\"window\":{{\"hwnd\":{},\"foreground\":{},\"style\":{},\"clientWidth\":{},\"clientHeight\":{},"
                "\"outerWidth\":{},\"outerHeight\":{},\"x\":{},\"y\":{}}},"
                "\"renderer\":{{\"width\":{},\"height\":{},\"fullscreen\":{},\"borderless\":{},"
                "\"swapWidth\":{},\"swapHeight\":{},\"swapWindowed\":{}}},"
                "\"overlay\":{{\"active\":{},\"titleScreen\":{},\"width\":{},\"height\":{},"
                "\"cursorVisible\":{},\"cursorX\":{},\"cursorY\":{}}},"
                "\"systemCursor\":{{\"visible\":{}}},\"mainMenuOpen\":{}",
                reinterpret_cast<uintptr_t>(pWindow->hWnd), GetForegroundWindow() == pWindow->hWnd,
                style, client.right, client.bottom,
                outer.right - outer.left, outer.bottom - outer.top, outer.left, outer.top,
                pRenderer->RenderWindowA[0].uiWindowWidth, pRenderer->RenderWindowA[0].uiWindowHeight,
                pRenderer->bAppFullScreen, pRenderer->bBorderlessWindow,
                haveSwap ? swap.BufferDesc.Width : 0, haveSwap ? swap.BufferDesc.Height : 0,
                haveSwap ? swap.Windowed != FALSE : false,
                m_world.GetOverlayService().GetActive(), m_world.GetOverlayService().GetTitleScreen(),
                overlayWidth, overlayHeight, cefCursor, cursorX, cursorY,
                (cursor.flags & CURSOR_SHOWING) != 0, mainMenu));
        }
        return Error(id, "unknown command");
    }
    catch (const std::exception& exception)
    {
        return Error(id, exception.what());
    }
    catch (...)
    {
        return Error(id, "native exception");
    }
}
