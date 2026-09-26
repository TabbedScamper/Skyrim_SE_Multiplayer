#include <TiltedOnlinePCH.h>
#include <Games/Skyrim/DialogueListenHooks.h>
#include <Games/Misc/MenuTopicManager.h>
#include <Games/Misc/SubtitleManager.h>
#include <Interface/UI.h>
#include <Interface/IMenu.h>
#include <PlayerCharacter.h>
#include <Camera/PlayerCamera.h>
#include <Camera/TESCameraState.h>
#include <AI/Movement/PlayerControls.h>
#include <NetImmerse/NiNode.h>
#include <FunctionHook.hpp>
#include <atomic>
#include <cmath>
#include <mutex>

namespace DialogueListenNative
{
namespace
{
std::mutex s_mutex;
Observation s_observed;
uint32_t s_hiddenForm{};
uint64_t s_hideSerial{};
uint64_t s_choiceSerial{};
std::atomic<bool> s_listeningInput{};
std::atomic<bool> s_exitRequested{};
using Advance = void(IMenu*, float, uint32_t);
Advance* s_advance{};
void** s_menuVtable{};

// CommonLib GFxValue and MenuTopicManager::Dialogue; confirmed by 51515.
struct Value
{
    void* Interface{};
    uint32_t Type{};
    uint32_t Pad{};
    union { double Number; void* Object; } Data{};
    ~Value()
    {
        if ((Type & 0x40) && Interface)
        {
            using Release = void(void*, Value*, void*);
            POINTER_SKYRIMSE(Release, release, 82270);
            release.Get()(Interface, this, Data.Object);
        }
    }
};
static_assert(sizeof(Value) == 0x18);
struct Topic
{
    const char* Text;
    uint8_t Pad08[0x49 - 8];
    bool NeverSaid;
    bool Hidden;
};
struct Node { Topic* Data; Node* Next; };

bool Number(void* aMovie, const char* aPath, double& aResult)
{
    if (!aMovie) return false;
    using Get = bool(void*, Value*, const char*);
    auto get = reinterpret_cast<Get*>((*reinterpret_cast<void***>(aMovie))[0x11]);
    Value value;
    if (!get(aMovie, &value, aPath) || (value.Type & 0x8F) != 3 || !std::isfinite(value.Data.Number))
        return false;
    aResult = value.Data.Number;
    return true;
}

void Capture(IMenu* aMenu)
{
    Observation observed;
    auto* manager = MenuTopicManager::Get();
    if (!manager || !manager->menuOpen) return;
    auto* npc = TESObjectREFR::GetByHandle(manager->speaker.handle.iBits);
    if (!npc) return;
    observed.NpcFormId = npc->formID;
    observed.State.Active = true;
    // 14092B0D0 (51515) holds this lock, skips the greeting node at B7 and
    // topics with +4A set, and passes +49 (neverSaid) alongside the text.
    auto* critical = reinterpret_cast<CRITICAL_SECTION*>(reinterpret_cast<uint8_t*>(manager) + 0x40);
    EnterCriticalSection(critical);
    auto* node = reinterpret_cast<Node*>(manager->pOptions);
    if (node && *(reinterpret_cast<uint8_t*>(manager) + 0xB7)) node = node->Next;
    for (uint32_t index = 0; node && index < 1024; node = node->Next, ++index)
    {
        auto* topic = node->Data;
        if (!topic || topic->Hidden) continue;
        if (observed.State.Topics.size() == DialogueListenState::MaxTopics) break;
        observed.State.Topics.push_back({index,
            String(topic->Text ? topic->Text : "").substr(0, DialogueListenState::MaxText), !topic->NeverSaid});
    }
    LeaveCriticalSection(critical);
    // Mardoxx/skyrimui DialogueMenu.as: TopicList = TopicListHolder.List_mc;
    // selectedEntry.topicIndex is the index passed to TopicClicked, not a row index.
    constexpr const char* paths[] = {
        "_root.DialogueMenu_mc.TopicList.selectedEntry.topicIndex",
        "_root.DialogueMenu_mc.TopicListHolder.List_mc.selectedEntry.topicIndex"};
    for (const auto* path : paths)
    {
        double index{};
        if (Number(aMenu->uiMovie, path, index) && index >= 0 && index < 1024 && std::floor(index) == index)
        {
            for (const auto& topic : observed.State.Topics)
                if (topic.Index == static_cast<uint32_t>(index)) observed.State.Highlighted = topic.Index;
            break;
        }
    }
    if (auto* camera = PlayerCamera::Get())
    {
        const auto fov = camera->GetWorldFov();
        if (std::isfinite(fov) && fov >= 1 && fov <= 179)
            observed.State.Fov = static_cast<uint32_t>(std::round(fov));
    }
    std::lock_guard lock(s_mutex);
    if (s_observed.NpcFormId == observed.NpcFormId)
    {
        observed.State.Chosen = s_observed.State.Chosen;
        observed.State.ChosenText = s_observed.State.ChosenText;
        observed.State.ChoiceSerial = s_observed.State.ChoiceSerial;
    }
    s_observed = std::move(observed);
}

void HookAdvance(IMenu* aMenu, float aInterval, uint32_t aTime)
{
    s_advance(aMenu, aInterval, aTime);
    Capture(aMenu);
}

using Click = void(void*);
Click* s_click{};
void HookClick(void* aArgs)
{
    // DialogueMenu::TopicClicked, 51509 / 14092AB70, FxDelegateArgs.
    const auto* bytes = static_cast<const uint8_t*>(aArgs);
    if (bytes && *reinterpret_cast<void* const*>(bytes + 0x18) &&
        *reinterpret_cast<const uint32_t*>(bytes + 0x30) == 1)
    {
        const auto* value = *reinterpret_cast<Value* const*>(bytes + 0x28);
        if (value && (value->Type & 0x8F) == 3 && value->Data.Number >= 0 &&
            value->Data.Number < 1024)
        {
            std::lock_guard lock(s_mutex);
            const auto index = static_cast<uint32_t>(value->Data.Number);
            for (const auto& topic : s_observed.State.Topics)
                if (topic.Index == index)
                {
                    s_observed.State.Chosen = index;
                    s_observed.State.ChosenText = topic.Text;
                    s_observed.State.ChoiceSerial = ++s_choiceSerial;
                    break;
                }
        }
    }
    s_click(aArgs);
}

using Hide = void*(SubtitleManager*, TESObjectREFR*);
Hide* s_hide{};
void* HookHide(SubtitleManager* aManager, TESObjectREFR* aSpeaker)
{
    if (aSpeaker && MenuTopicManager::IsPlayerDialogueSpeaker(aSpeaker))
    {
        std::lock_guard lock(s_mutex);
        s_hiddenForm = aSpeaker->formID;
        ++s_hideSerial;
    }
    return s_hide(aManager, aSpeaker);
}

// CommonLib InputEvent/IDEvent/ButtonEvent, verified by MenuControls::ProcessEvent
// 52200 / 14095F020. Filter only this sink's linked view, then restore every link.
struct Input
{
    void* Vtable;
    uint32_t Device;
    uint32_t Type;
    Input* Next;
};
struct Button : Input
{
    void* UserEvent;
    uint32_t Code;
    uint32_t Pad;
    float Value;
    float Held;
};
static_assert(sizeof(Button) == 0x30);
using MenuInput = uint32_t(void*, Input* const*, void*);
MenuInput* s_menuInput{};
uint32_t HookMenuInput(void* aControls, Input* const* aEvents, void* aSource)
{
    static bool eatTab{}, eatB{};
    if (!aEvents || (!s_listeningInput.load() && !eatTab && !eatB))
        return s_menuInput(aControls, aEvents, aSource);
    Input* head = *aEvents;
    auto** link = &head;
    std::vector<std::pair<Input**, Input*>> removed;
    while (*link)
    {
        auto* event = *link;
        bool eat = false;
        if (event->Type == 0)
        {
            auto* button = static_cast<Button*>(event);
            const bool tab = button->Device == 0 && button->Code == 0x0F;
            const bool back = button->Device == 2 && button->Code == 0x2000;
            if (tab || back)
            {
                bool& held = tab ? eatTab : eatB;
                if (s_listeningInput.load() && button->Value > 0)
                { held = true; s_exitRequested.store(true); }
                eat = held;
                if (button->Value == 0) held = false;
            }
        }
        if (eat)
        {
            removed.emplace_back(link, event);
            *link = event->Next;
        }
        else link = &event->Next;
    }
    const auto result = s_menuInput(aControls, &head, aSource);
    for (auto it = removed.rbegin(); it != removed.rend(); ++it) *it->first = it->second;
    return result;
}

TiltedPhoques::Initializer s_hooks([] {
    POINTER_SKYRIMSE(Click, click, 51509);
    s_click = click.Get(); TP_HOOK(&s_click, HookClick);
    POINTER_SKYRIMSE(Hide, hide, 52627);
    s_hide = hide.Get(); TP_HOOK(&s_hide, HookHide);
    POINTER_SKYRIMSE(MenuInput, menuInput, 52200);
    s_menuInput = menuInput.Get(); TP_HOOK(&s_menuInput, HookMenuInput);
});
}

void ObserveMenu() noexcept
{
    auto* ui = UI::Get();
    static BSFixedString name("Dialogue Menu");
    auto* menu = ui && ui->GetMenuOpen(name) ? ui->FindMenuByName(name) : nullptr;
    if (!menu) { Reset(); return; }
    auto** table = *reinterpret_cast<void***>(menu);
    if (table != s_menuVtable)
    {
        DWORD previous{};
        if (VirtualProtect(&table[5], sizeof(void*), PAGE_EXECUTE_READWRITE, &previous))
        {
            s_advance = reinterpret_cast<Advance*>(table[5]);
            table[5] = reinterpret_cast<void*>(&HookAdvance);
            DWORD ignored{};
            VirtualProtect(&table[5], sizeof(void*), previous, &ignored);
            s_menuVtable = table;
        }
    }
}

Observation Read() noexcept
{
    std::lock_guard lock(s_mutex);
    return s_observed;
}
void Reset() noexcept
{
    std::lock_guard lock(s_mutex);
    s_observed = {};
}
uint64_t SubtitleHideSerial(uint32_t aFormId) noexcept
{
    std::lock_guard lock(s_mutex);
    return s_hiddenForm == aFormId ? s_hideSerial : 0;
}

struct Presentation::State
{
    bool Held{};
    uint32_t CameraState{};
    float Fov{};
    float AppliedFov{};
    NiPoint3 Rotation{};
    NiPoint3 AppliedRotation{};
    struct Handler { PlayerInputHandler* Pointer{}; bool Enabled{}; };
    std::vector<Handler> Handlers;
};
Presentation::Presentation() : m_state(std::make_unique<State>()) {}
Presentation::~Presentation() { End(); }

bool Presentation::Begin(Actor* aNpc) noexcept
{
    End();
    auto* player = PlayerCharacter::Get();
    auto* camera = PlayerCamera::Get();
    auto* controls = PlayerControls::GetInstance();
    if (!aNpc || !player || !camera || !camera->state || !controls ||
        (camera->state->id != 0 && camera->state->id != 9)) return false;
    auto& state = *m_state;
    state.CameraState = camera->state->id;
    state.Fov = state.AppliedFov = camera->GetWorldFov();
    state.Rotation = state.AppliedRotation = player->rotation;
    // Keep MenuControls live for Tab/B. Do not overwrite ControlMap's script locks.
    for (auto* handler : {controls->pMovementHandler, controls->pLookHandler,
        controls->pSprintHandler, controls->pAutoMoveHandler, controls->pJumpHandler,
        controls->pActivateHandler, controls->togglePOVHandler, controls->attackBlockHandler,
        controls->pReadyWeaponHandler, controls->shoutHandler})
    {
        if (handler) { state.Handlers.push_back({handler, handler->isEnabled}); handler->isEnabled = false; }
    }
    controls->Data.MoveInputVec = {}; controls->Data.PrevMoveVec = {};
    controls->Data.LookInputVec = {}; controls->Data.PrevLookVec = {};
    controls->Data.bAutoMove = false;
    state.Held = true;
    s_exitRequested.store(false);
    s_listeningInput.store(true);
    camera->ForceFirstPerson();
    if (!camera->state || camera->state->id != 0) { End(); return false; }
    return true;
}

bool Presentation::Update(Actor* aNpc, uint32_t aFov) noexcept
{
    auto* player = PlayerCharacter::Get();
    auto* camera = PlayerCamera::Get();
    auto* controls = PlayerControls::GetInstance();
    if (!m_state->Held || !aNpc || !player || !camera || !camera->state ||
        camera->state->id != 0 || !controls) return false;
    // Yield to scripts that reclaim an input handler while we are listening.
    for (const auto& handler : m_state->Handlers)
        if (handler.Pointer->isEnabled) return false;
    if (camera->GetWorldFov() != m_state->AppliedFov) return false;
    auto target = aNpc->position;
    target.z += 100.f;
    static BSFixedString head("NPC Head [Head]");
    if (auto* root = aNpc->GetNiNode())
        if (auto* node = root->GetByName(head)) target = node->world.translate;
    auto origin = player->position;
    origin.z += 110.f;
    if (camera->cameraNode) origin = camera->cameraNode->world.translate;
    const auto dx = target.x - origin.x, dy = target.y - origin.y, dz = target.z - origin.z;
    const auto pitch = std::atan2(-dz, std::sqrt(dx * dx + dy * dy));
    const auto yaw = std::atan2(dx, dy);
    player->SetRotation(pitch, player->rotation.y, yaw);
    m_state->AppliedRotation = player->rotation;
    m_state->AppliedFov = static_cast<float>(aFov);
    camera->SetWorldFov(m_state->AppliedFov);
    controls->Data.MoveInputVec = {}; controls->Data.PrevMoveVec = {};
    return true;
}

void Presentation::End() noexcept
{
    auto& state = *m_state;
    if (!state.Held) return;
    state.Held = false;
    s_listeningInput.store(false);
    for (const auto& handler : state.Handlers)
        if (!handler.Pointer->isEnabled) handler.Pointer->isEnabled = handler.Enabled;
    state.Handlers.clear();
    if (auto* camera = PlayerCamera::Get())
    {
        if (camera->GetWorldFov() == state.AppliedFov) camera->SetWorldFov(state.Fov);
        if (camera->state && camera->state->id == 0)
        {
            if (auto* player = PlayerCharacter::Get(); player &&
                player->rotation.x == state.AppliedRotation.x && player->rotation.z == state.AppliedRotation.z)
                player->SetRotation(state.Rotation.x, state.Rotation.y, state.Rotation.z);
            if (state.CameraState == 9) camera->ForceThirdPerson();
        }
    }
}

bool Presentation::ExitPressed() noexcept
{
    return s_exitRequested.exchange(false);
}
}
