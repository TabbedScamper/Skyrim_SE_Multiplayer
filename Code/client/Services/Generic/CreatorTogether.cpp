#include <Services/CreatorTogether.h>

#include <World.h>
#include <Components.h>
#include <Games/Skyrim/Actor.h>
#include <Games/Skyrim/PlayerCharacter.h>
#include <Games/Skyrim/Interface/UI.h>
#include <Games/Skyrim/NetImmerse/NiNode.h>
#include <Games/Skyrim/BSGraphics/BSGraphicsRenderer.h>
#include <Services/OverlayService.h>

#include <atomic>
#include <mutex>

namespace
{
using TAddMessage = void(void*, const BSFixedString*, UIMessage::UI_MESSAGE_TYPE, void*);
TAddMessage* s_realAddMessage{};

std::atomic<bool> s_holding{};
std::atomic<bool> s_releaseRequested{};
std::atomic<bool> s_done{};
std::atomic<bool> s_releasing{};

// RaceSexMenu's begin-closing (ID 52388, VA 14096b7d0; called by ChangeName after Done and the name):
// Scaleform FadeOut, camera saved, menu byte +0x1a0 = 1 (closing), after which the menu's per-frame
// character update stops and every pose froze. Held while the party is still creating: the menu stays
// in its editing state (idles keep playing); run for real by Release. Found by Reviewer B from the corpus.
using TBeginClosing = void(void*);
TBeginClosing* s_realBeginClosing{};
std::atomic<void*> s_heldMenu{};

void HookBeginClosing(void* apMenu)
{
    if (s_holding.load() && !s_releasing.load())
    {
        s_heldMenu.store(apMenu);
        if (!s_done.exchange(true))
            spdlog::info("Character creator: Done held in the editing state until every player is done");
        return;
    }
    s_realBeginClosing(apMenu);
}

std::mutex s_lock;
bool s_active{};                   // the creator is open here, in a party New Game
std::vector<uint32_t> s_players;   // every player's character here (0x14 for this player), in party order
size_t s_view{SIZE_MAX};           // index into s_players of the character shown
bool s_prevKey{};
bool s_nextKey{};
bool s_hintShown{};
bool s_logAfterSwitch{};
std::unordered_map<uint32_t, bool> s_remoteReady; // form id -> that player clicked Done
int s_bannerPlayer{-1};
int s_bannerCount{-1};
int s_bannerReady{-1};

bool IsRaceSexMenu(const BSFixedString* apName) noexcept
{
    return apName && apName->data && std::strcmp(apName->data, "RaceSex Menu") == 0;
}

// Held while the party is still creating: the player's Done closes nothing yet.
void HookAddMessage(void* apQueue, const BSFixedString* apName, UIMessage::UI_MESSAGE_TYPE aType, void* apData)
{
    if (s_holding.load() && !s_releasing.load() && (aType == UIMessage::kHide || aType == UIMessage::kForceHide) &&
        IsRaceSexMenu(apName))
    {
        if (!s_done.exchange(true))
            spdlog::info("Character creator: Done held until every player is done");
        return;
    }
    s_realAddMessage(apQueue, apName, aType, apData);
}

void ShowNotice(const char* apText) noexcept
{
    // SendHUDMessage::ShowHUDMessage (ID 52933).
    using TShowHUDMessage = void(const char*, const char*, bool);
    POINTER_SKYRIMSE(TShowHUDMessage, s_showHUDMessage, 52933);
    s_showHUDMessage.Get()(apText, nullptr, true);
}

// Hidden by hiding its meshes (NiAVObject flags bit 0 on every leaf of its 3D): the creator
// ignores the root's flag (the engine sets it on the player every frame), and shrinking the root
// dragged the head bone, which the creator camera tracks, down to the feet. Restored when shown.
std::unordered_map<uint32_t, std::vector<NiAVObject*>> s_hiddenMeshes;

void CollectLeaves(NiAVObject* apNode, std::vector<NiAVObject*>& arLeaves, int aDepth) noexcept
{
    if (!apNode || aDepth > 64)
        return;
    auto* pNode = apNode->AsNode();
    if (!pNode)
    {
        arLeaves.push_back(apNode);
        return;
    }
    for (uint16_t i = 0; i < pNode->children.length; ++i)
        CollectLeaves(pNode->children.data[i], arLeaves, aDepth + 1);
}

// Main thread only. The cached meshes are only ever touched when they are still leaves of the
// actor's current 3D: an appearance update rebuilds the 3D (freeing the old meshes), and writing
// the hidden flag into those freed meshes corrupted the heap (a hard crash while cycling).
void SetHidden(Actor* apActor, bool aHidden) noexcept
{
    auto* pRoot = apActor ? apActor->GetNiNode() : nullptr;
    if (!pRoot)
    {
        if (apActor)
            s_hiddenMeshes.erase(apActor->formID);
        return;
    }
    std::vector<NiAVObject*> leaves;
    CollectLeaves(pRoot, leaves, 0);
    auto& hiddenByUs = s_hiddenMeshes[apActor->formID];
    // Forget meshes that are gone (freed by a rebuild), without touching them.
    std::erase_if(hiddenByUs, [&leaves](NiAVObject* apMesh) { return std::find(leaves.begin(), leaves.end(), apMesh) == leaves.end(); });
    if (aHidden)
    {
        // Every frame: a rebuilt 3D brings new, visible meshes.
        for (auto* pLeaf : leaves)
            if (!(pLeaf->flags & 1))
            {
                pLeaf->flags |= 1;
                hiddenByUs.push_back(pLeaf);
            }
    }
    else
    {
        for (auto* pMesh : hiddenByUs)
            pMesh->flags &= ~1u;
        s_hiddenMeshes.erase(apActor->formID);
    }
}

bool KeyPressed(int aKey) noexcept
{
    return (GetAsyncKeyState(aKey) & 0x8000) != 0;
}

bool GameInFocus() noexcept
{
    auto* pWindow = BSGraphics::GetMainWindow();
    return pWindow && pWindow->hWnd && GetForegroundWindow() == pWindow->hWnd;
}
} // namespace

namespace CreatorTogether
{
void Update(World& aWorld, const bool aHolding, const bool aCreatorOpen) noexcept
{
    s_holding.store(aHolding);
    // No longer holding (the party broke up, a player disconnected, the session moved on) with Done
    // held: close the creator, or this player is stuck in it.
    if (!aHolding && s_done.load())
        Release();
    std::lock_guard lock(s_lock);
    const bool active = aHolding && aCreatorOpen;
    if (!active)
    {
        if (s_active)
        {
            // Leaving the creator: everyone is shown again, on the main thread (OnMainFrame).
            s_view = SIZE_MAX;
            s_hintShown = false;
            World::Get().GetOverlayService().SetCreatorView(false, 1, 1, false);
            s_bannerPlayer = s_bannerCount = s_bannerReady = -1;
        }
        s_active = false;
        return;
    }
    if (!s_active)
        spdlog::info("Character creator together: active (window in focus {})", GameInFocus());
    s_active = true;

    // Every player's character here in party order (player id), this player's own included, so
    // "PLAYER k/N" names the same player on every PC.
    std::vector<std::pair<uint32_t, uint32_t>> players; // player id, form id
    players.emplace_back(aWorld.GetTransport().GetLocalPlayerId(), 0x14);
    auto view = aWorld.view<FormIdComponent, PlayerComponent>();
    for (auto entity : view)
    {
        const auto formId = view.get<FormIdComponent>(entity).Id;
        if (formId != 0x14)
            players.emplace_back(view.get<PlayerComponent>(entity).Id, formId);
    }
    std::sort(players.begin(), players.end());
    s_players.clear();
    size_t self = 0;
    for (const auto& [playerId, formId] : players)
    {
        if (formId == 0x14)
            self = s_players.size();
        s_players.push_back(formId);
    }
    if (s_view >= s_players.size())
        s_view = self;


    // [ and ] cycle whose character stands on the spot.
    const bool focus = GameInFocus();
    const bool prev = focus && KeyPressed(VK_OEM_4);
    const bool next = focus && KeyPressed(VK_OEM_6);
    const size_t count = s_players.size();
    size_t wanted = s_view;
    if (prev && !s_prevKey)
        wanted = (s_view + count - 1) % count;
    if (next && !s_nextKey)
        wanted = (s_view + 1) % count;
    if ((prev && !s_prevKey) || (next && !s_nextKey))
        spdlog::info("Character creator together: {} pressed, {} players", prev ? "[" : "]", s_players.size());
    s_prevKey = prev;
    s_nextKey = next;
    if (wanted != s_view)
    {
        s_view = wanted;
        spdlog::info("Character creator together: viewing {:X}", s_players[s_view]);
        s_logAfterSwitch = true;
    }

    auto* pPlayer = PlayerCharacter::Get();

    // Diagnostic: what the switch did to the 3D (one line after each switch).
    if (s_logAfterSwitch && pPlayer && pPlayer->GetNiNode())
    {
        s_logAfterSwitch = false;
        for (size_t i = 0; i < s_players.size(); ++i)
        {
            auto* pActor = s_players[i] == 0x14 ? static_cast<Actor*>(pPlayer) : Cast<Actor>(TESForm::GetById(s_players[i]));
            auto* pRoot = pActor ? pActor->GetNiNode() : nullptr;
            if (pRoot)
                spdlog::info("Character creator together: {:X} root world scale {:.4f}, hidden meshes {}, world ({:.0f}, {:.0f}, {:.0f}) player at ({:.0f}, {:.0f}, {:.0f})",
                    pActor->formID, pRoot->world.scale, s_hiddenMeshes.contains(pActor->formID) ? s_hiddenMeshes[pActor->formID].size() : 0, pRoot->world.translate.x, pRoot->world.translate.y, pRoot->world.translate.z,
                    pPlayer->position.x, pPlayer->position.y, pPlayer->position.z);
            else if (pActor)
                spdlog::info("Character creator together: {:X} has no 3D", pActor->formID);
        }
    }

    // The banner: whose character this is, and whether that player is ready.
    const bool ready = s_players[s_view] == 0x14 ? s_done.load() : s_remoteReady[s_players[s_view]];
    const int player = static_cast<int>(s_view) + 1;
    const int playerCount = static_cast<int>(count);
    if (player != s_bannerPlayer || playerCount != s_bannerCount || static_cast<int>(ready) != s_bannerReady)
    {
        s_bannerPlayer = player;
        s_bannerCount = playerCount;
        s_bannerReady = static_cast<int>(ready);
        World::Get().GetOverlayService().SetCreatorView(true, player, playerCount, ready);
    }
}

void SetRemoteReady(const uint32_t aFormId, const bool aReady) noexcept
{
    std::lock_guard lock(s_lock);
    s_remoteReady[aFormId] = aReady;
}

bool IsDone() noexcept
{
    return s_done.load();
}

void Release() noexcept
{
    s_releaseRequested.store(true);
}

void OnMainFrame() noexcept
{
    {
        std::lock_guard lock(s_lock);
        auto* pPlayer = PlayerCharacter::Get();
        if (!s_active)
        {
            // Not in the creator (any more): show everyone this feature hid (SetHidden erases the
            // entry, so collect the ids first).
            std::vector<uint32_t> hidden;
            for (const auto& [formId, meshes] : s_hiddenMeshes)
                hidden.push_back(formId);
            for (const auto formId : hidden)
                SetHidden(formId == 0x14 ? static_cast<Actor*>(pPlayer) : Cast<Actor>(TESForm::GetById(formId)), false);
            s_hiddenMeshes.clear();
        }
        else if (s_view < s_players.size())
        {
            // Only the viewed character is visible; it stands on this player's spot, its 3D moved at once
            // (the world is paused while the creator is open, so nothing else would move it).
            for (size_t i = 0; i < s_players.size(); ++i)
            {
                const bool viewed = s_view == i;
                if (s_players[i] == 0x14)
                {
                    SetHidden(pPlayer, !viewed);
                    continue;
                }
                auto* pRemote = Cast<Actor>(TESForm::GetById(s_players[i]));
                SetHidden(pRemote, !viewed);
                if (viewed && pRemote && pPlayer)
                {
                    pRemote->position = pPlayer->position;
                    pRemote->SetRotation(pRemote->rotation.x, pRemote->rotation.y, pPlayer->rotation.z);
                    pRemote->Update3DPosition(true);
                    // Its scene graph advanced each frame as the menu does for this player's (NiAVObject::Update).
                    if (auto* pRoot = pRemote->GetNiNode())
                    {
                        using TNiUpdate = void(NiAVObject*, void*);
                        POINTER_SKYRIMSE(TNiUpdate, s_niUpdate, 70251);
                        uint8_t updateData[16]{};
                        s_niUpdate.Get()(pRoot, updateData);
                    }
                }
            }
        }
    }

    if (!s_releaseRequested.exchange(false))
        return;
    if (!s_done.exchange(false))
        return;
    s_releasing.store(true);
    // The menu's own close (fade, camera, then it queues its hide), or the hide itself if Done was
    // only held at the hide.
    if (auto* pMenu = s_heldMenu.exchange(nullptr); pMenu && s_realBeginClosing)
        s_realBeginClosing(pMenu);
    else
    {
        POINTER_SKYRIMSE(void*, s_uiMessageQueue, 400445);
        if (s_uiMessageQueue.Get() && *s_uiMessageQueue.Get() && s_realAddMessage)
        {
            BSFixedString name("RaceSex Menu");
            s_realAddMessage(*s_uiMessageQueue.Get(), &name, UIMessage::kHide, nullptr);
        }
    }
    s_releasing.store(false);
    spdlog::info("Character creator: every player is done, closed together");
}

bool GetDisplay(const uint32_t aFormId, NiPoint3& arPosition, float& arHeading) noexcept
{
    std::lock_guard lock(s_lock);
    auto* pPlayer = PlayerCharacter::Get();
    if (!s_active || !pPlayer || aFormId == 0x14 || std::find(s_players.begin(), s_players.end(), aFormId) == s_players.end())
        return false;
    arPosition = pPlayer->position;
    arHeading = pPlayer->rotation.z;
    return true;
}
} // namespace CreatorTogether

static TiltedPhoques::Initializer s_creatorTogetherHooks(
    []()
    {
        POINTER_SKYRIMSE(TAddMessage, s_addMessage, 13631);
        s_realAddMessage = s_addMessage.Get();
        TP_HOOK(&s_realAddMessage, HookAddMessage);
        POINTER_SKYRIMSE(TBeginClosing, s_beginClosing, 52388);
        s_realBeginClosing = s_beginClosing.Get();
        TP_HOOK(&s_realBeginClosing, HookBeginClosing);
    });
