#include <Services/CutsceneFollow.h>

#include <World.h>
#include <Components.h>
#include <Games/Skyrim/Actor.h>
#include <Games/Skyrim/PlayerCharacter.h>
#include <Games/Skyrim/AI/Movement/PlayerControls.h>
#include <Games/Skyrim/NetImmerse/NiNode.h>
#include <Games/Skyrim/Havok/PoseCopyAuthority.h>
#include <Services/PlayerCollision.h>

namespace
{
bool s_active{};
bool s_movementDisabledByUs{};
std::unordered_map<uint32_t, bool> s_hiddenByUs; // form id -> hidden by this mode

void SetHidden(Actor* apActor, const bool aHidden) noexcept
{
    auto* pRoot = apActor ? apActor->GetNiNode() : nullptr;
    if (!pRoot)
        return;
    const bool hidden = (pRoot->flags & 1) != 0;
    if (aHidden && !hidden)
    {
        pRoot->flags |= 1;
        s_hiddenByUs[apActor->formID] = true;
    }
}

void Restore() noexcept
{
    for (const auto& [formId, hidden] : s_hiddenByUs)
    {
        if (auto* pActor = Cast<Actor>(TESForm::GetById(formId)); pActor && pActor->GetNiNode())
            pActor->GetNiNode()->flags &= ~1u;
    }
    s_hiddenByUs.clear();
    PoseCopyAuthority::SetLocalMirror(0);
    PlayerCollision::SetLocalPassThrough(false);
    if (s_movementDisabledByUs)
    {
        if (auto* pControls = PlayerControls::GetInstance(); pControls && pControls->pMovementHandler)
            pControls->pMovementHandler->isEnabled = true;
        s_movementDisabledByUs = false;
    }
}
} // namespace

namespace CutsceneFollow
{
void Update(World& aWorld, const bool aActive, const bool aIsLeader, const uint32_t aLeaderPlayerId) noexcept
{
    if (!aActive)
    {
        if (s_active)
        {
            Restore();
            spdlog::info("Cutscene follow: the leader is free; players shown and released");
        }
        s_active = false;
        return;
    }
    auto* pPlayer = PlayerCharacter::Get();
    if (!pPlayer)
        return;

    // The other players' characters here, and the leader's among them.
    Actor* pLeader = nullptr;
    auto view = aWorld.view<FormIdComponent, PlayerComponent>();
    for (auto entity : view)
    {
        const auto formId = view.get<FormIdComponent>(entity).Id;
        if (formId == 0x14)
            continue;
        auto* pActor = Cast<Actor>(TESForm::GetById(formId));
        if (!pActor)
            continue;
        // Every other player is hidden: this player's own character plays the scene here.
        SetHidden(pActor, true);
        if (view.get<PlayerComponent>(entity).Id == aLeaderPlayerId)
            pLeader = pActor;
    }

    if (!s_active)
        spdlog::info("Cutscene follow: the leader has no free control; {}", aIsLeader ? "other players hidden" :
            "this player plays the leader's part");
    s_active = true;
    if (aIsLeader || !pLeader)
        return;

    // Follower: its character takes the leader's place, heading and pose; no movement of its own.
    PoseCopyAuthority::SetLocalMirror(pLeader->formID);
    PlayerCollision::SetLocalPassThrough(true);
    if (auto* pControls = PlayerControls::GetInstance(); pControls && pControls->pMovementHandler &&
        pControls->pMovementHandler->isEnabled)
    {
        pControls->pMovementHandler->isEnabled = false;
        s_movementDisabledByUs = true;
    }
    pPlayer->ForcePosition(pLeader->position);
    pPlayer->SetRotation(pPlayer->rotation.x, pPlayer->rotation.y, pLeader->rotation.z);
}
} // namespace CutsceneFollow
