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
std::atomic<bool> s_done{};
std::atomic<bool> s_releasing{};

std::mutex s_lock;
bool s_active{};                   // the creator is open here, in a party New Game
std::vector<uint32_t> s_remotePlayers; // other players' characters here, by player id order
size_t s_view{};                   // 0: this player's own character; n: s_remotePlayers[n - 1]
bool s_prevKey{};
bool s_nextKey{};
bool s_hintShown{};
std::unordered_map<uint32_t, bool> s_hiddenByUs; // form id -> hidden by this feature
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

void SetHidden(Actor* apActor, bool aHidden) noexcept
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
    else if (!aHidden && hidden && s_hiddenByUs.contains(apActor->formID))
    {
        pRoot->flags &= ~1u;
        s_hiddenByUs.erase(apActor->formID);
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
    std::lock_guard lock(s_lock);
    const bool active = aHolding && aCreatorOpen;
    if (!active)
    {
        if (s_active)
        {
            // Leaving the creator: everyone visible again.
            for (const auto& [formId, hidden] : s_hiddenByUs)
            {
                if (auto* pActor = Cast<Actor>(TESForm::GetById(formId)); pActor && pActor->GetNiNode())
                    pActor->GetNiNode()->flags &= ~1u;
            }
            s_hiddenByUs.clear();
            s_view = 0;
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

    // The other players' characters here, in player id order.
    std::vector<std::pair<uint32_t, uint32_t>> players; // player id, form id
    auto view = aWorld.view<FormIdComponent, PlayerComponent>();
    for (auto entity : view)
    {
        const auto formId = view.get<FormIdComponent>(entity).Id;
        if (formId != 0x14)
            players.emplace_back(view.get<PlayerComponent>(entity).Id, formId);
    }
    std::sort(players.begin(), players.end());
    s_remotePlayers.clear();
    for (const auto& [playerId, formId] : players)
        s_remotePlayers.push_back(formId);
    if (s_view > s_remotePlayers.size())
        s_view = 0;


    // [ and ] cycle whose character stands on the spot.
    const bool focus = GameInFocus();
    const bool prev = focus && KeyPressed(VK_OEM_4);
    const bool next = focus && KeyPressed(VK_OEM_6);
    const size_t count = s_remotePlayers.size() + 1;
    size_t wanted = s_view;
    if (prev && !s_prevKey)
        wanted = (s_view + count - 1) % count;
    if (next && !s_nextKey)
        wanted = (s_view + 1) % count;
    if ((prev && !s_prevKey) || (next && !s_nextKey))
        spdlog::info("Character creator together: {} pressed, {} other players", prev ? "[" : "]", s_remotePlayers.size());
    s_prevKey = prev;
    s_nextKey = next;
    if (wanted != s_view)
    {
        s_view = wanted;
        const auto text = s_view == 0 ? std::string("Viewing: your character") :
                                        fmt::format("Viewing: player {} of {}", s_view + 1, count);
        spdlog::info("Character creator together: viewing {}", s_view == 0 ? 0x14 : s_remotePlayers[s_view - 1]);
    }

    // Only the viewed character is visible; it stands on this player's spot, its 3D moved at once
    // (the world is paused while the creator is open, so nothing else would move it).
    auto* pPlayer = PlayerCharacter::Get();
    SetHidden(pPlayer, s_view != 0);
    for (size_t i = 0; i < s_remotePlayers.size(); ++i)
    {
        auto* pRemote = Cast<Actor>(TESForm::GetById(s_remotePlayers[i]));
        const bool viewed = s_view == i + 1;
        SetHidden(pRemote, !viewed);
        if (viewed && pRemote && pPlayer)
        {
            pRemote->position = pPlayer->position;
            pRemote->SetRotation(pRemote->rotation.x, pRemote->rotation.y, pPlayer->rotation.z);
            pRemote->Update3DPosition(true);
        }
    }

    // The banner: whose character this is, and whether that player is ready.
    const bool ready = s_view == 0 ? s_done.load() : s_remoteReady[s_remotePlayers[s_view - 1]];
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
    if (!s_done.exchange(false))
        return;
    POINTER_SKYRIMSE(void*, s_uiMessageQueue, 400445);
    if (!s_uiMessageQueue.Get() || !*s_uiMessageQueue.Get() || !s_realAddMessage)
        return;
    s_releasing.store(true);
    BSFixedString name("RaceSex Menu");
    s_realAddMessage(*s_uiMessageQueue.Get(), &name, UIMessage::kHide, nullptr);
    s_releasing.store(false);
    spdlog::info("Character creator: every player is done, closed together");
}

bool GetDisplay(const uint32_t aFormId, NiPoint3& arPosition, float& arHeading) noexcept
{
    std::lock_guard lock(s_lock);
    auto* pPlayer = PlayerCharacter::Get();
    if (!s_active || !pPlayer || std::find(s_remotePlayers.begin(), s_remotePlayers.end(), aFormId) == s_remotePlayers.end())
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
    });
