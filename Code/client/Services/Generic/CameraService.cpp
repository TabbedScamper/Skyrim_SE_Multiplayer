#include <Services/CameraService.h>

#include <World.h>
#include <Services/PartyService.h>
#include <Services/TransportService.h>
#include <Services/CutsceneFollow.h>
#include <Services/OverlayService.h>

#include <Events/UpdateEvent.h>
#include <Events/DisconnectedEvent.h>

#include <Messages/CameraStateRequest.h>
#include <Messages/NotifyCameraState.h>
#include <Messages/RequestScriptedCamera.h>
#include <Messages/NotifyScriptedCamera.h>

#include <Games/Skyrim/Camera/PlayerCamera.h>
#include <Games/Skyrim/Camera/TESCameraState.h>
#include <Games/Skyrim/PlayerCharacter.h>
#include <Games/Skyrim/AI/Movement/PlayerControls.h>
#include <Games/Skyrim/Forms/TESIdleForm.h>
#include <Games/Skyrim/Misc/BSFixedString.h>
#include <Games/Skyrim/Interface/UI.h>
#include <Games/Skyrim/NetImmerse/NiNode.h>
#include <Games/Skyrim/NetImmerse/NiCamera.h>
#include <Games/Skyrim/Havok/AnimationGraphUpdateTrace.h>
#include <Services/GameTestService.h>

#include <FunctionHook.hpp>

#include <cmath>

namespace
{
constexpr uint64_t kPublishIntervalMs = 33;
constexpr uint64_t kScriptedTimeoutMs = 1000;
std::atomic<uint8_t> s_walkingCamera{};
std::atomic<uint32_t> s_walkingStartIdle{};
std::atomic<uint32_t> s_walkingEndIdle{};
// Legacy transform probe: the 2026-09-22 two-PC intro test showed direct root/world camera
// writes flipping the follower view. Scripted authority below instead uses native GetRotation
// and camera policy; it does not enable this matrix-copy experiment.
// The full host rotation made the follower view flip in the paired intro.
// SmoothCam's source-backed native path first establishes position only;
// trial that independently before touching the camera orientation again.
constexpr bool kEnableCameraRotationWrites = false;
// Keep state changes disabled in the old position probe. The separate scripted path limits
// transitions to states with source-inspected Begin/End contracts and logs their outcomes.
constexpr bool kEnableNativeStateTransitionProbe = false;

bool IsFiniteTransform(const NiTransform& acTransform) noexcept
{
    if (!std::isfinite(acTransform.translate.x) || !std::isfinite(acTransform.translate.y) ||
        !std::isfinite(acTransform.translate.z) || !std::isfinite(acTransform.scale))
        return false;

    for (const auto& row : acTransform.rotate.entry)
    {
        for (const auto value : row)
        {
            if (!std::isfinite(value))
                return false;
        }
    }
    return true;
}

bool ReadNativePoint(const NiPoint3* apPoint, NiPoint3& aResult) noexcept
{
    SIZE_T bytesRead = 0;
    return apPoint && ReadProcessMemory(GetCurrentProcess(), apPoint,
        &aResult, sizeof(aResult), &bytesRead) && bytesRead == sizeof(aResult);
}

template <class T> bool ReadNativeValue(const void* apSource, T& aResult) noexcept
{
    SIZE_T bytesRead = 0;
    return apSource && ReadProcessMemory(GetCurrentProcess(), apSource,
        &aResult, sizeof(aResult), &bytesRead) && bytesRead == sizeof(aResult);
}

}

CameraService* CameraService::s_instance = nullptr;

CameraService::CameraService(World& aWorld, entt::dispatcher& aDispatcher, TransportService& aTransport) noexcept
    : m_world(aWorld)
    , m_transport(aTransport)
    , m_updateConnection(aDispatcher.sink<UpdateEvent>().connect<&CameraService::OnUpdate>(this))
    , m_disconnectedConnection(aDispatcher.sink<DisconnectedEvent>().connect<&CameraService::OnDisconnected>(this))
    , m_cameraStateConnection(aDispatcher.sink<NotifyCameraState>().connect<&CameraService::OnCameraState>(this))
    , m_scriptedCameraConnection(aDispatcher.sink<NotifyScriptedCamera>().connect<&CameraService::OnScriptedCamera>(this))
{
    s_instance = this;
}

void CameraService::NoteWalkingCameraIdle(const uint32_t aFormId, const bool aStart) noexcept
{
    (aStart ? s_walkingStartIdle : s_walkingEndIdle).store(aFormId, std::memory_order_relaxed);
    s_walkingCamera.store(aStart ? 2 : 1, std::memory_order_release);
}

bool CameraService::HasScriptedCamera() const noexcept
{
    const auto& party = m_world.GetPartyService();
    return m_transport.IsConnected() && party.IsInParty() && !party.IsLeader() &&
        party.GetSessionState() >= 2 && CutsceneFollow::IsActive() && m_scripted.Active &&
        m_scripted.Epoch == party.GetStartEpoch() && m_scriptedLeader == party.GetLeaderPlayerId() &&
        GetTickCount64() - m_scriptedReceivedMs < kScriptedTimeoutMs && !IsPresentationBlocked() &&
        !m_world.GetOverlayService().GetActive();
}

void CameraService::ApplyScriptedControls(const ScriptedCameraState& acState) noexcept
{
    auto* pMap = BSInputEnableManager::Get();
    auto* pControls = PlayerControls::GetInstance();
    if (!pMap || !pControls || !pControls->pLookHandler || !pControls->togglePOVHandler)
        return;
    // ControlMap::ToggleControls, ID 68545, VA 140CEFAA0: +120 is live,
    // +124 is a saved native snapshot. Never change the saved snapshot.
    const uint32_t current = *reinterpret_cast<const uint32_t*>(reinterpret_cast<const uint8_t*>(pMap) + 0x120) &
        ScriptedCameraState::kCameraControls;
    const bool look = pControls->pLookHandler->isEnabled;
    const bool pov = pControls->togglePOVHandler->isEnabled;
    const bool script = pControls->Data.povScriptMode;
    if (!m_scriptedControlsHeld)
    {
        m_savedCameraControls = current;
        m_savedLookHandler = look;
        m_savedPovHandler = pov;
        m_savedPovScript = script;
    }
    else
    {
        // Remember local script changes made while the policy is held, for disconnect/timeout.
        const uint32_t changed = current ^ m_appliedCameraControls;
        m_savedCameraControls = (m_savedCameraControls & ~changed) | (current & changed);
        if (look != m_appliedLookHandler) m_savedLookHandler = look;
        if (pov != m_appliedPovHandler) m_savedPovHandler = pov;
        if (script != m_appliedPovScript) m_savedPovScript = script;
    }
    m_scriptedControlsHeld = true;
    m_scriptedReleasePending = true;
    const uint32_t desired = acState.Controls;
    if (const auto disable = current & ~desired)
        pMap->EnableOtherEvent(disable, false, false);
    if (const auto enable = desired & ~current)
        pMap->EnableOtherEvent(enable, true, false);
    pControls->pLookHandler->isEnabled = acState.FreeLook;
    pControls->togglePOVHandler->isEnabled = acState.FreePov;
    pControls->Data.povScriptMode = !acState.FreePov;
    if (!acState.FreeLook)
    {
        pControls->Data.LookInputVec = {};
        pControls->Data.PrevLookVec = {};
    }
    m_appliedCameraControls = desired;
    m_appliedLookHandler = acState.FreeLook;
    m_appliedPovHandler = acState.FreePov;
    m_appliedPovScript = !acState.FreePov;
}

void CameraService::ApplyWalkingCamera(const ScriptedCameraState& acState) noexcept
{
    m_walkingCameraHeld |= s_walkingCamera.load(std::memory_order_acquire) == 2;
    if (acState.WalkingEnd)
        m_walkingEndForm = m_world.GetModSystem().GetGameId(acState.WalkingEnd);
    if (!acState.Walking || acState.Walking == s_walkingCamera.load(std::memory_order_acquire) ||
        GetTickCount64() < m_nextWalkingRetryMs)
        return;
    const auto id = acState.Walking == 2 ? acState.WalkingStart : acState.WalkingEnd;
    auto* pIdle = Cast<TESIdleForm>(TESForm::GetById(m_world.GetModSystem().GetGameId(id)));
    auto* pPlayer = PlayerCharacter::Get();
    if (!pIdle || !pPlayer)
        return;
    m_nextWalkingRetryMs = GetTickCount64() + 500;
    const bool played = pPlayer->PlayIdle(pIdle);
    if (played)
    {
        NoteWalkingCameraIdle(pIdle->formID, acState.Walking == 2);
        m_walkingCameraHeld |= acState.Walking == 2;
        m_nextWalkingRetryMs = 0;
    }
    spdlog::info("Scripted camera: walking={} idle={:08X} played={}", acState.Walking, pIdle->formID, played);
}

void CameraService::ReleaseScriptedCamera() noexcept
{
    if (m_scriptedControlsHeld)
    {
        auto* pMap = BSInputEnableManager::Get();
        auto* pControls = PlayerControls::GetInstance();
        if (pMap)
        {
            const uint32_t current = *reinterpret_cast<const uint32_t*>(reinterpret_cast<const uint8_t*>(pMap) + 0x120);
            const uint32_t owned = ~(current ^ m_appliedCameraControls) & ScriptedCameraState::kCameraControls;
            if (const auto disable = owned & ~m_savedCameraControls & current)
                pMap->EnableOtherEvent(disable, false, false);
            if (const auto enable = owned & m_savedCameraControls & ~current)
                pMap->EnableOtherEvent(enable, true, false);
        }
        if (pControls)
        {
            if (pControls->pLookHandler && pControls->pLookHandler->isEnabled == m_appliedLookHandler)
                pControls->pLookHandler->isEnabled = m_savedLookHandler;
            if (pControls->togglePOVHandler && pControls->togglePOVHandler->isEnabled == m_appliedPovHandler)
                pControls->togglePOVHandler->isEnabled = m_savedPovHandler;
            if (pControls->Data.povScriptMode == m_appliedPovScript)
                pControls->Data.povScriptMode = m_savedPovScript;
        }
        m_scriptedControlsHeld = false;
        spdlog::info("Scripted camera: local camera controls released");
    }
    if (m_walkingCameraHeld && s_walkingCamera.load(std::memory_order_acquire) == 2)
    {
        if (GetTickCount64() < m_nextWalkingRetryMs)
            return;
        m_nextWalkingRetryMs = GetTickCount64() + 500;
        const auto endForm = m_walkingEndForm ? m_walkingEndForm : s_walkingEndIdle.load(std::memory_order_relaxed);
        auto* pIdle = Cast<TESIdleForm>(TESForm::GetById(endForm));
        auto* pPlayer = PlayerCharacter::Get();
        if (!pPlayer || !pPlayer->GetNiNode())
            return;
        // A disconnect can precede the first observed end idle. In that case use the same
        // graph event by name, without a vanilla form ID or a quest-specific fallback.
        static BSFixedString endEvent("IdleWalkingCameraEnd");
        if (!(pIdle ? pPlayer->PlayIdle(pIdle) : pPlayer->SendAnimationEvent(&endEvent)))
            return;
        s_walkingCamera.store(1, std::memory_order_release);
        m_nextWalkingRetryMs = 0;
    }
    m_walkingCameraHeld = false;
    m_walkingEndForm = 0;
}

void CameraService::OnScriptedCamera(const NotifyScriptedCamera& acMessage) noexcept
{
    const auto& party = m_world.GetPartyService();
    const auto& state = acMessage.State;
    if (!m_transport.IsConnected() || !party.IsInParty() || party.IsLeader() || party.GetSessionState() < 2 ||
        acMessage.LeaderId != party.GetLeaderPlayerId() || state.Epoch != party.GetStartEpoch() || !state.IsValid())
        return;
    if (m_scriptedLeader == acMessage.LeaderId && m_scripted.Epoch == state.Epoch && state.Sequence <= m_scripted.Sequence)
        return;
    m_scripted = state;
    m_scriptedLeader = acMessage.LeaderId;
    m_scriptedReceivedMs = GetTickCount64();
}

void CameraService::UpdateScriptedCamera() noexcept
{
    const auto& party = m_world.GetPartyService();
    const auto now = GetTickCount64();
    if (m_scriptedEpoch != party.GetStartEpoch())
    {
        ReleaseScriptedCamera();
        m_scripted = {};
        m_scriptedEpoch = party.GetStartEpoch();
        m_scriptedNextSendMs = 0;
        m_scriptedReleasePending = false;
        s_walkingCamera.store(0, std::memory_order_release);
    }
    auto* pCamera = PlayerCamera::Get();
    auto* pPlayer = PlayerCharacter::Get();
    if (party.IsLeader())
    {
        ReleaseScriptedCamera();
        if (IsPresentationBlocked() || m_world.GetOverlayService().GetActive())
            return;
        const bool active = CutsceneFollow::IsActive();
        if (!party.GetStartEpoch() || party.GetSessionState() < 2 || !pCamera || !pCamera->state ||
            pCamera->state->id >= 13 || !pCamera->cameraNode || !pPlayer || !pPlayer->parentCell || !pPlayer->GetNiNode() ||
            (active == m_scriptedWasActive && now < m_scriptedNextSendMs))
            return;
        auto* pMap = BSInputEnableManager::Get();
        auto* pControls = PlayerControls::GetInstance();
        if (!pMap || !pControls || !pControls->pLookHandler || !pControls->togglePOVHandler)
            return;
        RequestScriptedCamera request;
        auto& state = request.State;
        state.Epoch = party.GetStartEpoch();
        state.Sequence = ++m_scriptedSequence;
        state.Active = active;
        state.StateId = static_cast<uint8_t>(pCamera->state->id);
        state.Controls = *reinterpret_cast<const uint32_t*>(reinterpret_cast<const uint8_t*>(pMap) + 0x120) &
            ScriptedCameraState::kCameraControls;
        state.FreeLook = (state.Controls & 2) && pControls->pLookHandler->isEnabled && !pControls->bBlockPlayerInput;
        state.FreePov = (state.Controls & 0x20) && pControls->togglePOVHandler->isEnabled &&
            !pControls->Data.povScriptMode && !pControls->Data.povBeastMode && !pControls->bBlockPlayerInput;
        state.Pitch = pPlayer->rotation.x;
        state.Heading = pPlayer->rotation.z;
        state.Walking = s_walkingCamera.load(std::memory_order_acquire);
        m_world.GetModSystem().GetServerModId(s_walkingStartIdle.load(std::memory_order_relaxed), state.WalkingStart);
        m_world.GetModSystem().GetServerModId(s_walkingEndIdle.load(std::memory_order_relaxed), state.WalkingEnd);
        // Same quaternion contract consumed by native state Update, before camera shake and node transforms.
        (*reinterpret_cast<CameraRotationFn**>(pCamera->state))[4](pCamera->state, state.Rotation.data());
        if (!state.IsValid())
            return;
        const uint8_t policy = state.Controls | (state.FreeLook ? 4 : 0) | (state.FreePov ? 8 : 0) | (active ? 0x80 : 0);
        if (state.StateId != m_scriptedLogState || policy != m_scriptedLogPolicy)
        {
            spdlog::info("Scripted camera host: active={} state={} controls={:02X} freeLook={} freePov={} walking={}",
                active, state.StateId, state.Controls, state.FreeLook, state.FreePov, state.Walking);
            m_scriptedLogState = state.StateId;
            m_scriptedLogPolicy = policy;
        }
        m_transport.Send(request);
        m_scriptedWasActive = active;
        m_scriptedNextSendMs = now + (active ? kPublishIntervalMs : 1000);
        return;
    }
    if (!HasScriptedCamera() || !pCamera || !pCamera->state || !pPlayer)
    {
        // A final host release carries the current policy, avoiding restoration of old cutscene locks.
        if (m_scriptedReleasePending && !m_scripted.Active && m_scripted.Epoch == party.GetStartEpoch() &&
            m_scriptedLeader == party.GetLeaderPlayerId() && now - m_scriptedReceivedMs < kScriptedTimeoutMs &&
            !IsPresentationBlocked() && !m_world.GetOverlayService().GetActive())
        {
            ApplyScriptedControls(m_scripted);
            m_savedCameraControls = m_appliedCameraControls;
            m_savedLookHandler = m_appliedLookHandler;
            m_savedPovHandler = m_appliedPovHandler;
            m_savedPovScript = m_appliedPovScript;
            m_scriptedReleasePending = false;
        }
        ReleaseScriptedCamera();
        return;
    }
    ApplyScriptedControls(m_scripted);
    ApplyWalkingCamera(m_scripted);
    const auto localState = pCamera->state->id;
    if (localState != m_scripted.StateId)
    {
        // These native states bind their own local camera objects in Begin. Transitional/menu,
        // mount and VATS states require additional local engine context; let Skyrim create those.
        if (m_scripted.StateId == 0)
            pCamera->ForceFirstPerson();
        else if (m_scripted.StateId == 9)
            pCamera->ForceThirdPerson();
        else if (m_scripted.StateId == 5 || m_scripted.StateId == 8)
        {
            if (auto* pTarget = pCamera->GetStateById(m_scripted.StateId))
                pCamera->SetState(pTarget);
        }
    }
    if (!m_scripted.FreeLook)
        pPlayer->SetRotation(m_scripted.Pitch, pPlayer->rotation.y, m_scripted.Heading);
    const uint8_t policy = m_scripted.Controls | (m_scripted.FreeLook ? 4 : 0) | (m_scripted.FreePov ? 8 : 0);
    if (m_scripted.StateId != m_scriptedLogState || policy != m_scriptedLogPolicy ||
        pCamera->state->id != m_scriptedLogLocalState)
    {
        spdlog::info("Scripted camera follower: local={} host={} controls={:02X} freeLook={} freePov={} walking={}",
            pCamera->state->id, m_scripted.StateId, m_scripted.Controls, m_scripted.FreeLook, m_scripted.FreePov, m_scripted.Walking);
        m_scriptedLogState = m_scripted.StateId;
        m_scriptedLogPolicy = policy;
        m_scriptedLogLocalState = static_cast<uint8_t>(pCamera->state->id);
    }
}

void CameraService::HookCameraRotation(TESCameraState* apState, float* apRotation) noexcept
{
    auto* pService = s_instance;
    if (!pService || !apState || !apRotation)
        return;
    auto* pVtable = *reinterpret_cast<void***>(apState);
    for (size_t i = 0; i < pService->m_hookCount; ++i)
    {
        if (pService->m_hookedVtables[i] == pVtable)
        {
            if (pService->m_originalRotations[i])
                pService->m_originalRotations[i](apState, apRotation);
            break;
        }
    }
    auto* pCamera = PlayerCamera::Get();
    if (pCamera && apState->camera == pCamera && pCamera->state == apState &&
        apState->id == pService->m_scripted.StateId && !pService->m_scripted.FreeLook && pService->HasScriptedCamera())
        std::copy(pService->m_scripted.Rotation.begin(), pService->m_scripted.Rotation.end(), apRotation);
}

CameraService::Diagnostic CameraService::GetDiagnostic() const noexcept
{
    const auto* pCamera = PlayerCamera::Get();
    return {m_world.GetPartyService().IsFollowerCinematicInputGated(),
        m_hasSnapshot,
        pCamera && pCamera->state && pCamera->state->id < 13 ?
            static_cast<uint8_t>(pCamera->state->id) : uint8_t{0xFF},
        m_hasSnapshot ? m_snapshot.StateId : uint8_t{0xFF},
        m_lastReceivedTick, m_receivedPackets,
        m_nativePostUpdates.load(std::memory_order_relaxed),
        m_lastNativeThreadId.load(std::memory_order_relaxed),
        m_hookCount};
}

CameraService::NativeUpdateTrace CameraService::GetNativeUpdateTrace() const noexcept
{
    NativeUpdateTrace result{};
    std::lock_guard lock(m_nativeTraceMutex);
    const auto now = GetTickCount64();
    const size_t start = (m_nativeTraceNext + m_nativeTrace.size() -
        m_nativeTraceCount) % m_nativeTrace.size();
    for (size_t i = 0; i < m_nativeTraceCount; ++i)
    {
        const auto& sample = m_nativeTrace[(start + i) % m_nativeTrace.size()];
        if (now >= sample.TimeMs && now - sample.TimeMs <= 5000)
            result.Samples[result.Count++] = sample;
    }
    return result;
}

void CameraService::SetPositionProbeEnabled(bool aEnabled) noexcept
{
    m_positionProbeEnabled.store(aEnabled, std::memory_order_release);
}

bool CameraService::IsPositionProbeEnabled() const noexcept
{
    return m_positionProbeEnabled.load(std::memory_order_acquire);
}

bool CameraService::IsPresentationBlocked() const noexcept
{
    const auto* pUI = UI::Get();
    return !pUI || pUI->GetMenuOpen(BSFixedString("Loading Menu")) ||
        pUI->GetMenuOpen(BSFixedString("RaceSex Menu")) || pUI->GetMenuOpen(BSFixedString("Main Menu"));
}

bool CameraService::Capture(CameraStateSnapshot& aSnapshot) const noexcept
{
    const auto* pCamera = PlayerCamera::Get();
    if (!pCamera || !pCamera->cameraNode || !pCamera->state ||
        pCamera->state->id >= 13 || !IsFiniteTransform(pCamera->cameraNode->world))
        return false;

    const auto& transform = pCamera->cameraNode->world;
    aSnapshot.Position = transform.translate;
    for (size_t row = 0; row < 3; ++row)
    {
        for (size_t column = 0; column < 3; ++column)
            aSnapshot.Rotation[row * 3 + column] = transform.rotate.entry[row][column];
    }
    aSnapshot.Scale = transform.scale;
    aSnapshot.Fov = pCamera->GetWorldFov();
    aSnapshot.StateId = static_cast<uint8_t>(pCamera->state->id);
    return aSnapshot.IsValid();
}

void CameraService::Apply(const CameraStateSnapshot& acSnapshot) noexcept
{
    auto* pCamera = PlayerCamera::Get();
    if (!pCamera || !pCamera->cameraNode || !acSnapshot.IsValid())
        return;

    // Native camera states have Begin/End side effects. Transition once per
    // host state change, never once per network packet/frame.
    const uint8_t currentStateId = pCamera->state && pCamera->state->id < 13
        ? static_cast<uint8_t>(pCamera->state->id)
        : 0xFF;
    if (currentStateId != acSnapshot.StateId)
    {
        if (m_lastStateMismatch != acSnapshot.StateId)
        {
            spdlog::warn("Camera authority state mismatch (local {}, host {}); native SetState probe is disabled",
                currentStateId, acSnapshot.StateId);
            m_lastStateMismatch = acSnapshot.StateId;
        }

        if constexpr (kEnableNativeStateTransitionProbe)
        {
            auto* pTargetState = pCamera->GetStateById(acSnapshot.StateId);
            if (!pTargetState || !pCamera->SetState(pTargetState))
                return;
        }
        else
        {
            // Until SetState is validated on the live 1.7.104 executable,
            // only take transform authority when Skyrim has independently
            // reached the same camera state on both peers.
            return;
        }
    }
    else
        m_lastStateMismatch = 0xFF;
    m_appliedStateId = acSnapshot.StateId;

    NiTransform transform = pCamera->cameraNode->world;
    transform.translate = acSnapshot.Position;
    if constexpr (kEnableCameraRotationWrites)
    {
        for (size_t row = 0; row < 3; ++row)
        {
            for (size_t column = 0; column < 3; ++column)
                transform.rotate.entry[row][column] = acSnapshot.Rotation[row * 3 + column];
        }
        transform.scale = acSnapshot.Scale;
    }

    // SmoothCam demonstrates that Skyrim consumes all three locations during
    // the same camera update. Set root local/world and child-camera world after
    // the native state update so the engine cannot overwrite the host pose.
    if constexpr (kEnableCameraRotationWrites)
    {
        pCamera->cameraNode->local = transform;
        pCamera->cameraNode->world = transform;
        pCamera->cameraNode->previousWorld = transform;
        if (auto* pNiCamera = pCamera->GetNiCamera())
        {
            pNiCamera->world = transform;
            pNiCamera->previousWorld = transform;
        }
    }
    else
    {
        pCamera->cameraNode->local.translate = transform.translate;
        pCamera->cameraNode->world.translate = transform.translate;
        pCamera->cameraNode->previousWorld.translate = transform.translate;
        if (auto* pNiCamera = pCamera->GetNiCamera())
        {
            pNiCamera->world.translate = transform.translate;
            pNiCamera->previousWorld.translate = transform.translate;
        }
    }
    pCamera->SetWorldFov(acSnapshot.Fov);
}

void CameraService::ApplyAfterNativeUpdate() noexcept
{
    if (!IsPositionProbeEnabled())
        return;

    const auto& party = m_world.GetPartyService();
    if (!m_hasSnapshot || IsPresentationBlocked() ||
        !party.IsFollowerCinematicInputGated() ||
        m_snapshot.AuthorityEpoch != party.GetStartEpoch())
        return;

    Apply(m_snapshot);
}

void CameraService::HookCameraUpdate(TESCameraState* apState, void* apNextState) noexcept
{
    auto* pService = s_instance;
    if (!pService || !apState)
        return;

    // Papyrus can change controls/states after UpdateEvent. Reconcile immediately before the
    // native evaluation as well. TESCamera::Update (33025, 140558E70) reads currentState again
    // after this callback, so evaluate the new state's original Update after a transition.
    auto* pCamera = PlayerCamera::Get();
    if (pCamera && pCamera->state == apState && pService->m_transport.IsConnected() &&
        pService->m_world.GetPartyService().IsInParty() && !pService->m_world.GetPartyService().IsLeader())
    {
        pService->UpdateScriptedCamera();
        apState = pCamera->state;
        if (!apState)
            return;
    }

    auto* pVtable = *reinterpret_cast<void***>(apState);
    CameraUpdateFn pOriginal = nullptr;
    for (size_t i = 0; i < pService->m_hookCount; ++i)
    {
        if (pService->m_hookedVtables[i] == pVtable)
        {
            pOriginal = pService->m_originalUpdates[i];
            break;
        }
    }

    const uint64_t now = GetTickCount64();
    uint64_t nextTrace = pService->m_nextNativeTraceMs.load(std::memory_order_relaxed);
    const bool sampleTrace = GameTestService::IsDiagnosticCaptureArmed() && now >= nextTrace &&
        pService->m_nextNativeTraceMs.compare_exchange_strong(nextTrace, now + 50,
            std::memory_order_relaxed);
    auto* pPlayerCamera = PlayerCamera::Get();
    auto* pBeforeNode = sampleTrace && pPlayerCamera &&
        apState->camera == pPlayerCamera && pOriginal ?
        pPlayerCamera->cameraNode : nullptr;
    NiPoint3 before{};
    NiPoint3 beforeLocal{};
    NiPoint3 beforeParentWorld{};
    NiAVObject* pBeforeParent = nullptr;
    float pitchBefore{};
    float targetPitchBefore{};
    NiAVObject* pFirstPersonObject = nullptr;
    NiPoint3 objectBefore{};
    bool firstPersonBeforeReadable = false;
    bool firstPersonObjectBeforeReadable = false;
    float graphPitchBefore{};
    bool graphPitchBeforeReadable = false;
    if (pBeforeNode)
    {
        before = pBeforeNode->world.translate;
        beforeLocal = pBeforeNode->local.translate;
        pBeforeParent = pBeforeNode->parent;
        if (pBeforeParent && !ReadNativePoint(
                &pBeforeParent->world.translate, beforeParentWorld))
            pBeforeParent = nullptr;
        // FirstPersonState fields are pinned to CommonLibSSE-NG's 1.7-aware
        // layout. Observe both sides of this exact native state update to
        // distinguish an upstream graph/object input from a camera-state write.
        if (apState->id == 0)
        {
            static BSFixedString pitchOffsetName("PitchOffset");
            if (auto* pPlayer = PlayerCharacter::Get())
                graphPitchBeforeReadable =
                    pPlayer->animationGraphHolder.GetVariableFloat(
                        &pitchOffsetName, &graphPitchBefore) &&
                    std::isfinite(graphPitchBefore);
            const auto* pState = reinterpret_cast<const uint8_t*>(apState);
            firstPersonBeforeReadable =
                ReadNativeValue(pState + 0x74, pitchBefore) &&
                ReadNativeValue(pState + 0x78, targetPitchBefore) &&
                std::isfinite(pitchBefore) &&
                std::isfinite(targetPitchBefore);
            if (ReadNativeValue(pState + 0x58, pFirstPersonObject) &&
                pFirstPersonObject)
                firstPersonObjectBeforeReadable = ReadNativePoint(
                    &pFirstPersonObject->world.translate, objectBefore);
        }
    }

    if (pOriginal)
    {
        pOriginal(apState, apNextState);
        if (apState->id == 0 && apState->camera == PlayerCamera::Get())
            AnimationGraphUpdateTrace::NoteNativeCameraUpdate();
    }
    if (pBeforeNode)
    {
        auto* pAfterCamera = PlayerCamera::Get();
        if (pAfterCamera && pAfterCamera->cameraNode == pBeforeNode)
        {
            const auto& after = pBeforeNode->world.translate;
            if (std::isfinite(before.x) && std::isfinite(before.y) &&
                std::isfinite(before.z) && std::isfinite(after.x) &&
                std::isfinite(after.y) && std::isfinite(after.z) &&
                std::isfinite(beforeLocal.x) &&
                std::isfinite(beforeLocal.y) &&
                std::isfinite(beforeLocal.z) &&
                std::isfinite(pBeforeNode->local.translate.x) &&
                std::isfinite(pBeforeNode->local.translate.y) &&
                std::isfinite(pBeforeNode->local.translate.z))
            {
                NativeUpdateSample sample{};
                sample.TimeMs = now;
                sample.StateId = apState->id < 13 ?
                    static_cast<uint8_t>(apState->id) : 0xFF;
                sample.Before[0] = before.x;
                sample.Before[1] = before.y;
                sample.Before[2] = before.z;
                sample.After[0] = after.x;
                sample.After[1] = after.y;
                sample.After[2] = after.z;
                const auto& afterLocal = pBeforeNode->local.translate;
                sample.BeforeLocal[0] = beforeLocal.x;
                sample.BeforeLocal[1] = beforeLocal.y;
                sample.BeforeLocal[2] = beforeLocal.z;
                sample.AfterLocal[0] = afterLocal.x;
                sample.AfterLocal[1] = afterLocal.y;
                sample.AfterLocal[2] = afterLocal.z;
                NiPoint3 afterParentWorld{};
                sample.ParentStable = pBeforeParent &&
                    pBeforeNode->parent == pBeforeParent &&
                    std::isfinite(beforeParentWorld.x) &&
                    std::isfinite(beforeParentWorld.y) &&
                    std::isfinite(beforeParentWorld.z) &&
                    ReadNativePoint(
                        &pBeforeParent->world.translate, afterParentWorld) &&
                    std::isfinite(afterParentWorld.x) &&
                    std::isfinite(afterParentWorld.y) &&
                    std::isfinite(afterParentWorld.z);
                if (sample.ParentStable)
                {
                    sample.BeforeParentWorld[0] = beforeParentWorld.x;
                    sample.BeforeParentWorld[1] = beforeParentWorld.y;
                    sample.BeforeParentWorld[2] = beforeParentWorld.z;
                    sample.AfterParentWorld[0] = afterParentWorld.x;
                    sample.AfterParentWorld[1] = afterParentWorld.y;
                    sample.AfterParentWorld[2] = afterParentWorld.z;
                }
                if (firstPersonBeforeReadable)
                {
                    const auto* pState = reinterpret_cast<const uint8_t*>(apState);
                    sample.FirstPersonStateReadable =
                        ReadNativeValue(pState + 0x74, sample.PitchAfter) &&
                        ReadNativeValue(pState + 0x78, sample.TargetPitchAfter) &&
                        std::isfinite(sample.PitchAfter) &&
                        std::isfinite(sample.TargetPitchAfter);
                    if (sample.FirstPersonStateReadable)
                    {
                        sample.PitchBefore = pitchBefore;
                        sample.TargetPitchBefore = targetPitchBefore;
                    }
                    else
                    {
                        sample.PitchAfter = 0.f;
                        sample.TargetPitchAfter = 0.f;
                    }
                }
                if (firstPersonObjectBeforeReadable &&
                    std::isfinite(objectBefore.x) &&
                    std::isfinite(objectBefore.y) &&
                    std::isfinite(objectBefore.z))
                {
                    NiAVObject* pAfterObject = nullptr;
                    NiPoint3 objectAfter{};
                    sample.FirstPersonObjectStable =
                        ReadNativeValue(reinterpret_cast<const uint8_t*>(apState) +
                            0x58, pAfterObject) &&
                        pAfterObject == pFirstPersonObject &&
                        ReadNativePoint(&pAfterObject->world.translate,
                            objectAfter) &&
                        std::isfinite(objectAfter.x) &&
                        std::isfinite(objectAfter.y) &&
                        std::isfinite(objectAfter.z);
                    if (sample.FirstPersonObjectStable)
                    {
                        sample.ObjectBefore[0] = objectBefore.x;
                        sample.ObjectBefore[1] = objectBefore.y;
                        sample.ObjectBefore[2] = objectBefore.z;
                        sample.ObjectAfter[0] = objectAfter.x;
                        sample.ObjectAfter[1] = objectAfter.y;
                        sample.ObjectAfter[2] = objectAfter.z;
                    }
                }
                if (graphPitchBeforeReadable)
                {
                    static BSFixedString pitchOffsetName("PitchOffset");
                    float graphPitchAfter{};
                    if (auto* pPlayer = PlayerCharacter::Get())
                        sample.GraphPitchReadable =
                            pPlayer->animationGraphHolder.GetVariableFloat(
                                &pitchOffsetName, &graphPitchAfter) &&
                            std::isfinite(graphPitchAfter);
                    if (sample.GraphPitchReadable)
                    {
                        sample.GraphPitchBefore = graphPitchBefore;
                        sample.GraphPitchAfter = graphPitchAfter;
                    }
                }
                std::lock_guard lock(pService->m_nativeTraceMutex);
                pService->m_nativeTrace[pService->m_nativeTraceNext] = sample;
                pService->m_nativeTraceNext = (pService->m_nativeTraceNext + 1) %
                    pService->m_nativeTrace.size();
                pService->m_nativeTraceCount = std::min(pService->m_nativeTraceCount + 1,
                    pService->m_nativeTrace.size());
            }
        }
    }
    pService->m_nativePostUpdates.fetch_add(1, std::memory_order_relaxed);
    pService->m_lastNativeThreadId.store(GetCurrentThreadId(),
        std::memory_order_relaxed);
    pService->ApplyAfterNativeUpdate();
}

void CameraService::InstallStateUpdateHooks() noexcept
{
    auto* pCamera = PlayerCamera::Get();
    if (!pCamera)
        return;

    for (uint8_t stateId = 0; stateId < 13 && m_hookCount < m_hookedVtables.size(); ++stateId)
    {
        auto* pState = pCamera->GetStateById(stateId);
        if (!pState)
            continue;

        auto* pVtable = *reinterpret_cast<void***>(pState);
        if (!pVtable || std::find(m_hookedVtables.begin(), m_hookedVtables.begin() + m_hookCount, pVtable) !=
                m_hookedVtables.begin() + m_hookCount)
            continue;

        const auto pOriginal = TiltedPhoques::HookVTable(pState, 3, &CameraService::HookCameraUpdate);
        if (!pOriginal || pOriginal == &CameraService::HookCameraUpdate)
            continue;
        m_hookedVtables[m_hookCount] = pVtable;
        m_originalUpdates[m_hookCount] = pOriginal;
        m_originalRotations[m_hookCount] = TiltedPhoques::HookVTable(pState, 4, &CameraService::HookCameraRotation);
        ++m_hookCount;
    }
}

void CameraService::OnUpdate(const UpdateEvent&) noexcept
{
    InstallStateUpdateHooks();

    const auto& party = m_world.GetPartyService();
    if (!m_transport.IsConnected() || !party.IsInParty())
    {
        Clear();
        return;
    }

    UpdateScriptedCamera();

    if (party.IsLeader())
    {
        if (IsPresentationBlocked() || party.GetStartEpoch() == 0)
            return;

        const auto tick = m_transport.GetClock().GetCurrentTick();
        if (tick < m_nextPublishTick)
            return;
        m_nextPublishTick = tick + kPublishIntervalMs;

        CameraStateRequest request{};
        request.Snapshot.Tick = tick;
        request.Snapshot.AuthorityEpoch = party.GetStartEpoch();
        if (Capture(request.Snapshot))
            m_transport.Send(request);
        return;
    }

    // Followers apply from HookCameraUpdate after Skyrim evaluates the native
    // state. Applying here (before GameVM::Update) is overwritten later.
}

void CameraService::OnDisconnected(const DisconnectedEvent&) noexcept
{
    Clear();
}

void CameraService::OnCameraState(const NotifyCameraState& acMessage) noexcept
{
    const auto& party = m_world.GetPartyService();
    const auto& snapshot = acMessage.Snapshot;
    if (!party.IsFollowerCinematicInputGated() ||
        snapshot.AuthorityEpoch != party.GetStartEpoch() ||
        snapshot.Tick <= m_lastReceivedTick || !snapshot.IsValid())
        return;

    m_snapshot = snapshot;
    m_lastReceivedTick = snapshot.Tick;
    m_hasSnapshot = true;
    ++m_receivedPackets;
}

void CameraService::Clear() noexcept
{
    ReleaseScriptedCamera();
    m_scripted = {};
    m_scriptedReceivedMs = 0;
    m_scriptedNextSendMs = 0;
    m_scriptedEpoch = 0;
    m_scriptedLeader = 0;
    m_scriptedWasActive = false;
    m_scriptedReleasePending = false;
    m_scriptedLogState = 0xFF;
    m_scriptedLogPolicy = 0xFF;
    m_scriptedLogLocalState = 0xFF;
    SetPositionProbeEnabled(false);
    m_snapshot = {};
    m_lastReceivedTick = 0;
    m_nextPublishTick = 0;
    m_appliedStateId = 0xFF;
    m_lastStateMismatch = 0xFF;
    m_hasSnapshot = false;
}
