#pragma once

#include <Structs/ActionEvent.h>
#include <atomic>

struct ActorExtension
{
    enum
    {
        kRemote = 1 << 0,
        kPlayer = 1 << 1,
    };

    enum class ReconciliationStage
    {
        None,
        WaitingForDisable,
        WaitingFor3D
    };

    bool IsRemote() const noexcept;
    bool IsLocal() const noexcept;
    bool IsPlayer() const noexcept;
    bool IsRemotePlayer() const noexcept;
    bool IsLocalPlayer() const noexcept;
    void SetRemote(bool aSet) noexcept;
    void SetPlayer(bool aSet) noexcept;

    ActionEvent LatestAnimation{};
    // Set by the action sink after the native hook captures LatestAnimation.
    // 0 = no sink result, 1 = queued for owner, 2 = buffered before owner
    // assignment, 3 = no matching actor entity/buffer at dispatch time.
    uint8_t LatestAnimationDispatch{};
    // Local form ID selected from the host's presented combat-target stream.
    // UINT32_MAX disables native selector authority until a resolvable point
    // has reached the presentation timeline.
    std::atomic<uint32_t> PresentedCombatTargetFormId{0xFFFFFFFFu};
    size_t GraphDescriptorHash = 0;

    // TODO: atomic? bool instead? maybe simplify to `IsReenabling()` ?
    // Protects discovery while rebuilding a leveled NPC.
    ReconciliationStage Reconciliation{ReconciliationStage::None};

private:
    uint32_t onlineFlags{0};
};
