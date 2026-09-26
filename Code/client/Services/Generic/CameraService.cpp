#include <Services/CameraService.h>

#include <World.h>
#include <Services/PartyService.h>
#include <Services/TransportService.h>

#include <Events/UpdateEvent.h>
#include <Events/DisconnectedEvent.h>

#include <Messages/CameraStateRequest.h>
#include <Messages/NotifyCameraState.h>

#include <Games/Skyrim/Camera/PlayerCamera.h>
#include <Games/Skyrim/Camera/TESCameraState.h>
#include <Games/Skyrim/PlayerCharacter.h>
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
// The 2026-09-22 two-PC intro test showed direct root/world camera writes
// flipping the follower view. Keep the authority stream and diagnostics, but
// do not mutate the native camera until its state/local-space contract is
// validated independently of cart physics.
// The full host rotation made the follower view flip in the paired intro.
// SmoothCam's source-backed native path first establishes position only;
// trial that independently before touching the camera orientation again.
constexpr bool kEnableCameraRotationWrites = false;
// TESCamera::SetState AE ID 33026 is source-backed but has not yet been
// exercised on runtime 1.7.104. Keep native state mutation disabled until the
// explicit probe passes; transform/FOV authority remains active when states
// already agree.
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
{
    s_instance = this;
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
        pUI->GetMenuOpen(BSFixedString("RaceSex Menu"));
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
    SetPositionProbeEnabled(false);
    m_snapshot = {};
    m_lastReceivedTick = 0;
    m_nextPublishTick = 0;
    m_appliedStateId = 0xFF;
    m_lastStateMismatch = 0xFF;
    m_hasSnapshot = false;
}
