#include <Services/PlayerCollision.h>

#include <World.h>
#include <Components.h>
#include <Games/ActorExtension.h>
#include <Games/Skyrim/Actor.h>
#include <Games/Skyrim/PlayerCharacter.h>
#include <Games/Skyrim/AI/Movement/PlayerControls.h>
#include <AI/AIProcess.h>
#include <Services/PartyService.h>

#include <chrono>
#include <unordered_map>

namespace
{
// Skyrim collision filter info: bit 14 is "no collision".
constexpr uint32_t kNoCollision = 1u << 14;
constexpr auto kSettleAfterScript = std::chrono::seconds(5);

bool s_mirroring = false;
std::chrono::steady_clock::time_point s_lastScripted{};
// Original filter of each remote player whose controller we changed.
std::unordered_map<uint32_t, uint32_t> s_changed;

// bhkCharacterController of an actor: AIProcess -> middle-high process +0x250 (ID 39856).
void* GetController(Actor* apActor) noexcept
{
    auto* pProcess = apActor ? apActor->currentProcess : nullptr;
    if (!pProcess || !pProcess->middleProcess)
        return nullptr;
    return *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(pProcess->middleProcess) + 0x250);
}

// bhkCharacterController virtual slots 8/9: GetCollisionFilterInfo(uint32&) / SetCollisionFilterInfo(uint32).
uint32_t GetFilter(void* apController) noexcept
{
    uint32_t filter{};
    using TGet = uint32_t*(void*, uint32_t*);
    (*reinterpret_cast<TGet***>(apController))[8](apController, &filter);
    return filter;
}

void SetFilter(void* apController, uint32_t aFilter) noexcept
{
    using TSet = void(void*, uint32_t);
    (*reinterpret_cast<TSet***>(apController))[9](apController, aFilter);
}

bool LocalPlayerScripted() noexcept
{
    auto* pPlayer = PlayerCharacter::Get();
    if (!pPlayer)
        return true;
    // PlayerCharacter::SetAIDriven keeps its flag in bit 3 of +0xBEA.
    const bool aiDriven = (*(reinterpret_cast<const uint8_t*>(pPlayer) + 0xBEA) & 0x8) != 0;
    auto* pControls = PlayerControls::GetInstance();
    const bool movement = pControls && pControls->pMovementHandler && pControls->pMovementHandler->isEnabled;
    return aiDriven || !movement || s_mirroring;
}
} // namespace

namespace PlayerCollision
{
void SetMirroringScript(const bool aMirroring) noexcept
{
    s_mirroring = aMirroring;
}

void Update(World& aWorld) noexcept
{
    const auto& party = aWorld.GetPartyService();
    const auto now = std::chrono::steady_clock::now();
    bool separate = false;
    if (party.IsInParty() && party.GetSessionState() >= 1)
    {
        if (party.GetSessionState() < 3 || LocalPlayerScripted())
            s_lastScripted = now;
        separate = now - s_lastScripted < kSettleAfterScript;
    }

    auto view = aWorld.view<FormIdComponent, RemoteComponent>();
    for (auto entity : view)
    {
        const auto formId = view.get<FormIdComponent>(entity).Id;
        auto* pActor = Cast<Actor>(TESForm::GetById(formId));
        if (!pActor || !pActor->GetExtension()->IsRemotePlayer())
            continue;
        void* pController = GetController(pActor);
        if (!pController)
            continue;
        const uint32_t filter = GetFilter(pController);
        const auto it = s_changed.find(formId);
        if (separate && !(filter & kNoCollision))
        {
            s_changed[formId] = filter;
            SetFilter(pController, filter | kNoCollision);
            spdlog::info("Players pass through each other: {:X} (filter {:08X})", formId, filter);
        }
        else if (!separate && it != s_changed.end())
        {
            SetFilter(pController, it->second & ~kNoCollision);
            s_changed.erase(it);
            spdlog::info("Players collide again: {:X}", formId);
        }
    }
}
} // namespace PlayerCollision
