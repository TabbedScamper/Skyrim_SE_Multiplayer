#include <TiltedOnlinePCH.h>
#include <Services/DoorVoteService.h>
#include <Services/PlayerCollision.h>
#include <World.h>
#include <Games/TES.h>
#include <Games/Misc/Lock.h>
#include <PlayerCharacter.h>
#include <TESObjectREFR.h>
#include <Forms/TESObjectCELL.h>
#include <Forms/TESWorldSpace.h>
#include <Forms/TESBoundObject.h>
#include <Forms/TESQuest.h>
#include <Forms/BGSKeyword.h>
#include <Components/TESFullName.h>
#include <Components/BGSKeywordForm.h>
#include <Interface/UI.h>
#include <Events/UpdateEvent.h>
#include <Events/DisconnectedEvent.h>
#include <Messages/DoorVoteRequest.h>
#include <Messages/NotifyDoorVote.h>
#include <Messages/NotifyLeaderControl.h>
#include <cmath>
#include <atomic>

// Research handoff for docs/REFERENCE_RESEARCH.md (outside this task's edit allowlist):
// CommonLibSSE-NG src/RE/P/PlayerCharacter.cpp maps ActivatePickRef to AE ID 40548.
// https://github.com/CharmedBaryon/CommonLibSSE-NG/blob/main/src/RE/P/PlayerCharacter.cpp
// TiltedEvolution's TESObjectREFR.cpp supplies the existing ActivateRef detour/replay.
// https://github.com/tiltedphoques/TiltedEvolution/blob/master/Code/client/Games/Skyrim/TESObjectREFR.cpp
// Adopt its native activation ABI; reject player-argument-only detection, since scripts use it too.
// 1.7.104 corpus: ActivateHandler::ProcessButton 42420 / 0x1407B2660 calls
// ActivatePickRef 40548 / 0x140751580, whose CALL at +0x10D targets
// ActivateRef 19796 / 0x1402F11B0. The old +0x112 return site IS correct.
// Papyrus Activate 56139 / 0x140A446E0 is a separate caller with the same arguments.
// ActivateChoiceMenuCallback 40926 / 0x140769000 calls ActivateRef at +0x6E only
// for the player's default Activate choice; perk alternatives never use that CALL.
// Helgen's exterior entrances use AutoLoadDoor01 (FNAM 2), not manual activation.
// CommonLib include/RE/T/TESObjectDOOR.h names flag value 0x2 kAutomatic.
// https://github.com/CharmedBaryon/CommonLibSSE-NG/blob/main/include/RE/T/TESObjectDOOR.h
// Clone3D 17934 / 0x1402848F0 attaches BSPlayerDistanceCheckController;
// its update 40201 / 0x140738120 calls the door callback only on distance crossings.
// Callback 17933 / 0x1402846B0 starts a fade on entering band 1. AutoDoorFadeCallback
// 17967 / 0x140285CF0 queues player+0x7E8, consumed by player update
// 40447 / 0x140745200 at CALL +0x74C (return +0x751), NOT 40548+0x112.
// Intercept the distance callback before the fade, not the later ActivateRef call
// which would leave a voting player looking at a black screen.
// MQ101 aliases 98/99 are the linked interior doors of BOTH exterior entrances;
// objective 50 aliases 124/125 are markers, not doors. Use the running-alias rule,
// never quest/form IDs in policy. Checked Skyrim.esm and QF_MQ101_0003372B.psc.

namespace
{
using TAutomaticDoor = void(void*, uint32_t, bool);
TAutomaticDoor* s_automaticDoor{};

void OnAutomaticDoor(void* aObject3D, uint32_t aDistanceBand, bool aEntering)
{
    // This controller measures the local player, not NPCs or Papyrus activators.
    // Band 0 is the approach label; band 1 starts travel. Preserve leaving/label callbacks.
    if (DoorVotePolicy::IsAutomaticEntry(aDistanceBand, aEntering))
    {
        using TFindReference = TESObjectREFR*(void*);
        POINTER_SKYRIMSE(TFindReference, findReference, 19750);
        auto* door = findReference.Get()(aObject3D);
        if (World::Get().GetDoorVoteService().TryHold(door, PlayerCharacter::Get(), 0, nullptr, 1, 0, nullptr, true))
            return;
    }
    s_automaticDoor(aObject3D, aDistanceBand, aEntering);
}

void ReportSkip(TESObjectREFR* aDoor, DoorVotePolicy::Skip aReason, const void* aCaller) noexcept
{
    static constexpr const char* reasons[] = {"none", "missing reference", "not a door", "not a load door",
        "not the local player", "not player input", "offline", "not in a party", "fewer than two members",
        "locked", "free door", "already loading", "missing source cell", "missing destination", "too far away",
        "unmapped door", "unmapped source cell", "unmapped destination", "unmapped worldspace", "send failed"};
    static_assert(std::size(reasons) == static_cast<size_t>(DoorVotePolicy::Skip::Count));
    // Per reason, so NPC/script traffic cannot hide an input or transport failure.
    static std::array<std::atomic<uint64_t>, std::size(reasons)> next{};
    auto& deadline = next[static_cast<size_t>(aReason)];
    const auto now = GetTickCount64();
    auto previous = deadline.load(std::memory_order_relaxed);
    if (now >= previous && deadline.compare_exchange_strong(previous, now + 5000, std::memory_order_relaxed))
        spdlog::info("Door vote: skip {:X} ({}) caller={:X}", aDoor ? aDoor->formID : 0,
            reasons[static_cast<size_t>(aReason)], reinterpret_cast<uintptr_t>(aCaller));
}

// CommonLib's ExtraTeleport/DoorTeleportData and BGSLocation prefixes. Only read here.
struct TeleportData
{
    uint32_t LinkedDoor;
    NiPoint3 Position;
    NiPoint3 Rotation;
    uint32_t Flags;
};
struct TeleportExtra : BSExtraData
{
    TeleportData* Data;
};
struct LocationView : TESForm
{
    TESFullName FullName;
    BGSKeywordForm Keywords;
    LocationView* Parent;
};
static_assert(sizeof(TeleportData) == 0x20);
static_assert(offsetof(TeleportExtra, Data) == 0x10);
static_assert(offsetof(LocationView, Keywords) == 0x30);
static_assert(offsetof(LocationView, Parent) == 0x48);

constexpr const char* kKeywords[] = {
    "LocTypeDungeon", "LocTypeClearable", "LocTypeCave", "LocTypeMine",
    "LocTypeDwarvenAutomatons", "LocTypeDraugrCrypt", "LocTypeFalmerHive",
    "LocTypeAnimalDen", "LocTypeBanditCamp", "LocTypeVampireLair", "LocTypeWarlockLair",
    "LocTypeForswornCamp", "LocTypeGiantCamp", "LocTypeHagravenNest",
    "LocTypeDragonPriestLair", "LocTypeDragonLair"};

TESObjectREFR* DestinationDoor(TESObjectREFR* aDoor) noexcept
{
    if (!aDoor || !aDoor->baseForm || aDoor->baseForm->formType != FormType::Door)
        return nullptr;
    const auto* extra = static_cast<TeleportExtra*>(aDoor->extraData.GetByType(ExtraDataType::Teleport));
    return extra && extra->Data ? TESObjectREFR::GetByHandle(extra->Data->LinkedDoor) : nullptr;
}

void ShowNotice(const char* aText) noexcept
{
    using TShowHUDMessage = void(const char*, const char*, bool);
    POINTER_SKYRIMSE(TShowHUDMessage, show, 52933);
    show.Get()(aText, nullptr, true);
}

bool NearDoor(TESObjectREFR* aDoor) noexcept
{
    auto* player = PlayerCharacter::Get();
    if (!player || !aDoor || aDoor->IsDeleted() || aDoor->IsDisabled())
        return false;
    auto* cell = aDoor->GetParentCellEx();
    auto* playerCell = player->GetParentCellEx();
    if (!cell || !playerCell || (cell->worldspace ? playerCell->worldspace != cell->worldspace : cell != playerCell))
        return false;
    const auto delta = player->position - aDoor->position;
    const float distance = delta.x * delta.x + delta.y * delta.y + delta.z * delta.z;
    return std::isfinite(distance) && distance <= 400.f * 400.f;
}
}

DoorVoteService::DoorVoteService(World& aWorld, entt::dispatcher& aDispatcher, TransportService& aTransport) noexcept
    : m_world(aWorld)
    , m_transport(aTransport)
    , m_updateConnection(aDispatcher.sink<UpdateEvent>().connect<&DoorVoteService::OnUpdate>(this))
    , m_disconnectConnection(aDispatcher.sink<DisconnectedEvent>().connect<&DoorVoteService::OnDisconnected>(this))
    , m_notifyConnection(aDispatcher.sink<NotifyDoorVote>().connect<&DoorVoteService::OnNotify>(this))
    , m_controlConnection(aDispatcher.sink<NotifyLeaderControl>().connect<&DoorVoteService::OnLeaderControl>(this))
{
}

DoorVoteService::~DoorVoteService()
{
    Reset();
}

void DoorVoteService::InitKeywords() noexcept
{
    if (m_keywordsReady)
        return;
    auto* mods = ModManager::Get();
    if (!mods || !PlayerCharacter::Get() || !PlayerCharacter::Get()->parentCell)
        return;
    // TESDataHandler::AddFormToDataHandler indexes formArrays at +0x10, stride 0x18.
    // Keyword is form type 4, not a form ID. Resolve names once after data is loaded.
    const auto& keywords = *reinterpret_cast<const GameArray<BGSKeyword*>*>(reinterpret_cast<const uint8_t*>(mods) + 0x70);
    for (auto* keyword : keywords)
    {
        const auto* editorId = keyword ? keyword->GetFormEditorID() : nullptr;
        if (!editorId)
            continue;
        for (size_t i = 0; i < std::size(kKeywords); ++i)
            if (std::strcmp(editorId, kKeywords[i]) == 0)
                m_keywords[i] = keyword;
    }
    m_keywordsReady = true;
}

bool DoorVoteService::IsVoteDoor(TESObjectREFR* aDoor) noexcept
{
    const auto report = [aDoor](bool vote, const char* reason)
    {
        spdlog::info("Door vote: classify {:X} {} ({})", aDoor ? aDoor->formID : 0, vote ? "VOTE" : "FREE", reason);
        return vote;
    };
    if (!aDoor || !aDoor->baseForm || aDoor->baseForm->formType != FormType::Door ||
        !aDoor->extraData.Contains(ExtraDataType::Teleport))
        return report(false, "not a load door");
    const auto& party = m_world.GetPartyService();
    if (m_state.VoteId && m_state.Epoch == party.GetStartEpoch() &&
        m_world.GetModSystem().GetGameId(m_state.Door) == aDoor->formID)
        return report(true, "party vote already active");
    if (party.IsLeader() ? !PlayerCollision::LocalHasFreeControl() :
        (!m_leaderFree || party.IsFollowerCinematicInputGated()))
        return report(true, "leader has no free control");
    InitKeywords();
    auto* destination = DestinationDoor(aDoor);
    auto* cell = destination ? destination->GetParentCellEx() : nullptr;
    using TGetLocation = LocationView*(const TESObjectCELL*);
    POINTER_SKYRIMSE(TGetLocation, getLocation, 18905);
    // Bound traversal also tolerates malformed parent cycles in third-party plugins.
    auto* location = cell ? getLocation.Get()(cell) : nullptr;
    for (size_t depth = 0; location && depth < 64; ++depth, location = location->Parent)
    {
        for (size_t i = 0; i < std::size(kKeywords); ++i)
        {
            // A mine alone is free; Clearable already qualifies independently.
            if (i != 3 && m_keywords[i] && location->Keywords.Contains(m_keywords[i]))
                return report(true, kKeywords[i]);
        }
    }
    if (auto* mods = ModManager::Get())
    {
        for (auto* quest : mods->quests)
        {
            if (!quest)
                continue;
            if (quest->IsEnabled())
            {
                for (auto* alias : quest->aliases)
                {
                    if (!alias || !alias->IsReference())
                        continue;
                    auto* ref = quest->GetAliasedRef(alias->aliasID);
                    if (ref && (ref == aDoor || ref == destination))
                    {
                        spdlog::info("Door vote: quest {:X} alias {} matches {:X}", quest->formID, alias->aliasID, ref->formID);
                        return report(true, "running quest reference alias");
                    }
                }
            }
            if (!quest->IsActive())
                continue;
            for (auto* objective : quest->objectives)
            {
                if (!objective || objective->state != 1)
                    continue;
                // BGSQuestObjective: targets +0x10, count +0x18 (CommonLib).
                void** targets{};
                uint32_t count{};
                std::memcpy(&targets, objective->pad10, sizeof(targets));
                std::memcpy(&count, objective->pad10 + 8, sizeof(count));
                using TGetTarget = uint32_t*(void*, uint32_t*, bool, const TESQuest*);
                POINTER_SKYRIMSE(TGetTarget, getTarget, 25284);
                for (uint32_t i = 0; targets && i < count; ++i)
                {
                    if (!targets[i])
                        continue;
                    uint32_t handle{};
                    getTarget.Get()(targets[i], &handle, false, quest);
                    auto* ref = TESObjectREFR::GetByHandle(handle);
                    if (ref && (ref == aDoor || ref == destination))
                        return report(true, "active quest objective");
                }
            }
        }
    }
    return report(false, "no story or dungeon classification");
}

bool DoorVoteService::TryHold(TESObjectREFR* aDoor, TESObjectREFR* aActivator, uint8_t aUnk1,
    TESBoundObject* aObject, int32_t aCount, char aDefaultProcessing, const void* aCaller, bool aAtAutomaticDoor) noexcept
{
    using DoorVotePolicy::Skip;
    const auto skip = [aDoor, aCaller](Skip reason, bool held = false)
    {
        ReportSkip(aDoor, reason, aCaller);
        return held;
    };
    using TPick = void();
    POINTER_SKYRIMSE(TPick, pick, 40548);
    POINTER_SKYRIMSE(TPick, choice, 40926);
    const auto& party = m_world.GetPartyService();
    const bool input = aAtAutomaticDoor || DoorVotePolicy::IsInputCall(reinterpret_cast<uintptr_t>(aCaller),
        reinterpret_cast<uintptr_t>(pick.Get()), reinterpret_cast<uintptr_t>(choice.Get()));
    const auto reason = DoorVotePolicy::Decide(aDoor != nullptr,
        aDoor && aDoor->baseForm && aDoor->baseForm->formType == FormType::Door,
        aDoor && aDoor->extraData.Contains(ExtraDataType::Teleport),
        aActivator && aActivator == PlayerCharacter::Get(), input, m_transport.IsOnline(),
        party.IsInParty(), party.GetPartyMembers().size());
    if (reason != Skip::None)
        return skip(reason);
    if (const auto* lock = aDoor->GetLock(); lock && lock->IsLocked())
        return skip(Skip::Locked);
    if (!IsVoteDoor(aDoor))
        return skip(Skip::FreeDoor);
    if (m_loading)
        return skip(Skip::Loading, true);
    auto* destination = DestinationDoor(aDoor);
    auto* cell = aDoor->GetParentCellEx();
    auto* targetCell = destination ? destination->GetParentCellEx() : nullptr;
    DoorVoteRequest request;
    auto& mods = m_world.GetModSystem();
    const auto unavailable = !cell ? Skip::MissingCell : !targetCell ? Skip::MissingDestination :
        !NearDoor(aDoor) ? Skip::TooFar :
        !mods.GetServerModId(aDoor->formID, request.Door) ? Skip::UnmappedDoor :
        !mods.GetServerModId(cell->formID, request.Cell) ? Skip::UnmappedCell :
        !mods.GetServerModId(targetCell->formID, request.Destination) ? Skip::UnmappedDestination :
        (cell->worldspace && !mods.GetServerModId(cell->worldspace->formID, request.WorldSpace)) ?
        Skip::UnmappedWorldSpace : Skip::None;
    if (unavailable != Skip::None)
    {
        ShowNotice("Door vote unavailable: the destination is not ready.");
        return skip(unavailable, true);
    }
    request.Epoch = m_world.GetPartyService().GetStartEpoch();
    request.VoteId = m_state.Door == request.Door ? m_state.VoteId : 0;
    request.Position = glm::vec3(aDoor->position.x, aDoor->position.y, aDoor->position.z);
    const auto* name = targetCell->GetName();
    if (!name || !*name)
        name = aDoor->baseForm->GetName();
    request.Name = String(name && *name ? name : "the door").substr(0, 160);
    m_heldDoor = request.Door;
    m_doorForm = aDoor->formID;
    m_objectForm = aObject ? aObject->formID : 0;
    m_unk1 = aUnk1;
    m_count = aCount;
    m_defaultProcessing = aDefaultProcessing;
    m_withdrawSent = false;
    m_deadline = GetTickCount64() + 125000;
    if (!m_transport.Send(request))
    {
        Reset();
        ShowNotice("Door vote unavailable: connection interrupted.");
        return skip(Skip::SendFailed, true);
    }
    spdlog::info("Door vote: held {:X} for {} ({} members, {})", aDoor->formID,
        request.Name, party.GetPartyMembers().size(), aAtAutomaticDoor ? "automatic approach" : "player Activate");
    return true;
}

void DoorVoteService::SendAction(DoorVoteAction aAction) noexcept
{
    DoorVoteRequest request;
    static_cast<DoorVoteData&>(request) = m_state;
    request.Action = aAction;
    m_transport.Send(request);
}

void DoorVoteService::OnNotify(const NotifyDoorVote& aMessage) noexcept
{
    const auto& party = m_world.GetPartyService();
    if (!aMessage.IsValid() || !party.IsInParty() || aMessage.Epoch != party.GetStartEpoch() ||
        aMessage.VoteId == 0 || aMessage.VoteId < m_lastVoteId)
        return;
    m_lastVoteId = aMessage.VoteId;
    if (aMessage.Action == DoorVoteAction::State && !aMessage.Name.empty())
    {
        const auto prompt = fmt::format("Enter {}? (vote) {}/{} ready. Approach or activate the entrance to agree.",
            aMessage.Name, aMessage.ReadyCount, aMessage.TotalCount);
        ShowNotice(prompt.c_str());
    }
    else if (!aMessage.Notice.empty())
        ShowNotice(aMessage.Notice.c_str());
    if (aMessage.Action == DoorVoteAction::Cancel || aMessage.Action == DoorVoteAction::Release)
    {
        spdlog::info("Door vote: {} vote {}", aMessage.Action == DoorVoteAction::Cancel ? "cancelled" : "released", aMessage.VoteId);
        m_lastVoteId = aMessage.VoteId + 1;
        if (m_state.VoteId == aMessage.VoteId)
        {
            // A replacement activation can be awaiting its start notification.
            if (m_heldDoor != GameId{} && m_heldDoor != aMessage.Door)
                m_state = {};
            else
                Reset();
        }
        return;
    }
    const bool lostReadyVoter = DoorVotePolicy::LostReadyVoter(m_state.VoteId == aMessage.VoteId,
        m_state.ReadyCount, aMessage.ReadyCount, aMessage.Ready, m_loading);
    m_state = aMessage;
    m_deadline = GetTickCount64() + (aMessage.Action == DoorVoteAction::Go ? 305000 : 125000);
    if (aMessage.Action == DoorVoteAction::State)
    {
        m_state.Tick = 0;
        if (lostReadyVoter && !m_withdrawSent)
        {
            // The server may observe departure first and remove that player's readiness.
            // A still-ready participant can cancel even if the departed player's Failed is stale.
            m_withdrawSent = true;
            SendAction(DoorVoteAction::Failed);
            spdlog::info("Door vote: cancel requested for vote {} (a voter left)", m_state.VoteId);
        }
    }
    if (aMessage.Action == DoorVoteAction::Go)
        spdlog::info("Door vote: go at tick {} (vote {})", aMessage.Tick, aMessage.VoteId);
}

void DoorVoteService::OnLeaderControl(const NotifyLeaderControl& aMessage) noexcept
{
    m_leaderFree = aMessage.FreeControl;
}

void DoorVoteService::Reset() noexcept
{
    if (m_gateHeld)
    {
        if (auto* ui = UI::Get(); ui && ui->numPausesGame > 0)
            --ui->numPausesGame;
    }
    m_gateHeld = false;
    m_loading = false;
    m_activationTime = 0;
    m_state = {};
    m_heldDoor = {};
    m_doorForm = 0;
    m_withdrawSent = false;
    m_deadline = 0;
    m_nextLoadedSend = 0;
}

void DoorVoteService::OnDisconnected(const DisconnectedEvent&) noexcept
{
    Reset();
    m_lastVoteId = 0;
    m_leaderFree = true;
}

void DoorVoteService::OnUpdate(const UpdateEvent&) noexcept
{
    InitKeywords();
    const auto& party = m_world.GetPartyService();
    if (!party.IsInParty() || (m_state.VoteId && (m_state.Epoch != party.GetStartEpoch() ||
        m_state.TotalCount != party.GetPartyMembers().size())) ||
        (m_leader && m_leader != party.GetLeaderPlayerId()))
        Reset();
    m_leader = party.GetLeaderPlayerId();
    if (!m_deadline)
        return;
    if (GetTickCount64() > m_deadline)
    {
        ShowNotice("Door vote cancelled: timed out.");
        Reset();
        return;
    }
    auto* door = Cast<TESObjectREFR>(TESForm::GetById(m_doorForm));
    if (!m_loading && m_state.Ready && m_state.Door == m_heldDoor)
    {
        const bool goNow = m_state.Tick && m_world.GetTick() >= m_state.Tick;
        const auto* lock = door ? door->GetLock() : nullptr;
        auto* target = DestinationDoor(door);
        auto* targetCell = target ? target->GetParentCellEx() : nullptr;
        GameId targetId{};
        const bool validTarget = targetCell && m_world.GetModSystem().GetServerModId(targetCell->formID, targetId) &&
            targetId == m_state.Destination;
        if (!NearDoor(door) || !validTarget || (lock && lock->IsLocked()) || (goNow && m_withdrawSent))
        {
            if (goNow)
            {
                SendAction(DoorVoteAction::Failed);
                Reset();
                return;
            }
            if (!m_withdrawSent)
            {
                // Failed cancels the party's pending traversal for any ready voter, not only
                // the initiator. Keep the local hold until the cancellation is acknowledged.
                spdlog::info("Door vote: cancel requested for vote {} (left door, locked or destination changed)", m_state.VoteId);
                SendAction(DoorVoteAction::Failed);
            }
            m_withdrawSent = true;
            return;
        }
        if (goNow)
        {
            m_loading = true;
            m_activationTime = GetTickCount64();
            auto* object = m_objectForm ? Cast<TESBoundObject>(TESForm::GetById(m_objectForm)) : nullptr;
            // TESObjectREFR::Activate calls RealActivate directly, bypassing hold and network echo.
            const bool result = door->Activate(PlayerCharacter::Get(), m_unk1, object, m_count, m_defaultProcessing);
            spdlog::info("Door vote: activated {:X} at tick {} (shared {}, result {})", m_doorForm, m_world.GetTick(), m_state.Tick, result);
            if (!result)
            {
                SendAction(DoorVoteAction::Failed);
                Reset();
                return;
            }
        }
    }
    if (!m_loading)
        return;
    auto* player = PlayerCharacter::Get();
    auto* cell = player ? player->GetParentCellEx() : nullptr;
    auto* ui = UI::Get();
    GameId cellId{};
    if (!m_gateHeld && ui && !ui->GetMenuOpen(BSFixedString("Loading Menu")) &&
        GetTickCount64() > m_activationTime + 15000 &&
        (!cell || !m_world.GetModSystem().GetServerModId(cell->formID, cellId) || cellId != m_state.Destination))
    {
        SendAction(DoorVoteAction::Failed);
        Reset();
        return;
    }
    if (!cell || !cell->IsAttached() || !player->GetNiNode() || !ui || ui->GetMenuOpen(BSFixedString("Loading Menu")) ||
        !m_world.GetModSystem().GetServerModId(cell->formID, cellId) || cellId != m_state.Destination)
        return;
    if (!m_gateHeld)
    {
        ++ui->numPausesGame;
        m_gateHeld = true;
        ShowNotice("Waiting for the party to finish loading.");
        spdlog::info("Door vote: loaded, holding world gate for vote {}", m_state.VoteId);
    }
    if (GetTickCount64() >= m_nextLoadedSend)
    {
        // Cell-change replication may reach the server after this service's first ready packet.
        SendAction(DoorVoteAction::Loaded);
        m_nextLoadedSend = GetTickCount64() + 500;
    }
}

static TiltedPhoques::Initializer s_doorVoteInput([]()
{
    POINTER_SKYRIMSE(TAutomaticDoor, automaticDoor, 17933);
    s_automaticDoor = automaticDoor.Get();
    if (s_automaticDoor)
        TP_HOOK_IMMEDIATE(&s_automaticDoor, OnAutomaticDoor);
    if (!s_automaticDoor || s_automaticDoor == automaticDoor.Get())
        spdlog::error("Door vote: automatic-door hook unavailable (17933)");
    else
        spdlog::info("Door vote: automatic-door hook installed (17933, before fade)");
});
