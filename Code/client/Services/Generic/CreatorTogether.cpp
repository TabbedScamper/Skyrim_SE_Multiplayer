#include <Services/CreatorTogether.h>
#include <Services/Generic/BoundPoseKeeper.h>

#include <World.h>
#include <Components.h>
#include <Games/ActorExtension.h>
#include <AI/AIProcess.h>
#include <Games/Skyrim/Actor.h>
#include <Games/Skyrim/PlayerCharacter.h>
#include <Games/Skyrim/Interface/UI.h>
#include <Games/Skyrim/Interface/IMenu.h>
#include <Forms/TESNPC.h>
#include <Forms/TESRace.h>
#include <Messages/NotifyPlayerAppearance.h>
#include <Systems/FaceGenSystem.h>
#include <Games/Skyrim/NetImmerse/NiNode.h>
#include <Games/Skyrim/BSGraphics/BSGraphicsRenderer.h>
#include <Services/OverlayService.h>

#include <array>
#include <atomic>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <xinput.h>

namespace
{
using TAddMessage = void(void*, const BSFixedString*, UIMessage::UI_MESSAGE_TYPE, void*);
TAddMessage* s_realAddMessage{};

std::atomic<bool> s_holding{};
std::atomic<bool> s_releaseRequested{};
std::atomic<bool> s_forceReleaseRequested{};
std::atomic<bool> s_participating{};
std::atomic<bool> s_done{};
std::atomic<bool> s_releasing{};
// This player is looking at another player's character: its own customization panels are hidden.
std::atomic<bool> s_viewingOther{};
std::atomic<uint32_t> s_leakedFrames{}; // see LeakedFrames
std::atomic<uint32_t> s_appliedLooks{};

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
std::mutex s_appearanceLock;
bool s_active{};                   // the creator is open here, in a party New Game
std::vector<uint32_t> s_players;   // every player's character here (0x14 for this player), in party order
size_t s_view{SIZE_MAX};           // index into s_players of the character shown
std::atomic<bool> s_gamepadInput{};
bool s_hintShown{};
bool s_logAfterSwitch{};
std::unordered_map<uint32_t, bool> s_remoteReady; // form id -> that player clicked Done
int s_bannerPlayer{-1};
int s_bannerCount{-1};
int s_bannerReady{-1};
int s_bannerLocalReady{-1};
int s_bannerGamepad{-1};
std::unordered_map<uint32_t, NotifyPlayerAppearance> s_pendingAppearances;
std::unordered_map<uint32_t, NotifyPlayerAppearance> s_appliedAppearances;
std::unordered_map<uint32_t, uint32_t> s_remoteActors;
void* s_hiddenMovie{};
bool s_movieWasVisible{};

IMenu* GetCreatorMenu() noexcept
{
    auto* pUI = UI::Get();
    const BSFixedString name("RaceSex Menu");
    return pUI && pUI->GetMenuOpen(name) ? pUI->FindMenuByName(name) : nullptr;
}

void SetPanelsHidden(IMenu* apMenu, bool aHidden) noexcept
{
    auto* pMovie = apMenu ? apMenu->uiMovie : nullptr;
    if (s_hiddenMovie && s_hiddenMovie != pMovie)
        s_hiddenMovie = nullptr;
    if (!pMovie)
        return;
    // GFxMovie slots 08/09; native DialogueMenu uses 08 at 14092a5a0 (ID 51506).
    auto** pTable = *reinterpret_cast<void***>(pMovie);
    const auto setVisible = reinterpret_cast<void (*)(void*, bool)>(pTable[8]);
    const auto getVisible = reinterpret_cast<bool (*)(void*)>(pTable[9]);
    if (aHidden)
    {
        if (!s_hiddenMovie)
        {
            s_movieWasVisible = getVisible(pMovie);
            s_hiddenMovie = pMovie;
            spdlog::info("Character creator: panels hidden, Backspace or View/Back to edit again");
        }
        setVisible(pMovie, false);
    }
    else if (s_hiddenMovie)
    {
        setVisible(pMovie, s_movieWasVisible);
        s_hiddenMovie = nullptr;
        spdlog::info("Character creator: panels restored");
    }
}

using TProcessCreatorMessage = UI_MESSAGE_RESULTS(IMenu*, UIMessage&);
TProcessCreatorMessage* s_realProcessCreatorMessage{};

UI_MESSAGE_RESULTS HookProcessCreatorMessage(IMenu* apMenu, UIMessage& aMessage)
{
    // SetVisible suppresses drawing, not keyboard/controller events to the focused movie.
    if (!s_releasing.load() && ((s_holding.load() && s_done.load()) || s_viewingOther.load()) &&
        (aMessage.eType == UIMessage::kScaleformEvent || aMessage.eType == UIMessage::kUserEvent))
        return UI_MESSAGE_RESULTS::kHandled;
    // BoundPoseKeeper observes the shared native rebuild (52391), including
    // preset changes that do not pass through the initial-preview gate.
    return s_realProcessCreatorMessage(apMenu, aMessage);
}

bool IsRaceSexMenu(const BSFixedString* apName) noexcept
{
    return apName && apName->data && std::strcmp(apName->data, "RaceSex Menu") == 0;
}

// Held while the party is still creating: the player's Done closes nothing yet.
void HookAddMessage(void* apQueue, const BSFixedString* apName, UIMessage::UI_MESSAGE_TYPE aType, void* apData)
{
    if (s_holding.load() && !s_releasing.load() && aType == UIMessage::kHide &&
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
using Mesh = std::shared_ptr<NiAVObject>;
std::unordered_map<uint32_t, std::vector<Mesh>> s_hiddenMeshes;

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

// Main thread only. Retain only meshes we hid, restoring the flag before releasing each reference.
// Detached parts can be reused by a rebuild; pointer membership alone did not own their lifetime.
void SetHidden(Actor* apActor, bool aHidden) noexcept
{
    if (!apActor)
        return;
    if (!aHidden)
    {
        s_hiddenMeshes.erase(apActor->formID);
        return;
    }
    auto* pRoot = apActor->GetNiNode();
    if (!pRoot)
    {
        s_hiddenMeshes.erase(apActor->formID);
        return;
    }
    std::vector<NiAVObject*> leaves;
    CollectLeaves(pRoot, leaves, 0);
    auto& hiddenByUs = s_hiddenMeshes[apActor->formID];
    // Restore detached parts as well, before dropping our reference to them.
    std::erase_if(hiddenByUs, [&leaves](const Mesh& apMesh) { return std::find(leaves.begin(), leaves.end(), apMesh.get()) == leaves.end(); });
    // Every frame: a rebuilt 3D brings new, visible meshes.
    for (auto* pLeaf : leaves)
        if (!(pLeaf->flags & 1))
        {
            pLeaf->flags |= 1;
            if (std::none_of(hiddenByUs.begin(), hiddenByUs.end(), [pLeaf](const Mesh& apMesh) { return apMesh.get() == pLeaf; }))
            {
                pLeaf->IncRef();
                hiddenByUs.emplace_back(pLeaf, [](NiAVObject* apMesh) {
                    apMesh->flags &= ~1u;
                    apMesh->DecRef();
                });
            }
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

struct CreatorInput
{
    bool Previous{};
    bool Next{};
    bool Edit{};
};

// Main thread only, including the edge history. Poll without consuming vanilla menu input.
CreatorInput PollCreatorInput(bool aActive) noexcept
{
    using TXInputGetState = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);
    static const auto getState = []() -> TXInputGetState {
        for (const wchar_t* library : {L"xinput1_4.dll", L"xinput1_3.dll", L"xinput9_1_0.dll"})
            if (const auto module = LoadLibraryW(library))
            {
                if (const auto proc = GetProcAddress(module, "XInputGetState"))
                    return reinterpret_cast<TXInputGetState>(proc);
                FreeLibrary(module);
            }
        return nullptr;
    }();
    static bool wasFocused = false;
    static std::array<bool, 256> previousKeys{};
    static POINT previousCursor{};
    static bool hadCursor = false;
    static DWORD previousIndex = XUSER_MAX_COUNT;
    static XINPUT_GAMEPAD previousPad{};
    if (!aActive || !GameInFocus())
    {
        wasFocused = false;
        previousIndex = XUSER_MAX_COUNT;
        hadCursor = false;
        return {};
    }

    std::array<bool, 256> keys{};
    bool keyboardInput = false;
    for (int key = 1; key < 256; ++key)
    {
        // Windows' gamepad virtual keys are not keyboard activity.
        if (key >= 0xC3 && key <= 0xDA)
            continue;
        keys[key] = KeyPressed(key);
        keyboardInput |= keys[key] && (!wasFocused || !previousKeys[key]);
    }
    POINT cursor{};
    const bool hasCursor = GetCursorPos(&cursor) != FALSE;
    keyboardInput |= wasFocused && hasCursor && hadCursor &&
        (cursor.x != previousCursor.x || cursor.y != previousCursor.y);

    XINPUT_STATE state{};
    DWORD index = 0;
    for (; index < XUSER_MAX_COUNT; ++index)
        if (getState && getState(index, &state) == ERROR_SUCCESS)
            break;
    const bool connected = index < XUSER_MAX_COUNT;
    const auto& pad = state.Gamepad;
    const bool samePad = wasFocused && connected && index == previousIndex;
    const WORD pressed = connected ? static_cast<WORD>(pad.wButtons & ~(samePad ? previousPad.wButtons : 0)) : 0;
    const auto stickMoved = [](SHORT aNow, SHORT aBefore, int aDeadzone) {
        return std::abs(static_cast<int>(aNow)) > aDeadzone &&
            (std::abs(static_cast<int>(aBefore)) <= aDeadzone ||
                std::abs(static_cast<int>(aNow) - aBefore) > 2048);
    };
    const bool padInput = connected && (pressed ||
        (pad.bLeftTrigger > XINPUT_GAMEPAD_TRIGGER_THRESHOLD && (!samePad || previousPad.bLeftTrigger <= XINPUT_GAMEPAD_TRIGGER_THRESHOLD)) ||
        (pad.bRightTrigger > XINPUT_GAMEPAD_TRIGGER_THRESHOLD && (!samePad || previousPad.bRightTrigger <= XINPUT_GAMEPAD_TRIGGER_THRESHOLD)) ||
        stickMoved(pad.sThumbLX, samePad ? previousPad.sThumbLX : 0, XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE) ||
        stickMoved(pad.sThumbLY, samePad ? previousPad.sThumbLY : 0, XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE) ||
        stickMoved(pad.sThumbRX, samePad ? previousPad.sThumbRX : 0, XINPUT_GAMEPAD_RIGHT_THUMB_DEADZONE) ||
        stickMoved(pad.sThumbRY, samePad ? previousPad.sThumbRY : 0, XINPUT_GAMEPAD_RIGHT_THUMB_DEADZONE));
    if (padInput)
        s_gamepadInput.store(true);
    if (keyboardInput || !connected)
        s_gamepadInput.store(false);

    // Seed history on focus gain, menu entry and reconnection. A held button is not a new press.
    const auto keyPressed = [&](int aKey) { return wasFocused && keys[aKey] && !previousKeys[aKey]; };
    const WORD padPressed = samePad ? pressed : 0;
    // RaceSexPanels.handleInput uses L1/R1/L2/R2, not stick clicks. Native CanProcess
    // (ID 52384, VA 14096b0d0) accepts Rotate/stick motion, not the Item Zoom R3 event.
    CreatorInput input{
        keyPressed(VK_OEM_4) || (padPressed & XINPUT_GAMEPAD_LEFT_THUMB) != 0,
        keyPressed(VK_OEM_6) || (padPressed & XINPUT_GAMEPAD_RIGHT_THUMB) != 0,
        keyPressed(VK_BACK) || (padPressed & XINPUT_GAMEPAD_BACK) != 0};
    previousKeys = keys;
    previousCursor = cursor;
    hadCursor = hasCursor;
    previousIndex = index;
    previousPad = pad;
    wasFocused = true;
    return input;
}
} // namespace

namespace CreatorTogether
{
void Update(World& aWorld, const bool aHolding, const bool aCreatorOpen) noexcept
{
    s_holding.store(aHolding);
    if (!aCreatorOpen)
    {
        s_done.store(false);
        s_releasing.store(false);
        s_heldMenu.store(nullptr);
        s_participating.store(false);
    }
    else if (aHolding)
        s_participating.store(true);
    // No longer holding (the party broke up, a player disconnected, the session moved on) with Done
    // held: close the creator, or this player is stuck in it.
    if (!aHolding && s_done.load())
        Release();
    std::lock_guard lock(s_lock);
    // Only the client update reads the registry; the main-thread mailbox uses this identity snapshot.
    s_remoteActors.clear();
    auto remoteActors = aWorld.view<FormIdComponent, RemoteComponent, PlayerComponent>();
    for (const auto entity : remoteActors)
        s_remoteActors.emplace(remoteActors.get<FormIdComponent>(entity).Id, remoteActors.get<RemoteComponent>(entity).Id);
    std::erase_if(s_remoteReady, [](const auto& aEntry) { return !s_remoteActors.contains(aEntry.first); });
    if (!aHolding && !aCreatorOpen)
        s_remoteReady.clear();
    const bool active = aHolding && aCreatorOpen;
    if (!active)
    {
        if (s_active)
        {
            // Leaving the creator: everyone is shown again, on the main thread (OnMainFrame).
            s_view = SIZE_MAX;
            s_hintShown = false;
            World::Get().GetOverlayService().SetCreatorView(false, 1, 1, false);
            s_bannerPlayer = s_bannerCount = s_bannerReady = s_bannerLocalReady = s_bannerGamepad = -1;
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
    const auto viewedFormId = s_view < s_players.size() ? s_players[s_view] : 0x14;
    s_players.clear();
    size_t self = 0;
    for (const auto& [playerId, formId] : players)
    {
        if (formId == 0x14)
            self = s_players.size();
        s_players.push_back(formId);
    }
    const auto selected = std::find(s_players.begin(), s_players.end(), viewedFormId);
    s_view = selected == s_players.end() ? self : static_cast<size_t>(selected - s_players.begin());


    const size_t count = s_players.size();

    // The banner: whose character this is, and whether that player is ready.
    const bool ready = s_players[s_view] == 0x14 ? s_done.load() : s_remoteReady[s_players[s_view]];
    const int player = static_cast<int>(s_view) + 1;
    const int playerCount = static_cast<int>(count);
    const bool localReady = s_done.load();
    const bool gamepad = s_gamepadInput.load();
    if (player != s_bannerPlayer || playerCount != s_bannerCount || static_cast<int>(ready) != s_bannerReady ||
        static_cast<int>(localReady) != s_bannerLocalReady || static_cast<int>(gamepad) != s_bannerGamepad)
    {
        s_bannerPlayer = player;
        s_bannerCount = playerCount;
        s_bannerReady = static_cast<int>(ready);
        s_bannerLocalReady = static_cast<int>(localReady);
        s_bannerGamepad = static_cast<int>(gamepad);
        World::Get().GetOverlayService().SetCreatorView(true, player, playerCount, ready, localReady, gamepad);
    }
}

void SetRemoteReady(const uint32_t aFormId, const bool aReady) noexcept
{
    std::lock_guard lock(s_lock);
    s_remoteReady[aFormId] = aReady;
}

void QueueAppearance(const uint32_t aFormId, const NotifyPlayerAppearance& acAppearance) noexcept
{
    std::lock_guard lock(s_lock);
    s_remoteActors[aFormId] = acAppearance.ServerId;
    s_pendingAppearances[aFormId] = acAppearance;
}

bool OthersDone() noexcept
{
    std::lock_guard lock(s_lock);
    for (const auto& [formId, serverId] : s_remoteActors)
    {
        const auto it = s_remoteReady.find(formId);
        if (it == s_remoteReady.end() || !it->second)
            return false;
    }
    return true;
}

bool IsDone() noexcept
{
    return s_done.load();
}

std::mutex& AppearanceMutex() noexcept
{
    return s_appearanceLock;
}

void Release(const bool aForce) noexcept
{
    if (aForce)
    {
        if (!s_participating.load())
            return;
        s_forceReleaseRequested.store(true);
    }
    s_releaseRequested.store(true);
}

void OnMainFrame() noexcept
{
    if (!entt::locator<World>::has_value())
        return;
    auto* pMenu = GetCreatorMenu();
    BoundPoseKeeper::OnMainFrame(pMenu != nullptr, s_participating.load());
    if (pMenu)
    {
        // Read-only: did a character that is not being viewed render last frame (any leaf mesh not hidden)?
        std::vector<uint32_t> players;
        size_t view = SIZE_MAX;
        {
            std::lock_guard lock(s_lock);
            if (s_active)
            {
                players = s_players;
                view = s_view;
            }
        }
        for (size_t i = 0; i < players.size() && view < players.size(); ++i)
        {
            // Only a copy this creator already hid: the opening frame still shows it as it was in the world.
            auto* pActor = i == view || !s_hiddenMeshes.contains(players[i]) ? nullptr : Cast<Actor>(TESForm::GetById(players[i]));
            auto* pRoot = pActor ? pActor->GetNiNode() : nullptr;
            if (!pRoot || (pRoot->flags & 1))
                continue;
            std::vector<NiAVObject*> leaves;
            CollectLeaves(pRoot, leaves, 0);
            const auto shown = std::count_if(leaves.begin(), leaves.end(), [](NiAVObject* apLeaf) { return !(apLeaf->flags & 1); });
            if (shown)
            {
                const auto count = ++s_leakedFrames;
                if (count <= 20 || count % 100 == 0)
                    spdlog::info("Character creator together: {:X} is not viewed but {} of {} meshes rendered last frame (leak {})",
                        players[i], shown, leaves.size(), count);
                break;
            }
        }
    }
    std::unordered_map<uint32_t, NotifyPlayerAppearance> pending;
    std::unordered_map<uint32_t, uint32_t> remoteActors;
    {
        std::lock_guard lock(s_lock);
        pending.swap(s_pendingAppearances);
        remoteActors = s_remoteActors;
    }
    auto& world = World::Get();
    std::erase_if(s_appliedAppearances, [&remoteActors](const auto& aEntry) {
        const auto actor = remoteActors.find(aEntry.first);
        return actor == remoteActors.end() || actor->second != aEntry.second.ServerId;
    });
    size_t rebuildBudget = 2;
    for (const auto& [formId, appearance] : pending)
    {
        std::lock_guard appearanceLock(s_appearanceLock);
        const auto actor = remoteActors.find(formId);
        auto* pActor = actor != remoteActors.end() && actor->second == appearance.ServerId ? Cast<Actor>(TESForm::GetById(formId)) : nullptr;
        auto* pNpc = pActor ? Cast<TESNPC>(pActor->baseForm) : nullptr;
        if (!pNpc || formId == 0x14 || !pActor->GetExtension() || !pActor->GetExtension()->IsRemotePlayer() || pActor->IsDeleted())
            continue;
        if (!pNpc->IsTemporary() || pNpc->faceNPC)
        {
            spdlog::error("Rejected player appearance for actor {:X}: NPC {:X} is not an independent replica", formId, pNpc->formID);
            continue;
        }
        // A native rebuild removes parts before queuing work. Defer while physics owns this body.
        if (!pActor->GetNiNode() || !pActor->currentProcess || !pActor->currentProcess->middleProcess ||
            !pActor->currentProcess->unk8 ||
            ((pActor->actorState.flags1 >> 21) & 0x7F) != 0 || !rebuildBudget ||
            !BoundPoseKeeper::CanRebuildNow(pActor))
        {
            std::lock_guard lock(s_lock);
            s_pendingAppearances.try_emplace(formId, appearance);
            continue;
        }
        const auto previous = s_appliedAppearances.find(formId);
        if (previous != s_appliedAppearances.end() && previous->second.ServerId == appearance.ServerId &&
            previous->second.AppearanceBuffer == appearance.AppearanceBuffer &&
            previous->second.ChangeFlags == appearance.ChangeFlags && previous->second.FaceTints == appearance.FaceTints)
        {
            // A final packet can carry identical cosmetics. Still consume its
            // creator lifecycle flag so later ordinary looks cannot rebind it.
            previous->second.InCreator = appearance.InCreator;
            continue;
        }

        --rebuildBudget;
        SetHidden(pActor, false);
        // Shared New Game creator looks obey the same vanilla restoration as
        // their owner even when this replica missed its initial bound event.
        // Later cosmetic menus only preserve an actually observed bound pose.
        const bool creatorRebuild = s_participating.load() && (appearance.InCreator ||
            (previous != s_appliedAppearances.end() && previous->second.InCreator));
        const auto boundPose = creatorRebuild ? BoundPoseKeeper::Pose{} : BoundPoseKeeper::ReadPose(pActor);
        const auto oldSex = pNpc->actorData.actorBaseFlags & 1;
        auto* pOldRace = pActor->race;
        if (!pNpc->Deserialize(appearance.AppearanceBuffer, appearance.ChangeFlags))
            continue;
        const bool newSkeleton = pOldRace != pNpc->raceForm.race || oldSex != (pNpc->actorData.actorBaseFlags & 1);
        if (pOldRace != pNpc->raceForm.race)
        {
            auto* pNewRace = pNpc->raceForm.race;
            // TESNPC::SwitchRace returns early if Deserialize already set the new race. Let
            // Actor::SwitchRace run the full transition, then restore the received head parts.
            pNpc->raceForm.race = pOldRace;
            using TSwitchRace = void(Actor*, TESRace*, bool);
            POINTER_SKYRIMSE(TSwitchRace, s_switchRace, 37925);
            s_switchRace.Get()(pActor, pNewRace, false);
            if (!pNpc->Deserialize(appearance.AppearanceBuffer, appearance.ChangeFlags))
                continue;
        }
        // Keep the same runtime FaceGen invariant as TESNPC::Create after every native load.
        pNpc->originalRace = nullptr;
        if (newSkeleton)
        {
            // AIProcess::Update3DModel_Impl (ID 39395) uses 0x20 to replace the skeleton too.
            using TSet3DFlags = void(AIProcess*, uint8_t);
            POINTER_SKYRIMSE(TSet3DFlags, s_set3DFlags, 39907);
            s_set3DFlags.Get()(pActor->currentProcess, 0x20);
        }
        // QueueReset3D's inventory queue deliberately excludes player replicas. This is the
        // appearance-specific main-thread path, matching RaceSex's DoReset3D(true), ID 40255.
        using TReset3D = void(Actor*, bool);
        POINTER_SKYRIMSE(TReset3D, s_reset3D, 40255);
        s_reset3D.Get()(pActor, true);
        BoundPoseKeeper::AfterRebuild(pActor, boundPose, "replica appearance", creatorRebuild);
        world.GetRunner().Queue([formId, serverId = appearance.ServerId, tints = appearance.FaceTints]() {
            auto& world = World::Get();
            auto actors = world.view<FormIdComponent, RemoteComponent, PlayerComponent>();
            for (const auto entity : actors)
                if (actors.get<FormIdComponent>(entity).Id == formId && actors.get<RemoteComponent>(entity).Id == serverId)
                {
                    FaceGenSystem::Setup(world, entity, tints);
                    break;
                }
        });
        s_appliedAppearances[formId] = appearance;
        ++s_appliedLooks;
        spdlog::info("Player {:X}: {} look applied on main thread, rebuild requested (skeleton {}, race {:X}, head {}, NPC {:X})",
            formId, appearance.InCreator ? "live creator" : "final", newSkeleton,
            pNpc->raceForm.race ? pNpc->raceForm.race->formID : 0,
            pActor->GetFaceGenNiNode() != nullptr, pNpc->formID);
    }

    const auto input = PollCreatorInput(pMenu && s_holding.load() && !s_releasing.load());
    if (input.Previous || input.Next)
    {
        std::lock_guard lock(s_lock);
        if (s_active && s_view < s_players.size())
        {
            const auto count = s_players.size();
            const auto wanted = input.Next ? (s_view + 1) % count : (s_view + count - 1) % count;
            spdlog::info("Character creator together: {} pressed, {} players",
                input.Next ? "] / R3" : "[ / L3", count);
            if (wanted != s_view)
            {
                s_view = wanted;
                spdlog::info("Character creator together: viewing {:X}", s_players[s_view]);
                s_logAfterSwitch = true;
            }
        }
    }
    if (input.Edit && s_holding.load() && !s_releaseRequested.load() &&
        !s_releasing.load() && s_done.exchange(false))
    {
        s_heldMenu.store(nullptr);
        std::lock_guard lock(s_lock);
        const auto self = std::find(s_players.begin(), s_players.end(), 0x14);
        if (self != s_players.end())
            s_view = static_cast<size_t>(self - s_players.begin());
        s_logAfterSwitch = true;
        spdlog::info("Character creator: editing again, readiness withdrawn");
    }
    {
        std::lock_guard lock(s_lock);
        s_viewingOther.store(s_active && s_view < s_players.size() && s_players[s_view] != 0x14);
    }
    // Hidden once Done, and while looking at another player's character (back when viewing its own again).
    SetPanelsHidden(pMenu, !s_releasing.load() && ((s_holding.load() && s_done.load()) || s_viewingOther.load()));

    std::vector<uint32_t> players;
    size_t view;
    bool active;
    bool logAfterSwitch;
    {
        std::lock_guard lock(s_lock);
        players = s_players;
        view = s_view;
        active = s_active;
        logAfterSwitch = s_logAfterSwitch;
        s_logAfterSwitch = false;
    }
    {
        auto* pPlayer = PlayerCharacter::Get();
        if (!active || !pMenu)
        {
            s_hiddenMeshes.clear();
        }
        else if (view < players.size())
        {
            std::erase_if(s_hiddenMeshes, [&players](const auto& aEntry) {
                return std::find(players.begin(), players.end(), aEntry.first) == players.end();
            });
            static auto previousFrame = std::chrono::steady_clock::now();
            const auto now = std::chrono::steady_clock::now();
            const float delta = std::clamp(std::chrono::duration<float>(now - previousFrame).count(), 0.f, 0.1f);
            previousFrame = now;
            // Only the viewed character is visible; it stands on this player's spot, its 3D moved at once
            // (the world is paused while the creator is open, so nothing else would move it).
            for (size_t i = 0; i < players.size(); ++i)
            {
                const bool viewed = view == i;
                if (players[i] == 0x14)
                {
                    SetHidden(pPlayer, !viewed);
                    continue;
                }
                auto* pRemote = Cast<Actor>(TESForm::GetById(players[i]));
                if (viewed)
                    SetHidden(pRemote, false);
                if (viewed && pRemote && pPlayer)
                {
                    // The previewed copy advances its own graph here and never received the bound-hands idle the
                    // local player keeps (owner report: her character in the creator was not bound like the host's).
                    // Send it the same instant bound event BoundPoseKeeper uses, once per copy 3D while viewed.
                    static std::unordered_map<uint32_t, const void*> s_boundCopies;
                    auto* pCopyRoot = pRemote->GetNiNode();
                    if (pCopyRoot && s_boundCopies[pRemote->formID] != pCopyRoot)
                    {
                        BSFixedString boundEvent("OffsetBoundStandingPlayerInstant");
                        const bool accepted = pRemote->animationGraphHolder.SendAnimationEvent(&boundEvent);
                        if (accepted)
                            s_boundCopies[pRemote->formID] = pCopyRoot;
                        spdlog::info("Character creator together: bound pose sent to previewed copy {:X}: {}", pRemote->formID,
                            accepted ? "accepted" : "rejected");
                    }
                    pRemote->position = pPlayer->position;
                    pRemote->SetRotation(pRemote->rotation.x, pRemote->rotation.y, pPlayer->rotation.z);
                    pRemote->Update3DPosition(true);
                    // RaceSex::ProcessMessage advances the player's animation through slot 7D
                    // before NiAVObject::Update (ID 52345, VA 140968010). Do the same for the preview.
                    using TUpdateAnimation = void (*)(Actor*, float);
                    auto** pTable = *reinterpret_cast<void***>(pRemote);
                    reinterpret_cast<TUpdateAnimation>(pTable[0x7D])(pRemote, delta);
                    // Advance its scene graph as the menu does for this player's (NiAVObject::Update).
                    if (auto* pRoot = pRemote->GetNiNode())
                    {
                        using TNiUpdate = void(NiAVObject*, void*);
                        POINTER_SKYRIMSE(TNiUpdate, s_niUpdate, 70251);
                        // A root app-cull rejects the entire subtree (ID 70267). Mesh restoration
                        // alone cannot undo a root hidden earlier by cutscene follow.
                        pRoot->flags &= ~1u;
                        struct { float time; uint32_t flags; } updateData{delta, 0};
                        s_niUpdate.Get()(pRoot, &updateData);
                    }
                }
                SetHidden(pRemote, !viewed);
            }
            if (logAfterSwitch && pPlayer)
            {
                for (const auto formId : players)
                {
                    auto* pActor = Cast<Actor>(TESForm::GetById(formId));
                    auto* pRoot = pActor ? pActor->GetNiNode() : nullptr;
                    if (!pRoot)
                    {
                        spdlog::info("Character creator together: {:X} has no 3D after switch", formId);
                        continue;
                    }
                    spdlog::info("Character creator together: {:X} after switch, viewed {}, root hidden {}, fade {:.3f}, head {}, root world scale {:.4f}, hidden meshes {}, world ({:.0f}, {:.0f}, {:.0f}) player at ({:.0f}, {:.0f}, {:.0f})",
                        formId, formId == players[view], (pRoot->flags & 1) != 0, pRoot->fadeAmount,
                        pActor->GetFaceGenNiNode() != nullptr, pRoot->world.scale,
                        s_hiddenMeshes.contains(formId) ? s_hiddenMeshes[formId].size() : 0,
                        pRoot->world.translate.x, pRoot->world.translate.y, pRoot->world.translate.z,
                        pPlayer->position.x, pPlayer->position.y, pPlayer->position.z);
                }
            }
        }
    }

    if (!s_releaseRequested.exchange(false))
        return;
    const bool forced = s_forceReleaseRequested.exchange(false);
    if (!pMenu || (!s_done.load() && !forced) || s_releasing.exchange(true))
        return;
    if (!s_done.exchange(true))
        spdlog::info("Character creator: server released the party before readiness withdrawal arrived");
    SetPanelsHidden(pMenu, false);
    // The menu's own close (fade, camera, then it queues its hide), or the hide itself if Done was
    // only held at the hide.
    if (auto* pHeldMenu = s_heldMenu.exchange(nullptr); s_realBeginClosing && (forced || pHeldMenu == pMenu))
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
    spdlog::info("Character creator: party release requested, closing together");
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

uint32_t LeakedFrames() noexcept
{
    return s_leakedFrames.load();
}

uint32_t AppliedLooks() noexcept
{
    return s_appliedLooks.load();
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
        POINTER_SKYRIMSE(TProcessCreatorMessage, s_processCreatorMessage, 52345);
        s_realProcessCreatorMessage = s_processCreatorMessage.Get();
        TP_HOOK(&s_realProcessCreatorMessage, HookProcessCreatorMessage);
    });
