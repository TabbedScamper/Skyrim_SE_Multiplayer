#pragma once

#include <type_traits>
#include <cassert>
#include <string_view>

#include <Misc/GameVM.h>

struct TESForm;
struct TESObjectREFR;
struct PapyrusFunctionRegisterEvent;

/**
 * @brief Handles registering and executing Papyrus functions.
 */
struct PapyrusService
{
    PapyrusService(entt::dispatcher& aDispatcher) noexcept;
    ~PapyrusService() noexcept = default;

    TP_NOCOPYMOVE(PapyrusService);

    const void* Get(const String& acNamespace, const String& acFunction) const noexcept;

    void HandlePapyrusFunctionEvent(const PapyrusFunctionRegisterEvent&) noexcept;
    // From the VM's native registration hook (the game's thread that registers them), at once.
    void Register(const char* apNamespace, const char* apName, void* apFunction) noexcept;
    [[nodiscard]] size_t RegisteredCount() const noexcept;

private:
    mutable std::mutex m_lock;
    Map<String, void*> m_functions;

    entt::scoped_connection m_papyrusFunctionRegisterConnection;
};

// Looks up a registered Papyrus native (PapyrusService, filled as the VM registers its natives). A wrapper built by
// name retries the lookup until it resolves: the PAPYRUS_FUNCTION statics are initialized on first use, and a first use
// before the VM had registered the native kept a null pointer for the whole process (host crash in Actor::IsInCombat,
// follower crash in TESObjectREFR::RemoveAllItems, 2026-09-29). An unresolved call returns a default value instead.
const void* ResolvePapyrusFunction(const char* apNamespace, const char* apName) noexcept;
// Names still unresolved after the VM registered its natives (a test gate: must stay 0).
extern std::atomic<uint32_t> g_unresolvedPapyrusNatives;

#ifndef NDEBUG
inline bool IsLatentPapyrusNative(const char* apNamespace, const char* apName) noexcept
{
    if (!apNamespace || !apName)
        return false;
    // SkyrimAtlas papyrus/natives.tsv: all 48 binders marked latent for 1.7.104.
    constexpr std::string_view latent[] = {
        "Actor::MoveToPackageLocation", "Actor::PathToReference", "Actor::Resurrect", "Actor::ShowGiftMenu",
        "Debug::CenterOnCellAndWait", "Debug::GetConfigName", "Debug::GetPlatformName", "Debug::GetVersionNumber",
        "Debug::PlayerMoveToAndWait", "Faction::SendPlayerToJail", "Game::AddHavokBallAndSocketConstraint",
        "Game::GetGameSettingString", "Game::RemoveHavokConstraints", "Keyword::SendStoryEventAndWait",
        "MagicEffect::GetAssociatedSkill", "Message::Show", "ObjectReference::AddItem",
        "ObjectReference::ApplyHavokImpulse", "ObjectReference::DamageObject", "ObjectReference::Delete",
        "ObjectReference::Disable", "ObjectReference::DropObject", "ObjectReference::Enable",
        "ObjectReference::ForceAddRagdollToWorld", "ObjectReference::ForceRemoveRagdollFromWorld",
        "ObjectReference::MoveTo", "ObjectReference::MoveToInteractionLocation", "ObjectReference::MoveToMyEditorLocation",
        "ObjectReference::MoveToNode", "ObjectReference::PlayAnimationAndWait",
        "ObjectReference::PlaySyncedAnimationAndWaitSS", "ObjectReference::RemoveItem", "ObjectReference::Reset",
        "ObjectReference::SetAngle", "ObjectReference::SetMotionType", "ObjectReference::SetPosition",
        "ObjectReference::SetScale", "ObjectReference::WaitForAnimationEvent", "Quest::SetCurrentStageID",
        "Quest::Start", "Scroll::Cast", "Sound::PlayAndWait", "Spell::Cast", "Spell::RemoteCast",
        "Utility::GameTimeToString", "Utility::Wait", "Utility::WaitGameTime", "Utility::WaitMenuMode"};
    const std::string_view ns(apNamespace), name(apName);
    for (const auto entry : latent)
    {
        const auto split = entry.find("::");
        if (entry.substr(0, split) == ns && entry.substr(split + 2) == name)
            return true;
    }
    return false;
}
#endif

template <class TFunction> struct PapyrusBinding
{
    PapyrusBinding(const void* apAddress) noexcept
        : m_pFunction(reinterpret_cast<TFunction>(apAddress))
    {
    }
    PapyrusBinding(const char* apNamespace, const char* apName) noexcept
        : m_pNamespace(apNamespace)
        , m_pName(apName)
    {
        Resolve();
    }
    bool Resolve() const noexcept
    {
        if (!m_pFunction && m_pName)
            m_pFunction = reinterpret_cast<TFunction>(ResolvePapyrusFunction(m_pNamespace, m_pName));
        return m_pFunction != nullptr;
    }
    mutable TFunction m_pFunction{};
    const char* m_pNamespace{};
    const char* m_pName{};
};

template <class Return, class Type, class... Args> struct PapyrusFunction
{
    using TFunction = Return(__fastcall*)(BSScript::IVirtualMachine*, uint32_t, const Type*, Args...);

    PapyrusFunction(const void* apAddress)
        : m_binding(apAddress)
    {
    }
    PapyrusFunction(const char* apNamespace, const char* apName)
        : m_binding(apNamespace, apName)
    {
    }

    explicit operator bool() const noexcept { return m_binding.Resolve(); }

    Return operator()(const Type* apThis, Args... args) const noexcept
    {
#ifndef NDEBUG
        assert(!IsLatentPapyrusNative(m_binding.m_pNamespace, m_binding.m_pName) &&
               "latent native must use an explicit wrapper and stack id");
#endif
        return CallWithStackId(UINT32_MAX, apThis, std::forward<Args>(args)...);
    }

    Return CallWithStackId(uint32_t aStackId, const Type* apThis, Args... args) const noexcept
    {
        assert(aStackId != 0 && "mod Papyrus calls need a nonzero stack id for latent natives");
        if (!m_binding.Resolve())
        {
            if constexpr (std::is_void_v<Return>)
                return;
            else
                return Return{};
        }
        return m_binding.m_pFunction(GameVM::Get()->virtualMachine, aStackId, apThis, std::forward<Args>(args)...);
    }

private:
    PapyrusBinding<TFunction> m_binding;
};

template <class Return, class... Args> struct GlobalPapyrusFunction
{
    using TFunction = Return(__fastcall*)(BSScript::IVirtualMachine*, Args...);

    GlobalPapyrusFunction(const void* apAddress)
        : m_binding(apAddress)
    {
    }
    GlobalPapyrusFunction(const char* apNamespace, const char* apName)
        : m_binding(apNamespace, apName)
    {
    }

    Return operator()(Args... args) const noexcept
    {
        if (!m_binding.Resolve())
        {
            if constexpr (std::is_void_v<Return>)
                return;
            else
                return Return{};
        }
        return m_binding.m_pFunction(GameVM::Get()->virtualMachine, std::forward<Args>(args)...);
    }

private:
    PapyrusBinding<TFunction> m_binding;
};

struct RefrOrInventoryObj
{
    const TESObjectREFR* pRefr;
    TESForm* pInventoryForm;
    uint16_t itemCount;
};

template <class Return, class Type, class... Args> struct LatentPapyrusFunction
{
    using TFunction = Return(__fastcall*)(BSScript::IVirtualMachine*, uint32_t, const RefrOrInventoryObj&, Args...);

    LatentPapyrusFunction(const void* apAddress)
        : m_binding(apAddress)
    {
    }
    LatentPapyrusFunction(const char* apNamespace, const char* apName)
        : m_binding(apNamespace, apName)
    {
    }

    Return operator()(const Type* apThis, Args... args) const noexcept
    {
        if (!m_binding.Resolve())
        {
            if constexpr (std::is_void_v<Return>)
                return;
            else
                return Return{};
        }
        RefrOrInventoryObj self{apThis, nullptr, 0};

        return m_binding.m_pFunction(GameVM::Get()->virtualMachine, UINT32_MAX, self, std::forward<Args>(args)...);
    }

private:
    PapyrusBinding<TFunction> m_binding;
};

#define PAPYRUS_FUNCTION(returnType, scope, name, ...) static PapyrusFunction<returnType, scope, __VA_ARGS__> s_p##name(#scope, #name);
#define GLOBAL_PAPYRUS_FUNCTION(returnType, scope, name, ...) static GlobalPapyrusFunction<returnType, __VA_ARGS__> s_p##name(#scope, #name);
#define LATENT_PAPYRUS_FUNCTION(returnType, scope, name, ...) static LatentPapyrusFunction<returnType, scope, __VA_ARGS__> s_p##name(#scope, #name);
