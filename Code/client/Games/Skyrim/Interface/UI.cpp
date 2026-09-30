#include <Games/Skyrim/Interface/IMenu.h>
#include <Games/Skyrim/Interface/UI.h>
#include <Misc/BSFixedString.h>
#include <TiltedOnlinePCH.h>
#include "immersive_launcher/stubs/DllBlocklist.h"

#include <World.h>

static bool g_RequestUnpauseAll{false};

UI* UI::Get()
{
    POINTER_SKYRIMSE(UI*, s_instance, 400327);
    return *s_instance.Get();
}

bool UI::GetMenuOpen(const BSFixedString& acName) const
{
    if (acName.data == nullptr)
        return false;

    TP_THIS_FUNCTION(TMenuSystem_IsOpen, bool, const UI, const BSFixedString&);
    POINTER_SKYRIMSE(TMenuSystem_IsOpen, s_isMenuOpen, 82074);

    return TiltedPhoques::ThisCall(s_isMenuOpen.Get(), this, acName);
}

bool UI::SelectCharacterConfirmationForTest() noexcept
{
    // Port of CommonLibSSE-NG's MessageBoxMenu::SelectOption. This is only
    // used by the local test bridge after both native menus are visibly open.
    if (!GetMenuOpen(BSFixedString("RaceSex Menu")) ||
        !GetMenuOpen(BSFixedString("MessageBoxMenu")))
        return false;

    struct MessageDataView
    {
        uint8_t pad00[0x10];
        const char* bodyText;
        uint8_t pad18[0x40 - 0x18];
        void* callback;
        uint8_t pad48[0x4C - 0x48];
        uint8_t buttonPressOffset;
    };
    static_assert(offsetof(MessageDataView, callback) == 0x40);
    static_assert(offsetof(MessageDataView, buttonPressOffset) == 0x4C);

    POINTER_SKYRIMSE(GameArray<MessageDataView*>, s_messageQueue, 406362);
    auto* pQueue = s_messageQueue.Get();
    if (!pQueue || !pQueue->data || pQueue->length != 1 ||
        pQueue->capacity < pQueue->length)
        return false;
    auto* pData = pQueue->data[0];
    if (!pData || !pData->callback || !pData->bodyText)
        return false;
    char body[96]{};
    SIZE_T copied = 0;
    if (!ReadProcessMemory(GetCurrentProcess(), pData->bodyText, body,
            sizeof(body) - 1, &copied) ||
        std::string_view(body).find("Finish and name your character?") == std::string_view::npos)
        return false;

    auto* pCallback = pData->callback;
    auto* pCallbackVtable = *reinterpret_cast<void***>(pCallback);
    if (!pCallbackVtable || !pCallbackVtable[0] || !pCallbackVtable[1])
        return false;
    const uint8_t option = pData->buttonPressOffset;
    // IMessageBoxCallback is virtual (vptr +0); its native intrusive count
    // is at +8. Retain through the queue removal and callback re-entry.
    auto& count = *reinterpret_cast<uint32_t*>(reinterpret_cast<uint8_t*>(pCallback) + 8);
    std::atomic_ref<uint32_t> refCount(count);
    refCount.fetch_add(1, std::memory_order_acq_rel);

    using TRemove = void(void*, MessageDataView*);
    POINTER_SKYRIMSE(TRemove, s_removeMessage, 52284);
    if (!s_removeMessage.Get())
    {
        refCount.fetch_sub(1, std::memory_order_acq_rel);
        return false;
    }
    s_removeMessage.Get()(nullptr, pData);
    if (pQueue->length == 0)
    {
        POINTER_SKYRIMSE(void*, s_uiMessageQueue, 400445);
        using TAddMessage = void(void*, const BSFixedString&, UIMessage::UI_MESSAGE_TYPE, void*);
        POINTER_SKYRIMSE(TAddMessage, s_addMessage, 13631);
        if (s_uiMessageQueue.Get() && *s_uiMessageQueue.Get() && s_addMessage.Get())
            s_addMessage.Get()(*s_uiMessageQueue.Get(), BSFixedString("MessageBoxMenu"),
                UIMessage::kHide, nullptr);
    }
    using TRun = void(void*, uint8_t);
    reinterpret_cast<TRun*>(pCallbackVtable[1])(pCallback, option);
    if (refCount.fetch_sub(1, std::memory_order_acq_rel) == 1)
    {
        using TDeletingDestructor = void(void*, uint32_t);
        reinterpret_cast<TDeletingDestructor*>(pCallbackVtable[0])(pCallback, 1);
    }
    return true;
}

void UI::CloseAllMenus()
{
    TP_THIS_FUNCTION(TUI_CloseAll, void, const UI);
    POINTER_SKYRIMSE(TUI_CloseAll, s_CloseAll, 82088);

    TiltedPhoques::ThisCall(s_CloseAll.Get(), this);
}

BSFixedString* UI::LookupMenuNameByInstance(IMenu* apMenu)
{
    for (auto& it : menuMap)
    {
        if (it.value.spMenu == apMenu)
            return &it.key;
    }
    return nullptr;
}

IMenu* UI::FindMenuByName(const BSFixedString& acName)
{
    for (const auto& it : menuMap)
    {
        if (it.key == acName)
            return it.value.spMenu;
    }
    return nullptr;
}

void UI::DebugLogAllMenus()
{
    for (auto& e : menuStack)
    {
        spdlog::info("Menu {}", e->uiMenuFlags);
    }
}

static void UnfreezeMenu(IMenu* apEntry)
{
    if (apEntry->PausesGame())
        apEntry->ClearFlag(IMenu::kPausesGame);

    if (apEntry->FreezesBackground())
        apEntry->ClearFlag(IMenu::kFreezeFrameBackground);

    if (apEntry->FreezesFramePause())
        apEntry->ClearFlag(IMenu::kFreezeFramePause);
}

static constexpr const char* kAllowList[] = {
    "TweenMenu",     "MagicMenu",     "StatsMenu",     "InventoryMenu", "MessageBoxMenu",
    "ContainerMenu", "FavoritesMenu", "Tutorial Menu", "Console",       "Journal Menu",
    // "Lockpicking Menu" keeps its vanilla pause and freeze-frame (owner, 2026-09-29): unpaused, the minigame drew no
    // lock and its look/move input moved the character. BusyLockService keeps a lock to one picker at a time.
    //"MapMenu", // MapMenu is disabled till we find a proper fix for first person.
};

static void* (*UI_AddToActiveQueue)(UI*, IMenu*, void*);

static void* UI_AddToActiveQueue_Hook(UI* apSelf, IMenu* apMenu, void* apFoundItem /*In reality a reference*/)
{
    // if the menu is empty we let the real function handle it.
    if (!apMenu || !World::Get().GetTransport().IsConnected() || stubs::g_IsSoulsREActive)
        return UI_AddToActiveQueue(apSelf, apMenu, apFoundItem);

#if 0
        if (auto* pName = apSelf->LookupMenuNameByInstance(apEntry))
        {
            spdlog::info("Menu requested {}", pName->AsAscii());
        }
#endif

    // NOTE(Force): could also compare by RTTI later on...
    for (const char* item : kAllowList)
    {
        if (auto* pMenu = apSelf->FindMenuByName(item))
        {
            if (pMenu == apMenu)
                UnfreezeMenu(apMenu);
        }
    }

    return UI_AddToActiveQueue(apSelf, apMenu, apFoundItem);
}

using TCallback = void(void*, const BSFixedString*, uint32_t, void*);
static TCallback* UIMessageQueue__AddMessage_Real;

// Useful for debugging UI related issues.
void UIMessageQueue__AddMessage(void* a1, const BSFixedString* a2, UIMessage::UI_MESSAGE_TYPE a3, void* a4)
{
    spdlog::info("Adding Message {} with prio {} from {}", a2->AsAscii(), a3, fmt::ptr(_ReturnAddress()));
    UIMessageQueue__AddMessage_Real(a1, a2, a3, a4);
}

static TiltedPhoques::Initializer s_s(
    []()
    {
        // pray that this doesnt fail!
        VersionDbPtr<uint8_t> ProcessHook(82082);
        TiltedPhoques::SwapCall(ProcessHook.Get() + 0x682, UI_AddToActiveQueue, &UI_AddToActiveQueue_Hook);

        // Ignore startup movie
        // TODO: Move me later.
        VersionDbPtr<uint8_t> MainInit(36548);
        TiltedPhoques::Put<uint8_t>(MainInit.Get() + 0xFE, 0xEB);

        // Credits to Skyrim Souls RE for this fix.
        // Allows the favorites menu to be numbered during connect.
        VersionDbPtr<uint8_t> FavoritesCanProcess(51538);
        TiltedPhoques::Put<uint16_t>(FavoritesCanProcess.Get() + 0x15, 0x9090);

        // Some experiments:
        // POINTER_SKYRIMSE(TCallback, s_start, 13631);
        // UIMessageQueue__AddMessage_Real = s_start.Get();
        // TP_HOOK(&UIMessageQueue__AddMessage_Real, UIMessageQueue__AddMessage);

        // This kills the loading spinner
        // TiltedPhoques::Put<uint8_t>(0x1405D51C1, 0xEB);
        // TiltedPhoques::Nop(0x1405D51A2, 5);

        // use 8 threads by default!
        // TiltedPhoques::Put<uint8_t>(0x141E45770, 8);
    });
