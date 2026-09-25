#include <TiltedOnlinePCH.h>

#include <Services/GameTestService.h>
#include <Misc/NativeDispatchDiagnostic.h>
#include <Services/GameSettingsService.h>
#include <Services/OverlayService.h>
#include <Services/CameraService.h>
#include <Services/CharacterService.h>
#include <Services/PlayerService.h>
#include <Services/PapyrusService.h>
#include <Services/CorpseRagdollService.h>
#include <World.h>
#include <GameLoopDiagnostic.h>
#include <DInputHook.hpp>
#include <Games/Skyrim/BSInput/InputPollDiagnostic.h>

#include <Games/Skyrim/BSGraphics/BSGraphicsRenderer.h>
#include <Games/ActorExtension.h>
#include <Games/Misc/MenuTopicManager.h>
#include <Games/Skyrim/Interface/UI.h>
#include <Games/Skyrim/Interface/LoadingScreenProbe.h>
#include <Games/Skyrim/PlayerCharacter.h>
#include <Games/Skyrim/SaveLoad.h>
#include <Games/Skyrim/Forms/ActorValueInfo.h>
#include <Games/Skyrim/Forms/TESQuest.h>
#include <Games/Skyrim/Forms/TESNPC.h>
#include <Games/Skyrim/Forms/BGSOutfit.h>
#include <Games/Skyrim/Forms/TESObjectCELL.h>
#include <Games/Skyrim/Forms/TESWorldSpace.h>
#include <Games/Skyrim/Forms/TESPackage.h>
#include <Games/Skyrim/AI/Movement/PlayerControls.h>
#include <Games/Skyrim/Camera/PlayerCamera.h>
#include <Games/Skyrim/Camera/TESCameraState.h>
#include <Games/Skyrim/AI/AIProcess.h>
#include <Games/Skyrim/Actor.h>
#include <Forms/TESIdleForm.h>
#include <Games/Skyrim/Havok/PoseCopyAuthority.h>
#include <Combat/CombatController.h>
#include <Games/Skyrim/NetImmerse/NiNode.h>
#include <Games/Skyrim/BSAnimationGraphManager.h>
#include <Games/Skyrim/Havok/ActorPoseDiagnosticViews.h>
#include <Games/Skyrim/Havok/AnimationGraphUpdateTrace.h>
#include <Games/Skyrim/Havok/VisualPoseMailbox.h>
#include <Games/Skyrim/Havok/hkbGenerator.h>
#include <Games/Skyrim/Havok/hkbStateMachine.h>
#include <Games/TES.h>
#include <Services/PartyService.h>
#include <Services/TransportService.h>
#include <Services/ObjectService.h>
#include <Services/QuestService.h>
#include <Messages/PartyStartRequest.h>
#include <Components.h>
#include <Structs/AnimationGraphDescriptorManager.h>
#include <OverlayApp.hpp>
#include <OverlayRenderHandler.hpp>

namespace
{
constexpr wchar_t cTestPipeName[] = LR"(\\.\pipe\SkyrimSEMultiplayer.Test)";
constexpr DWORD cPipeRejectRemoteClients = 0x00000008;

std::string EscapeJson(const std::string& acValue)
{
    std::string result;
    result.reserve(acValue.size() + 16);
    for (const char value : acValue)
    {
        switch (value)
        {
        case '\\': result += "\\\\"; break;
        case '"': result += "\\\""; break;
        case '\n': result += "\\n"; break;
        case '\r': result += "\\r"; break;
        case '\t': result += "\\t"; break;
        default:
            if (static_cast<uint8_t>(value) < 0x20)
                result += fmt::format("\\u{:04x}", static_cast<uint8_t>(value));
            else
                result += value;
            break;
        }
    }
    return result;
}

std::string GetJsonString(const std::string& acJson, const char* acName)
{
    const std::string key = std::string("\"") + acName + "\"";
    auto position = acJson.find(key);
    if (position == std::string::npos)
        return {};
    position = acJson.find(':', position + key.size());
    if (position == std::string::npos)
        return {};
    position = acJson.find('"', position + 1);
    if (position == std::string::npos)
        return {};
    std::string result;
    for (++position; position < acJson.size(); ++position)
    {
        const char value = acJson[position];
        if (value == '"')
            break;
        if (value == '\\' && position + 1 < acJson.size())
        {
            const char escaped = acJson[++position];
            if (escaped == 'n') result += '\n';
            else if (escaped == 'r') result += '\r';
            else if (escaped == 't') result += '\t';
            else result += escaped;
        }
        else
            result += value;
    }
    return result;
}

uint64_t GetJsonId(const std::string& acJson)
{
    const auto key = acJson.find("\"id\"");
    if (key == std::string::npos)
        return 0;
    const auto colon = acJson.find(':', key + 4);
    if (colon == std::string::npos)
        return 0;
    return std::strtoull(acJson.c_str() + colon + 1, nullptr, 10);
}

std::string Result(uint64_t aId, const std::string& acPayload)
{
    return fmt::format("{{\"id\":{},\"ok\":true,{}}}", aId, acPayload);
}

std::string Error(uint64_t aId, const std::string& acMessage)
{
    return fmt::format("{{\"id\":{},\"ok\":false,\"error\":\"{}\"}}", aId, EscapeJson(acMessage));
}

const char* JsonBool(bool aValue)
{
    return aValue ? "true" : "false";
}

bool HandlerEnabled(const PlayerInputHandler* apHandler)
{
    return apHandler && apHandler->isEnabled;
}

constexpr uint64_t cFnvOffsetBasis = 14695981039346656037ull;
constexpr uint64_t cFnvPrime = 1099511628211ull;

void HashWord(uint64_t& arHash, uint32_t aWord) noexcept
{
    for (uint32_t shift = 0; shift < 32; shift += 8)
        arHash = (arHash ^ ((aWord >> shift) & 0xFFu)) * cFnvPrime;
}

struct SceneActionDiagnosticView
{
    void* Vtable{};
    uint32_t ActorId{};
    uint16_t StartPhase{};
    uint16_t EndPhase{};
    uint32_t Flags{};
    uint8_t Unknown14[4]{};
    uint32_t ActionId{};
    uint32_t Unknown1C{};
};
static_assert(offsetof(SceneActionDiagnosticView, ActionId) == 0x18);
static_assert(sizeof(SceneActionDiagnosticView) == 0x20);

bool IsReadableRange(const void* apData, size_t aSize) noexcept
{
    if (!apData || aSize == 0)
        return false;

    const auto begin = reinterpret_cast<uintptr_t>(apData);
    if (begin > std::numeric_limits<uintptr_t>::max() - aSize)
        return false;
    const auto end = begin + aSize;

    auto cursor = begin;
    while (cursor < end)
    {
        MEMORY_BASIC_INFORMATION information{};
        if (!VirtualQuery(reinterpret_cast<const void*>(cursor), &information, sizeof(information)))
            return false;
        if (information.State != MEM_COMMIT ||
            (information.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0)
            return false;
        switch (information.Protect & 0xFF)
        {
        case PAGE_READONLY:
        case PAGE_READWRITE:
        case PAGE_WRITECOPY:
        case PAGE_EXECUTE_READ:
        case PAGE_EXECUTE_READWRITE:
        case PAGE_EXECUTE_WRITECOPY:
            break;
        default:
            return false;
        }

        const auto regionBegin = reinterpret_cast<uintptr_t>(information.BaseAddress);
        if (regionBegin > std::numeric_limits<uintptr_t>::max() - information.RegionSize)
            return false;
        const auto regionEnd = regionBegin + information.RegionSize;
        if (regionEnd <= cursor)
            return false;
        cursor = (std::min)(end, regionEnd);
    }
    return true;
}

template <class T> bool ReadNative(const void* apData, T& arValue) noexcept
{
    if (!IsReadableRange(apData, sizeof(T)))
        return false;
    std::memcpy(&arValue, apData, sizeof(T));
    return true;
}

uint32_t ReadProcessHandle(const AIProcess* apProcess, size_t aOffset) noexcept
{
    uint32_t handle{};
    if (apProcess)
        ReadNative(reinterpret_cast<const uint8_t*>(apProcess) + aOffset, handle);
    return handle;
}

uint32_t ResolveHandleFormId(uint32_t aHandle) noexcept
{
    if (!aHandle || aHandle == UINT32_MAX)
        return 0;
    const auto* pReference = TESObjectREFR::GetByHandle(aHandle);
    return pReference ? pReference->formID : 0;
}

bool TryReadPackedGraphInput(const AnimationGraphDescriptor& acDescriptor,
    const AnimationVariables& acVariables, uint32_t aIndex,
    uint32_t& arRawValue) noexcept
{
    const auto boolean = std::find(acDescriptor.BooleanLookUpTable.begin(),
        acDescriptor.BooleanLookUpTable.end(), aIndex);
    if (boolean != acDescriptor.BooleanLookUpTable.end())
    {
        const auto offset = std::distance(acDescriptor.BooleanLookUpTable.begin(), boolean);
        if (offset >= static_cast<ptrdiff_t>(acVariables.Booleans.size()))
            return false;
        arRawValue = acVariables.Booleans[offset] ? 1u : 0u;
        return true;
    }
    const auto integer = std::find(acDescriptor.IntegerLookupTable.begin(),
        acDescriptor.IntegerLookupTable.end(), aIndex);
    if (integer != acDescriptor.IntegerLookupTable.end())
    {
        const auto offset = std::distance(acDescriptor.IntegerLookupTable.begin(), integer);
        if (offset >= static_cast<ptrdiff_t>(acVariables.Integers.size()))
            return false;
        arRawValue = acVariables.Integers[offset];
        return true;
    }
    const auto floating = std::find(acDescriptor.FloatLookupTable.begin(),
        acDescriptor.FloatLookupTable.end(), aIndex);
    if (floating == acDescriptor.FloatLookupTable.end())
        return false;
    const auto offset = std::distance(acDescriptor.FloatLookupTable.begin(), floating);
    if (offset >= static_cast<ptrdiff_t>(acVariables.Floats.size()))
        return false;
    std::memcpy(&arRawValue, &acVariables.Floats[offset], sizeof(arRawValue));
    return true;
}

std::string ReadNativeString(const char* apText, bool& arReadable, size_t aLimit = 256)
{
    arReadable = false;
    if (!apText)
    {
        arReadable = true;
        return {};
    }

    std::string result;
    result.reserve(64);
    for (size_t i = 0; i < aLimit; ++i)
    {
        char value{};
        if (!ReadNative(apText + i, value))
            return result;
        if (value == '\0')
        {
            arReadable = true;
            return result;
        }
        result.push_back(value);
    }
    return result;
}

void HashBytes(uint64_t& arHash, const void* apData, size_t aSize) noexcept
{
    const auto* pBytes = static_cast<const uint8_t*>(apData);
    for (size_t i = 0; i < aSize; ++i)
    {
        arHash ^= pBytes[i];
        arHash *= cFnvPrime;
    }
}

template <class T> void HashValue(uint64_t& arHash, const T& acValue) noexcept
{
    HashBytes(arHash, std::addressof(acValue), sizeof(T));
}

bool HashQuantizedFloats(uint64_t& arHash, const float* apValues, size_t aCount) noexcept
{
    for (size_t i = 0; i < aCount; ++i)
    {
        const double value = apValues[i];
        if (!std::isfinite(value) || std::abs(value) > 100000000.0)
            return false;
        const auto quantized = static_cast<int64_t>(std::llround(value * 1000.0));
        HashValue(arHash, quantized);
    }
    return true;
}

template <class T>
bool ValidateHavokArray(const ActorPoseDiagnosticViews::HavokArray<T>& acArray,
    int32_t aMaximum) noexcept
{
    constexpr uint32_t cCapacityMask = 0x3FFFFFFF;
    if (acArray.size < 0 || acArray.size > aMaximum)
        return false;
    const auto capacity = static_cast<uint32_t>(acArray.capacityAndFlags) & cCapacityMask;
    if (capacity < static_cast<uint32_t>(acArray.size))
        return false;
    if (acArray.size == 0)
        return true;
    return acArray.data && IsReadableRange(acArray.data,
        static_cast<size_t>(acArray.size) * sizeof(T));
}

struct GraphManagerRef
{
    BSAnimationGraphManager* pointer{};
    ~GraphManagerRef()
    {
        if (pointer)
            pointer->Release();
    }
};

// Sample the actual local graph and rendered bone transforms. Network pose
// checksums only establish packet delivery; they cannot diagnose a frozen
// follower graph or a horse whose bones never reach the scene graph.
struct NativeActorAnimationSample
{
    bool GraphReady{};
    uint32_t GraphCount{};
    uint32_t GraphIndex{};
    uint32_t StateId{};
    float TimeInState{};
    bool RootClonePresent{};
    bool RootCloneSameAsTemplate{};
    bool BehaviorActive{};
    bool BehaviorLinked{};
    bool CloneStateReadable{};
    bool CloneStateActive{};
    int32_t CloneStateId{-1};
    int32_t ClonePreviousStateId{-1};
    float CloneTimeInState{};
    uint32_t PoseCount{};
    uint64_t PoseChecksum{};
    uint32_t RenderBoneCount{};
    uint64_t RenderBoneChecksum{};
    uint64_t RenderWorldBoneChecksum{};
    uint32_t GraphVariableCount{};
    uint64_t GraphVariableChecksum{};
    std::vector<uint32_t> GraphVariables;
    uint32_t GraphVariableNameCount{};
    uint32_t GraphVariableInfoCount{};
    std::vector<std::string> GraphVariableNames;
};

NativeActorAnimationSample SampleNativeActorAnimation(Actor* apActor,
    bool aIncludeVariableNames = false)
{
    using namespace ActorPoseDiagnosticViews;
    NativeActorAnimationSample result;
    GraphManagerRef manager;
    if (!apActor || !apActor->animationGraphHolder.GetBSAnimationGraph(&manager.pointer) ||
        !manager.pointer)
        return result;
    BSScopedLock<BSRecursiveLock> graphLock(manager.pointer->lock);
    const auto count = manager.pointer->animationGraphs.size;
    const auto index = manager.pointer->animationGraphIndex;
    result.GraphCount = count;
    result.GraphIndex = index;
    if (!count || count > 32 || index >= count)
        return result;
    AnimationGraph graph{};
    if (!ReadNative(manager.pointer->animationGraphs.Get(index), graph))
        return result;
    result.GraphReady = true;
    if (graph.behaviorGraph && aIncludeVariableNames)
    {
        // Read the active graph's own string table. The player has separate
        // third- and first-person graphs, so graph-0 descriptors cannot name
        // graph-1 slots. Keep this diagnostic bounded and read-only.
        const auto readSafe = [](const void* apSource, void* apTarget,
            size_t aSize) noexcept {
            SIZE_T copied{};
            return apSource && apTarget && aSize &&
                ReadProcessMemory(GetCurrentProcess(), apSource, apTarget,
                    aSize, &copied) && copied == aSize;
        };
        const auto* pBehavior = reinterpret_cast<const uint8_t*>(graph.behaviorGraph);
        void* pGraphData{};
        void* pStringData{};
        HavokArray<const char*> names{};
        HavokArray<uint8_t> infos{};
        constexpr uint32_t cCapacityMask = 0x3FFFFFFF;
        if (readSafe(pBehavior + 0x88, &pGraphData, sizeof(pGraphData)) &&
            pGraphData && readSafe(
                reinterpret_cast<const uint8_t*>(pGraphData) + 0x78,
                &pStringData, sizeof(pStringData)) && pStringData &&
            readSafe(reinterpret_cast<const uint8_t*>(pGraphData) + 0x20,
                &infos, sizeof(infos)) &&
            readSafe(reinterpret_cast<const uint8_t*>(pStringData) + 0x30,
                &names, sizeof(names)) &&
            names.size > 0 && names.size <= 512 &&
            infos.size >= 0 && infos.size <= 512 &&
            (static_cast<uint32_t>(names.capacityAndFlags) & cCapacityMask) >=
                static_cast<uint32_t>(names.size) &&
            (static_cast<uint32_t>(infos.capacityAndFlags) & cCapacityMask) >=
                static_cast<uint32_t>(infos.size))
        {
            result.GraphVariableNameCount = static_cast<uint32_t>(names.size);
            result.GraphVariableInfoCount = static_cast<uint32_t>(infos.size);
            std::vector<const char*> namePointers(names.size);
            if (readSafe(names.data, namePointers.data(),
                    namePointers.size() * sizeof(const char*)))
            {
                result.GraphVariableNames.reserve(names.size);
                for (auto* pName : namePointers)
                {
                    pName = reinterpret_cast<const char*>(
                        reinterpret_cast<uintptr_t>(pName) & ~uintptr_t{1});
                    if (!pName)
                        break;
                    std::array<char, 96> buffer{};
                    if (!readSafe(pName, buffer.data(), buffer.size()))
                        break;
                    const auto* pEnd = static_cast<const char*>(std::memchr(
                        buffer.data(), '\0', buffer.size()));
                    if (!pEnd)
                        break;
                    result.GraphVariableNames.emplace_back(buffer.data(),
                        static_cast<size_t>(pEnd - buffer.data()));
                }
            }
        }
        void* pVariables{};
        void* pValues{};
        uint32_t variableCount{};
        if (ReadNative(reinterpret_cast<const uint8_t*>(graph.behaviorGraph) +
                0xD8, pVariables) && pVariables &&
            ReadNative(reinterpret_cast<const uint8_t*>(pVariables) + 0x10,
                pValues) &&
            ReadNative(reinterpret_cast<const uint8_t*>(pVariables) + 0x18,
                variableCount) && variableCount > 0 && variableCount <= 512 &&
            IsReadableRange(pValues, sizeof(uint32_t) * variableCount))
        {
            result.GraphVariableCount = variableCount;
            result.GraphVariables.assign(static_cast<const uint32_t*>(pValues),
                static_cast<const uint32_t*>(pValues) +
                    std::min<uint32_t>(variableCount,
                        aIncludeVariableNames ? 512u : 256u));
            uint64_t checksum = cFnvOffsetBasis;
            HashBytes(checksum, pValues, sizeof(uint32_t) * variableCount);
            result.GraphVariableChecksum = checksum;
        }
    }
    const auto poseCount = graph.characterInstance.numPoseLocal;
    if (poseCount > 0 && poseCount <= 1024 &&
        IsReadableRange(graph.characterInstance.poseLocal,
            static_cast<size_t>(poseCount) * sizeof(QsTransform)))
    {
        uint64_t checksum = cFnvOffsetBasis;
        bool valid = true;
        for (int32_t i = 0; i < poseCount && valid; ++i)
        {
            const auto& transform = graph.characterInstance.poseLocal[i];
            valid = HashQuantizedFloats(checksum, transform.translation, 4) &&
                HashQuantizedFloats(checksum, transform.rotation, 4) &&
                HashQuantizedFloats(checksum, transform.scale, 4);
        }
        if (valid)
        {
            result.PoseCount = poseCount;
            result.PoseChecksum = checksum;
        }
    }
    if (graph.behaviorGraph)
    {
        BehaviorGraph behavior{};
        StateMachine state{};
        const bool behaviorReadable = ReadNative(graph.behaviorGraph, behavior);
        auto* pState = behaviorReadable && behavior.rootGenerator &&
            IsReadableRange(behavior.rootGenerator, sizeof(void*)) ?
            Cast<hkbStateMachine>(reinterpret_cast<hkbGenerator*>(
                behavior.rootGenerator)) : nullptr;
        if (pState && ReadNative(pState, state))
        {
            result.StateId = state.currentStateID;
            result.TimeInState = state.timeInState;
        }
        if (behaviorReadable)
        {
            result.BehaviorActive = behavior.isActive;
            result.BehaviorLinked = behavior.isLinked;
            result.RootClonePresent = behavior.rootGeneratorClone != nullptr;
            result.RootCloneSameAsTemplate = behavior.rootGeneratorClone &&
                behavior.rootGeneratorClone == behavior.rootGenerator;
            uintptr_t templateVtable{};
            uintptr_t cloneVtable{};
            if (behavior.isActive && behavior.isLinked && pState &&
                behavior.rootGeneratorClone &&
                IsReadableRange(behavior.rootGeneratorClone,
                    sizeof(StateMachine)) &&
                ReadNative(behavior.rootGenerator, templateVtable) &&
                ReadNative(behavior.rootGeneratorClone, cloneVtable) &&
                templateVtable && cloneVtable == templateVtable)
            {
                StateMachine clone{};
                if (ReadNative(behavior.rootGeneratorClone, clone) &&
                    std::isfinite(clone.timeInState) &&
                    clone.timeInState >= 0.f && clone.timeInState < 1e7f &&
                    clone.currentStateID >= -1 &&
                    clone.currentStateID < 4096)
                {
                    result.CloneStateReadable = true;
                    result.CloneStateActive = clone.isActive;
                    result.CloneStateId = clone.currentStateID;
                    result.ClonePreviousStateId = clone.previousStateID;
                    result.CloneTimeInState = clone.timeInState;
                }
            }
        }
    }
    const auto renderCount = graph.boneNodes.length;
    if (renderCount > 0 && renderCount <= 1024 &&
        graph.boneNodes.capacity >= renderCount &&
        IsReadableRange(graph.boneNodes.data,
            static_cast<size_t>(renderCount) * sizeof(BoneNodeEntry)))
    {
        uint64_t checksum = cFnvOffsetBasis;
        uint64_t worldChecksum = cFnvOffsetBasis;
        uint32_t readable = 0;
        uint32_t worldReadable = 0;
        for (uint32_t i = 0; i < renderCount; ++i)
        {
            BoneNodeEntry entry{};
            NiTransform local{};
            if (!ReadNative(graph.boneNodes.data + i, entry) || !entry.node ||
                !ReadNative(reinterpret_cast<const uint8_t*>(entry.node) +
                    offsetof(NiAVObject, local), local))
                continue;
            if (!HashQuantizedFloats(checksum, &local.rotate.entry[0][0], 9) ||
                !HashQuantizedFloats(checksum, &local.translate.x, 3) ||
                !HashQuantizedFloats(checksum, &local.scale, 1))
                continue;
            ++readable;
            NiTransform world{};
            if (ReadNative(reinterpret_cast<const uint8_t*>(entry.node) +
                    offsetof(NiAVObject, world), world) &&
                HashQuantizedFloats(worldChecksum, &world.rotate.entry[0][0], 9) &&
                HashQuantizedFloats(worldChecksum, &world.translate.x, 3) &&
                HashQuantizedFloats(worldChecksum, &world.scale, 1))
                ++worldReadable;
        }
        result.RenderBoneCount = readable;
        if (readable)
            result.RenderBoneChecksum = checksum;
        if (worldReadable == readable && worldReadable)
            result.RenderWorldBoneChecksum = worldChecksum;
    }
    return result;
}

void AppendActorPoseDiagnostic(std::string& arSnapshot, Actor* apActor, const char* acpSource)
{
    using namespace ActorPoseDiagnosticViews;

    const auto* pExtension = apActor ? apActor->GetExtension() : nullptr;
    arSnapshot += fmt::format(
        "{{\"formId\":{},\"source\":\"{}\",\"dead\":{},\"bleedingOut\":{},"
        "\"graphDescriptor\":{}",
        apActor ? apActor->formID : 0, acpSource, JsonBool(apActor && apActor->IsDead()),
        JsonBool(apActor && apActor->actorState.IsBleedingOut()),
        pExtension ? pExtension->GraphDescriptorHash : 0);

    GraphManagerRef manager;
    const bool graphReady = apActor &&
        apActor->animationGraphHolder.GetBSAnimationGraph(&manager.pointer) && manager.pointer;
    arSnapshot += fmt::format(",\"graphReady\":{}", JsonBool(graphReady));
    if (!graphReady)
    {
        arSnapshot += ",\"validation\":{\"graphReadable\":false,\"reason\":\"manager-unavailable\"}}";
        return;
    }

    BSScopedLock<BSRecursiveLock> graphLock(manager.pointer->lock);
    const uint32_t graphCount = manager.pointer->animationGraphs.size;
    const uint32_t graphIndex = manager.pointer->animationGraphIndex;
    arSnapshot += fmt::format(",\"graphCount\":{},\"activeGraphIndex\":{}", graphCount, graphIndex);
    if (graphCount == 0 || graphCount > 32 || graphIndex >= graphCount)
    {
        arSnapshot += ",\"validation\":{\"graphReadable\":false,\"reason\":\"graph-index-invalid\"}}";
        return;
    }

    const auto* pNativeGraph = manager.pointer->animationGraphs.Get(graphIndex);
    AnimationGraph graph{};
    if (!ReadNative(pNativeGraph, graph))
    {
        arSnapshot += ",\"validation\":{\"graphReadable\":false,\"reason\":\"graph-range-unreadable\"}}";
        return;
    }

    bool projectNameReadable = false;
    const auto projectName = ReadNativeString(graph.projectName, projectNameReadable);
    BehaviorGraph characterBehavior{};
    const bool characterBehaviorReadable = graph.characterInstance.behaviorGraph &&
        ReadNative(graph.characterInstance.behaviorGraph, characterBehavior);
    arSnapshot += fmt::format(
        ",\"graph\":{{\"projectName\":\"{}\",\"projectNameReadable\":{},"
        "\"behaviorGraphPresent\":{},\"characterBehaviorGraphPresent\":{},"
        "\"characterBehaviorGraphSameAsGraph\":{},"
        "\"characterBehaviorReadable\":{},\"characterBehaviorActive\":{},"
        "\"characterBehaviorLinked\":{},\"characterRootClonePresent\":{},"
        "\"rootNodePresent\":{},\"physicsWorldPresent\":{},\"currentLod\":{},"
        "\"numTracksInLod\":{},\"poseLocalCount\":{},\"reportedAnimBoneCount\":{}}}",
        EscapeJson(projectName), JsonBool(projectNameReadable), JsonBool(graph.behaviorGraph != nullptr),
        JsonBool(graph.characterInstance.behaviorGraph != nullptr),
        JsonBool(graph.characterInstance.behaviorGraph == graph.behaviorGraph),
        JsonBool(characterBehaviorReadable),
        JsonBool(characterBehaviorReadable && characterBehavior.isActive),
        JsonBool(characterBehaviorReadable && characterBehavior.isLinked),
        JsonBool(characterBehaviorReadable && characterBehavior.rootGeneratorClone),
        JsonBool(graph.rootNode != nullptr),
        JsonBool(graph.physicsWorld != nullptr), graph.characterInstance.currentLOD,
        graph.characterInstance.numTracksInLOD, graph.characterInstance.numPoseLocal,
        graph.numAnimBones);

    const auto localPoseCount = graph.characterInstance.numPoseLocal;
    const bool localPoseReadable = localPoseCount > 0 && localPoseCount <= 1024 &&
        graph.characterInstance.poseLocal &&
        IsReadableRange(graph.characterInstance.poseLocal,
            static_cast<size_t>(localPoseCount) * sizeof(QsTransform));
    uint64_t localPoseChecksum = cFnvOffsetBasis;
    if (localPoseReadable)
    {
        for (int32_t i = 0; i < localPoseCount; ++i)
        {
            const auto& transform = graph.characterInstance.poseLocal[i];
            HashValue(localPoseChecksum, i);
            if (!HashQuantizedFloats(localPoseChecksum, transform.translation, 4) ||
                !HashQuantizedFloats(localPoseChecksum, transform.rotation, 4) ||
                !HashQuantizedFloats(localPoseChecksum, transform.scale, 4))
            {
                localPoseChecksum = 0;
                break;
            }
        }
    }
    arSnapshot += fmt::format(",\"evaluatedLocalPose\":{{\"readable\":{},\"count\":{},\"checksum\":{},\"transforms\":[",
        JsonBool(localPoseReadable && localPoseChecksum != 0),
        localPoseReadable ? localPoseCount : 0, localPoseReadable ? localPoseChecksum : 0);
    if (localPoseReadable && localPoseChecksum != 0)
    {
        for (int32_t i = 0; i < localPoseCount; ++i)
        {
            if (i != 0)
                arSnapshot += ',';
            const auto& transform = graph.characterInstance.poseLocal[i];
            arSnapshot += fmt::format(
                "{{\"t\":[{},{},{}],\"q\":[{},{},{},{}],\"s\":[{},{},{}]}}",
                transform.translation[0], transform.translation[1], transform.translation[2],
                transform.rotation[0], transform.rotation[1], transform.rotation[2],
                transform.rotation[3], transform.scale[0], transform.scale[1],
                transform.scale[2]);
        }
    }
    arSnapshot += "]}";

    BehaviorGraph behavior{};
    const bool behaviorReadable = graph.behaviorGraph && ReadNative(graph.behaviorGraph, behavior);
    StateMachine stateMachine{};
    bool stateMachineReadable = false;
    if (behaviorReadable && behavior.rootGenerator &&
        IsReadableRange(behavior.rootGenerator, sizeof(void*)))
    {
        auto* pStateMachine = Cast<hkbStateMachine>(
            reinterpret_cast<hkbGenerator*>(behavior.rootGenerator));
        stateMachineReadable = pStateMachine && ReadNative(pStateMachine, stateMachine);
    }
    StateMachine cloneStateMachine{};
    bool cloneStateMachineReadable = false;
    uintptr_t templateVtable{};
    uintptr_t cloneVtable{};
    if (stateMachineReadable)
        ReadNative(behavior.rootGenerator, templateVtable);
    if (stateMachineReadable && behavior.isActive && behavior.isLinked &&
        behavior.rootGeneratorClone &&
        IsReadableRange(behavior.rootGeneratorClone, sizeof(StateMachine)) &&
        ReadNative(behavior.rootGeneratorClone, cloneVtable) &&
        templateVtable && cloneVtable == templateVtable &&
        ReadNative(behavior.rootGeneratorClone, cloneStateMachine) &&
        std::isfinite(cloneStateMachine.timeInState) &&
        cloneStateMachine.timeInState >= 0.f &&
        cloneStateMachine.timeInState < 1e7f &&
        cloneStateMachine.currentStateID >= -1 &&
        cloneStateMachine.currentStateID < 4096)
        cloneStateMachineReadable = true;

    bool stateNameReadable = false;
    const auto stateMachineName = stateMachineReadable ?
        ReadNativeString(stateMachine.name, stateNameReadable) : std::string{};
    arSnapshot += fmt::format(
        ",\"graphState\":{{\"behaviorReadable\":{},\"active\":{},\"linked\":{},"
        "\"updateActiveNodes\":{},\"stateOrTransitionChanged\":{},"
        "\"rootStateMachineReadable\":{},\"rootStateMachineName\":\"{}\","
        "\"rootStateMachineNameReadable\":{},\"currentStateId\":{},"
        "\"previousStateId\":{},\"timeInState\":{},\"stateMachineActive\":{},"
        "\"stateMachineTransitionChanged\":{},"
        "\"clonePresent\":{},\"cloneReadable\":{},"
        "\"cloneCurrentStateId\":{},\"clonePreviousStateId\":{},"
        "\"cloneTimeInState\":{},\"cloneActive\":{}}}",
        JsonBool(behaviorReadable), JsonBool(behaviorReadable && behavior.isActive),
        JsonBool(behaviorReadable && behavior.isLinked),
        JsonBool(behaviorReadable && behavior.updateActiveNodes),
        JsonBool(behaviorReadable && behavior.stateOrTransitionChanged), JsonBool(stateMachineReadable),
        EscapeJson(stateMachineName), JsonBool(stateNameReadable),
        stateMachineReadable ? stateMachine.currentStateID : -1,
        stateMachineReadable ? stateMachine.previousStateID : -1,
        stateMachineReadable && std::isfinite(stateMachine.timeInState) ? stateMachine.timeInState : 0.f,
        JsonBool(stateMachineReadable && stateMachine.isActive),
        JsonBool(stateMachineReadable && stateMachine.stateOrTransitionChanged),
        JsonBool(behaviorReadable && behavior.rootGeneratorClone),
        JsonBool(cloneStateMachineReadable),
        cloneStateMachineReadable ? cloneStateMachine.currentStateID : -1,
        cloneStateMachineReadable ? cloneStateMachine.previousStateID : -1,
        cloneStateMachineReadable ? cloneStateMachine.timeInState : 0.f,
        JsonBool(cloneStateMachineReadable && cloneStateMachine.isActive));

    // Havok's active-node list, not rootGeneratorClone alone, identifies the
    // state machines currently participating in this character's behavior.
    ActiveNodeList activeNodeList{};
    const bool activeNodeListReadable = behaviorReadable && behavior.activeNodes &&
        ReadNative(behavior.activeNodes, activeNodeList) &&
        activeNodeList.size >= 0 && activeNodeList.size <= 1024 &&
        (activeNodeList.size == 0 ||
            IsReadableRange(activeNodeList.data,
                static_cast<size_t>(activeNodeList.size) * sizeof(ActiveNodeInfo)));
    const auto scannedActiveNodes = activeNodeListReadable ?
        std::min(activeNodeList.size, 256) : 0;
    arSnapshot += fmt::format(
        ",\"activeBehaviorNodes\":{{\"listReadable\":{},\"count\":{},"
        "\"scanned\":{},\"stateMachineTypeScope\":\"root-vtable\","
        "\"rootStateMachineTypeAvailable\":{},"
        "\"stateMachines\":[",
        JsonBool(activeNodeListReadable),
        activeNodeListReadable ? activeNodeList.size : 0, scannedActiveNodes,
        JsonBool(templateVtable != 0));
    uint32_t activeStateMachines = 0;
    for (int32_t i = 0; i < scannedActiveNodes && activeStateMachines < 32; ++i)
    {
        ActiveNodeInfo info{};
        const auto* pInfo = static_cast<const ActiveNodeInfo*>(activeNodeList.data) + i;
        if (!ReadNative(pInfo, info) || !info.nodeTemplate || !info.nodeClone ||
            !templateVtable)
            continue;
        uintptr_t nodeTemplateVtable{};
        uintptr_t nodeCloneVtable{};
        if (!ReadNative(info.nodeTemplate, nodeTemplateVtable) ||
            !ReadNative(info.nodeClone, nodeCloneVtable) ||
            nodeTemplateVtable != templateVtable ||
            nodeCloneVtable != templateVtable)
            continue;
        StateMachine activeState{};
        if (!ReadNative(info.nodeClone, activeState) ||
            !std::isfinite(activeState.timeInState) ||
            activeState.timeInState < 0.f || activeState.timeInState >= 1e7f ||
            activeState.currentStateID < -1 || activeState.currentStateID >= 4096)
            continue;
        bool activeNameReadable = false;
        const auto activeName = ReadNativeString(activeState.name, activeNameReadable);
        if (activeStateMachines++)
            arSnapshot += ',';
        arSnapshot += fmt::format(
            "{{\"index\":{},\"nodeId\":{},\"name\":\"{}\",\"nameReadable\":{},"
            "\"currentStateId\":{},\"previousStateId\":{},\"timeInState\":{},"
            "\"active\":{},\"nodeByte84\":{},\"nodeByte85\":{}}}",
            i, activeState.nodeID, EscapeJson(activeName), JsonBool(activeNameReadable),
            activeState.currentStateID, activeState.previousStateID,
            activeState.timeInState, JsonBool(activeState.isActive),
            info.byte84, info.byte85);
    }
    arSnapshot += fmt::format("],\"stateMachineCount\":{}}}", activeStateMachines);

    uint64_t boneChecksum = cFnvOffsetBasis;
    uint32_t validBoneCount = 0;
    uint32_t invalidBoneCount = 0;
    const bool boneArrayValid = graph.boneNodes.length <= 1024 &&
        graph.boneNodes.capacity >= graph.boneNodes.length &&
        (graph.boneNodes.length == 0 ||
            (graph.boneNodes.data && IsReadableRange(graph.boneNodes.data,
                static_cast<size_t>(graph.boneNodes.length) * sizeof(BoneNodeEntry))));
    if (boneArrayValid)
    {
        for (uint32_t i = 0; i < graph.boneNodes.length; ++i)
        {
            BoneNodeEntry entry{};
            NiTransform transform{};
            if (!ReadNative(graph.boneNodes.data + i, entry))
            {
                ++invalidBoneCount;
                continue;
            }
            const auto* pWorld = entry.node ?
                reinterpret_cast<const uint8_t*>(entry.node) + offsetof(NiAVObject, world) : nullptr;
            if (!pWorld || !ReadNative(pWorld, transform))
            {
                ++invalidBoneCount;
                continue;
            }

            uint64_t sampleHash = boneChecksum;
            HashValue(sampleHash, i);
            if (!HashQuantizedFloats(sampleHash, &transform.rotate.entry[0][0], 9) ||
                !HashQuantizedFloats(sampleHash, &transform.translate.x, 3) ||
                !HashQuantizedFloats(sampleHash, &transform.scale, 1))
            {
                ++invalidBoneCount;
                continue;
            }
            boneChecksum = sampleHash;
            ++validBoneCount;
        }
    }
    arSnapshot += fmt::format(
        ",\"skeleton\":{{\"boneArrayValid\":{},\"boneCount\":{},\"validBoneCount\":{},"
        "\"invalidBoneCount\":{},\"checksum\":\"{:016x}\",\"quantization\":0.001}}",
        JsonBool(boneArrayValid), boneArrayValid ? graph.boneNodes.length : 0,
        validBoneCount, invalidBoneCount, boneChecksum);

    // Keep root and sampled bone transforms in the one-shot probe. Matching
    // locals do not imply matching world transforms when the actor root or
    // the graph's world-update phase differs between peers.
    NiTransform rootLocal{};
    NiTransform rootWorld{};
    const bool rootReadable = graph.rootNode &&
        ReadNative(reinterpret_cast<const uint8_t*>(graph.rootNode) +
            offsetof(NiAVObject, local), rootLocal) &&
        ReadNative(reinterpret_cast<const uint8_t*>(graph.rootNode) +
            offsetof(NiAVObject, world), rootWorld);
    arSnapshot += fmt::format(
        ",\"renderRoot\":{{\"readable\":{},\"localT\":[{},{},{}],"
        "\"worldT\":[{},{},{}]}}",
        JsonBool(rootReadable), rootLocal.translate.x, rootLocal.translate.y,
        rootLocal.translate.z, rootWorld.translate.x,
        rootWorld.translate.y, rootWorld.translate.z);
    arSnapshot += ",\"renderLocalSamples\":[";
    std::string worldSamples;
    worldSamples += ",\"renderWorldSamples\":[";
    bool firstRenderLocal = true;
    bool firstRenderWorld = true;
    constexpr uint32_t cSampleIndices[] =
        {0, 5, 10, 20, 30, 40, 50, 60, 70, 80, 90, 98};
    if (boneArrayValid)
    {
        for (const auto index : cSampleIndices)
        {
            if (index >= graph.boneNodes.length)
                continue;
            BoneNodeEntry entry{};
            NiTransform local{};
            if (!ReadNative(graph.boneNodes.data + index, entry) ||
                !entry.node ||
                !ReadNative(reinterpret_cast<const uint8_t*>(entry.node) +
                    offsetof(NiAVObject, local), local))
                continue;
            if (!firstRenderLocal)
                arSnapshot += ',';
            firstRenderLocal = false;
            arSnapshot += fmt::format(
                "{{\"index\":{},\"t\":[{},{},{}],\"r\":[{},{},{},{},{},{},{},{},{}],\"s\":{}}}",
                index, local.translate.x, local.translate.y,
                local.translate.z, local.rotate.entry[0][0],
                local.rotate.entry[0][1], local.rotate.entry[0][2],
                local.rotate.entry[1][0], local.rotate.entry[1][1],
                local.rotate.entry[1][2], local.rotate.entry[2][0],
                local.rotate.entry[2][1], local.rotate.entry[2][2],
                local.scale);
            NiTransform world{};
            if (!ReadNative(reinterpret_cast<const uint8_t*>(entry.node) +
                    offsetof(NiAVObject, world), world))
                continue;
            if (!firstRenderWorld)
                worldSamples += ',';
            firstRenderWorld = false;
            worldSamples += fmt::format(
                "{{\"index\":{},\"t\":[{},{},{}],\"r\":[{},{},{},{},{},{},{},{},{}],\"s\":{}}}",
                index, world.translate.x, world.translate.y,
                world.translate.z, world.rotate.entry[0][0],
                world.rotate.entry[0][1], world.rotate.entry[0][2],
                world.rotate.entry[1][0], world.rotate.entry[1][1],
                world.rotate.entry[1][2], world.rotate.entry[2][0],
                world.rotate.entry[2][1], world.rotate.entry[2][2],
                world.scale);
        }
    }
    arSnapshot += ']';
    worldSamples += ']';
    arSnapshot += worldSamples;

    RagdollDriver driver{};
    const bool driverReadable = graph.characterInstance.ragdollDriver &&
        ReadNative(graph.characterInstance.ragdollDriver, driver);
    RagdollInstance ragdoll{};
    const bool ragdollReadable = driverReadable && driver.ragdoll &&
        ReadNative(driver.ragdoll, ragdoll);
    const bool bodyArrayValid = ragdollReadable && ValidateHavokArray(ragdoll.rigidBodies, 256);
    const bool constraintArrayValid = ragdollReadable &&
        ValidateHavokArray(ragdoll.constraints, 512);
    const bool boneMapValid = ragdollReadable &&
        ValidateHavokArray(ragdoll.boneToRigidBodyMap, 1024);

    uint64_t bodyChecksum = cFnvOffsetBasis;
    uint64_t boneMapChecksum = cFnvOffsetBasis;
    uint32_t validBodyCount = 0;
    uint32_t invalidBodyCount = 0;
    uint32_t bodiesInWorld = 0;
    uint32_t dynamicBodies = 0;
    uint32_t keyframedBodies = 0;
    uint32_t fixedBodies = 0;
    std::string bodySamples = "[";
    uint32_t sampledBodyCount = 0;
    if (bodyArrayValid)
    {
        for (int32_t i = 0; i < ragdoll.rigidBodies.size; ++i)
        {
            void* pBody{};
            RigidBody body{};
            if (!ReadNative(ragdoll.rigidBodies.data + i, pBody) || !pBody ||
                !ReadNative(pBody, body))
            {
                ++invalidBodyCount;
                continue;
            }

            uint64_t sampleHash = bodyChecksum;
            HashValue(sampleHash, i);
            HashValue(sampleHash, body.motionType);
            const bool inWorld = body.world != nullptr;
            HashValue(sampleHash, inWorld);
            if (!HashQuantizedFloats(sampleHash, body.transform, std::size(body.transform)) ||
                !HashQuantizedFloats(sampleHash, body.linearVelocity, std::size(body.linearVelocity)) ||
                !HashQuantizedFloats(sampleHash, body.angularVelocity, std::size(body.angularVelocity)))
            {
                ++invalidBodyCount;
                continue;
            }

            bodyChecksum = sampleHash;
            ++validBodyCount;
            bodiesInWorld += inWorld ? 1u : 0u;
            // Skyrim's hkpMotion types 2/3 (sphere/box inertia) and 6
            // (thin box) are simulated too, not just type 1.
            dynamicBodies += (body.motionType == 1 || body.motionType == 2 ||
                body.motionType == 3 || body.motionType == 6) ? 1u : 0u;
            keyframedBodies += body.motionType == 4 ? 1u : 0u;
            fixedBodies += body.motionType == 5 ? 1u : 0u;
            if (sampledBodyCount < 32)
            {
                if (sampledBodyCount != 0)
                    bodySamples += ',';
                bodySamples += fmt::format(
                    "{{\"index\":{},\"uid\":{},\"motionType\":{},\"inWorld\":{},"
                    "\"transform\":[{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{}],"
                    "\"linearVelocity\":[{},{},{}],\"angularVelocity\":[{},{},{}]}}",
                    i, body.uid, body.motionType, JsonBool(inWorld),
                    body.transform[0], body.transform[1], body.transform[2], body.transform[3],
                    body.transform[4], body.transform[5], body.transform[6], body.transform[7],
                    body.transform[8], body.transform[9], body.transform[10], body.transform[11],
                    body.transform[12], body.transform[13], body.transform[14], body.transform[15],
                    body.linearVelocity[0], body.linearVelocity[1], body.linearVelocity[2],
                    body.angularVelocity[0], body.angularVelocity[1], body.angularVelocity[2]);
                ++sampledBodyCount;
            }
        }
    }
    bodySamples += ']';
    if (boneMapValid)
    {
        for (int32_t i = 0; i < ragdoll.boneToRigidBodyMap.size; ++i)
        {
            int32_t bodyIndex{};
            if (!ReadNative(ragdoll.boneToRigidBodyMap.data + i, bodyIndex))
            {
                boneMapChecksum = 0;
                break;
            }
            HashValue(boneMapChecksum, bodyIndex);
        }
    }

    uint64_t transitionChecksum = cFnvOffsetBasis;
    const bool ragdollPresent = ragdollReadable;
    const bool graphTransitionChanged = behaviorReadable && behavior.stateOrTransitionChanged;
    const bool stateTransitionChanged = stateMachineReadable && stateMachine.stateOrTransitionChanged;
    const bool dead = apActor && apActor->IsDead();
    const bool bleedingOut = apActor && apActor->actorState.IsBleedingOut();
    HashValue(transitionChecksum, ragdollPresent);
    HashValue(transitionChecksum, bodiesInWorld);
    HashValue(transitionChecksum, dynamicBodies);
    HashValue(transitionChecksum, keyframedBodies);
    HashValue(transitionChecksum, fixedBodies);
    HashValue(transitionChecksum, graphTransitionChanged);
    HashValue(transitionChecksum, stateTransitionChanged);
    HashValue(transitionChecksum, dead);
    HashValue(transitionChecksum, bleedingOut);

    arSnapshot += fmt::format(
        ",\"ragdoll\":{{\"driverPresent\":{},\"driverReadable\":{},\"present\":{},"
        "\"bodyArrayValid\":{},\"bodyCount\":{},\"validBodyCount\":{},"
        "\"invalidBodyCount\":{},\"constraintArrayValid\":{},\"constraintCount\":{},"
        "\"boneMapValid\":{},\"boneMapCount\":{},\"bodiesInWorld\":{},"
        "\"dynamicBodies\":{},\"keyframedBodies\":{},\"fixedBodies\":{},"
        "\"bodyChecksum\":\"{:016x}\",\"boneMapChecksum\":\"{:016x}\","
        "\"bodySamples\":{}}}",
        JsonBool(graph.characterInstance.ragdollDriver != nullptr), JsonBool(driverReadable),
        JsonBool(ragdollPresent), JsonBool(bodyArrayValid),
        bodyArrayValid ? ragdoll.rigidBodies.size : 0, validBodyCount, invalidBodyCount,
        JsonBool(constraintArrayValid), constraintArrayValid ? ragdoll.constraints.size : 0,
        JsonBool(boneMapValid), boneMapValid ? ragdoll.boneToRigidBodyMap.size : 0,
        bodiesInWorld, dynamicBodies, keyframedBodies, fixedBodies, bodyChecksum,
        boneMapChecksum, bodySamples);
    arSnapshot += fmt::format(
        ",\"transitionObservables\":{{\"signature\":\"{:016x}\","
        "\"graphTransitionChanged\":{},\"stateMachineTransitionChanged\":{},"
        "\"ragdollBodiesAttached\":{},\"dead\":{},\"bleedingOut\":{}}}",
        transitionChecksum, JsonBool(graphTransitionChanged), JsonBool(stateTransitionChanged),
        JsonBool(bodiesInWorld != 0), JsonBool(dead), JsonBool(bleedingOut));
    arSnapshot += fmt::format(
        ",\"validation\":{{\"layoutSource\":\"commonlibsse-ng-b93280e\","
        "\"runtimeCandidate\":\"1.7.104\",\"graphReadable\":true,"
        "\"boneArrayValid\":{},\"ragdollArraysValid\":{},\"bodySamplesValid\":{}}}}}",
        JsonBool(boneArrayValid),
        JsonBool(!ragdollReadable || (bodyArrayValid && constraintArrayValid && boneMapValid)),
        JsonBool(!bodyArrayValid || invalidBodyCount == 0));
}
}

GameTestService::GameTestService(World& aWorld) noexcept
    : m_world(aWorld)
    , m_pipeThread([this]() { PipeMain(); })
{
    if (auto* pEvents = EventDispatcherManager::Get())
    {
        pEvents->triggerEnterEvent.RegisterSink(this);
        pEvents->triggerLeaveEvent.RegisterSink(this);
    }
    spdlog::info("In-game test bridge starting at \\\\.\\pipe\\SkyrimSEMultiplayer.Test");
}

GameTestService::~GameTestService() noexcept
{
    if (auto* pEvents = EventDispatcherManager::Get())
    {
        pEvents->triggerEnterEvent.UnRegisterSink(this);
        pEvents->triggerLeaveEvent.UnRegisterSink(this);
    }
    m_stopping = true;
    if (m_pipeThread.joinable())
        CancelSynchronousIo(static_cast<HANDLE>(m_pipeThread.native_handle()));
    // Wake a blocking ConnectNamedPipe during orderly shutdown.
    if (const HANDLE pipe = CreateFileW(cTestPipeName, GENERIC_READ | GENERIC_WRITE, 0, nullptr,
            OPEN_EXISTING, 0, nullptr); pipe != INVALID_HANDLE_VALUE)
        CloseHandle(pipe);
    if (m_pipeThread.joinable())
        m_pipeThread.join();
}

void GameTestService::RecordTriggerEvent(bool aEnter, uint32_t aTriggerFormId,
    uint32_t aActorFormId) noexcept
{
    std::lock_guard lock(m_triggerMutex);
    auto& event = m_triggerEvents[m_triggerNext];
    event = {++m_triggerSequence, GetTickCount64(), aTriggerFormId,
        aActorFormId, aEnter};
    m_triggerNext = (m_triggerNext + 1) % m_triggerEvents.size();
    m_triggerCount = std::min(m_triggerCount + 1, m_triggerEvents.size());
}

BSTEventResult GameTestService::OnEvent(const TESTriggerEnterEvent* apEvent,
    const EventDispatcher<TESTriggerEnterEvent>*)
{
    if (apEvent)
        RecordTriggerEvent(true, apEvent->pTrigger ? apEvent->pTrigger->formID : 0,
            apEvent->pActionRef ? apEvent->pActionRef->formID : 0);
    return BSTEventResult::kOk;
}

BSTEventResult GameTestService::OnEvent(const TESTriggerLeaveEvent* apEvent,
    const EventDispatcher<TESTriggerLeaveEvent>*)
{
    if (apEvent)
        RecordTriggerEvent(false, apEvent->pTrigger ? apEvent->pTrigger->formID : 0,
            apEvent->pActionRef ? apEvent->pActionRef->formID : 0);
    return BSTEventResult::kOk;
}

void GameTestService::WakeWindowThread() noexcept
{
    if (auto* pWindow = BSGraphics::GetMainWindow(); pWindow && pWindow->hWnd)
        PostMessageW(pWindow->hWnd, cGameTestWakeMessage, 0, 0);
}

void GameTestService::PipeMain() noexcept
{
    while (!m_stopping)
    {
        const HANDLE pipe = CreateNamedPipeW(cTestPipeName, PIPE_ACCESS_DUPLEX,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | cPipeRejectRemoteClients,
            1, 64 * 1024, 64 * 1024, 0, nullptr);
        if (pipe == INVALID_HANDLE_VALUE)
        {
            Sleep(1000);
            continue;
        }

        const bool connected = ConnectNamedPipe(pipe, nullptr) != FALSE || GetLastError() == ERROR_PIPE_CONNECTED;
        if (!connected || m_stopping)
        {
            CloseHandle(pipe);
            continue;
        }

        std::string buffered;
        char chunk[4096];
        DWORD bytesRead = 0;
        bool requestServed = false;
        while (!m_stopping && !requestServed && ReadFile(pipe, chunk, sizeof(chunk), &bytesRead, nullptr) && bytesRead)
        {
            buffered.append(chunk, bytesRead);
            for (auto newline = buffered.find('\n'); newline != std::string::npos; newline = buffered.find('\n'))
            {
                auto line = buffered.substr(0, newline);
                buffered.erase(0, newline + 1);
                if (!line.empty() && line.back() == '\r')
                    line.pop_back();
                if (line.empty())
                    continue;

                auto request = std::make_shared<Request>();
                request->Line = std::move(line);
                {
                    std::scoped_lock lock(m_queueMutex);
                    m_requests.push_back(request);
                }
                WakeWindowThread();

                std::unique_lock lock(request->Mutex);
                if (!request->Completed.wait_for(lock, 15s, [&]() { return request->Complete || m_stopping; }))
                    request->Response = Error(GetJsonId(request->Line), "window-thread timeout");
                request->Response += '\n';
                DWORD written = 0;
                if (!WriteFile(pipe, request->Response.data(), static_cast<DWORD>(request->Response.size()), &written, nullptr))
                    break;
                // The client is blocked reading this response, so flushing here
                // guarantees delivery before the one-shot server disconnects.
                FlushFileBuffers(pipe);
                // One request per connection keeps a disconnected or idle
                // diagnostic client from monopolizing the sole local pipe.
                requestServed = true;
                break;
            }
        }
        DisconnectNamedPipe(pipe);
        CloseHandle(pipe);
    }
}

void GameTestService::OnWindowThread() noexcept
{
    std::deque<std::shared_ptr<Request>> requests;
    {
        std::scoped_lock lock(m_queueMutex);
        requests.swap(m_requests);
    }
    for (const auto& request : requests)
    {
        const auto response = Execute(request->Line);
        {
            std::scoped_lock lock(request->Mutex);
            request->Response = response;
            request->Complete = true;
        }
        request->Completed.notify_one();
    }
}

void GameTestService::OnGameThread() noexcept
{
    InstallVirtualMachineDiagnostic();
    if (const uint32_t formId = m_testCorpseDisplaceFormId.exchange(0,
            std::memory_order_acq_rel))
    {
        auto* pActor = Cast<Actor>(TESForm::GetById(formId));
        auto* pExtension = pActor ? pActor->GetExtension() : nullptr;
        auto* pCell = pActor ? pActor->GetParentCellEx() : nullptr;
        if (!m_world.GetPartyService().IsInParty() ||
            m_world.GetPartyService().IsLeader() || !pActor ||
            !pExtension || !pExtension->IsRemote() ||
            pExtension->IsPlayer() || !pActor->actorState.IsDeadState() ||
            !pActor->GetNiNode() || !pCell ||
            pActor->GetNativeMountState().InteractionExtra)
        {
            spdlog::warn("Corpse displacement test rejected actor={:X}", formId);
        }
        else
        {
            NiPoint3 target = pActor->position;
            target.x += 320.f;
            pActor->MoveTo(pCell, target);
            spdlog::info("Corpse displacement test actor={:X} cell={:X} target=({},{},{})",
                formId, pCell->formID, target.x, target.y, target.z);
        }
    }
    if (m_nativeCreatorConfirmRequested.exchange(false, std::memory_order_acq_rel))
    {
        auto* pUI = UI::Get();
        const bool selected = pUI && pUI->SelectCharacterConfirmationForTest();
        {
            std::scoped_lock lock(m_snapshotMutex);
            m_nativeCreatorConfirmSucceeded = selected;
            m_nativeCreatorConfirmComplete = true;
        }
        spdlog::info("Native character confirmation test selection {}",
            selected ? "accepted" : "rejected");
    }
    const auto now = GetTickCount64();
    const auto* pTitleUI = UI::Get();
    const bool titleMenuOpen = pTitleUI && pTitleUI->GetMenuOpen(BSFixedString("TitleSequence Menu"));
    if (titleMenuOpen != m_titleSequenceMenuOpen)
    {
        m_titleSequenceMenuOpen = titleMenuOpen;
        m_lastTitleSequenceTransitionMs = now;
        ++m_titleSequenceMenuTransitions;
        spdlog::info("Title sequence menu {} tick={} transition={} leader={}",
            titleMenuOpen ? "opened" : "closed", now, m_titleSequenceMenuTransitions,
            m_world.GetPartyService().IsLeader());
    }
    // Sample at the game update cadence, not the slower cached-snapshot
    // cadence. This catches a one-frame physics snap even when an SSH-based
    // two-PC comparison happens to land between snaps.
    constexpr uint32_t cartIds[]{0x000B9DF3, 0x000BB970};
    constexpr uint32_t horseIds[]{0x000B9DF2, 0x000BB971};
    const auto frameTiming = GetGameLoopDiagnostic();
    auto recordMotion = [this, now, &frameTiming](uint32_t aFormId,
        uint32_t aDeltaMs, float aStep, const TESObjectREFR* apReference)
    {
        auto& event = m_hitchMotionEvents[m_hitchMotionNext];
        event = {now, m_world.GetTick(), aFormId,
            frameTiming.WorldLastEntryGapUs,
            frameTiming.VmLastEntryGapUs, frameTiming.VmLastAppUs,
            frameTiming.VmLastOriginalUs, frameTiming.WorldLastGameTestUs,
            aDeltaMs, aStep,
            {apReference ? apReference->position.x : 0.f,
                apReference ? apReference->position.y : 0.f,
                apReference ? apReference->position.z : 0.f}};
        const uint32_t horseId = aFormId == 0x000B9DF3 ? 0x000B9DF2 :
            (aFormId == 0x000BB970 ? 0x000BB971 : 0);
        auto* pHorse = horseId ? Cast<Actor>(TESForm::GetById(horseId)) : nullptr;
        if (pHorse && pHorse->loadedState)
        {
            event.HorsePresent = true;
            event.HorsePosition[0] = pHorse->position.x;
            event.HorsePosition[1] = pHorse->position.y;
            event.HorsePosition[2] = pHorse->position.z;
        }
        m_hitchMotionNext = (m_hitchMotionNext + 1) % m_hitchMotionEvents.size();
        m_hitchMotionCount = (std::min)(m_hitchMotionCount + 1,
            static_cast<uint32_t>(m_hitchMotionEvents.size()));
    };
    bool recordedGap = false;
    for (size_t i = 0; i < std::size(cartIds); ++i)
    {
        auto& stats = m_introCartMotionStats[i];
        auto* pCart = Cast<TESObjectREFR>(TESForm::GetById(cartIds[i]));
        if (!m_world.GetPartyService().IsInParty() || !pCart || !pCart->loadedState ||
            !std::isfinite(pCart->position.x) || !std::isfinite(pCart->position.y) ||
            !std::isfinite(pCart->position.z))
        {
            stats = {};
            continue;
        }
        if (stats.LastMs && now > stats.LastMs && now - stats.LastMs <= 100)
        {
            const float dx = pCart->position.x - stats.Position[0];
            const float dy = pCart->position.y - stats.Position[1];
            const float dz = pCart->position.z - stats.Position[2];
            const float step = std::sqrt(dx * dx + dy * dy + dz * dz);
            stats.PeakStep = (std::max)(stats.PeakStep, step);
            stats.PeakSpeed = (std::max)(stats.PeakSpeed, step * 1000.f / static_cast<float>(now - stats.LastMs));
            if (step >= 75.f && now - stats.LastMs <= 50)
                ++stats.LargeSteps;
            if (step >= 75.f || frameTiming.WorldLastEntryGapUs >= 50000)
            {
                recordMotion(cartIds[i], static_cast<uint32_t>(now - stats.LastMs),
                    step, pCart);
                recordedGap = recordedGap || frameTiming.WorldLastEntryGapUs >= 50000;
            }
        }
        else if (stats.LastMs && now > stats.LastMs &&
            frameTiming.WorldLastEntryGapUs >= 50000)
        {
            const float dx = pCart->position.x - stats.Position[0];
            const float dy = pCart->position.y - stats.Position[1];
            const float dz = pCart->position.z - stats.Position[2];
            recordMotion(cartIds[i], static_cast<uint32_t>((std::min)(
                uint64_t{UINT32_MAX}, now - stats.LastMs)),
                std::sqrt(dx * dx + dy * dy + dz * dz), pCart);
            recordedGap = true;
        }
        stats.Position[0] = pCart->position.x;
        stats.Position[1] = pCart->position.y;
        stats.Position[2] = pCart->position.z;
        stats.LastMs = now;
        ++stats.Samples;
    }
    if (frameTiming.WorldLastEntryGapUs >= 50000 && !recordedGap)
        recordMotion(0, 0, 0.f, nullptr);
    if (now >= m_nextHitchSnapshotMs)
    {
        m_nextHitchSnapshotMs = now + 100;
        auto& cartSample = m_hitchCartHistory[m_hitchCartNext];
        cartSample = {};
        cartSample.WorldTick = m_world.GetTick();
        for (size_t i = 0; i < std::size(cartIds); ++i)
        {
            const auto& stats = m_introCartMotionStats[i];
            cartSample.Present[i] = stats.Samples != 0;
            std::copy(std::begin(stats.Position), std::end(stats.Position),
                cartSample.Position[i].begin());
            auto* pHorse = Cast<Actor>(TESForm::GetById(horseIds[i]));
            if (pHorse && pHorse->loadedState)
            {
                cartSample.HorsePresent[i] = true;
                cartSample.HorsePosition[i] = {pHorse->position.x,
                    pHorse->position.y, pHorse->position.z};
            }
        }
        m_hitchCartNext = (m_hitchCartNext + 1) % m_hitchCartHistory.size();
        m_hitchCartCount = (std::min)(m_hitchCartCount + 1,
            static_cast<uint32_t>(m_hitchCartHistory.size()));
        const auto physics = ObjectService::GetPreStepPlaybackDiagnostic();
        const auto worldPhysics = ObjectService::GetWorldUpdateDiagnostic();
        const auto poseProduction =
            m_world.GetCharacterService().GetLocalPoseProductionDiagnostic();
        std::string hitch = fmt::format(
            "{{\"worldTick\":{},\"sampleTimeMs\":{},"
            "\"worldLastGapUs\":{},\"worldMaxGapUs\":{},"
            "\"worldMaxGameTestUs\":{},\"vmMaxAppUs\":{},"
            "\"vmMaxOriginalUs\":{},\"hostPhysicsPackets\":{},"
            "\"hostPhysicsUpdates\":{},\"hostBodyOnlyUpdates\":{},"
            "\"followerPhysicsPackets\":{},\"hostLastScanUs\":{},"
            "\"hostScanCount\":{},\"hostScanTotalUs\":{},"
            "\"hostScanMaxUs\":{},\"hostScanLastReferencesVisited\":{},"
            "\"hostScanLastCandidateCount\":{},"
            "\"hostKnownRefreshLastUs\":{},\"hostKnownRefreshMaxUs\":{},\"hostKnownRefreshTotalUs\":{},"
            "\"hostCurrentDiscoveryLastUs\":{},\"hostCurrentDiscoveryMaxUs\":{},\"hostCurrentDiscoveryTotalUs\":{},"
            "\"hostGridDiscoveryLastUs\":{},\"hostGridDiscoveryMaxUs\":{},\"hostGridDiscoveryTotalUs\":{},"
            "\"hostPruneLastUs\":{},\"hostPruneMaxUs\":{},\"hostPruneTotalUs\":{},"
            "\"poseSelectedBatches\":{},\"poseSelectedActors\":{},"
            "\"poseSelectedTotalUs\":{},\"poseSelectedMaxActorUs\":{},"
            "\"poseSelectedLastBatchUs\":{},\"poseSelectedLastBatchActors\":{},"
            "\"selectedBodyPeakStep\":{},"
            "\"selectedBodyStepsOver75\":{},"
            "\"selectedBodyPeakTimeMs\":{},"
            "\"selectedBodyPeakDt\":{},"
            "\"selectedBodyPeakPreLinearSpeed\":{},"
            "\"selectedBodyPeakPostLinearSpeed\":{},"
            "\"selectedBodyPeakPreAngularSpeed\":{},"
            "\"selectedBodyPeakPostAngularSpeed\":{},"
            "\"selectedBodyPeakMotionType\":{},"
            "\"selectedBodyPeakTargetApplied\":{},"
            "\"selectedBodyPeakTargetAgeMs\":{},"
            "\"selectedBodyPeakVelocityAfterWrite\":{},"
            "\"carts\":[",
            m_world.GetTick(), now, frameTiming.WorldLastEntryGapUs,
            frameTiming.WorldMaxEntryGapUs,
            frameTiming.WorldMaxGameTestUs, frameTiming.VmMaxAppUs,
            frameTiming.VmMaxOriginalUs, physics.HostPacketsSent,
            physics.HostUpdatesQueued, physics.HostBodyOnlyUpdates,
            physics.FollowerPacketsReceived, physics.LastHostScanDurationUs,
            physics.HostScans, physics.HostScanTotalUs, physics.HostScanMaxUs,
            physics.HostLastReferencesVisited, physics.HostLastCandidateCount,
            physics.HostKnownRefreshLastUs, physics.HostKnownRefreshMaxUs,
            physics.HostKnownRefreshTotalUs,
            physics.HostCurrentDiscoveryLastUs, physics.HostCurrentDiscoveryMaxUs,
            physics.HostCurrentDiscoveryTotalUs,
            physics.HostGridDiscoveryLastUs, physics.HostGridDiscoveryMaxUs,
            physics.HostGridDiscoveryTotalUs,
            physics.HostPruneLastUs, physics.HostPruneMaxUs,
            physics.HostPruneTotalUs,
            poseProduction.Batches, poseProduction.Actors,
            poseProduction.TotalUs, poseProduction.MaxActorUs,
            poseProduction.LastBatchUs, poseProduction.LastBatchActors,
            worldPhysics.PeakSelectedBodyStepDelta,
            worldPhysics.SelectedBodyStepsOver75Units,
            worldPhysics.PeakSelectedBodyStepTimeMs,
            worldPhysics.PeakSelectedBodyStepDt,
            worldPhysics.PeakSelectedBodyPreLinearSpeed,
            worldPhysics.PeakSelectedBodyPostLinearSpeed,
            worldPhysics.PeakSelectedBodyPreAngularSpeed,
            worldPhysics.PeakSelectedBodyPostAngularSpeed,
            worldPhysics.PeakSelectedBodyMotionType,
            JsonBool(worldPhysics.PeakSelectedBodyTargetApplied),
            worldPhysics.PeakSelectedBodyTargetAgeMs,
            worldPhysics.PeakSelectedBodyVelocityAfterWrite);
        for (size_t i = 0; i < std::size(cartIds); ++i)
        {
            if (i)
                hitch += ',';
            const auto& stats = m_introCartMotionStats[i];
            hitch += fmt::format(
                "{{\"formId\":{},\"samples\":{},\"peakStep\":{},"
                "\"largeSteps\":{},\"position\":[{},{},{}]}}",
                cartIds[i], stats.Samples, stats.PeakStep, stats.LargeSteps,
                stats.Position[0], stats.Position[1], stats.Position[2]);
        }
        hitch += "],\"history\":[";
        for (uint32_t i = 0; i < m_hitchCartCount; ++i)
        {
            const auto index = (m_hitchCartNext + m_hitchCartHistory.size() -
                m_hitchCartCount + i) % m_hitchCartHistory.size();
            const auto& sample = m_hitchCartHistory[index];
            if (i)
                hitch += ',';
            hitch += fmt::format(
                "{{\"tick\":{},\"present\":[{},{}],"
                "\"position\":[[{},{},{}],[{},{},{}]],"
                "\"horsePresent\":[{},{}],"
                "\"horsePosition\":[[{},{},{}],[{},{},{}]]}}",
                sample.WorldTick, JsonBool(sample.Present[0]),
                JsonBool(sample.Present[1]),
                sample.Position[0][0], sample.Position[0][1],
                sample.Position[0][2], sample.Position[1][0],
                sample.Position[1][1], sample.Position[1][2],
                JsonBool(sample.HorsePresent[0]),
                JsonBool(sample.HorsePresent[1]),
                sample.HorsePosition[0][0], sample.HorsePosition[0][1],
                sample.HorsePosition[0][2], sample.HorsePosition[1][0],
                sample.HorsePosition[1][1], sample.HorsePosition[1][2]);
        }
        hitch += "],\"events\":[";
        for (uint32_t i = 0; i < m_hitchMotionCount; ++i)
        {
            const auto index = (m_hitchMotionNext + m_hitchMotionEvents.size() -
                m_hitchMotionCount + i) % m_hitchMotionEvents.size();
            const auto& event = m_hitchMotionEvents[index];
            if (i)
                hitch += ',';
            hitch += fmt::format(
                "{{\"timeMs\":{},\"worldTick\":{},\"formId\":{},"
                "\"worldGapUs\":{},\"vmGapUs\":{},"
                "\"priorVmAppUs\":{},\"priorVmOriginalUs\":{},"
                "\"priorGameTestUs\":{},\"cartDeltaMs\":{},"
                "\"cartStep\":{},\"position\":[{},{},{}],"
                "\"horsePresent\":{},\"horsePosition\":[{},{},{}]}}",
                event.TimeMs, event.WorldTick, event.FormId,
                event.WorldGapUs, event.VmGapUs,
                event.PriorVmAppUs, event.PriorVmOriginalUs,
                event.PriorGameTestUs, event.CartDeltaMs, event.CartStep,
                event.Position[0], event.Position[1], event.Position[2],
                JsonBool(event.HorsePresent), event.HorsePosition[0],
                event.HorsePosition[1], event.HorsePosition[2]);
        }
        hitch += "]}";
        std::scoped_lock lock(m_snapshotMutex);
        m_hitchSnapshot = std::move(hitch);
    }
    bool captureRequested = false;
    {
        std::scoped_lock lock(m_snapshotScheduleMutex);
        if (m_snapshotRelativeDueWallMs)
        {
            const bool sameEpoch = m_world.GetTransport().GetAuthorityEpoch() ==
                m_snapshotTargetAuthorityEpoch;
            if ((sameEpoch && m_world.GetTick() >= m_snapshotTargetTick) ||
                GetTickCount64() >= m_snapshotRelativeDueWallMs)
            {
                captureRequested = true;
                m_snapshotRelativeDueWallMs = 0;
                m_snapshotTargetTick = 0;
            }
        }
        else if (m_snapshotTargetTick)
        {
            const auto currentEpoch = m_world.GetTransport().GetAuthorityEpoch();
            if (currentEpoch != m_snapshotTargetAuthorityEpoch)
            {
                spdlog::info("Canceled game snapshot across authority epoch {} -> {}",
                    m_snapshotTargetAuthorityEpoch, currentEpoch);
                m_snapshotTargetTick = 0;
            }
            else if (m_world.GetTick() >= m_snapshotTargetTick)
            {
                captureRequested = true;
                m_snapshotTargetTick = 0;
            }
        }
    }
    // The full native-actor audit can hold the game thread for seconds. Never
    // run it implicitly at startup; a caller must explicitly schedule it.
    if (!captureRequested)
        return;

    try
    {
        std::string snapshot = "{";
        std::array<uint32_t, 7> snapshotPhaseUs{};
        auto phaseStarted = std::chrono::steady_clock::now();
        auto markSnapshotPhase = [&](size_t aIndex)
        {
            const auto ended = std::chrono::steady_clock::now();
            snapshotPhaseUs[aIndex] = static_cast<uint32_t>((std::min)(
                int64_t{UINT32_MAX},
                std::chrono::duration_cast<std::chrono::microseconds>(
                    ended - phaseStarted).count()));
            phaseStarted = ended;
        };
        const auto worldTick = m_world.GetTick();
        snapshot += fmt::format("\"sampleTimeMs\":{},\"worldTick\":{}", now, worldTick);
        snapshot += fmt::format(",\"nativeCrashGuard\":{{\"nullKnockExplosionSkips\":{}}}",
            Actor::GetNullKnockExplosionSkips());
        const auto death = m_world.ctx().at<PlayerService>().GetDeathDiagnostic();
        snapshot += fmt::format(
            ",\"playerDeath\":{{\"respawns\":{},\"lastRespawnMs\":{},"
            "\"postRespawnKnockAttempts\":{},"
            "\"postRespawnKnocksApplied\":{},"
            "\"lastPostRespawnKnockMs\":{},"
            "\"skipNextPostRespawnKnock\":{},"
            "\"lastKnockSkipped\":{},"
            "\"lastBleedingOutAtKnock\":{},"
            "\"lastHad3DAtKnock\":{},"
            "\"lastHadProcessAtKnock\":{}}}",
            death.Respawns, death.LastRespawnMs,
            death.PostRespawnKnockAttempts,
            death.PostRespawnKnocksApplied,
            death.LastPostRespawnKnockMs,
            JsonBool(death.SkipNextPostRespawnKnock),
            JsonBool(death.LastKnockSkipped),
            JsonBool(death.LastBleedingOutAtKnock),
            JsonBool(death.LastHad3DAtKnock),
            JsonBool(death.LastHadProcessAtKnock));
        const auto watchedGraph = AnimationGraphUpdateTrace::GetWatchedPoseSample();
        snapshot += fmt::format(",\"graphUpdateTrace\":{{\"registered\":{},"
            "\"totalCalls\":{},\"lastPostCallMs\":{},"
            "\"watchedFormId\":{},\"watchedPoseBoneCount\":{},"
            "\"watchedRenderBoneCount\":{},\"watchedValidRenderNodeCount\":{},"
            "\"watchedThreadId\":{},"
            "\"watchedLastSampleMs\":{},\"watchedPoseChecksum\":{},"
            "\"watchedRenderChecksum\":{},\"watchedRenderWorldChecksum\":{},"
            "\"watchedSamples\":{},"
            "\"watchedPoseChanges\":{},\"watchedRenderChanges\":{},"
            "\"watchedRenderWorldChanges\":{},"
            "\"watchedDurationUs\":{}}}",
            JsonBool(AnimationGraphUpdateTrace::IsHookRegistered()),
            AnimationGraphUpdateTrace::GetTotalCalls(),
            AnimationGraphUpdateTrace::GetLastPostCallMs(),
            watchedGraph.FormId, watchedGraph.PoseBoneCount,
            watchedGraph.RenderBoneCount, watchedGraph.ValidRenderNodeCount,
            watchedGraph.ThreadId,
            watchedGraph.LastSampleMs, watchedGraph.PoseChecksum,
            watchedGraph.RenderLocalChecksum, watchedGraph.RenderWorldChecksum,
            watchedGraph.Samples,
            watchedGraph.PoseChanges, watchedGraph.RenderLocalChanges,
            watchedGraph.RenderWorldChanges,
            watchedGraph.DurationUs);
        snapshot += fmt::format(",\"presentationDelayMs\":{}",
            m_world.GetCharacterService().GetPresentationDelayMs());
        const auto setVehicleNative = m_world.ctx().at<PapyrusService>().Get(
            "Actor", "SetVehicle");
        const auto gameBase = reinterpret_cast<uintptr_t>(GetModuleHandleW(L"SkyrimSE.exe"));
        const auto nativeAddress = reinterpret_cast<uintptr_t>(setVehicleNative);
        snapshot += fmt::format(",\"nativeVehicleBinding\":{{\"registered\":{},"
            "\"gameRva\":{}}}", JsonBool(setVehicleNative != nullptr),
            gameBase && nativeAddress >= gameBase && nativeAddress - gameBase < 0x10000000
                ? nativeAddress - gameBase : 0);
        snapshot += fmt::format(",\"nativeVehicleTrial\":{{\"riderId\":{},"
            "\"calls\":{},\"immediateSeats\":{},\"immediateHandle\":{}}}",
            m_world.GetCharacterService().GetVehicleTrialRiderId(),
            m_world.GetCharacterService().GetVehicleTrialCalls(),
            m_world.GetCharacterService().GetVehicleTrialImmediateSeats(),
            m_world.GetCharacterService().GetVehicleTrialImmediateHandle());
        snapshot += fmt::format(",\"remoteProcessTrialTicks\":{}",
            Actor::GetRemoteProcessTrialTicks());
        const auto targetTrial = CombatController::GetTargetAuthorityTrialDiagnostics();
        snapshot += fmt::format(
            ",\"combatTargetAuthorityTrial\":{{\"actorFormId\":{},"
            "\"calls\":{},\"overrides\":{},\"lastRequestedFormId\":{},"
            "\"lastPresentedFormId\":{},\"lastNativeFormId\":{},"
            "\"lastCallerRva\":{}}}",
            targetTrial.ActorFormId, targetTrial.Calls,
            targetTrial.Overrides, targetTrial.LastRequestedFormId,
            targetTrial.LastPresentedFormId, targetTrial.LastNativeFormId,
            targetTrial.LastCallerRva);
        const auto mountDiagnostic = m_world.GetCharacterService().GetMountDiagnostic();
        snapshot += fmt::format(",\"mountDiagnostic\":{{\"pending\":{},"
            "\"notifications\":{},\"waitedFor3D\":{},\"applied\":{},\"seated\":{},"
            "\"rejected\":{},\"lastRiderId\":{},\"lastMountId\":{}}}",
            mountDiagnostic.Pending, mountDiagnostic.Notifications,
            mountDiagnostic.WaitedFor3D, mountDiagnostic.Applied,
            mountDiagnostic.Seated,
            mountDiagnostic.Rejected, mountDiagnostic.LastRiderId,
            mountDiagnostic.LastMountId);
        snapshot += ",\"mountRelations\":[";
        bool firstMountRelation = true;
        for (const auto& relation : m_world.GetCharacterService().GetPendingMountRelations())
        {
            if (!firstMountRelation)
                snapshot += ',';
            firstMountRelation = false;
            snapshot += fmt::format(
                "{{\"riderId\":{},\"mountId\":{},\"riderFormId\":{},"
                "\"mountFormId\":{},\"nativeMountFormId\":{},"
                "\"nativeVehicleHandle\":{},"
                "\"horseExtra\":{},\"horseHandle\":{},"
                "\"interactionExtra\":{},\"interactionPointerPresent\":{},"
                "\"interactionActorHandle\":{},\"interactionTargetHandle\":{},"
                "\"riderHas3D\":{},\"mountHas3D\":{},"
                "\"wasSeated\":{},\"attempts\":{}}}",
                relation.RiderId, relation.MountId, relation.RiderFormId,
                relation.MountFormId, relation.NativeMountFormId,
                relation.NativeVehicleHandle,
                JsonBool(relation.HorseExtra), relation.HorseHandle,
                JsonBool(relation.InteractionExtra),
                JsonBool(relation.InteractionPointerPresent),
                relation.InteractionActorHandle, relation.InteractionTargetHandle,
                JsonBool(relation.RiderHas3D), JsonBool(relation.MountHas3D),
                JsonBool(relation.WasSeated), relation.Attempts);
        }
        snapshot += ']';
        const auto visualInspection = VisualPoseMailbox::GetInspection();
        snapshot += fmt::format(",\"visualPoseMailbox\":{{\"published\":{},"
            "\"accepted\":{},\"stale\":{},\"rejectedGraph\":{},"
            "\"inspected\":{},\"lastFormId\":{},\"lastEpoch\":{},"
            "\"lastSourceAgeMs\":{},\"lastEligibleBones\":{},"
            "\"lastMaxElementErrorMilli\":{},\"lastDurationUs\":{},"
            "\"applyEnabled\":{},\"applyFormId\":{},"
            "\"appliedFrames\":{},\"appliedBones\":{},"
            "\"oldFrameApplySkips\":{},\"timelineMisses\":{},"
            "\"interpolatedFrames\":{},\"lastInterpolationSpanMs\":{},"
            "\"writeFailures\":{},"
            "\"lastReadbackErrorMilli\":{},\"rootSamples\":{},"
            "\"lastRootLatestErrorMilli\":{},"
            "\"lastRootPresentationErrorMilli\":{},"
            "\"rootDiagnosticFormId\":{}}}",
            visualInspection.Published, visualInspection.Accepted,
            visualInspection.Stale, visualInspection.RejectedGraph,
            visualInspection.Inspected, visualInspection.LastFormId,
            visualInspection.LastEpoch, visualInspection.LastSourceAgeMs,
            visualInspection.LastEligibleBones,
            visualInspection.LastMaxElementErrorMilli,
            visualInspection.LastDurationUs,
            JsonBool(visualInspection.ApplyEnabled),
            visualInspection.ApplyFormId,
            visualInspection.AppliedFrames, visualInspection.AppliedBones,
            visualInspection.OldFrameApplySkips,
            visualInspection.TimelineMisses,
            visualInspection.InterpolatedFrames,
            visualInspection.LastInterpolationSpanMs,
            visualInspection.WriteFailures,
            visualInspection.LastReadbackErrorMilli,
            visualInspection.RootSamples,
            visualInspection.LastRootLatestErrorMilli,
            visualInspection.LastRootPresentationErrorMilli,
            visualInspection.RootDiagnosticFormId);
        const auto bodyPlayback = ObjectService::GetBodyPlaybackDiagnostic();
        snapshot += fmt::format(
            ",\"bodyPlaybackProbe\":{{\"selectedFormId\":{},\"attempts\":{},"
            "\"succeeded\":{},\"staleSkips\":{},\"lastSourceAgeMs\":{},"
            "\"lastDurationUs\":{},\"lastPreError\":{},\"lastPostError\":{},"
            "\"lastStep\":{}}}",
            bodyPlayback.SelectedFormId, bodyPlayback.Attempts,
            bodyPlayback.Succeeded, bodyPlayback.StaleSkips,
            bodyPlayback.LastSourceAgeMs, bodyPlayback.LastDurationUs,
            bodyPlayback.LastPreError, bodyPlayback.LastPostError,
            bodyPlayback.LastStep);
        const auto referencePhase = ObjectService::GetReferencePhaseDiagnostic();
        snapshot += fmt::format(
            ",\"referencePhaseProbe\":{{\"selectedFormId\":{},\"hookInstalled\":{},"
            "\"update3DCalls\":{},\"moveHavokCalls\":{},\"lastThreadId\":{},"
            "\"lastMethod\":{},\"lastDurationUs\":{},\"lastReferenceDelta\":{},"
            "\"lastNodeDelta\":{},\"lastBodyDelta\":{},\"lastRefBodyError\":{},"
            "\"lastRefNodeError\":{},\"setPositionCalls\":{},"
            "\"setPositionRemoteSuppressedCalls\":{},"
            "\"setPositionRemoteOverrideCalls\":{},"
            "\"setPositionCallerRva\":{},\"setPositionThreadId\":{},"
            "\"setPositionFormType\":{},\"setPositionSourceIsNodeWorld\":{},"
            "\"setPositionInput\":[{},{},{}],"
            "\"setPositionPreReferenceError\":{}}}",
            referencePhase.SelectedFormId, JsonBool(referencePhase.HookInstalled),
            referencePhase.Update3DCalls, referencePhase.MoveHavokCalls,
            referencePhase.LastThreadId, referencePhase.LastMethod,
            referencePhase.LastDurationUs, referencePhase.LastReferenceDelta,
            referencePhase.LastNodeDelta, referencePhase.LastBodyDelta,
            referencePhase.LastRefBodyError, referencePhase.LastRefNodeError,
            referencePhase.SetPositionCalls,
            referencePhase.SetPositionRemoteSuppressedCalls,
            referencePhase.SetPositionRemoteOverrideCalls,
            referencePhase.SetPositionCallerRva,
            referencePhase.SetPositionThreadId, referencePhase.SetPositionFormType,
            JsonBool(referencePhase.SetPositionSourceIsNodeWorld),
            referencePhase.SetPositionInputX, referencePhase.SetPositionInputY,
            referencePhase.SetPositionInputZ,
            referencePhase.SetPositionPreReferenceError);
        const auto nodePhase = ObjectService::GetRenderNodePhaseDiagnostic();
        snapshot += fmt::format(
            ",\"renderNodePhaseProbe\":{{\"hookInstalled\":{},"
            "\"downwardCalls\":{},\"worldDataCalls\":{},"
            "\"transformBoundsCalls\":{},\"lastMethod\":{},"
            "\"lastThreadId\":{},\"lastDurationUs\":{},"
            "\"lastLocalDelta\":{},\"lastWorldDelta\":{},"
            "\"lastReferenceDelta\":{},\"lastRefNodeError\":{},"
            "\"worldDataCallerRva\":{},\"transformBoundsCallerRva\":{},"
            "\"worldDataReferenceDelta\":{},"
            "\"transformBoundsReferenceDelta\":{},"
            "\"worldDataTargetRva\":{},\"transformBoundsTargetRva\":{}}}",
            JsonBool(nodePhase.HookInstalled), nodePhase.DownwardCalls,
            nodePhase.WorldDataCalls, nodePhase.TransformBoundsCalls,
            nodePhase.LastMethod, nodePhase.LastThreadId,
            nodePhase.LastDurationUs, nodePhase.LastLocalDelta,
            nodePhase.LastWorldDelta, nodePhase.LastReferenceDelta,
            nodePhase.LastRefNodeError, nodePhase.WorldDataCallerRva,
            nodePhase.TransformBoundsCallerRva,
            nodePhase.WorldDataReferenceDelta,
            nodePhase.TransformBoundsReferenceDelta,
            nodePhase.WorldDataTargetRva,
            nodePhase.TransformBoundsTargetRva);
        const auto collisionSync = ObjectService::GetCollisionSyncDiagnostic();
        snapshot += fmt::format(
            ",\"collisionSyncProbe\":{{\"selectedCalls\":{},"
            "\"lastCallerRva\":{},"
            "\"lastThreadId\":{},\"lastDurationUs\":{},"
            "\"lastNodeDelta\":{},\"lastReferenceDelta\":{},"
            "\"lastBodyDelta\":{},\"lastPreNodeReferenceError\":{},"
            "\"lastPostNodeReferenceError\":{}}}",
            collisionSync.SelectedCalls, collisionSync.LastCallerRva,
            collisionSync.LastThreadId,
            collisionSync.LastDurationUs, collisionSync.LastNodeDelta,
            collisionSync.LastReferenceDelta, collisionSync.LastBodyDelta,
            collisionSync.LastPreNodeReferenceError,
            collisionSync.LastPostNodeReferenceError);
        const auto collisionWorld = ObjectService::GetCollisionWorldDiagnostic();
        snapshot += fmt::format(
            ",\"collisionWorldProbe\":{{\"selectedCalls\":{},"
            "\"lastCallerRva\":{},\"lastThreadId\":{},"
            "\"lastDurationUs\":{},\"lastInput\":[{},{},{}],"
            "\"lastInputBodyError\":{},\"lastInputNodeError\":{},"
            "\"lastPostNodeInputError\":{},"
            "\"lastPostReferenceInputError\":{}}}",
            collisionWorld.SelectedCalls, collisionWorld.LastCallerRva,
            collisionWorld.LastThreadId, collisionWorld.LastDurationUs,
            collisionWorld.LastInputX, collisionWorld.LastInputY,
            collisionWorld.LastInputZ, collisionWorld.LastInputBodyError,
            collisionWorld.LastInputNodeError,
            collisionWorld.LastPostNodeInputError,
            collisionWorld.LastPostReferenceInputError);
        const auto worldUpdate = ObjectService::GetWorldUpdateDiagnostic();
        snapshot += fmt::format(
            ",\"worldUpdateProbe\":{{\"calls\":{},\"lastThreadId\":{},"
            "\"lastDurationUs\":{},\"nativeStepCalls\":{},"
            "\"nativeStepLastThreadId\":{},\"nativeStepLastDurationUs\":{},"
            "\"selectedBodySteps\":{},\"selectedBodyChangedSteps\":{},"
            "\"lastSelectedBodyStepDelta\":{},"
            "\"peakSelectedBodyStepDelta\":{},"
            "\"selectedBodyStepsOver75Units\":{},"
            "\"peakSelectedBodyStepTimeMs\":{},"
            "\"peakSelectedBodyStepDt\":{},"
            "\"peakSelectedBodyPreLinearSpeed\":{},"
            "\"peakSelectedBodyPostLinearSpeed\":{},"
            "\"peakSelectedBodyPreAngularSpeed\":{},"
            "\"peakSelectedBodyPostAngularSpeed\":{},"
            "\"peakSelectedBodyMotionType\":{},"
            "\"peakSelectedBodyTargetApplied\":{},"
            "\"peakSelectedBodyTargetAgeMs\":{},"
            "\"peakSelectedBodyVelocityAfterWrite\":{},"
            "\"selectedBodyCacheRefreshes\":{},"
            "\"selectedStepWorldMatches\":{},"
            "\"selectedStepBodyReads\":{},"
            "\"selectedCollisionWorldDuringUpdate\":{},"
            "\"selectedCollisionWorldOutsideUpdate\":{},"
            "\"lastSelectedCollisionWorldAfterUpdateUs\":{},"
            "\"lastSelectedCollisionWorldAfterStepUs\":{}}}",
            worldUpdate.Calls, worldUpdate.LastThreadId,
            worldUpdate.LastDurationUs,
            worldUpdate.NativeStepCalls,
            worldUpdate.NativeStepLastThreadId,
            worldUpdate.NativeStepLastDurationUs,
            worldUpdate.SelectedBodySteps,
            worldUpdate.SelectedBodyChangedSteps,
            worldUpdate.LastSelectedBodyStepDelta,
            worldUpdate.PeakSelectedBodyStepDelta,
            worldUpdate.SelectedBodyStepsOver75Units,
            worldUpdate.PeakSelectedBodyStepTimeMs,
            worldUpdate.PeakSelectedBodyStepDt,
            worldUpdate.PeakSelectedBodyPreLinearSpeed,
            worldUpdate.PeakSelectedBodyPostLinearSpeed,
            worldUpdate.PeakSelectedBodyPreAngularSpeed,
            worldUpdate.PeakSelectedBodyPostAngularSpeed,
            worldUpdate.PeakSelectedBodyMotionType,
            JsonBool(worldUpdate.PeakSelectedBodyTargetApplied),
            worldUpdate.PeakSelectedBodyTargetAgeMs,
            worldUpdate.PeakSelectedBodyVelocityAfterWrite,
            worldUpdate.SelectedBodyCacheRefreshes,
            worldUpdate.SelectedStepWorldMatches,
            worldUpdate.SelectedStepBodyReads,
            worldUpdate.SelectedCollisionWorldDuringUpdate,
            worldUpdate.SelectedCollisionWorldOutsideUpdate,
            worldUpdate.LastSelectedCollisionWorldAfterUpdateUs,
            worldUpdate.LastSelectedCollisionWorldAfterStepUs);
        const auto preStepPlayback = ObjectService::GetPreStepPlaybackDiagnostic();
        snapshot += fmt::format(
            ",\"preStepPlaybackProbe\":{{\"selectedFormId\":{},\"mode\":{},"
            "\"publishedTargets\":{},\"attempts\":{},\"applied\":{},"
            "\"staleSkips\":{},\"lastSourceAgeMs\":{},"
            "\"lastPreError\":{},\"lastPostError\":{},"
            "\"lastVelocityCorrection\":{},\"poseWrites\":{},"
            "\"lastPoseStep\":{},\"hostScans\":{},"
            "\"hostPacketsSent\":{},\"hostUpdatesQueued\":{},"
            "\"hostBodyOnlyUpdates\":{},\"hostSelectedObserved\":{},"
            "\"hostSelectedQueued\":{},\"followerPacketsReceived\":{},"
            "\"followerSelectedReceived\":{},"
            "\"lastSelectedTransitAgeMs\":{},"
            "\"lastHostScanDurationUs\":{},"
            "\"lastHostReferencesVisited\":{},"
            "\"lastHostUpdatesQueued\":{}}}",
            preStepPlayback.SelectedFormId,
            preStepPlayback.Mode,
            preStepPlayback.PublishedTargets, preStepPlayback.Attempts,
            preStepPlayback.Applied, preStepPlayback.StaleSkips,
            preStepPlayback.LastSourceAgeMs, preStepPlayback.LastPreError,
            preStepPlayback.LastPostError,
            preStepPlayback.LastVelocityCorrection,
            preStepPlayback.PoseWrites, preStepPlayback.LastPoseStep,
            preStepPlayback.HostScans, preStepPlayback.HostPacketsSent,
            preStepPlayback.HostUpdatesQueued,
            preStepPlayback.HostBodyOnlyUpdates,
            preStepPlayback.HostSelectedObserved,
            preStepPlayback.HostSelectedQueued,
            preStepPlayback.FollowerPacketsReceived,
            preStepPlayback.FollowerSelectedReceived,
            preStepPlayback.LastSelectedTransitAgeMs,
            preStepPlayback.LastHostScanDurationUs,
            preStepPlayback.LastHostReferencesVisited,
            preStepPlayback.LastHostUpdatesQueued);

        const auto& party = m_world.GetPartyService();
        const auto& transport = m_world.GetTransport();
        snapshot += fmt::format(
            ",\"session\":{{\"online\":{},\"localPlayerId\":{},\"inParty\":{},\"leader\":{},"
            "\"leaderPlayerId\":{},\"memberCount\":{},\"campaignId\":\"{}\","
            "\"campaignRevision\":{},\"authorityEpoch\":{}}}",
            JsonBool(transport.IsOnline()), transport.GetLocalPlayerId(), JsonBool(party.IsInParty()),
            JsonBool(party.IsLeader()), party.GetLeaderPlayerId(), party.GetPartyMembers().size(),
            EscapeJson(transport.GetCampaignId().c_str()), transport.GetCampaignRevision(),
            transport.GetAuthorityEpoch());

        const auto nativeDispatch = GetNativeDispatchDiagnostic();
        const auto gameLoop = GetGameLoopDiagnostic();
        snapshot += fmt::format(
            ",\"gameLoopTiming\":{{\"vmHookCalls\":{},"
            "\"vmActiveCalls\":{},\"vmInactiveCalls\":{},"
            "\"vmAppTotalUs\":{},\"vmOriginalTotalUs\":{},"
            "\"vmLastAppUs\":{},\"vmLastOriginalUs\":{},"
            "\"vmLastEntryGapUs\":{},\"vmMaxEntryGapUs\":{},"
            "\"vmMaxAppUs\":{},\"vmMaxOriginalUs\":{},"
            "\"worldCalls\":{},\"worldPreUpdateTotalUs\":{},"
            "\"worldRunnerTotalUs\":{},"
            "\"worldDispatcherTotalUs\":{},"
            "\"worldGameTestTotalUs\":{},"
            "\"worldLastGameTestUs\":{},\"worldMaxGameTestUs\":{},"
            "\"worldLastEntryGapUs\":{},"
            "\"worldMaxDispatcherUs\":{},"
            "\"worldMaxEntryGapUs\":{},"
            "\"worldGapsOver50Ms\":{},"
            "\"worldGapsOver100Ms\":{},"
            "\"worldGapsOver250Ms\":{}}}",
            gameLoop.VmHookCalls, gameLoop.VmActiveCalls,
            gameLoop.VmInactiveCalls, gameLoop.VmAppTotalUs,
            gameLoop.VmOriginalTotalUs, gameLoop.VmLastAppUs,
            gameLoop.VmLastOriginalUs, gameLoop.VmLastEntryGapUs,
            gameLoop.VmMaxEntryGapUs, gameLoop.VmMaxAppUs,
            gameLoop.VmMaxOriginalUs, gameLoop.WorldCalls,
            gameLoop.WorldPreUpdateTotalUs, gameLoop.WorldRunnerTotalUs,
            gameLoop.WorldDispatcherTotalUs,
            gameLoop.WorldGameTestTotalUs,
            gameLoop.WorldLastGameTestUs, gameLoop.WorldMaxGameTestUs,
            gameLoop.WorldLastEntryGapUs,
            gameLoop.WorldMaxDispatcherUs,
            gameLoop.WorldMaxEntryGapUs,
            gameLoop.WorldGapsOver50Ms,
            gameLoop.WorldGapsOver100Ms,
            gameLoop.WorldGapsOver250Ms);
        snapshot += fmt::format(
            ",\"papyrusNativeDispatch\":{{\"count\":{},\"lastFunctionHash\":{},\"lastTimeMs\":{},"
            "\"vmUpdateCount\":{},\"vmUpdateLastStartMs\":{},\"vmUpdateLastDurationUs\":{},"
            "\"vmTaskletCount\":{},\"vmTaskletLastStartMs\":{},\"vmTaskletLastDurationUs\":{},"
            "\"disablePlayerControlsCalls\":{},\"enablePlayerControlsCalls\":{},"
            "\"lastControlCallTimeMs\":{},\"lastControlCallEnabled\":{}}}",
            nativeDispatch.Count, nativeDispatch.LastFunctionHash, nativeDispatch.LastTimeMs,
            nativeDispatch.VmUpdateCount, nativeDispatch.VmUpdateLastStartMs,
            nativeDispatch.VmUpdateLastDurationUs, nativeDispatch.VmTaskletCount,
            nativeDispatch.VmTaskletLastStartMs, nativeDispatch.VmTaskletLastDurationUs,
            nativeDispatch.DisablePlayerControlsCalls, nativeDispatch.EnablePlayerControlsCalls,
            nativeDispatch.LastControlCallTimeMs, JsonBool(nativeDispatch.LastControlCallEnabled));

        if (auto* pPlayer = PlayerCharacter::Get())
        {
            auto* pCell = pPlayer->GetParentCellEx();
            auto* pWorldspace = pPlayer->GetWorldSpace();
            auto* pPackage = pPlayer->currentProcess ? pPlayer->currentProcess->package : nullptr;
            snapshot += fmt::format(
                ",\"player\":{{\"present\":true,\"formId\":{},\"cellId\":{},\"worldspaceId\":{},"
                "\"position\":[{},{},{}],\"rotation\":[{},{},{}],\"dead\":{},\"bleedingOut\":{},"
                "\"inCombat\":{},\"weaponDrawn\":{},\"weaponFullyDrawn\":{},\"dialogueHandle\":{},"
                "\"combatHandle\":{},\"killerHandle\":{},\"packageFormId\":{},\"movementType\":{},"
                "\"health\":{},\"magicka\":{},\"stamina\":{},\"speed\":{},"
                "\"actorStateFlags1\":{},\"actorStateFlags2\":{}}}",
                pPlayer->formID, pCell ? pCell->formID : 0, pWorldspace ? pWorldspace->formID : 0,
                pPlayer->position.x, pPlayer->position.y, pPlayer->position.z,
                pPlayer->rotation.x, pPlayer->rotation.y, pPlayer->rotation.z,
                JsonBool(pPlayer->IsDead()), JsonBool(pPlayer->actorState.IsBleedingOut()),
                JsonBool(pPlayer->IsInCombat()), JsonBool(pPlayer->actorState.IsWeaponDrawn()),
                JsonBool(pPlayer->actorState.IsWeaponFullyDrawn()), pPlayer->dialogueHandle,
                pPlayer->combatHandle, pPlayer->killerHandle, pPackage ? pPackage->formID : 0,
                pPlayer->currentProcess ? pPlayer->currentProcess->movementType : -1,
                pPlayer->GetActorValue(ActorValueInfo::kHealth),
                pPlayer->GetActorValue(ActorValueInfo::kMagicka),
                pPlayer->GetActorValue(ActorValueInfo::kStamina), pPlayer->GetSpeed(),
                pPlayer->actorState.flags1, pPlayer->actorState.flags2);

            const auto playerInventory = pPlayer->GetActorInventory();
            uint64_t inventoryChecksum = 1469598103934665603ull;
            for (const auto& entry : playerInventory.Entries)
            {
                HashValue(inventoryChecksum, entry.BaseId.ModId);
                HashValue(inventoryChecksum, entry.BaseId.BaseId);
                HashValue(inventoryChecksum, entry.Count);
                HashValue(inventoryChecksum, entry.ExtraWorn);
                HashValue(inventoryChecksum, entry.ExtraWornLeft);
                HashValue(inventoryChecksum, entry.IsQuestItem);
            }
            snapshot += fmt::format(
                ",\"playerInventory\":{{\"entryCount\":{},\"checksum\":{},\"entries\":[",
                playerInventory.Entries.size(), inventoryChecksum);
            bool firstInventoryEntry = true;
            for (size_t i = 0; i < playerInventory.Entries.size() && i < 128; ++i)
            {
                const auto& entry = playerInventory.Entries[i];
                if (!firstInventoryEntry)
                    snapshot += ',';
                firstInventoryEntry = false;
                snapshot += fmt::format(
                    "{{\"modId\":{},\"baseId\":{},\"count\":{},\"worn\":{},\"questItem\":{}}}",
                    entry.BaseId.ModId, entry.BaseId.BaseId, entry.Count,
                    JsonBool(entry.IsWorn()), JsonBool(entry.IsQuestItem));
            }
            snapshot += "]}";

            if (const auto* pExtension = pPlayer->GetExtension())
            {
                const auto& action = pExtension->LatestAnimation;
                snapshot += fmt::format(
                    ",\"playerAnimation\":{{\"graphReady\":{},\"graphDescriptor\":{},"
                    "\"reconciliationStage\":{},\"tick\":{},\"actionId\":{},\"targetId\":{},"
                    "\"idleId\":{},\"type\":{},\"state1\":{},\"state2\":{},\"event\":\"{}\"}}",
                    JsonBool(pPlayer->animationGraphHolder.IsReady()), pExtension->GraphDescriptorHash,
                    static_cast<uint32_t>(pExtension->Reconciliation), action.Tick, action.ActionId,
                    action.TargetId, action.IdleId, action.Type, action.State1, action.State2,
                    EscapeJson(action.EventName.c_str()));
            }

            // One-shot player graph audit: the stationary follower camera can
            // move even when cinematic camera playback is inactive. Capture
            // the native behavior inputs that can drive first-person motion,
            // using the same bounded, locked reader as the NPC pose audit.
            const auto playerNativeAnimation = SampleNativeActorAnimation(pPlayer, true);
            snapshot += fmt::format(
                ",\"playerNativeAnimation\":{{\"graphReady\":{},"
                "\"graphCount\":{},\"graphIndex\":{},"
                "\"stateId\":{},\"timeInState\":{},"
                "\"behaviorActive\":{},\"behaviorLinked\":{},"
                "\"rootClonePresent\":{},\"rootCloneSameAsTemplate\":{},"
                "\"cloneStateReadable\":{},"
                "\"cloneStateActive\":{},\"cloneStateId\":{},"
                "\"clonePreviousStateId\":{},\"cloneTimeInState\":{},"
                "\"poseCount\":{},\"poseChecksum\":{},"
                "\"renderBoneCount\":{},\"renderBoneChecksum\":{},"
                "\"renderWorldBoneChecksum\":{},"
                "\"graphVariableCount\":{},\"graphVariableChecksum\":{},"
                "\"sampledCount\":{},\"values\":[",
                JsonBool(playerNativeAnimation.GraphReady),
                playerNativeAnimation.GraphCount,
                playerNativeAnimation.GraphIndex,
                playerNativeAnimation.StateId,
                std::isfinite(playerNativeAnimation.TimeInState) ?
                    playerNativeAnimation.TimeInState : 0.f,
                JsonBool(playerNativeAnimation.BehaviorActive),
                JsonBool(playerNativeAnimation.BehaviorLinked),
                JsonBool(playerNativeAnimation.RootClonePresent),
                JsonBool(playerNativeAnimation.RootCloneSameAsTemplate),
                JsonBool(playerNativeAnimation.CloneStateReadable),
                JsonBool(playerNativeAnimation.CloneStateActive),
                playerNativeAnimation.CloneStateId,
                playerNativeAnimation.ClonePreviousStateId,
                std::isfinite(playerNativeAnimation.CloneTimeInState) ?
                    playerNativeAnimation.CloneTimeInState : 0.f,
                playerNativeAnimation.PoseCount,
                playerNativeAnimation.PoseChecksum,
                playerNativeAnimation.RenderBoneCount,
                playerNativeAnimation.RenderBoneChecksum,
                playerNativeAnimation.RenderWorldBoneChecksum,
                playerNativeAnimation.GraphVariableCount,
                playerNativeAnimation.GraphVariableChecksum,
                playerNativeAnimation.GraphVariables.size());
            for (size_t i = 0; i < playerNativeAnimation.GraphVariables.size(); ++i)
            {
                if (i != 0)
                    snapshot += ',';
                snapshot += fmt::format("{}", playerNativeAnimation.GraphVariables[i]);
            }
            snapshot += fmt::format("],\"nameCount\":{},\"infoCount\":{},"
                "\"sampledNameCount\":{},\"names\":[",
                playerNativeAnimation.GraphVariableNameCount,
                playerNativeAnimation.GraphVariableInfoCount,
                playerNativeAnimation.GraphVariableNames.size());
            for (size_t i = 0; i < playerNativeAnimation.GraphVariableNames.size(); ++i)
            {
                if (i != 0)
                    snapshot += ',';
                snapshot += fmt::format("\"{}\"",
                    EscapeJson(playerNativeAnimation.GraphVariableNames[i]));
            }
            snapshot += "]}";
        }
        else
            snapshot += ",\"player\":{\"present\":false}";

        if (auto* pControls = PlayerControls::GetInstance())
        {
            snapshot += fmt::format(
                ",\"controls\":{{\"present\":true,\"blocked\":{},\"movement\":{},\"look\":{},"
                "\"sprint\":{},\"readyWeapon\":{},\"activate\":{},\"jump\":{},\"shout\":{},"
                "\"attackBlock\":{},\"sneak\":{},\"togglePov\":{},\"autoMove\":{},"
                "\"running\":{},\"povScriptMode\":{},\"remapMode\":{},"
                "\"moveInput\":[{},{}],\"lookInput\":[{},{}]}}",
                JsonBool(pControls->bBlockPlayerInput), JsonBool(HandlerEnabled(pControls->pMovementHandler)),
                JsonBool(HandlerEnabled(pControls->pLookHandler)), JsonBool(HandlerEnabled(pControls->pSprintHandler)),
                JsonBool(HandlerEnabled(pControls->pReadyWeaponHandler)), JsonBool(HandlerEnabled(pControls->pActivateHandler)),
                JsonBool(HandlerEnabled(pControls->pJumpHandler)), JsonBool(HandlerEnabled(pControls->shoutHandler)),
                JsonBool(HandlerEnabled(pControls->attackBlockHandler)), JsonBool(HandlerEnabled(pControls->sneakHandler)),
                JsonBool(HandlerEnabled(pControls->togglePOVHandler)), JsonBool(pControls->Data.bAutoMove),
                JsonBool(pControls->Data.bRunning), JsonBool(pControls->Data.povScriptMode),
                JsonBool(pControls->Data.remapMode), pControls->Data.MoveInputVec.x,
                pControls->Data.MoveInputVec.y, pControls->Data.LookInputVec.x,
                pControls->Data.LookInputVec.y);
        }
        else
            snapshot += ",\"controls\":{\"present\":false}";

        // ControlMap::enabledControls is independent of PlayerControls'
        // handler enable flags. Skyrim AE's ToggleControls (ID 68545, named
        // EnableOtherEvent by this project's legacy wrapper) writes
        // the effective mask at +0x120; Store/LoadStoredControls use +0x124.
        // The +0x118 CommonLibSSE-NG declaration does not fit this AE exe.
        uint32_t controlMapEnabledFlags{};
        uint32_t controlMapStoredFlags{};
        const auto* pControlMap = BSInputEnableManager::Get();
        const bool controlMapReadable = pControlMap &&
            ReadNative(reinterpret_cast<const uint8_t*>(pControlMap) + 0x120,
                       controlMapEnabledFlags) &&
            ReadNative(reinterpret_cast<const uint8_t*>(pControlMap) + 0x124,
                       controlMapStoredFlags);
        snapshot += fmt::format(
            ",\"controlMap\":{{\"present\":{},\"enabledFlags\":{},"
            "\"storedFlags\":{},\"storedValid\":{},"
            "\"movement\":{},\"looking\":{},\"activate\":{},"
            "\"menu\":{},\"console\":{},\"povSwitch\":{},"
            "\"fighting\":{},\"sneaking\":{},\"mainFour\":{},"
            "\"wheelZoom\":{},\"jumping\":{},\"vats\":{}}}",
            JsonBool(controlMapReadable),
            controlMapReadable ? fmt::format("{}", controlMapEnabledFlags) : "null",
            controlMapReadable ? fmt::format("{}", controlMapStoredFlags) : "null",
            JsonBool(controlMapReadable && controlMapStoredFlags != 0x80000000u),
            JsonBool(controlMapReadable && (controlMapEnabledFlags & (1u << 0)) != 0),
            JsonBool(controlMapReadable && (controlMapEnabledFlags & (1u << 1)) != 0),
            JsonBool(controlMapReadable && (controlMapEnabledFlags & (1u << 2)) != 0),
            JsonBool(controlMapReadable && (controlMapEnabledFlags & (1u << 3)) != 0),
            JsonBool(controlMapReadable && (controlMapEnabledFlags & (1u << 4)) != 0),
            JsonBool(controlMapReadable && (controlMapEnabledFlags & (1u << 5)) != 0),
            JsonBool(controlMapReadable && (controlMapEnabledFlags & (1u << 6)) != 0),
            JsonBool(controlMapReadable && (controlMapEnabledFlags & (1u << 7)) != 0),
            JsonBool(controlMapReadable && (controlMapEnabledFlags & (1u << 8)) != 0),
            JsonBool(controlMapReadable && (controlMapEnabledFlags & (1u << 9)) != 0),
            JsonBool(controlMapReadable && (controlMapEnabledFlags & (1u << 10)) != 0),
            JsonBool(controlMapReadable && (controlMapEnabledFlags & (1u << 11)) != 0));

        if (auto* pCamera = PlayerCamera::Get())
        {
            const auto* pCameraRoot = pCamera->cameraNode;
            snapshot += fmt::format(
                ",\"camera\":{{\"present\":true,\"firstPerson\":{},\"hasState\":{},"
                "\"position\":[{},{},{}],\"rotation\":[{},{}],\"zoom\":{},"
                "\"rootPresent\":{},\"rootWorldPosition\":[{},{},{}],"
                "\"rootWorldRotation\":[{},{},{},{},{},{},{},{},{}]}}",
                JsonBool(pCamera->IsFirstPerson()), JsonBool(pCamera->state != nullptr),
                pCamera->pos.x, pCamera->pos.y, pCamera->pos.z, pCamera->rotX, pCamera->rotZ, pCamera->zoom,
                JsonBool(pCameraRoot != nullptr),
                pCameraRoot ? pCameraRoot->world.translate.x : 0.f,
                pCameraRoot ? pCameraRoot->world.translate.y : 0.f,
                pCameraRoot ? pCameraRoot->world.translate.z : 0.f,
                pCameraRoot ? pCameraRoot->world.rotate.entry[0][0] : 0.f,
                pCameraRoot ? pCameraRoot->world.rotate.entry[0][1] : 0.f,
                pCameraRoot ? pCameraRoot->world.rotate.entry[0][2] : 0.f,
                pCameraRoot ? pCameraRoot->world.rotate.entry[1][0] : 0.f,
                pCameraRoot ? pCameraRoot->world.rotate.entry[1][1] : 0.f,
                pCameraRoot ? pCameraRoot->world.rotate.entry[1][2] : 0.f,
                pCameraRoot ? pCameraRoot->world.rotate.entry[2][0] : 0.f,
                pCameraRoot ? pCameraRoot->world.rotate.entry[2][1] : 0.f,
                pCameraRoot ? pCameraRoot->world.rotate.entry[2][2] : 0.f);

            // CommonLibSSE-NG 1.7 candidate FirstPersonState layout. Read the
            // native spring and pitch fields only in a requested snapshot;
            // these can distinguish persistent state motion from graph pose.
            NiPoint3 lastPosition{};
            NiPoint3 springVelocity{};
            NiPoint3 dampeningOffset{};
            float currentPitchOffset{};
            float targetPitchOffset{};
            uint8_t cameraOverride{};
            uint8_t cameraPitchOverride{};
            uint32_t nativeStateId{0xFFFFFFFFu};
            void* pFirstPersonCameraObject{};
            NiPoint3 firstPersonObjectWorld{};
            NiPoint3 player3DRootWorld{};
            const auto* pStateBytes = reinterpret_cast<const uint8_t*>(pCamera->state);
            const bool firstPersonStateReadable = pStateBytes &&
                ReadNative(pStateBytes + 0x18, nativeStateId) &&
                nativeStateId == 0 &&
                ReadNative(pStateBytes + 0x30, lastPosition) &&
                ReadNative(pStateBytes + 0x3C, springVelocity) &&
                ReadNative(pStateBytes + 0x48, dampeningOffset) &&
                ReadNative(pStateBytes + 0x74, currentPitchOffset) &&
                ReadNative(pStateBytes + 0x78, targetPitchOffset) &&
                ReadNative(pStateBytes + 0x84, cameraOverride) &&
                ReadNative(pStateBytes + 0x85, cameraPitchOverride) &&
                std::isfinite(lastPosition.x) && std::isfinite(lastPosition.y) &&
                std::isfinite(lastPosition.z) &&
                std::isfinite(springVelocity.x) &&
                std::isfinite(springVelocity.y) &&
                std::isfinite(springVelocity.z) &&
                std::isfinite(dampeningOffset.x) &&
                std::isfinite(dampeningOffset.y) &&
                std::isfinite(dampeningOffset.z) &&
                std::isfinite(currentPitchOffset) &&
                std::isfinite(targetPitchOffset);
            const bool cameraObjectReadable = firstPersonStateReadable &&
                ReadNative(pStateBytes + 0x58, pFirstPersonCameraObject) &&
                pFirstPersonCameraObject &&
                ReadNative(reinterpret_cast<const uint8_t*>(
                    pFirstPersonCameraObject) + offsetof(NiAVObject, world) +
                    offsetof(NiTransform, translate), firstPersonObjectWorld) &&
                std::isfinite(firstPersonObjectWorld.x) &&
                std::isfinite(firstPersonObjectWorld.y) &&
                std::isfinite(firstPersonObjectWorld.z);
            if (cameraObjectReadable)
            {
                if (auto* pLocalPlayer = PlayerCharacter::Get())
                    AnimationGraphUpdateTrace::WatchPlayerCameraObject(
                        &pLocalPlayer->animationGraphHolder, pCamera->state,
                        pFirstPersonCameraObject);
            }
            auto* pPlayer3D = PlayerCharacter::Get() ?
                PlayerCharacter::Get()->GetNiNode() : nullptr;
            const bool player3DRootReadable = pPlayer3D &&
                ReadNative(&pPlayer3D->world.translate, player3DRootWorld) &&
                std::isfinite(player3DRootWorld.x) &&
                std::isfinite(player3DRootWorld.y) &&
                std::isfinite(player3DRootWorld.z);
            snapshot += fmt::format(
                ",\"firstPersonNativeState\":{{\"readable\":{},"
                "\"lastPosition\":[{},{},{}],"
                "\"springVelocity\":[{},{},{}],"
                "\"dampeningOffset\":[{},{},{}],"
                "\"currentPitchOffset\":{},\"targetPitchOffset\":{},"
                "\"cameraOverride\":{},\"cameraPitchOverride\":{},"
                "\"cameraObjectReadable\":{},\"cameraObjectWorld\":[{},{},{}],"
                "\"player3DRootReadable\":{},\"player3DRootWorld\":[{},{},{}]}}",
                JsonBool(firstPersonStateReadable),
                firstPersonStateReadable ? lastPosition.x : 0.f,
                firstPersonStateReadable ? lastPosition.y : 0.f,
                firstPersonStateReadable ? lastPosition.z : 0.f,
                firstPersonStateReadable ? springVelocity.x : 0.f,
                firstPersonStateReadable ? springVelocity.y : 0.f,
                firstPersonStateReadable ? springVelocity.z : 0.f,
                firstPersonStateReadable ? dampeningOffset.x : 0.f,
                firstPersonStateReadable ? dampeningOffset.y : 0.f,
                firstPersonStateReadable ? dampeningOffset.z : 0.f,
                firstPersonStateReadable ? currentPitchOffset : 0.f,
                firstPersonStateReadable ? targetPitchOffset : 0.f,
                JsonBool(firstPersonStateReadable && cameraOverride != 0),
                JsonBool(firstPersonStateReadable && cameraPitchOverride != 0),
                JsonBool(cameraObjectReadable),
                cameraObjectReadable ? firstPersonObjectWorld.x : 0.f,
                cameraObjectReadable ? firstPersonObjectWorld.y : 0.f,
                cameraObjectReadable ? firstPersonObjectWorld.z : 0.f,
                JsonBool(player3DRootReadable),
                player3DRootReadable ? player3DRootWorld.x : 0.f,
                player3DRootReadable ? player3DRootWorld.y : 0.f,
                player3DRootReadable ? player3DRootWorld.z : 0.f);
        }
        else
            snapshot += ",\"camera\":{\"present\":false}";

        const auto cameraAuthority = m_world.ctx().at<CameraService>().GetDiagnostic();
        snapshot += fmt::format(
            ",\"cameraAuthority\":{{\"inputGated\":{},"
            "\"hasHostSnapshot\":{},\"localStateId\":{},"
            "\"hostStateId\":{},\"lastHostTick\":{},"
            "\"receivedPackets\":{},\"nativePostUpdates\":{},"
            "\"lastNativeThreadId\":{},\"hookedVtables\":{}}}",
            JsonBool(cameraAuthority.InputGated),
            JsonBool(cameraAuthority.HasHostSnapshot),
            cameraAuthority.LocalStateId, cameraAuthority.HostStateId,
            cameraAuthority.LastHostTick, cameraAuthority.ReceivedPackets,
            cameraAuthority.NativePostUpdates,
            cameraAuthority.LastNativeThreadId,
            cameraAuthority.HookedVtables);

        const auto cameraTrace = m_world.ctx().at<CameraService>().GetNativeUpdateTrace();
        snapshot += ",\"nativeCameraUpdateTrace\":[";
        for (size_t i = 0; i < cameraTrace.Count; ++i)
        {
            const auto& sample = cameraTrace.Samples[i];
            if (i != 0)
                snapshot += ',';
            snapshot += fmt::format(
                "{{\"timeMs\":{},\"stateId\":{},\"before\":[{},{},{}],"
                "\"after\":[{},{},{}],\"beforeLocal\":[{},{},{}],"
                "\"afterLocal\":[{},{},{}],\"parentStable\":{},"
                "\"beforeParentWorld\":[{},{},{}],"
                "\"afterParentWorld\":[{},{},{}],"
                "\"firstPersonStateReadable\":{},"
                "\"pitchBefore\":{},\"pitchAfter\":{},"
                "\"targetPitchBefore\":{},\"targetPitchAfter\":{},"
                "\"firstPersonObjectStable\":{},"
                "\"objectBefore\":[{},{},{}],"
                "\"objectAfter\":[{},{},{}],"
                "\"graphPitchReadable\":{},"
                "\"graphPitchBefore\":{},\"graphPitchAfter\":{}}}",
                sample.TimeMs, sample.StateId,
                sample.Before[0], sample.Before[1], sample.Before[2],
                sample.After[0], sample.After[1], sample.After[2],
                sample.BeforeLocal[0], sample.BeforeLocal[1], sample.BeforeLocal[2],
                sample.AfterLocal[0], sample.AfterLocal[1], sample.AfterLocal[2],
                JsonBool(sample.ParentStable),
                sample.BeforeParentWorld[0], sample.BeforeParentWorld[1],
                sample.BeforeParentWorld[2],
                sample.AfterParentWorld[0], sample.AfterParentWorld[1],
                sample.AfterParentWorld[2],
                JsonBool(sample.FirstPersonStateReadable),
                sample.PitchBefore, sample.PitchAfter,
                sample.TargetPitchBefore, sample.TargetPitchAfter,
                JsonBool(sample.FirstPersonObjectStable),
                sample.ObjectBefore[0], sample.ObjectBefore[1],
                sample.ObjectBefore[2],
                sample.ObjectAfter[0], sample.ObjectAfter[1],
                sample.ObjectAfter[2],
                JsonBool(sample.GraphPitchReadable),
                sample.GraphPitchBefore, sample.GraphPitchAfter);
        }
        snapshot += ']';

        const auto graphCameraTrace =
            AnimationGraphUpdateTrace::GetPlayerCameraObjectTrace();
        snapshot += fmt::format(
            ",\"playerCameraObjectGraphWatchGeneration\":{},"
            "\"playerCameraObjectGraphTrace\":[",
            graphCameraTrace.WatchGeneration);
        for (size_t i = 0; i < graphCameraTrace.Count; ++i)
        {
            const auto& sample = graphCameraTrace.Samples[i];
            if (i != 0)
                snapshot += ',';
            snapshot += fmt::format(
                "{{\"startMs\":{},\"endMs\":{},\"threadId\":{},"
                "\"watchGeneration\":{},"
                "\"cameraSequenceBefore\":{},\"cameraSequenceAfter\":{},"
                "\"valid\":{},\"localBefore\":[{},{},{}],"
                "\"localAfter\":[{},{},{}],"
                "\"worldBefore\":[{},{},{}],"
                "\"worldAfter\":[{},{},{}]}}",
                sample.StartMs, sample.EndMs, sample.ThreadId,
                sample.WatchGeneration,
                sample.CameraSequenceBefore, sample.CameraSequenceAfter,
                JsonBool(sample.Valid),
                sample.LocalBefore[0], sample.LocalBefore[1],
                sample.LocalBefore[2],
                sample.LocalAfter[0], sample.LocalAfter[1],
                sample.LocalAfter[2],
                sample.WorldBefore[0], sample.WorldBefore[1],
                sample.WorldBefore[2],
                sample.WorldAfter[0], sample.WorldAfter[1],
                sample.WorldAfter[2]);
        }
        snapshot += ']';

        snapshot += ",\"menus\":[";
        if (auto* pUI = UI::Get())
        {
            bool firstMenu = true;
            for (auto* pMenu : pUI->menuStack)
            {
                if (!pMenu)
                    continue;
                auto* pName = pUI->LookupMenuNameByInstance(pMenu);
                if (!pName)
                    continue;
                if (!firstMenu)
                    snapshot += ',';
                snapshot += fmt::format("\"{}\"", EscapeJson(pName->AsAscii()));
                firstMenu = false;
            }
        }
        snapshot += ']';

        snapshot += fmt::format(
            ",\"titleSequence\":{{\"menuOpen\":{},\"transitions\":{},\"lastTransitionMs\":{}}}",
            JsonBool(m_titleSequenceMenuOpen), m_titleSequenceMenuTransitions,
            m_lastTitleSequenceTransitionMs);

        if (auto* pUI = UI::Get())
        {
            snapshot += fmt::format(
                ",\"ui\":{{\"loading\":{},\"dialogue\":{},\"pausesGame\":{},\"allowSaving\":{},"
                "\"disablePauseMenu\":{},\"modal\":{},\"visible\":{},\"closingAllMenus\":{},"
                "\"dontHideCursor\":{}}}",
                JsonBool(pUI->GetMenuOpen(BSFixedString("Loading Menu"))),
                JsonBool(pUI->GetMenuOpen(BSFixedString("Dialogue Menu"))), pUI->numPausesGame,
                pUI->numAllowSaving, pUI->numDisablePauseMenu, JsonBool(pUI->modal),
                JsonBool(pUI->menuSystemVisible), JsonBool(pUI->closingAllMenus),
                pUI->numDontHideCursorWhenTopmost);
        }
        else
            snapshot += ",\"ui\":{\"present\":false}";

        const auto& loading = LoadingScreenProbe::Get().Sample();
        snapshot += fmt::format(
            ",\"loadingPresentation\":{{\"epoch\":{},\"loadingMenuPresent\":{},"
            "\"candidateListReadable\":{},\"mistMenuPresent\":{},\"mistStateReadable\":{},"
            "\"showMist\":{},\"showLoadScreen\":{},\"modelPresent\":{},"
            "\"modelHasUserData\":{},\"cameraPathPresent\":{},\"cameraFov\":{},"
            "\"angleZ\":{},\"selectedIdentityResolved\":{},\"selectedFormId\":{},"
            "\"eligibleFormIds\":[",
            loading.Epoch, JsonBool(loading.LoadingMenuPresent),
            JsonBool(loading.CandidateListReadable), JsonBool(loading.MistMenuPresent),
            JsonBool(loading.MistStateReadable), JsonBool(loading.ShowMist),
            JsonBool(loading.ShowLoadScreen), JsonBool(loading.LoadScreenModelPresent),
            JsonBool(loading.LoadScreenModelHasUserData), JsonBool(loading.CameraPathPresent),
            loading.CameraFov, loading.AngleZ, JsonBool(loading.SelectedIdentityResolved),
            loading.SelectedFormId);
        for (size_t i = 0; i < loading.EligibleFormIds.size(); ++i)
        {
            if (i != 0)
                snapshot += ',';
            snapshot += std::to_string(loading.EligibleFormIds[i]);
        }
        snapshot += "]}";

        if (auto* pDialogue = MenuTopicManager::Get())
        {
            const auto* pSpeaker = TESObjectREFR::GetByHandle(pDialogue->speaker.handle.iBits);
            snapshot += fmt::format(
                ",\"dialogue\":{{\"menuOpen\":{},\"speakerHandle\":{},\"speakerFormId\":{},"
                "\"hasOptions\":{}}}",
                JsonBool(pDialogue->menuOpen), pDialogue->speaker.handle.iBits,
                pSpeaker ? pSpeaker->formID : 0,
                JsonBool(pDialogue->pOptions != nullptr));
        }
        else
            snapshot += ",\"dialogue\":{\"present\":false}";

        if (auto* pTes = TES::Get())
        {
            snapshot += fmt::format(
                ",\"world\":{{\"centerGrid\":[{},{}],\"currentGrid\":[{},{}],"
                "\"interiorCellId\":{}}}",
                pTes->centerGridX, pTes->centerGridY, pTes->currentGridX, pTes->currentGridY,
                pTes->interiorCell ? pTes->interiorCell->formID : 0);
        }
        if (auto* pProcesses = ProcessLists::Get())
        {
            snapshot += fmt::format(
                ",\"actorProcessing\":{{\"highCount\":{},\"highHandles\":{},"
                "\"middleHighHandles\":{},\"middleLowHandles\":{},\"lowHandles\":{}}}",
                pProcesses->numberHighActors, pProcesses->highActorHandleArray.length,
                pProcesses->middleHighActorHandleArray.length, pProcesses->middleLowActorHandleArray.length,
                pProcesses->lowActorHandleArray.length);
        }

        // Source-pinned, read-only animation/ragdoll probe. Native graph and
        // bone reads are expensive, so keep this diagnostic bounded enough
        // that observation does not materially change scene timing.
        constexpr size_t cPoseProbeActorLimit = 12;
        const auto poseProbeStart = std::chrono::steady_clock::now();
        auto poseTargetTick = m_poseProbeTargetTick.load(std::memory_order_relaxed);
        const bool capturePose = poseTargetTick != 0 && worldTick >= poseTargetTick &&
            m_poseProbeTargetTick.compare_exchange_strong(poseTargetTick, 0,
                std::memory_order_relaxed);
        markSnapshotPhase(0);
        snapshot += ",\"actorPoseDiagnostics\":[";
        bool firstPoseDiagnostic = true;
        Set<uint32_t> sampledActorIds;
        auto appendPoseDiagnostic = [&](Actor* apActor, const char* acpSource)
        {
            if (!apActor || sampledActorIds.contains(apActor->formID) ||
                sampledActorIds.size() >= cPoseProbeActorLimit)
                return;
            sampledActorIds.insert(apActor->formID);
            if (!firstPoseDiagnostic)
                snapshot += ',';
            firstPoseDiagnostic = false;
            AppendActorPoseDiagnostic(snapshot, apActor, acpSource);
        };
        if (capturePose)
        {
            const auto selectedFormId = m_poseProbeFormId.load(
                std::memory_order_acquire);
            if (selectedFormId)
                appendPoseDiagnostic(Cast<Actor>(TESForm::GetById(selectedFormId)),
                    "selected");
            else
            {
                appendPoseDiagnostic(PlayerCharacter::Get(), "player");
                if (auto* pProcesses = ProcessLists::Get())
                {
                    const auto& handles = pProcesses->highActorHandleArray;
                    const bool handlesReadable = handles.length <= 2048 &&
                        handles.capacity >= handles.length &&
                        (handles.length == 0 ||
                            (handles.data && IsReadableRange(handles.data,
                                static_cast<size_t>(handles.length) * sizeof(uint32_t))));
                    if (handlesReadable)
                    {
                        for (uint32_t i = 0; i < handles.length &&
                            sampledActorIds.size() < cPoseProbeActorLimit; ++i)
                        {
                            auto* pReference = TESObjectREFR::GetByHandle(handles.data[i]);
                            appendPoseDiagnostic(pReference ? Cast<Actor>(pReference) : nullptr, "high");
                        }
                    }
                }
            }
        }
        snapshot += ']';
        if (capturePose)
            m_lastPoseSampleTick.store(worldTick, std::memory_order_relaxed);
        snapshot += fmt::format(",\"actorPoseProbeDurationUs\":{}",
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - poseProbeStart).count());
        snapshot += fmt::format(",\"actorPoseSampleTick\":{},\"actorPoseLastSampleTick\":{}",
            capturePose ? worldTick : 0, m_lastPoseSampleTick.load(std::memory_order_relaxed));

        if (const auto* pWindow = BSGraphics::GetMainWindow(); pWindow && pWindow->hWnd)
        {
            RECT client{};
            RECT window{};
            RECT clip{};
            GetClientRect(pWindow->hWnd, &client);
            GetWindowRect(pWindow->hWnd, &window);
            const bool cursorClipped = GetClipCursor(&clip) != FALSE &&
                (clip.left != 0 || clip.top != 0 || clip.right != GetSystemMetrics(SM_CXSCREEN) ||
                    clip.bottom != GetSystemMetrics(SM_CYSCREEN));
            snapshot += fmt::format(
                ",\"window\":{{\"foreground\":{},\"minimized\":{},\"visible\":{},"
                "\"client\":[{},{}],\"rect\":[{},{},{},{}],\"cursorClipped\":{}}}",
                JsonBool(GetForegroundWindow() == pWindow->hWnd), JsonBool(IsIconic(pWindow->hWnd) != FALSE),
                JsonBool(IsWindowVisible(pWindow->hWnd) != FALSE), client.right - client.left,
                client.bottom - client.top, window.left, window.top, window.right, window.bottom,
                JsonBool(cursorClipped));
        }

        markSnapshotPhase(1);
        // Dynamic Havok references (the Helgen carts in particular) are not
        // actors and therefore do not appear in networkEntities. Expose their
        // live reference transforms so paired captures can identify divergent
        // rigid bodies by form ID without guessing from the rendered image.
        snapshot += ",\"nearbyReferences\":[";
        bool firstReference = true;
        uint32_t referenceCount = 0;
        auto* pLocalPlayer = PlayerCharacter::Get();
        if (pLocalPlayer && pLocalPlayer->parentCell && pLocalPlayer->parentCell->refData.refArray)
        {
            const auto& references = pLocalPlayer->parentCell->refData;
            for (uint32_t i = 0; i < references.capacity && referenceCount < 256; ++i)
            {
                auto* pReference = references.refArray[i].Get();
                if (!pReference || pReference == pLocalPlayer || !pReference->baseForm ||
                    !pReference->loadedState || Cast<Actor>(pReference))
                    continue;

                const auto delta = pReference->position - pLocalPlayer->position;
                if (glm::dot(delta, delta) > 30000.f * 30000.f)
                    continue;

                if (!firstReference)
                    snapshot += ',';
                firstReference = false;
                ++referenceCount;
                ObjectService::RemotePhysicsDiagnostic authority{};
                const bool hasAuthority = m_world.ctx().at<ObjectService>().GetRemotePhysicsDiagnostic(
                    pReference->formID, authority);
                snapshot += '{';
                snapshot += fmt::format(
                    "\"formId\":{},\"baseId\":{},\"formType\":{},\"position\":[{},{},{}],"
                    "\"rotation\":[{},{},{}],\"hasHostPhysics\":{},"
                    "\"hostPhysicsPosition\":[{},{},{}],\"hostPhysicsTick\":{},"
                    "\"hostPhysicsAgeMs\":{},\"hostPhysicsBodyDriven\":{}",
                    pReference->formID, pReference->baseForm->formID,
                    static_cast<uint32_t>(pReference->baseForm->formType), pReference->position.x,
                    pReference->position.y, pReference->position.z, pReference->rotation.x,
                    pReference->rotation.y, pReference->rotation.z, JsonBool(hasAuthority),
                    authority.Position.x, authority.Position.y, authority.Position.z,
                    authority.Tick, authority.AgeMs, JsonBool(authority.BodyDriven));
                snapshot += '}';
            }
        }
        snapshot += ']';

        // A first-N cell walk can omit the gate and its trigger entirely.
        // Sort all nearby interactive references by distance for a bounded,
        // generic read-only door/activator view, not an MQ101 form-ID patch.
        snapshot += ",\"nearbyInteractiveReferences\":[";
        if (pLocalPlayer && pLocalPlayer->parentCell &&
            pLocalPlayer->parentCell->refData.refArray)
        {
            struct InteractiveCandidate
            {
                TESObjectREFR* Reference{};
                float DistanceSquared{};
            };
            std::vector<InteractiveCandidate> candidates;
            const auto& references = pLocalPlayer->parentCell->refData;
            const auto scanLimit = std::min<uint32_t>(references.capacity, 50000);
            for (uint32_t i = 0; i < scanLimit; ++i)
            {
                auto* pReference = references.refArray[i].Get();
                if (!pReference || !pReference->baseForm)
                    continue;
                const auto type = pReference->baseForm->formType;
                if (type != FormType::Door && type != FormType::Activator)
                    continue;
                const auto delta = pReference->position - pLocalPlayer->position;
                const float distanceSquared = glm::dot(delta, delta);
                if (distanceSquared <= 8000.f * 8000.f)
                    candidates.push_back({pReference, distanceSquared});
            }
            std::sort(candidates.begin(), candidates.end(),
                [](const auto& left, const auto& right) {
                    return left.DistanceSquared < right.DistanceSquared;
                });
            const bool openStateAvailable =
                m_world.ctx().at<PapyrusService>().Get(
                    "ObjectReference", "GetOpenState") != nullptr;
            const size_t limit = std::min<size_t>(candidates.size(), 96);
            for (size_t i = 0; i < limit; ++i)
            {
                const auto* pReference = candidates[i].Reference;
                const bool isDoor = pReference->baseForm->formType == FormType::Door;
                const bool openStateReadable = isDoor && pReference->loadedState &&
                    openStateAvailable;
                const auto openState = openStateReadable ?
                    const_cast<TESObjectREFR*>(pReference)->GetOpenState() :
                    TESObjectREFR::kNone;
                if (i != 0)
                    snapshot += ',';
                snapshot += fmt::format(
                    "{{\"formId\":{},\"baseId\":{},\"baseType\":{},"
                    "\"distance\":{},\"position\":[{},{},{}],"
                    "\"loaded\":{},\"disabled\":{},\"openStateReadable\":{},"
                    "\"openState\":{}}}",
                    pReference->formID, pReference->baseForm->formID,
                    static_cast<uint32_t>(pReference->baseForm->formType),
                    std::sqrt(candidates[i].DistanceSquared),
                    pReference->position.x, pReference->position.y,
                    pReference->position.z, JsonBool(pReference->loadedState != nullptr),
                    JsonBool(pReference->IsDisabled()), JsonBool(openStateReadable),
                    static_cast<uint32_t>(openState));
            }
        }
        snapshot += ']';

        // Scan active scenes across every quest, not only the optional watched
        // quest list. This makes the parity probe useful outside MQ101.
        snapshot += ",\"activeScenes\":[";
        bool firstActiveScene = true;
        uint32_t activeSceneCount = 0;
        if (auto* pModManager = ModManager::Get())
        {
            for (auto* pQuest : pModManager->quests)
            {
                if (!pQuest || !pQuest->IsEnabled() || activeSceneCount >= 64)
                    continue;
                const auto& scenes = pQuest->scenes;
                if (!scenes.data || scenes.length > 256 || scenes.length > scenes.capacity ||
                    !IsReadableRange(scenes.data, sizeof(BGSScene*) * scenes.length))
                    continue;
                for (uint32_t sceneIndex = 0; sceneIndex < scenes.length && activeSceneCount < 64; ++sceneIndex)
                {
                    const auto* pScene = scenes.data[sceneIndex];
                    if (!pScene || !IsReadableRange(pScene, sizeof(BGSScene)) || !pScene->isPlaying)
                        continue;
                    if (!firstActiveScene)
                        snapshot += ',';
                    firstActiveScene = false;
                    ++activeSceneCount;
                    uint64_t actionSignature = cFnvOffsetBasis;
                    uint32_t readableActions = 0;
                    const auto& actions = pScene->actions;
                    if (actions.data && actions.length <= 128 && actions.length <= actions.capacity &&
                        IsReadableRange(actions.data, sizeof(void*) * actions.length))
                    {
                        for (uint32_t actionIndex = 0; actionIndex < actions.length; ++actionIndex)
                        {
                            SceneActionDiagnosticView action{};
                            if (!ReadNative(actions.data[actionIndex], action))
                                continue;
                            HashWord(actionSignature, actionIndex);
                            HashWord(actionSignature, action.ActorId);
                            HashWord(actionSignature, action.StartPhase);
                            HashWord(actionSignature, action.EndPhase);
                            HashWord(actionSignature, action.Flags);
                            ++readableActions;
                        }
                    }
                    snapshot += fmt::format(
                        "{{\"questId\":{},\"questEditorId\":\"{}\","
                        "\"sceneId\":{},\"rawPhaseWord\":{},"
                        "\"actionCount\":{},\"readableActions\":{},\"actionSignature\":{}}}",
                        pQuest->formID, EscapeJson(pQuest->idName.AsAscii()),
                        pScene->formID, pScene->rawPhaseWord,
                        actions.length, readableActions, actionSignature);
                }
            }
        }
        snapshot += ']';

        markSnapshotPhase(2);
        // Targeted, read-only probe for the two vanilla MQ101 cart references.
        // This does not alter their script, animation, or Havok state.
        snapshot += ",\"introCartReferences\":[";
        for (size_t i = 0; i < std::size(cartIds); ++i)
        {
            if (i != 0)
                snapshot += ',';
            auto* pCart = Cast<TESObjectREFR>(TESForm::GetById(cartIds[i]));
            auto* pNode = pCart ? pCart->GetNiNode() : nullptr;
            BSAnimationGraphManager* pGraphManager = nullptr;
            const bool hasGraph = pCart && pCart->animationGraphHolder.GetBSAnimationGraph(&pGraphManager) && pGraphManager;
            // CommonLibSSE-NG's pinned bhkNiCollisionObject layout places its
            // bhkWorldObject pointer at +0x20; bhkRefObject then owns the
            // hkpRigidBody pointer at +0x10. This remains read-only and is
            // guarded at every indirection on the live runtime.
            uint32_t collisionFlags = 0;
            std::string collisionType;
            std::string collisionParentType;
            bool collisionTypeReadable = false;
            void* pBodyWrapper = nullptr;
            void* pHavokBody = nullptr;
            ActorPoseDiagnosticViews::RigidBody havokBody{};
            bool bodyReadable = false;
            if (pNode && pNode->collisionObject)
            {
                struct NativeNiRttiView
                {
                    const char* Name{};
                    const void* Parent{};
                };
                const auto* pRtti = reinterpret_cast<NiObject*>(
                    pNode->collisionObject)->GetRTTI();
                NativeNiRttiView rtti{};
                if (pRtti && ReadNative(pRtti, rtti))
                {
                    collisionType = ReadNativeString(rtti.Name,
                        collisionTypeReadable, 64);
                    NativeNiRttiView parent{};
                    if (rtti.Parent && ReadNative(rtti.Parent, parent))
                    {
                        bool parentReadable = false;
                        collisionParentType = ReadNativeString(parent.Name,
                            parentReadable, 64);
                        if (!parentReadable)
                            collisionParentType.clear();
                    }
                    if (!collisionTypeReadable)
                        collisionType.clear();
                }
                const auto collisionAddress = reinterpret_cast<uintptr_t>(pNode->collisionObject);
                if (collisionAddress <= std::numeric_limits<uintptr_t>::max() - 0x28 &&
                    ReadNative(reinterpret_cast<const void*>(collisionAddress + 0x18), collisionFlags) &&
                    ReadNative(reinterpret_cast<const void*>(collisionAddress + 0x20), pBodyWrapper) &&
                    pBodyWrapper)
                {
                    const auto wrapperAddress = reinterpret_cast<uintptr_t>(pBodyWrapper);
                    if (wrapperAddress <= std::numeric_limits<uintptr_t>::max() - 0x18 &&
                        ReadNative(reinterpret_cast<const void*>(wrapperAddress + 0x10), pHavokBody) &&
                        pHavokBody)
                        bodyReadable = ReadNative(pHavokBody, havokBody) && havokBody.motionType <= 7 &&
                            std::isfinite(havokBody.linearVelocity[0]) &&
                            std::isfinite(havokBody.linearVelocity[1]) &&
                            std::isfinite(havokBody.linearVelocity[2]) &&
                            std::isfinite(havokBody.transform[12]) &&
                            std::isfinite(havokBody.transform[13]) &&
                            std::isfinite(havokBody.transform[14]);
                }
            }
            snapshot += fmt::format(
                "{{\"formId\":{},\"present\":{},\"loaded\":{},\"node\":{},\"animationGraph\":{},"
                "\"baseId\":{},\"formType\":{},\"cellId\":{},\"position\":[{},{},{}],"
                "\"nodeWorldPosition\":[{},{},{}],\"nodeLocalPosition\":[{},{},{}],"
                "\"collisionObjectPresent\":{},\"collisionFlags\":{},"
                "\"collisionTypeReadable\":{},\"collisionType\":\"{}\","
                "\"collisionParentType\":\"{}\","
                "\"havokBodyReadable\":{},\"havokWorldPresent\":{},\"havokMotionType\":{},"
                "\"havokTransformPosition\":[{},{},{}],"
                "\"havokLinearVelocity\":[{},{},{}],\"motionSamples\":{},"
                "\"peakFrameStep\":{},\"peakFrameSpeed\":{},\"largeFrameSteps\":{}}}",
                cartIds[i], JsonBool(pCart != nullptr), JsonBool(pCart && pCart->loadedState),
                JsonBool(pNode != nullptr), JsonBool(hasGraph),
                pCart && pCart->baseForm ? pCart->baseForm->formID : 0,
                pCart && pCart->baseForm ? static_cast<uint32_t>(pCart->baseForm->formType) : 0,
                pCart ? pCart->GetCellId() : 0,
                pCart ? pCart->position.x : 0.f,
                pCart ? pCart->position.y : 0.f,
                pCart ? pCart->position.z : 0.f,
                pNode ? pNode->world.translate.x : 0.f,
                pNode ? pNode->world.translate.y : 0.f,
                pNode ? pNode->world.translate.z : 0.f,
                pNode ? pNode->local.translate.x : 0.f,
                pNode ? pNode->local.translate.y : 0.f,
                pNode ? pNode->local.translate.z : 0.f,
                JsonBool(pNode && pNode->collisionObject), collisionFlags,
                JsonBool(collisionTypeReadable), EscapeJson(collisionType),
                EscapeJson(collisionParentType),
                JsonBool(bodyReadable), JsonBool(bodyReadable && havokBody.world),
                bodyReadable ? havokBody.motionType : 0,
                bodyReadable ? havokBody.transform[12] : 0.f,
                bodyReadable ? havokBody.transform[13] : 0.f,
                bodyReadable ? havokBody.transform[14] : 0.f,
                bodyReadable ? havokBody.linearVelocity[0] : 0.f,
                bodyReadable ? havokBody.linearVelocity[1] : 0.f,
                bodyReadable ? havokBody.linearVelocity[2] : 0.f,
                m_introCartMotionStats[i].Samples, m_introCartMotionStats[i].PeakStep,
                m_introCartMotionStats[i].PeakSpeed, m_introCartMotionStats[i].LargeSteps);
        }
        snapshot += ']';

        snapshot += ",\"recentHitchMotionEvents\":[";
        for (uint32_t i = 0; i < m_hitchMotionCount; ++i)
        {
            const auto index = (m_hitchMotionNext + m_hitchMotionEvents.size() -
                m_hitchMotionCount + i) % m_hitchMotionEvents.size();
            const auto& event = m_hitchMotionEvents[index];
            if (i)
                snapshot += ',';
            snapshot += fmt::format(
                "{{\"timeMs\":{},\"worldTick\":{},\"formId\":{},\"worldGapUs\":{},"
                "\"vmGapUs\":{},\"priorVmAppUs\":{},"
                "\"priorVmOriginalUs\":{},\"priorGameTestUs\":{},"
                "\"cartDeltaMs\":{},\"cartStep\":{},"
                "\"position\":[{},{},{}]}}",
                event.TimeMs, event.WorldTick, event.FormId, event.WorldGapUs,
                event.VmGapUs, event.PriorVmAppUs,
                event.PriorVmOriginalUs, event.PriorGameTestUs,
                event.CartDeltaMs, event.CartStep,
                event.Position[0], event.Position[1], event.Position[2]);
        }
        snapshot += ']';

        markSnapshotPhase(3);
        snapshot += ",\"networkEntities\":[";
        bool firstEntity = true;
        size_t emittedEntities = 0;
        std::unordered_map<uint32_t, uint32_t> networkActorIds;
        const auto presentationNow = m_world.GetTransport().GetClock().GetCurrentTick();
        const auto presentationDelay = static_cast<uint64_t>(
            m_world.GetCharacterService().GetPresentationDelayMs());
        const auto presentationTick = presentationNow > presentationDelay ?
            presentationNow - presentationDelay : 0;
        const auto entityView = m_world.view<FormIdComponent>();
        for (const auto entity : entityView)
        {
            if (emittedEntities++ >= 256)
                break;
            const auto& form = entityView.get<FormIdComponent>(entity);
            const auto* pLocal = m_world.try_get<LocalComponent>(entity);
            const auto* pRemote = m_world.try_get<RemoteComponent>(entity);
            const auto* pNetworkPlayer = m_world.try_get<PlayerComponent>(entity);
            const auto* pLocalAnimation = m_world.try_get<LocalAnimationComponent>(entity);
            const auto* pRemoteAnimation = m_world.try_get<RemoteAnimationComponent>(entity);
            const auto* pInterpolation = m_world.try_get<InterpolationComponent>(entity);
            const auto* pVisual = pLocalAnimation ?
                &pLocalAnimation->LastSentVisualBones :
                (pRemoteAnimation ? &pRemoteAnimation->VisualBones : nullptr);
            auto* pForm = TESForm::GetById(form.Id);
            auto* pActor = Cast<Actor>(pForm);
            if (pActor)
                networkActorIds.emplace(form.Id, pLocal ? pLocal->Id :
                    (pRemote ? pRemote->Id : 0));
            auto* pRootNode = pActor ? pActor->GetNiNode() : nullptr;
            // Limit inventory scans: this snapshot is taken on the game thread.
            const auto wornArmorCount = pActor && emittedEntities <= 48 ?
                pActor->GetWornArmor().Entries.size() : 0;
            // The extra scan is only needed for the suspicious zero-armor
            // cases; normal snapshots retain their existing cost.
            const auto inventoryEntryCount = pActor && emittedEntities <= 48 &&
                wornArmorCount == 0 ? pActor->GetActorInventory().Entries.size() : 0;
            const auto* pNpcBase = pActor ? Cast<TESNPC>(pActor->baseForm) : nullptr;
            const uint32_t defaultOutfitId = pNpcBase && pNpcBase->defaultOutfit ?
                pNpcBase->defaultOutfit->formID : 0;
            const auto nativeMountState = pActor && emittedEntities <= 48 ?
                pActor->GetNativeMountState() : Actor::NativeMountState{};
            if (pActor && ((pRemoteAnimation &&
                    pRemoteAnimation->EvaluatedPose.Bones.size() == 99) ||
                (pLocalAnimation &&
                    pLocalAnimation->LastSentPose.Bones.size() == 99)))
                AnimationGraphUpdateTrace::WatchHolder(
                    &pActor->animationGraphHolder, form.Id);
            const auto graphTrace = AnimationGraphUpdateTrace::GetHolderSample(
                pActor ? &pActor->animationGraphHolder : nullptr);
            const auto nativeAnimation = SampleNativeActorAnimation(pActor);
            const auto* pExtension = pActor ? pActor->GetExtension() : nullptr;
            const auto* pPackage = pActor && pActor->currentProcess ?
                pActor->currentProcess->package : nullptr;
            auto* pCombatTarget = pActor ? pActor->GetCombatTarget() : nullptr;
            auto* pActorCell = pActor ? pActor->GetParentCellEx() : nullptr;
            const auto* pProcess = pActor ? pActor->currentProcess : nullptr;
            const auto followHandle = ReadProcessHandle(pProcess, 0x110);
            const auto aiTargetHandle = ReadProcessHandle(pProcess, 0x114);
            // The published AE getter crashed this installed runtime during
            // loading; do not call it until its ABI is verified locally.
            const uint32_t headtrackHandle = 0;
            uint8_t processLevel{};
            if (pProcess)
                ReadNative(reinterpret_cast<const uint8_t*>(pProcess) + 0x137,
                    processLevel);
            if (!firstEntity)
                snapshot += ',';
            firstEntity = false;
            snapshot += fmt::format(
                "{{\"formId\":{},\"playerId\":{},\"authority\":\"{}\",\"networkId\":{},"
                "\"ownershipEpoch\":{},\"waitingFor3D\":{},\"waitingForAssignment\":{},"
                "\"has3D\":{},\"hasAIProcess\":{},\"rootChildCount\":{},\"wornArmorCount\":{},\"inventoryEntryCount\":{},\"inventoryEntriesSampled\":{},\"defaultOutfitId\":{},\"nativeMountFormId\":{},"
                "\"horseExtra\":{},\"horseHandle\":{},"
                "\"interactionExtra\":{},\"interactionPointerPresent\":{},"
                "\"interactionActorHandle\":{},\"interactionTargetHandle\":{},"
                "\"animationQueued\":{},\"animationReplayQueued\":{},"
                "\"poseSourceTick\":{},\"poseBoneCount\":{},\"poseChecksum\":{},"
                "\"visualSourceTick\":{},\"visualBoneCount\":{},\"visualChecksum\":{},"
                "\"visualRootPresent\":{},\"visualRootWorldT\":[{},{},{}],"
                "\"graphPostCallMs\":{},\"graphPostAgeMs\":{},"
                "\"graphPostThreadId\":{},\"cellId\":{},\"position\":[{},{},{}],"
                "\"authorityCellId\":{},\"authorityWorldSpaceId\":{},"
                "\"authorityMovementTick\":{},\"authorityStableSinceTick\":{},"
                "\"corpseCorrectionAttempts\":{},\"corpseCorrectionAttemptsForTarget\":{},"
                "\"lastCorpseCorrectionTick\":{},"
                "\"authorityPosition\":[{},{},{}],"
                "\"packageFormId\":{},\"combatTargetFormId\":{},"
                "\"processLevel\":{},\"aiFollowHandle\":{},\"aiFollowFormId\":{},"
                "\"aiTargetHandle\":{},\"aiTargetFormId\":{},"
                "\"headtrackReadable\":false,\"headtrackHandle\":{},\"headtrackFormId\":{},"
                "\"dialogueHandle\":{},\"dialogueTargetFormId\":{},"
                "\"lastActionId\":{},\"lastActionTargetId\":{},\"lastActionEvent\":\"{}\","
                "\"lastActionTick\":{},\"lastActionType\":{},"
                "\"lastActionState1\":{},\"lastActionState2\":{},"
                "\"ownerProcessedTick\":{},\"ownerProcessedActionId\":{},"
                "\"ownerProcessedEvent\":\"{}\","
                "\"remoteRanTick\":{},\"remoteRanActionId\":{},"
                "\"remoteRanEvent\":\"{}\","
                "\"remoteProcessedTick\":{},\"remoteProcessedActionId\":{},"
                "\"nativeGraphReady\":{},\"nativeStateId\":{},\"nativeTimeInState\":{},"
                "\"nativeCloneStateReadable\":{},\"nativeCloneStateId\":{},"
                "\"nativeCloneTimeInState\":{},"
                "\"nativePoseCount\":{},\"nativePoseChecksum\":{},"
                "\"renderBoneCount\":{},\"renderBoneChecksum\":{},\"renderWorldBoneChecksum\":{},"
                "\"visualGeometrySampled\":{},\"wornArmorSampled\":{},"
                "\"rootChildArrayLength\":{},"
                "\"rootChildArrayCapacity\":{},\"rootChildren\":[",
                form.Id, pNetworkPlayer ? pNetworkPlayer->Id : 0,
                pLocal ? "local" : (pRemote ? "remote" : "unassigned"),
                pLocal ? pLocal->Id : (pRemote ? pRemote->Id : 0),
                pLocal ? pLocal->OwnershipEpoch : (pRemote ? pRemote->OwnershipEpoch : 0),
                JsonBool(m_world.all_of<WaitingFor3D>(entity)),
                JsonBool(m_world.all_of<WaitingForAssignmentComponent>(entity)),
                JsonBool(pRootNode != nullptr),
                JsonBool(pActor && pActor->currentProcess != nullptr),
                pRootNode ? pRootNode->children.length : 0,
                wornArmorCount,
                inventoryEntryCount,
                JsonBool(pActor && emittedEntities <= 48 && wornArmorCount == 0),
                defaultOutfitId,
                pActor ? pActor->GetNativeMountFormId() : 0,
                JsonBool(nativeMountState.HorseExtra), nativeMountState.HorseHandle,
                JsonBool(nativeMountState.InteractionExtra),
                JsonBool(nativeMountState.InteractionPointerPresent),
                nativeMountState.InteractionActorHandle,
                nativeMountState.InteractionTargetHandle,
                pLocalAnimation ? pLocalAnimation->Actions.size() : 0,
                pRemoteAnimation ? pRemoteAnimation->TimePoints.size() : 0,
                pLocalAnimation ? pLocalAnimation->LastSentPose.SourceTick :
                    (pRemoteAnimation ? pRemoteAnimation->EvaluatedPose.SourceTick : 0),
                pLocalAnimation ? pLocalAnimation->LastSentPose.Bones.size() :
                    (pRemoteAnimation ? pRemoteAnimation->EvaluatedPose.Bones.size() : 0),
                pLocalAnimation ? pLocalAnimation->LastSentPose.Checksum() :
                    (pRemoteAnimation ? pRemoteAnimation->EvaluatedPose.Checksum() : 0),
                pVisual ? pVisual->SourceTick : 0,
                pVisual ? pVisual->Bones.size() : 0,
                pVisual ? pVisual->Checksum() : 0,
                JsonBool(pVisual && pVisual->RootWorld.Present),
                pVisual ? pVisual->RootWorld.Translation[0] : 0.f,
                pVisual ? pVisual->RootWorld.Translation[1] : 0.f,
                pVisual ? pVisual->RootWorld.Translation[2] : 0.f,
                graphTrace.LastPostCallMs,
                graphTrace.LastPostCallMs && now >= graphTrace.LastPostCallMs ?
                    now - graphTrace.LastPostCallMs : 0,
                graphTrace.ThreadId,
                pActorCell ? pActorCell->formID : 0,
                pActor ? pActor->position.x : 0.f,
                pActor ? pActor->position.y : 0.f,
                pActor ? pActor->position.z : 0.f,
                pInterpolation && pInterpolation->AuthorityCellId ?
                    m_world.GetModSystem().GetGameId(pInterpolation->AuthorityCellId) : 0,
                pInterpolation && pInterpolation->AuthorityWorldSpaceId ?
                    m_world.GetModSystem().GetGameId(pInterpolation->AuthorityWorldSpaceId) : 0,
                pInterpolation ? pInterpolation->AuthorityTick : 0,
                pInterpolation ? pInterpolation->AuthorityStableSinceTick : 0,
                pInterpolation ? pInterpolation->CorpseCorrectionAttempts : 0,
                pInterpolation ? pInterpolation->CorpseCorrectionAttemptsForTarget : 0,
                pInterpolation ? pInterpolation->LastCorpseCorrectionTick : 0,
                pInterpolation ? pInterpolation->AuthorityPosition.x : 0.f,
                pInterpolation ? pInterpolation->AuthorityPosition.y : 0.f,
                pInterpolation ? pInterpolation->AuthorityPosition.z : 0.f,
                pPackage ? pPackage->formID : 0,
                pCombatTarget ? pCombatTarget->formID : 0,
                processLevel, followHandle, ResolveHandleFormId(followHandle),
                aiTargetHandle, ResolveHandleFormId(aiTargetHandle),
                headtrackHandle, ResolveHandleFormId(headtrackHandle),
                pActor ? pActor->dialogueHandle : 0,
                pActor ? ResolveHandleFormId(pActor->dialogueHandle) : 0,
                pExtension ? pExtension->LatestAnimation.ActionId : 0,
                pExtension ? pExtension->LatestAnimation.TargetId : 0,
                pExtension ? EscapeJson(pExtension->LatestAnimation.EventName.c_str()) : "",
                pExtension ? pExtension->LatestAnimation.Tick : 0,
                pExtension ? pExtension->LatestAnimation.Type : 0,
                pExtension ? pExtension->LatestAnimation.State1 : 0,
                pExtension ? pExtension->LatestAnimation.State2 : 0,
                pLocalAnimation ? pLocalAnimation->LastProcessedAction.Tick : 0,
                pLocalAnimation ? pLocalAnimation->LastProcessedAction.ActionId : 0,
                pLocalAnimation ? EscapeJson(
                    pLocalAnimation->LastProcessedAction.EventName.c_str()) : "",
                pRemoteAnimation ? pRemoteAnimation->LastRanAction.Tick : 0,
                pRemoteAnimation ? pRemoteAnimation->LastRanAction.ActionId : 0,
                pRemoteAnimation ? EscapeJson(
                    pRemoteAnimation->LastRanAction.EventName.c_str()) : "",
                pRemoteAnimation ? pRemoteAnimation->LastProcessedAction.Tick : 0,
                pRemoteAnimation ? pRemoteAnimation->LastProcessedAction.ActionId : 0,
                JsonBool(nativeAnimation.GraphReady), nativeAnimation.StateId,
                nativeAnimation.TimeInState,
                JsonBool(nativeAnimation.CloneStateReadable),
                nativeAnimation.CloneStateId,
                std::isfinite(nativeAnimation.CloneTimeInState) ?
                    nativeAnimation.CloneTimeInState : 0.f,
                nativeAnimation.PoseCount,
                nativeAnimation.PoseChecksum, nativeAnimation.RenderBoneCount,
                nativeAnimation.RenderBoneChecksum,
                nativeAnimation.RenderWorldBoneChecksum,
                JsonBool(emittedEntities <= 48), JsonBool(emittedEntities <= 48),
                pRootNode ? pRootNode->children.length : 0,
                pRootNode ? pRootNode->children.capacity : 0);

            // A root node alone does not prove the actor's body geometry has
            // attached. Keep this one-shot probe bounded and read-only so a
            // missing body can be distinguished from a pose or outfit issue.
            constexpr uint16_t cMaxRootChildren = 16;
            const auto* pChildren = pRootNode ? &pRootNode->children : nullptr;
            const bool childrenReadable = pChildren &&
                pChildren->length <= 256 &&
                (pChildren->length == 0 ||
                    (pChildren->data && IsReadableRange(pChildren->data,
                        sizeof(NiAVObject*) * pChildren->length)));
            if (childrenReadable && emittedEntities <= 48)
            {
                for (uint16_t index = 0;
                    index < std::min(pChildren->length, cMaxRootChildren); ++index)
                {
                    if (index != 0)
                        snapshot += ',';
                    NiAVObject* pChild{};
                    const bool childReadable = ReadNative(pChildren->data + index, pChild) &&
                        pChild && IsReadableRange(pChild, sizeof(NiAVObject));
                    const char* pName{};
                    uint32_t flags{};
                    float localScale{};
                    bool nameReadable = false;
                    if (childReadable)
                    {
                        ReadNative(reinterpret_cast<const uint8_t*>(pChild) + 0x10, pName);
                        ReadNative(reinterpret_cast<const uint8_t*>(pChild) +
                            offsetof(NiAVObject, flags), flags);
                        ReadNative(reinterpret_cast<const uint8_t*>(pChild) +
                            offsetof(NiAVObject, local) + offsetof(NiTransform, scale), localScale);
                    }
                    const auto name = ReadNativeString(pName, nameReadable, 80);
                    NiNode* pNestedNode = childReadable ? pChild->AsNode() : nullptr;
                    if (pNestedNode && !IsReadableRange(pNestedNode, sizeof(NiNode)))
                        pNestedNode = nullptr;
                    const auto* pNestedChildren = pNestedNode ? &pNestedNode->children : nullptr;
                    const bool nestedReadable = pNestedChildren &&
                        pNestedChildren->length <= 128 &&
                        (pNestedChildren->length == 0 ||
                            (pNestedChildren->data && IsReadableRange(
                                pNestedChildren->data,
                                sizeof(NiAVObject*) * pNestedChildren->length)));
                    uint16_t nestedPresentCount = 0;
                    std::string nestedNames;
                    if (nestedReadable)
                    {
                        constexpr uint16_t cMaxNestedNames = 16;
                        uint16_t emittedNestedNames = 0;
                        for (uint16_t nestedIndex = 0;
                            nestedIndex < pNestedChildren->length; ++nestedIndex)
                        {
                            NiAVObject* pNestedChild{};
                            if (!ReadNative(pNestedChildren->data + nestedIndex, pNestedChild) ||
                                !pNestedChild || !IsReadableRange(pNestedChild, sizeof(NiAVObject)))
                                continue;
                            ++nestedPresentCount;
                            if (emittedNestedNames >= cMaxNestedNames)
                                continue;
                            const char* pNestedName{};
                            bool nestedNameReadable = false;
                            ReadNative(reinterpret_cast<const uint8_t*>(pNestedChild) + 0x10,
                                pNestedName);
                            const auto nestedName = ReadNativeString(pNestedName,
                                nestedNameReadable, 80);
                            if (emittedNestedNames++ != 0)
                                nestedNames += ',';
                            nestedNames += fmt::format("\"{}\"", EscapeJson(nestedName));
                        }
                    }
                    snapshot += fmt::format(
                        "{{\"index\":{},\"present\":{},\"name\":\"{}\","
                        "\"nameReadable\":{},\"hidden\":{},\"scale\":{},"
                        "\"nestedNode\":{},\"nestedReadable\":{},"
                        "\"nestedChildCount\":{},\"nestedPresentCount\":{},"
                        "\"nestedNames\":[{}]}}",
                        index, JsonBool(childReadable), EscapeJson(name),
                        JsonBool(nameReadable), JsonBool(childReadable && (flags & 1u)),
                        childReadable && std::isfinite(localScale) ? localScale : 0.f,
                        JsonBool(pNestedNode != nullptr), JsonBool(nestedReadable),
                        pNestedChildren ? pNestedChildren->length : 0,
                        nestedPresentCount, nestedNames);
                }
            }
            snapshot += ']';
            snapshot += fmt::format(
                ",\"graphVariables\":{{\"count\":{},\"checksum\":{},"
                "\"sampledCount\":{},\"values\":[",
                nativeAnimation.GraphVariableCount,
                nativeAnimation.GraphVariableChecksum,
                nativeAnimation.GraphVariables.size());
            for (size_t i = 0; i < nativeAnimation.GraphVariables.size(); ++i)
            {
                if (i != 0)
                    snapshot += ',';
                snapshot += fmt::format("{}", nativeAnimation.GraphVariables[i]);
            }
            snapshot += "]}";
            snapshot += fmt::format(
                ",\"actionPipeline\":{{\"dispatchStage\":{},"
                "\"ownerQueued\":{},\"ownerLastSentTick\":{},"
                "\"ownerLastSentEvent\":\"{}\",\"ownerLastSentTargetId\":{},"
                "\"followerQueued\":{},\"followerLastReceivedTick\":{},"
                "\"followerLastReceivedEvent\":\"{}\","
                "\"followerLastReceivedTargetId\":{},"
                "\"followerLastReplayResult\":{}}}",
                pExtension ? pExtension->LatestAnimationDispatch : 0,
                pLocalAnimation ? pLocalAnimation->Actions.size() : 0,
                pLocalAnimation ? pLocalAnimation->LastSentAction.Tick : 0,
                pLocalAnimation ? EscapeJson(
                    pLocalAnimation->LastSentAction.EventName.c_str()) : "",
                pLocalAnimation ? pLocalAnimation->LastSentAction.TargetId : 0,
                pRemoteAnimation ? pRemoteAnimation->TimePoints.size() : 0,
                pRemoteAnimation ? pRemoteAnimation->LastReceivedAction.Tick : 0,
                pRemoteAnimation ? EscapeJson(
                    pRemoteAnimation->LastReceivedAction.EventName.c_str()) : "",
                pRemoteAnimation ? pRemoteAnimation->LastReceivedAction.TargetId : 0,
                JsonBool(pRemoteAnimation && pRemoteAnimation->LastRanActionResult));
            snapshot += fmt::format(
                ",\"combatTargetPipeline\":{{\"desiredServerId\":{},"
                "\"sourceTick\":{},\"lastReceivedTick\":{},"
                "\"queued\":{},\"lastApplyTick\":{},"
                "\"lastDesiredFormId\":{},\"lastBeforeFormId\":{},"
                "\"lastAfterFormId\":{},\"nativeHandle\":{},"
                "\"nativeFormId\":{}}}",
                pRemoteAnimation ? pRemoteAnimation->DesiredCombatTargetServerId : 0xFFFFFFFFu,
                pRemoteAnimation ? pRemoteAnimation->DesiredCombatTargetTick : 0,
                pRemoteAnimation ? pRemoteAnimation->LastReceivedCombatTargetTick : 0,
                pRemoteAnimation ? pRemoteAnimation->CombatTargetTimePoints.size() : 0,
                pRemoteAnimation ? pRemoteAnimation->LastCombatTargetApplyTick : 0,
                pRemoteAnimation ? pRemoteAnimation->LastCombatTargetApplyDesiredFormId : 0,
                pRemoteAnimation ? pRemoteAnimation->LastCombatTargetApplyBeforeFormId : 0,
                pRemoteAnimation ? pRemoteAnimation->LastCombatTargetApplyAfterFormId : 0,
                pActor && pActor->pCombatController ?
                    pActor->pCombatController->targetHandle : 0,
                pActor && pActor->pCombatController ? ResolveHandleFormId(
                    pActor->pCombatController->targetHandle) : 0);
            const InterpolationComponent::TimePoint* pMovementPoint{};
            if (pInterpolation && pInterpolation->TimePoints.size() >= 2)
            {
                const auto first = pInterpolation->TimePoints.begin();
                const auto second = std::next(first);
                pMovementPoint = presentationTick >= second->Tick ?
                    &*second : &*first;
            }
            const auto* pDescriptor = pExtension ?
                AnimationGraphDescriptorManager::Get().GetDescriptor(
                    pExtension->GraphDescriptorHash) : nullptr;
            snapshot += fmt::format(
                ",\"movementGraphInputs\":{{\"sourceTick\":{},\"presentationTick\":{},"
                "\"descriptorFound\":{},\"values\":[",
                pMovementPoint ? pMovementPoint->Tick : 0, presentationTick,
                JsonBool(pDescriptor != nullptr));
            if (pMovementPoint && pDescriptor)
            {
                constexpr uint32_t cDecisionIndices[] = {
                    0, 1, 2, 3, 6, 8, 13, 40, 47, 48, 53, 127, 155,
                    178, 184, 221, 229};
                bool firstInput = true;
                for (const auto index : cDecisionIndices)
                {
                    uint32_t rawValue{};
                    if (!TryReadPackedGraphInput(*pDescriptor,
                            pMovementPoint->Variables, index, rawValue))
                        continue;
                    if (!firstInput)
                        snapshot += ',';
                    firstInput = false;
                    snapshot += fmt::format("{{\"index\":{},\"raw\":{}}}",
                        index, rawValue);
                }
            }
            snapshot += "]}";
            const auto mailbox = pActor ? VisualPoseMailbox::GetHolderDiagnostics(
                &pActor->animationGraphHolder, pActor) :
                VisualPoseMailbox::HolderDiagnostics{};
            snapshot += fmt::format(
                ",\"mailbox\":{{\"slotIndex\":{},\"slotOwnerEvictions\":{},"
                "\"slotInspectMisses\":{},\"slotOwnerMatches\":{},"
                "\"frameMatchesHolder\":{},\"frameMatchesActor\":{},"
                "\"statsHolderMatches\":{},\"ownerPublishCount\":{},"
                "\"ownerInspectCount\":{},\"ownerApplyCount\":{},"
                "\"lastPublishAgeMs\":{},\"formId\":{},\"ownershipEpoch\":{},"
                "\"receiptAgeMs\":{},\"latestSourceTick\":{},\"historyCount\":{},"
                "\"presentationTick\":{},\"presentationBracketed\":{},"
                "\"lastInspectAgeMs\":{},\"lastEligibleBones\":{},"
                "\"lastWrittenBones\":{},\"lastSkipReason\":{},"
                "\"lastAppliedSourceTick\":{}}}}}",
                mailbox.SlotIndex, mailbox.SlotOwnerEvictions,
                mailbox.SlotInspectMisses, JsonBool(mailbox.SlotOwnerMatches),
                JsonBool(mailbox.FrameMatchesHolder), JsonBool(mailbox.FrameMatchesActor),
                JsonBool(mailbox.StatsHolderMatches), mailbox.OwnerPublishCount,
                mailbox.OwnerInspectCount, mailbox.OwnerApplyCount,
                mailbox.LastPublishAgeMs, mailbox.FormId, mailbox.OwnershipEpoch,
                mailbox.ReceiptAgeMs, mailbox.LatestSourceTick, mailbox.HistoryCount,
                mailbox.PresentationTick, JsonBool(mailbox.PresentationBracketed),
                mailbox.LastInspectAgeMs, mailbox.LastEligibleBones,
                mailbox.LastWrittenBones, static_cast<uint32_t>(mailbox.LastSkipReason),
                mailbox.LastAppliedSourceTick);
        }
        snapshot += ']';

        markSnapshotPhase(4);
        // A network-entity walk misses actors that were never registered with
        // the session. Inventory the player's loaded cell independently so
        // those NPCs are visible as explicit coverage gaps in paired audits.
        snapshot += ",\"nearbyActorAudit\":[";
        auto* pAuditPlayer = PlayerCharacter::Get();
        if (pAuditPlayer)
        {
            auto* pAuditCell = pAuditPlayer->GetParentCellEx();
            if (pAuditCell && pAuditCell->refData.refArray)
            {
                const auto& references = pAuditCell->refData;
                const auto scanLimit = std::min<uint32_t>(references.capacity, 50000);
                uint32_t actorCount = 0;
                for (uint32_t i = 0; i < scanLimit && actorCount < 256; ++i)
                {
                    auto* pReference = references.refArray[i].Get();
                    auto* pActor = pReference ? Cast<Actor>(pReference) : nullptr;
                    if (!pActor || !pActor->loadedState)
                        continue;
                    const auto delta = pActor->position - pAuditPlayer->position;
                    if (glm::dot(delta, delta) > 8000.f * 8000.f)
                        continue;
                    if (actorCount++ != 0)
                        snapshot += ',';
                    const auto nativeAnimation = SampleNativeActorAnimation(pActor);
                    const auto* pExtension = pActor->GetExtension();
                    const auto* pPackage = pActor->currentProcess ?
                        pActor->currentProcess->package : nullptr;
                    auto* pCombatTarget = pActor->GetCombatTarget();
                    const auto* pProcess = pActor->currentProcess;
                    const auto followHandle = ReadProcessHandle(pProcess, 0x110);
                    const auto aiTargetHandle = ReadProcessHandle(pProcess, 0x114);
                    const uint32_t headtrackHandle = 0;
                    const auto networkIt = networkActorIds.find(pActor->formID);
                    snapshot += fmt::format(
                        "{{\"formId\":{},\"baseId\":{},\"networked\":{},\"networkId\":{},"
                        "\"position\":[{},{},{}],\"dead\":{},\"bleedingOut\":{},"
                        "\"has3D\":{},\"packageFormId\":{},\"combatTargetFormId\":{},"
                        "\"aiFollowFormId\":{},\"aiTargetFormId\":{},"
                        "\"headtrackReadable\":false,\"headtrackFormId\":{},"
                        "\"dialogueTargetFormId\":{},"
                        "\"lastActionId\":{},\"lastActionTargetId\":{},\"lastActionEvent\":\"{}\","
                        "\"graphDescriptor\":{},\"nativeGraphReady\":{},\"nativeStateId\":{},"
                        "\"nativeTimeInState\":{},\"nativeCloneStateReadable\":{},"
                        "\"nativeCloneStateId\":{},\"nativeCloneTimeInState\":{},"
                        "\"nativePoseCount\":{},\"nativePoseChecksum\":{},"
                        "\"renderBoneCount\":{},\"renderBoneChecksum\":{},"
                        "\"renderWorldBoneChecksum\":{},"
                        "\"graphVariableCount\":{},\"graphVariableChecksum\":{}}}",
                        pActor->formID, pActor->baseForm ? pActor->baseForm->formID : 0,
                        JsonBool(networkIt != networkActorIds.end()),
                        networkIt != networkActorIds.end() ? networkIt->second : 0,
                        pActor->position.x, pActor->position.y, pActor->position.z,
                        JsonBool(pActor->IsDead()),
                        JsonBool(pActor->actorState.IsBleedingOut()),
                        JsonBool(pActor->GetNiNode() != nullptr),
                        pPackage ? pPackage->formID : 0,
                        pCombatTarget ? pCombatTarget->formID : 0,
                        ResolveHandleFormId(followHandle),
                        ResolveHandleFormId(aiTargetHandle),
                        ResolveHandleFormId(headtrackHandle),
                        ResolveHandleFormId(pActor->dialogueHandle),
                        pExtension ? pExtension->LatestAnimation.ActionId : 0,
                        pExtension ? pExtension->LatestAnimation.TargetId : 0,
                        pExtension ? EscapeJson(pExtension->LatestAnimation.EventName.c_str()) : "",
                        pExtension ? pExtension->GraphDescriptorHash : 0,
                        JsonBool(nativeAnimation.GraphReady), nativeAnimation.StateId,
                        nativeAnimation.TimeInState,
                        JsonBool(nativeAnimation.CloneStateReadable),
                        nativeAnimation.CloneStateId,
                        std::isfinite(nativeAnimation.CloneTimeInState) ?
                            nativeAnimation.CloneTimeInState : 0.f,
                        nativeAnimation.PoseCount,
                        nativeAnimation.PoseChecksum, nativeAnimation.RenderBoneCount,
                        nativeAnimation.RenderBoneChecksum,
                        nativeAnimation.RenderWorldBoneChecksum,
                        nativeAnimation.GraphVariableCount,
                        nativeAnimation.GraphVariableChecksum);
                }
            }
        }
        snapshot += ']';

        markSnapshotPhase(5);
        // Probe one specified actor even when it is outside the player's
        // loaded cell. This is especially useful for scene aliases (for
        // example a dragon approaching from outside the normal audit radius).
        const auto targetActorId = m_poseProbeFormId.load(std::memory_order_acquire);
        auto* pTargetActor = targetActorId ?
            Cast<Actor>(TESForm::GetById(targetActorId)) : nullptr;
        if (pTargetActor)
        {
            const auto* pTargetCell = pTargetActor->GetParentCellEx();
            const auto* pExtension = pTargetActor->GetExtension();
            const auto* pPackage = pTargetActor->currentProcess ?
                pTargetActor->currentProcess->package : nullptr;
            snapshot += fmt::format(
                ",\"targetActor\":{{\"formId\":{},\"cellId\":{},"
                "\"cellAttached\":{},\"loaded\":{},\"has3D\":{},"
                "\"remote\":{},\"dead\":{},\"packageFormId\":{},"
                "\"position\":[{},{},{}],\"lastActionEvent\":\"{}\"}}",
                pTargetActor->formID, pTargetCell ? pTargetCell->formID : 0,
                JsonBool(pTargetCell && pTargetCell->IsAttached()),
                JsonBool(pTargetActor->loadedState != nullptr),
                JsonBool(pTargetActor->GetNiNode() != nullptr),
                JsonBool(pExtension && pExtension->IsRemote()),
                JsonBool(pTargetActor->IsDead()),
                pPackage ? pPackage->formID : 0,
                pTargetActor->position.x, pTargetActor->position.y,
                pTargetActor->position.z,
                pExtension ? EscapeJson(pExtension->LatestAnimation.EventName.c_str()) : "");
        }
        Set<std::string> watchedQuests;
        {
            std::scoped_lock lock(m_snapshotMutex);
            watchedQuests = m_watchedQuests;
        }
        snapshot += ",\"quests\":[";
        bool firstQuest = true;
        const PapyrusFunction<bool, BGSScene, uint32_t> isSceneActionComplete(
            m_world.ctx().at<PapyrusService>().Get("Scene", "IsActionComplete"));
        const bool actionCompletionReadable = isSceneActionComplete && GameVM::Get() &&
            GameVM::Get()->virtualMachine;
        uint32_t completionCalls = 0;
        if (auto* pModManager = ModManager::Get())
        {
            for (auto* pQuest : pModManager->quests)
            {
                if (!pQuest || !watchedQuests.contains(pQuest->idName.AsAscii()))
                    continue;
                if (!firstQuest)
                    snapshot += ',';
                firstQuest = false;
                snapshot += fmt::format(
                    "{{\"editorId\":\"{}\",\"formId\":{},\"currentStage\":{},\"flags\":{},"
                    "\"state\":{},\"enabled\":{},\"active\":{},\"stopped\":{},\"doneStages\":[",
                    EscapeJson(pQuest->idName.AsAscii()), pQuest->formID, pQuest->currentStage, pQuest->flags,
                    static_cast<uint8_t>(pQuest->getState()), JsonBool(pQuest->IsEnabled()),
                    JsonBool(pQuest->IsActive()), JsonBool(pQuest->IsStopped()));
                bool firstStage = true;
                for (auto* pStage : pQuest->stages)
                {
                    if (!pStage || !pStage->IsDone())
                        continue;
                    if (!firstStage)
                        snapshot += ',';
                    snapshot += fmt::format("{}", pStage->stageIndex);
                    firstStage = false;
                }
                snapshot += "],\"objectives\":[";
                bool firstObjective = true;
                size_t objectiveCount = 0;
                for (auto* pObjective : pQuest->objectives)
                {
                    if (!pObjective || objectiveCount++ >= 128)
                        break;
                    if (!firstObjective)
                        snapshot += ',';
                    firstObjective = false;
                    snapshot += fmt::format("{{\"stageId\":{},\"state\":{}}}",
                        pObjective->stageId, pObjective->state);
                }
                snapshot += "],\"scenes\":[";
                bool firstScene = true;
                const auto& sceneArray = pQuest->scenes;
                if (sceneArray.data && sceneArray.length <= 128 &&
                    sceneArray.length <= sceneArray.capacity &&
                    IsReadableRange(sceneArray.data, sizeof(BGSScene*) * sceneArray.length))
                {
                    static Map<uint32_t, uint64_t> sceneStates;
                    for (uint32_t sceneIndex = 0; sceneIndex < sceneArray.length; ++sceneIndex)
                    {
                        const auto* pScene = sceneArray.data[sceneIndex];
                        if (!pScene || !IsReadableRange(pScene, 0xC0))
                            continue;
                        const auto sceneState = (static_cast<uint64_t>(pScene->isPlaying) << 32) |
                            pScene->rawPhaseWord;
                        auto [it, inserted] = sceneStates.try_emplace(pScene->formID, sceneState);
                        if (!inserted && it->second != sceneState)
                        {
                            spdlog::info("Native scene transition quest={:X} scene={:X} playing={} rawPhase={} tick={} leader={}",
                                pQuest->formID, pScene->formID, pScene->isPlaying, pScene->rawPhaseWord,
                                now, party.IsLeader());
                            sceneStates.insert_or_assign(pScene->formID, sceneState);
                        }
                        if (!firstScene)
                            snapshot += ',';
                        firstScene = false;
                        snapshot += fmt::format(
                            "{{\"formId\":{},\"playing\":{},\"rawPhaseWord\":{},\"phaseCount\":{},\"phaseEligibleActions\":[",
                            pScene->formID, JsonBool(pScene->isPlaying), pScene->rawPhaseWord,
                            pScene->phases.length);
                        bool firstAction = true;
                        const auto& actions = pScene->actions;
                        if (pScene->isPlaying && pScene->rawPhaseWord < pScene->phases.length &&
                            actions.data && actions.length <= 128 && actions.length <= actions.capacity &&
                            IsReadableRange(actions.data, sizeof(void*) * actions.length))
                        {
                            for (uint32_t actionIndex = 0; actionIndex < actions.length; ++actionIndex)
                            {
                                SceneActionDiagnosticView action{};
                                if (!ReadNative(actions.data[actionIndex], action) ||
                                    action.StartPhase > pScene->rawPhaseWord ||
                                    action.EndPhase < pScene->rawPhaseWord)
                                    continue;
                                if (!firstAction)
                                    snapshot += ',';
                                firstAction = false;
                                // Only watched quests reach this bounded
                                // action loop; resolve their active aliases
                                // so a phase mismatch can identify the actor
                                // whose local scene action is holding it.
                                auto* pAliasRef = pQuest->GetAliasedRef(action.ActorId);
                                auto* pAliasActor = Cast<Actor>(pAliasRef);
                                const auto* pAliasExtension = pAliasActor ?
                                    pAliasActor->GetExtension() : nullptr;
                                const auto* pAliasPackage = pAliasActor &&
                                    pAliasActor->currentProcess ?
                                    pAliasActor->currentProcess->package : nullptr;
                                const bool queryCompletion = actionCompletionReadable &&
                                    completionCalls < 32;
                                const bool actionComplete = queryCompletion ?
                                    isSceneActionComplete(pScene, action.ActionId) : false;
                                completionCalls += queryCompletion ? 1u : 0u;
                                snapshot += fmt::format(
                                    "{{\"index\":{},\"actionId\":{},\"actorId\":{},"
                                    "\"startPhase\":{},\"endPhase\":{},\"flags\":{},"
                                    "\"actionComplete\":{},\"actionStatusByte\":{},"
                                    "\"aliasFormId\":{},\"aliasRemote\":{},\"aliasDead\":{},\"aliasDisabled\":{},"
                                    "\"aliasPackageFormId\":{}}}",
                                    actionIndex, action.ActionId, action.ActorId,
                                    action.StartPhase, action.EndPhase, action.Flags,
                                    queryCompletion ? JsonBool(actionComplete) : "null",
                                    action.Unknown14[0],
                                    pAliasRef ? pAliasRef->formID : 0,
                                    JsonBool(pAliasExtension && pAliasExtension->IsRemote()),
                                    JsonBool(pAliasActor && pAliasActor->IsDead()),
                                    JsonBool(pAliasRef && pAliasRef->IsDisabled()),
                                    pAliasPackage ? pAliasPackage->formID : 0);
                            }
                        }
                        snapshot += "]}";
                    }
                }
                snapshot += "]}";
            }
        }
        snapshot += ']';

        snapshot += ",\"questEvents\":[";
        bool firstEvent = true;
        for (const auto& event : m_world.GetQuestService().GetRecentDebugEvents())
        {
            if (!firstEvent)
                snapshot += ',';
            snapshot += fmt::format(
                "{{\"sequence\":{},\"timeMs\":{},\"kind\":\"{}\",\"formId\":{},\"stage\":{},"
                "\"scopedOverride\":{},\"inParty\":{},\"leader\":{}}}",
                event.Sequence, event.TimeMs, EscapeJson(event.Kind.c_str()), event.FormId, event.Stage,
                JsonBool(event.ScopedOverride), JsonBool(event.InParty), JsonBool(event.Leader));
            firstEvent = false;
        }
        markSnapshotPhase(6);
        snapshot += ']';
        std::array<TriggerDiagnostic, 64> triggerEvents{};
        size_t triggerEventCount = 0;
        {
            std::lock_guard lock(m_triggerMutex);
            triggerEventCount = m_triggerCount;
            const size_t start = (m_triggerNext + m_triggerEvents.size() -
                m_triggerCount) % m_triggerEvents.size();
            for (size_t i = 0; i < triggerEventCount; ++i)
                triggerEvents[i] = m_triggerEvents[(start + i) % m_triggerEvents.size()];
        }
        snapshot += ",\"triggerEvents\":[";
        for (size_t i = 0; i < triggerEventCount; ++i)
        {
            const auto& event = triggerEvents[i];
            if (i != 0)
                snapshot += ',';
            snapshot += fmt::format(
                "{{\"sequence\":{},\"timeMs\":{},\"enter\":{},"
                "\"triggerFormId\":{},\"actorFormId\":{}}}",
                event.Sequence, event.TimeMs, JsonBool(event.Enter),
                event.TriggerFormId, event.ActorFormId);
        }
        snapshot += ']';
        snapshot += fmt::format(
            ",\"snapshotProfileUs\":{{\"setup\":{},\"actorPose\":{},"
            "\"referencesAndScenes\":{},\"cartAndTrace\":{},"
            "\"networkActors\":{},\"nearbyActors\":{},\"quests\":{}}}",
            snapshotPhaseUs[0], snapshotPhaseUs[1], snapshotPhaseUs[2],
            snapshotPhaseUs[3], snapshotPhaseUs[4], snapshotPhaseUs[5],
            snapshotPhaseUs[6]);
        snapshot += '}';

        std::scoped_lock lock(m_snapshotMutex);
        m_gameSnapshot = std::move(snapshot);
        m_gameSnapshotTimeMs = now;
        if (capturePose)
            m_lastPoseSnapshot = m_gameSnapshot;
        m_recentGameSnapshots.push_back({worldTick, now, m_gameSnapshot});
        while (m_recentGameSnapshots.size() > 80)
            m_recentGameSnapshots.pop_front();
    }
    catch (const std::exception& exception)
    {
        spdlog::warn("Game test snapshot failed: {}", exception.what());
    }
    catch (...)
    {
        spdlog::warn("Game test snapshot failed with native exception");
    }
}

std::string GameTestService::GetCachedGameSnapshot() const noexcept
{
    std::scoped_lock lock(m_snapshotMutex);
    return m_gameSnapshot;
}

std::string GameTestService::Execute(const std::string& acLine) noexcept
{
    const uint64_t id = GetJsonId(acLine);
    const auto command = GetJsonString(acLine, "command");
    try
    {
        if (command == "ping")
            return Result(id, fmt::format("\"pid\":{},\"protocol\":2", GetCurrentProcessId()));

        if (command == "capabilities")
            return Result(id, "\"protocol\":2,\"commands\":[\"ping\",\"capabilities\",\"snapshot\","
                "\"game_snapshot\",\"hitch_snapshot\",\"request_game_snapshot\",\"cancel_game_snapshot\",\"game_snapshot_at\",\"game_pose_snapshot\",\"watch_quest\",\"set_pose_probe\",\"set_pose_probe_actor\",\"test_displace_remote_corpse\",\"capture_bundle\",\"screenshot\",\"open_options\","
                "\"set_visual_pose_apply\",\"set_visual_root_diagnostic\",\"set_native_vehicle_trial\",\"set_remote_process_trial\",\"set_presentation_delay\",\"set_camera_position_probe\",\"set_skip_next_post_respawn_knock\","
                "\"open_coop\",\"party_state\",\"set_session_open\",\"join_friend\",\"set_ready\",\"start_new_campaign\",\"start_continue_campaign\",\"create_test_checkpoint\",\"test_checkpoint_status\","
                "\"close_options\",\"controller\",\"race_menu_key\",\"gameplay_key\",\"race_menu_state\",\"confirm_character_native\",\"confirm_character_native_status\","
                "\"toggle_window\",\"confirm_display\",\"setting\"]");

        if (command == "watch_quest")
        {
            const auto editorId = GetJsonString(acLine, "editorId");
            if (editorId.empty())
                return Error(id, "editorId is required");
            std::scoped_lock lock(m_snapshotMutex);
            m_watchedQuests.insert(editorId);
            return Result(id, fmt::format("\"editorId\":\"{}\",\"watched\":true", EscapeJson(editorId)));
        }

        if (command == "request_game_snapshot")
        {
            const auto tickText = GetJsonString(acLine, "tick");
            const auto delayText = GetJsonString(acLine, "delay_ms");
            if ((!tickText.empty() &&
                    tickText.find_first_not_of("0123456789") != std::string::npos) ||
                (!delayText.empty() &&
                    delayText.find_first_not_of("0123456789") != std::string::npos))
                return Error(id, "tick and delay_ms must be decimal strings");
            const auto delay = delayText.empty() ? uint64_t{1} :
                std::strtoull(delayText.c_str(), nullptr, 10);
            if (delay > 30000)
                return Error(id, "delay_ms exceeds 30000");
            uint64_t nowTick{};
            uint64_t target{};
            {
                std::scoped_lock lock(m_snapshotScheduleMutex);
                nowTick = m_world.GetTick();
                target = tickText.empty() ? nowTick + delay :
                    std::strtoull(tickText.c_str(), nullptr, 10);
                if (!target)
                    return Error(id, "target tick must be nonzero");
                if (m_snapshotRelativeDueWallMs || m_snapshotTargetTick)
                    return Error(id, fmt::format(
                        "snapshot already scheduled at tick {}", m_snapshotTargetTick));
                m_snapshotTargetAuthorityEpoch =
                    m_world.GetTransport().GetAuthorityEpoch();
                m_snapshotTargetTick = target;
                if (tickText.empty())
                    m_snapshotRelativeDueWallMs = GetTickCount64() + delay;
            }
            return Result(id, fmt::format("\"currentTick\":{},\"targetTick\":{},\"relative\":{}",
                nowTick, target, JsonBool(tickText.empty())));
        }
        if (command == "cancel_game_snapshot")
        {
            std::scoped_lock lock(m_snapshotScheduleMutex);
            const bool hadPending = m_snapshotRelativeDueWallMs || m_snapshotTargetTick;
            m_snapshotRelativeDueWallMs = 0;
            m_snapshotTargetTick = 0;
            return Result(id, fmt::format("\"cancelled\":{}", JsonBool(hadPending)));
        }
        if (command == "game_snapshot")
        {
            bool pending{};
            bool relative{};
            {
                std::scoped_lock scheduleLock(m_snapshotScheduleMutex);
                pending = m_snapshotRelativeDueWallMs || m_snapshotTargetTick;
                relative = m_snapshotRelativeDueWallMs != 0;
            }
            std::scoped_lock lock(m_snapshotMutex);
            const auto age = m_gameSnapshotTimeMs ? GetTickCount64() - m_gameSnapshotTimeMs : 0;
            return Result(id, fmt::format("\"ageMs\":{},\"capturePending\":{},\"relative\":{},\"game\":{}",
                age, JsonBool(pending), JsonBool(relative), m_gameSnapshot));
        }
        if (command == "hitch_snapshot")
        {
            std::scoped_lock lock(m_snapshotMutex);
            if (m_hitchSnapshot == "null")
                return Error(id, "no hitch snapshot available");
            return Result(id, fmt::format("\"game\":{}", m_hitchSnapshot));
        }
        if (command == "game_pose_snapshot")
        {
            std::scoped_lock lock(m_snapshotMutex);
            if (m_lastPoseSnapshot == "null")
                return Error(id, "no pose snapshot available");
            return Result(id, fmt::format("\"game\":{}", m_lastPoseSnapshot));
        }
        if (command == "game_snapshot_at")
        {
            const auto tickText = GetJsonString(acLine, "tick");
            if (tickText.empty() || tickText.find_first_not_of("0123456789") != std::string::npos)
                return Error(id, "tick must be a decimal string");
            const auto targetTick = std::strtoull(tickText.c_str(), nullptr, 10);
            std::scoped_lock lock(m_snapshotMutex);
            if (m_recentGameSnapshots.empty())
                return Error(id, "no game snapshots available");
            const auto nearest = std::min_element(m_recentGameSnapshots.begin(),
                m_recentGameSnapshots.end(), [targetTick](const auto& left, const auto& right)
                {
                    const auto leftError = left.WorldTick > targetTick ?
                        left.WorldTick - targetTick : targetTick - left.WorldTick;
                    const auto rightError = right.WorldTick > targetTick ?
                        right.WorldTick - targetTick : targetTick - right.WorldTick;
                    return leftError < rightError;
                });
            const auto errorMs = nearest->WorldTick > targetTick ?
                nearest->WorldTick - targetTick : targetTick - nearest->WorldTick;
            if (errorMs > 200)
                return Error(id, "no snapshot within 200 ms of requested tick");
            return Result(id, fmt::format(
                "\"requestedTick\":{},\"sampleTick\":{},\"errorMs\":{},\"game\":{}",
                targetTick, nearest->WorldTick, errorMs, nearest->Json));
        }
        if (command == "set_pose_probe")
        {
            const bool enabled = GetJsonString(acLine, "enabled") == "true";
            uint64_t targetTick = 0;
            if (enabled)
            {
                const auto tickText = GetJsonString(acLine, "tick");
                if (!tickText.empty() && tickText.find_first_not_of("0123456789") != std::string::npos)
                    return Error(id, "tick must be a decimal string");
                targetTick = tickText.empty() ? 1 : std::strtoull(tickText.c_str(), nullptr, 10);
                if (targetTick == 0)
                    return Error(id, "tick must be nonzero when enabling pose capture");
            }
            m_poseProbeTargetTick.store(targetTick, std::memory_order_relaxed);
            return Result(id, fmt::format("\"enabled\":{},\"targetTick\":{}",
                JsonBool(enabled), targetTick));
        }

        if (command == "set_pose_probe_actor")
        {
            const auto formText = GetJsonString(acLine, "form_id");
            if (formText.empty() ||
                formText.find_first_not_of("0123456789") != std::string::npos)
                return Error(id, "form_id must be a decimal string; zero restores ambient capture");
            const auto formId = std::strtoull(formText.c_str(), nullptr, 10);
            if (formId > std::numeric_limits<uint32_t>::max())
                return Error(id, "form_id exceeds uint32 range");
            m_poseProbeFormId.store(static_cast<uint32_t>(formId),
                std::memory_order_release);
            return Result(id, fmt::format("\"formId\":{}", formId));
        }

        if (command == "test_displace_remote_corpse")
        {
            const auto formText = GetJsonString(acLine, "form_id");
            if (formText.empty() ||
                formText.find_first_not_of("0123456789") != std::string::npos)
                return Error(id, "form_id must be a nonzero decimal string");
            const auto formId = std::strtoull(formText.c_str(), nullptr, 10);
            if (!formId || formId > std::numeric_limits<uint32_t>::max())
                return Error(id, "form_id out of range");
            uint32_t pending = 0;
            if (!m_testCorpseDisplaceFormId.compare_exchange_strong(pending,
                    static_cast<uint32_t>(formId), std::memory_order_acq_rel))
                return Error(id, "corpse displacement test already pending");
            return Result(id, fmt::format("\"queuedFormId\":{}", formId));
        }

        if (command == "set_visual_pose_apply")
        {
            const auto enabledText = GetJsonString(acLine, "enabled");
            if (enabledText != "true" && enabledText != "false")
                return Error(id, "enabled must be the string true or false");
            VisualPoseMailbox::SetApplyEnabled(enabledText == "true");
            return Result(id, fmt::format("\"enabled\":{}",
                JsonBool(VisualPoseMailbox::IsApplyEnabled())));
        }

        if (command == "set_visual_pose_apply_form")
        {
            const auto formText = GetJsonString(acLine, "form_id");
            if (formText.empty() ||
                formText.find_first_not_of("0123456789") != std::string::npos)
                return Error(id, "form_id must be a decimal form ID; zero disables selection");
            const auto formId = std::strtoull(formText.c_str(), nullptr, 10);
            if (formId > std::numeric_limits<uint32_t>::max())
                return Error(id, "form_id exceeds uint32 range");
            VisualPoseMailbox::SetApplyFormId(static_cast<uint32_t>(formId));
            return Result(id, fmt::format("\"formId\":{}", formId));
        }

        if (command == "set_native_vehicle_trial")
        {
            const auto riderText = GetJsonString(acLine, "rider_id");
            if (riderText.empty() ||
                riderText.find_first_not_of("0123456789") != std::string::npos)
                return Error(id, "rider_id must be a decimal server ID; zero disables the trial");
            const auto riderId = std::strtoull(riderText.c_str(), nullptr, 10);
            if (riderId > std::numeric_limits<uint32_t>::max())
                return Error(id, "rider_id exceeds uint32 range");
            m_world.GetCharacterService().SetVehicleTrialRiderId(
                static_cast<uint32_t>(riderId));
            return Result(id, fmt::format("\"riderId\":{}", riderId));
        }

        if (command == "set_combat_target_trial")
        {
            const auto actorText = GetJsonString(acLine, "actor_id");
            const auto targetText = GetJsonString(acLine, "target_id");
            const auto valid = [](const std::string& value) {
                return !value.empty() &&
                    value.find_first_not_of("0123456789") == std::string::npos;
            };
            if (!valid(actorText) || !valid(targetText))
                return Error(id, "actor_id and target_id must be decimal server IDs");
            const auto actorId = std::strtoull(actorText.c_str(), nullptr, 10);
            const auto targetId = std::strtoull(targetText.c_str(), nullptr, 10);
            if (actorId > std::numeric_limits<uint32_t>::max() ||
                targetId > std::numeric_limits<uint32_t>::max() || !actorId)
                return Error(id, "actor_id is invalid or a form ID exceeds uint32 range");
            auto* pActor = Utils::GetByServerId<Actor>(
                static_cast<uint32_t>(actorId));
            if (!pActor || !Utils::GetLocalOwnershipToken(pActor->formID) ||
                !pActor->pCombatController)
                return Error(id, "actor is not locally owned or has no combat controller");
            auto* pTarget = targetId ? Utils::GetByServerId<Actor>(
                static_cast<uint32_t>(targetId)) : nullptr;
            if (targetId && !pTarget)
                return Error(id, "target actor is not loaded");
            pActor->SetCombatTargetEx(pTarget);
            return Result(id, fmt::format("\"actorId\":{},\"targetId\":{},"
                "\"actorFormId\":{},\"targetFormId\":{}",
                actorId, targetId, pActor->formID,
                pTarget ? pTarget->formID : 0));
        }

        if (command == "set_combat_target_authority_trial")
        {
            const auto actorText = GetJsonString(acLine, "actor_form_id");
            if (actorText.empty() ||
                actorText.find_first_not_of("0123456789") != std::string::npos)
                return Error(id, "actor_form_id must be a decimal form ID; zero disables");
            const auto actorId = std::strtoull(actorText.c_str(), nullptr, 10);
            if (actorId > std::numeric_limits<uint32_t>::max())
                return Error(id, "actor_form_id exceeds uint32 range");
            CombatController::SetTargetAuthorityTrialActor(
                static_cast<uint32_t>(actorId));
            return Result(id, fmt::format("\"actorFormId\":{}", actorId));
        }

        if (command == "set_remote_process_trial")
        {
            const auto riderText = GetJsonString(acLine, "rider_form_id");
            const auto mountText = GetJsonString(acLine, "mount_form_id");
            const auto valid = [](const std::string& value) {
                return !value.empty() &&
                    value.find_first_not_of("0123456789") == std::string::npos;
            };
            if (!valid(riderText) || !valid(mountText))
                return Error(id, "rider_form_id and mount_form_id must be decimal strings; zero disables");
            const auto riderId = std::strtoull(riderText.c_str(), nullptr, 10);
            const auto mountId = std::strtoull(mountText.c_str(), nullptr, 10);
            if (riderId > std::numeric_limits<uint32_t>::max() ||
                mountId > std::numeric_limits<uint32_t>::max())
                return Error(id, "form ID exceeds uint32 range");
            Actor::SetRemoteProcessTrial(static_cast<uint32_t>(riderId),
                static_cast<uint32_t>(mountId));
            return Result(id, fmt::format("\"riderFormId\":{},\"mountFormId\":{}",
                riderId, mountId));
        }

        if (command == "set_visual_root_diagnostic")
        {
            const auto formText = GetJsonString(acLine, "form_id");
            if (formText.empty() ||
                formText.find_first_not_of("0123456789") != std::string::npos)
                return Error(id, "form_id must be a decimal string; zero disables the diagnostic");
            const auto formId = std::strtoull(formText.c_str(), nullptr, 10);
            if (formId > std::numeric_limits<uint32_t>::max())
                return Error(id, "form_id exceeds uint32 range");
            VisualPoseMailbox::SetRootDiagnosticFormId(static_cast<uint32_t>(formId));
            return Result(id, fmt::format("\"formId\":{}", formId));
        }

        if (command == "set_presentation_delay")
        {
            const auto delayText = GetJsonString(acLine, "delay_ms");
            if (delayText.empty() ||
                delayText.find_first_not_of("0123456789") != std::string::npos)
                return Error(id, "delay_ms must be a decimal string (50-500); zero restores 300");
            const auto requested = std::strtoull(delayText.c_str(), nullptr, 10);
            const auto delay = requested == 0 ? 300 : requested;
            if (delay < 50 || delay > 500)
                return Error(id, "delay_ms must be 50-500 or zero to restore 300");
            m_world.GetCharacterService().SetPresentationDelayMs(
                static_cast<uint32_t>(delay));
            return Result(id, fmt::format("\"delayMs\":{}", delay));
        }

        if (command == "set_camera_position_probe")
        {
            const auto enabledText = GetJsonString(acLine, "enabled");
            if (enabledText != "true" && enabledText != "false")
                return Error(id, "enabled must be the string true or false");
            auto& cameraService = m_world.ctx().at<CameraService>();
            cameraService.SetPositionProbeEnabled(enabledText == "true");
            return Result(id, fmt::format("\"enabled\":{}",
                JsonBool(cameraService.IsPositionProbeEnabled())));
        }

        if (command == "set_skip_next_post_respawn_knock")
        {
            const auto enabledText = GetJsonString(acLine, "enabled");
            if (enabledText != "true" && enabledText != "false")
                return Error(id, "enabled must be the string true or false");
            auto& playerService = m_world.ctx().at<PlayerService>();
            playerService.SetSkipNextPostRespawnKnock(enabledText == "true");
            return Result(id, fmt::format("\"enabled\":{}",
                JsonBool(playerService.GetDeathDiagnostic().SkipNextPostRespawnKnock)));
        }


        if (command == "set_body_playback_probe")
        {
            const auto formText = GetJsonString(acLine, "form_id");
            if (formText.empty() ||
                formText.find_first_not_of("0123456789") != std::string::npos)
                return Error(id, "form_id must be a decimal string; zero disables the probe");
            const auto formId = std::strtoull(formText.c_str(), nullptr, 10);
            if (formId > std::numeric_limits<uint32_t>::max())
                return Error(id, "form_id exceeds uint32 range");
            ObjectService::SetBodyPlaybackProbe(static_cast<uint32_t>(formId));
            return Result(id, fmt::format("\"selectedFormId\":{}",
                ObjectService::GetBodyPlaybackDiagnostic().SelectedFormId));
        }

        if (command == "set_pre_step_body_playback_probe")
        {
            const auto formText = GetJsonString(acLine, "form_id");
            if (formText.empty() ||
                formText.find_first_not_of("0123456789") != std::string::npos)
                return Error(id, "form_id must be a decimal string; zero disables the probe");
            const auto formId = std::strtoull(formText.c_str(), nullptr, 10);
            if (formId > std::numeric_limits<uint32_t>::max())
                return Error(id, "form_id exceeds uint32 range");
            const auto modeText = GetJsonString(acLine, "mode");
            if (formId && !modeText.empty() && modeText != "velocity" &&
                modeText != "pose" && modeText != "kinematic" &&
                modeText != "hard_kinematic")
                return Error(id, "mode must be velocity, pose, kinematic, or hard_kinematic");
            const uint32_t mode = modeText == "hard_kinematic" ? 4 :
                modeText == "kinematic" ? 3 :
                (modeText == "pose" ? 2 : 1);
            ObjectService::SetPreStepBodyPlaybackProbe(
                static_cast<uint32_t>(formId), mode);
            const auto diagnostic = ObjectService::GetPreStepPlaybackDiagnostic();
            return Result(id, fmt::format("\"selectedFormId\":{},\"mode\":{}",
                diagnostic.SelectedFormId, diagnostic.Mode));
        }

        if (command == "set_reference_phase_probe")
        {
            const auto formText = GetJsonString(acLine, "form_id");
            if (formText.empty() ||
                formText.find_first_not_of("0123456789") != std::string::npos)
                return Error(id, "form_id must be a decimal string; zero disables the probe");
            const auto formId = std::strtoull(formText.c_str(), nullptr, 10);
            if (formId > std::numeric_limits<uint32_t>::max())
                return Error(id, "form_id exceeds uint32 range");
            ObjectService::SetReferencePhaseProbe(static_cast<uint32_t>(formId));
            return Result(id, fmt::format("\"selectedFormId\":{}",
                ObjectService::GetReferencePhaseDiagnostic().SelectedFormId));
        }

        if (command == "native_reference_address")
        {
            const auto formText = GetJsonString(acLine, "form_id");
            if (formText.empty() ||
                formText.find_first_not_of("0123456789") != std::string::npos)
                return Error(id, "form_id must be a decimal string");
            const auto formId = std::strtoull(formText.c_str(), nullptr, 10);
            if (formId == 0 || formId > std::numeric_limits<uint32_t>::max())
                return Error(id, "form_id out of range");
            auto* pReference = Cast<TESObjectREFR>(TESForm::GetById(
                static_cast<uint32_t>(formId)));
            if (!pReference || !pReference->loadedState)
                return Error(id, "reference unavailable or not loaded");
            return Result(id, fmt::format(
                "\"processId\":{},\"formId\":{},"
                "\"referenceAddress\":\"{}\","
                "\"positionXAddress\":\"{}\"",
                GetCurrentProcessId(), pReference->formID,
                reinterpret_cast<uintptr_t>(pReference),
                reinterpret_cast<uintptr_t>(&pReference->position.x)));
        }

        if (command == "capture_bundle")
        {
            const auto path = m_world.GetGameSettingsService().CaptureTestScreenshot();
            if (path.empty())
                return Error(id, "screenshot failed");
            const auto statePath = path.parent_path() / (path.stem().string() + ".game.json");
            std::ofstream state(statePath, std::ios::binary);
            state << GetCachedGameSnapshot();
            if (!state)
                return Error(id, "game snapshot write failed");
            return Result(id, fmt::format("\"screenshotPath\":\"{}\",\"gameStatePath\":\"{}\"",
                EscapeJson(path.string()), EscapeJson(statePath.string())));
        }

        if (command == "open_options")
        {
            auto& overlay = m_world.GetOverlayService();
            overlay.SetActive(true);
            if (auto* pApp = overlay.GetOverlayApp())
                pApp->ExecuteAsync("showTitleOptions");
            m_world.GetGameSettingsService().RequestSettings();
            return Result(id, "\"action\":\"open_options\"");
        }
        if (command == "open_coop")
        {
            auto& overlay = m_world.GetOverlayService();
            overlay.SetActive(true);
            if (auto* pApp = overlay.GetOverlayApp())
                pApp->ExecuteAsync("showTitleLobby");
            m_world.GetSteamLobbyService().QueueRefreshLobbyState();
            return Result(id, "\"action\":\"open_coop\"");
        }
        if (command == "party_state")
        {
            const auto& party = m_world.GetPartyService();
            return Result(id, fmt::format(
                "\"inParty\":{},\"leader\":{},\"memberCount\":{},\"readyCount\":{},\"sessionState\":{},\"startEpoch\":{}",
                JsonBool(party.IsInParty()), JsonBool(party.IsLeader()), party.GetPartyMembers().size(),
                party.GetReadyPlayerCount(), party.GetSessionState(), party.GetStartEpoch()));
        }
        if (command == "create_test_checkpoint")
            return Error(id, "direct save is disabled after a paired cinematic hang; use gameplay_key quicksave after control handoff");
        if (command == "test_checkpoint_status")
            return Error(id, "direct save is disabled after a paired cinematic hang");
        // Every physics body in a reference's 3D tree (carts: body, wheels, harness...).
        // Testing ground setup: leave the intro on this PC. Stops MQ101 (0003372B) and undoes what it
        // leaves on the player: controls disabled, character-creation mode (no saving), AI driven,
        // restrained. Papyrus natives called with their (VM, stack, self or static tag, args) form.
        if (command == "skip_intro")
        {
            struct Game
            {
            };
            using Quest = TESQuest;
            auto* pPlayer = PlayerCharacter::Get();
            auto* pIntro = Cast<TESQuest>(TESForm::GetById(0x0003372B));
            if (!pPlayer)
                return Error(id, "player not found");
            // Each native only if it was found by name (a missing one was a call to address 0).
            std::string missing;
            PAPYRUS_FUNCTION(void, Quest, Stop);
            if (!s_pStop)
                missing += "Quest.Stop ";
            else if (pIntro)
                s_pStop(pIntro);
            PAPYRUS_FUNCTION(void, Game, SetInChargen, bool, bool, bool);
            if (!s_pSetInChargen)
                missing += "Game.SetInChargen ";
            else
                s_pSetInChargen(nullptr, false, false, false);
            PAPYRUS_FUNCTION(void, Game, EnablePlayerControls, bool, bool, bool, bool, bool, bool, bool, bool, int32_t);
            if (!s_pEnablePlayerControls)
                missing += "Game.EnablePlayerControls ";
            else
                s_pEnablePlayerControls(nullptr, true, true, true, true, true, true, true, true, 0);
            PAPYRUS_FUNCTION(void, Game, SetPlayerAIDriven, bool);
            if (!s_pSetPlayerAIDriven)
                missing += "Game.SetPlayerAIDriven ";
            else
                s_pSetPlayerAIDriven(nullptr, false);
            PAPYRUS_FUNCTION(void, Actor, SetRestrained, bool);
            if (!s_pSetRestrained)
                missing += "Actor.SetRestrained ";
            else
                s_pSetRestrained(pPlayer, false);
            return Result(id, fmt::format("\"introStopped\":{},\"missing\":\"{}\"", JsonBool(pIntro != nullptr), missing));
        }
        // Testing ground (docs: C:\Tools\skyrim_re\testground.ps1). Move this PC's player to a
        // persistent reference (a map marker), offset sideways so players do not overlap.
        if (command == "teleport_player")
        {
            // A worldspace and a position (the marker's, read from the plugin): the marker reference
            // itself is only loaded while its cell is.
            const auto worldText = GetJsonString(acLine, "world");
            const auto offsetText = GetJsonString(acLine, "offset");
            auto* pWorldSpace = worldText.empty() ? nullptr : Cast<TESWorldSpace>(TESForm::GetById(std::stoul(worldText, nullptr, 16)));
            auto* pPlayer = PlayerCharacter::Get();
            if (!pWorldSpace || !pPlayer || GetJsonString(acLine, "x").empty())
                return Error(id, "worldspace, position or player missing");
            NiPoint3 target{};
            target.x = std::stof(GetJsonString(acLine, "x")) + (offsetText.empty() ? 0.f : std::stof(offsetText));
            target.y = std::stof(GetJsonString(acLine, "y"));
            target.z = std::stof(GetJsonString(acLine, "z"));
            auto* pCell = ModManager::Get()->GetCellFromCoordinates(static_cast<int32_t>(std::floor(target.x / 4096.f)),
                static_cast<int32_t>(std::floor(target.y / 4096.f)), pWorldSpace, true);
            if (!pCell)
                return Error(id, "exterior cell not found");
            pPlayer->MoveTo(pCell, target);
            return Result(id, fmt::format("\"cell\":\"{:X}\",\"x\":{:.0f},\"y\":{:.0f},\"z\":{:.0f}", pCell->formID,
                target.x, target.y, target.z));
        }
        // Place an actor (a base form) in front of this PC's player.
        if (command == "spawn_actor")
        {
            const auto baseText = GetJsonString(acLine, "base");
            const auto distanceText = GetJsonString(acLine, "distance");
            auto* pBase = baseText.empty() ? nullptr : TESForm::GetById(std::stoul(baseText, nullptr, 16));
            auto* pPlayer = PlayerCharacter::Get();
            if (!pBase || !pPlayer)
                return Error(id, "base form or player not found");
            using ObjectReference = TESObjectREFR;
            PAPYRUS_FUNCTION(TESObjectREFR*, ObjectReference, PlaceAtMe, TESForm*, int32_t, bool, bool);
            auto* pPlaced = s_pPlaceAtMe(pPlayer, pBase, 1, false, false);
            if (!pPlaced)
                return Error(id, "PlaceAtMe returned nothing");
            const float distance = distanceText.empty() ? 300.f : std::stof(distanceText);
            NiPoint3 target = pPlayer->position;
            target.x += std::sin(pPlayer->rotation.z) * distance;
            target.y += std::cos(pPlayer->rotation.z) * distance;
            pPlaced->MoveTo(pPlayer->parentCell, target);
            return Result(id, fmt::format("\"form_id\":\"{:X}\"", pPlaced->formID));
        }
        // Knock an actor away from this PC's player (so it ragdolls), then kill it.
        if (command == "kill_actor")
        {
            const auto form = GetJsonString(acLine, "form_id");
            const auto pushText = GetJsonString(acLine, "push");
            auto* pActor = form.empty() ? nullptr : Cast<Actor>(TESForm::GetById(std::stoul(form, nullptr, 16)));
            auto* pPlayer = PlayerCharacter::Get();
            if (!pActor || !pPlayer || !pActor->currentProcess)
                return Error(id, "actor not found");
            const float push = pushText.empty() ? 20.f : std::stof(pushText);
            pActor->currentProcess->KnockExplosion(pActor, &pPlayer->position, push);
            pActor->Kill();
            return Result(id, fmt::format("\"push\":{}", push));
        }
        // An actor's state for comparing the PCs: life and knock state, position, ragdoll bodies, worn items.
        if (command == "actor_state")
        {
            const auto form = GetJsonString(acLine, "form_id");
            auto* pActor = form.empty() ? nullptr : Cast<Actor>(TESForm::GetById(std::stoul(form, nullptr, 16)));
            // Or the nearest non-player actor within 500 units of x,y,z (temporary actors have
            // different form ids on each PC).
            if (!pActor && !GetJsonString(acLine, "x").empty())
            {
                const float x = std::stof(GetJsonString(acLine, "x")), y = std::stof(GetJsonString(acLine, "y")),
                            z = std::stof(GetJsonString(acLine, "z"));
                float best = 500.f;
                auto view = m_world.view<FormIdComponent>();
                for (auto entity : view)
                {
                    auto* pCandidate = Cast<Actor>(TESForm::GetById(view.get<FormIdComponent>(entity).Id));
                    if (!pCandidate || !pCandidate->GetExtension() || pCandidate->GetExtension()->IsPlayer())
                        continue;
                    const float dx = pCandidate->position.x - x, dy = pCandidate->position.y - y, dz = pCandidate->position.z - z;
                    const float distance = std::sqrt(dx * dx + dy * dy + dz * dz);
                    if (distance < best)
                    {
                        best = distance;
                        pActor = pCandidate;
                    }
                }
            }
            if (!pActor)
                return Error(id, "actor not found");
            const uint32_t flags1 = pActor->actorState.flags1;
            std::string worn = "[";
            int wornCount = 0;
            for (const auto& entry : pActor->GetActorInventory().Entries)
            {
                if (!entry.IsWorn())
                    continue;
                worn += fmt::format("{}\"{:X}:{:X}\"", wornCount++ ? "," : "", entry.BaseId.ModId, entry.BaseId.BaseId);
            }
            worn += "]";
            const auto* pRoot = pActor->GetNiNode();
            return Result(id, fmt::format("\"form_id\":\"{:X}\",\"dead\":{},\"lifeState\":{},\"knockState\":{},\"position\":[{:.1f},{:.1f},{:.1f}],"
                "\"has3D\":{},\"bodies\":{},\"worn\":{}", pActor->formID, JsonBool(pActor->IsDead()), (flags1 >> 21) & 0xF, (flags1 >> 25) & 0x7,
                pActor->position.x, pActor->position.y, pActor->position.z, JsonBool(pRoot != nullptr),
                CorpseRagdollService::DescribeRagdollBodies(pActor), worn));
        }
        if (command == "ref_bodies")
        {
            const auto form = GetJsonString(acLine, "form_id");
            auto* pRef = form.empty() ? nullptr : Cast<TESObjectREFR>(TESForm::GetById(std::stoul(form, nullptr, 16)));
            if (!pRef || !pRef->GetNiNode())
                return Error(id, "reference or 3D not found");
            std::string bodies = "[";
            int emitted = 0;
            std::function<void(NiAVObject*, int)> walk = [&](NiAVObject* apNode, int aDepth)
            {
                if (!apNode || aDepth > 8 || emitted > 64)
                    return;
                const char* pName = *reinterpret_cast<const char* const*>(reinterpret_cast<const uint8_t*>(apNode) + 0x10);
                if (apNode->collisionObject)
                {
                    void* pWrapper{};
                    void* pBody{};
                    ActorPoseDiagnosticViews::RigidBody state{};
                    const bool readable = ReadNative(reinterpret_cast<const uint8_t*>(apNode->collisionObject) + 0x20, pWrapper) && pWrapper &&
                        ReadNative(reinterpret_cast<const uint8_t*>(pWrapper) + 0x10, pBody) && pBody && ReadNative(pBody, state);
                    const auto& lr = apNode->local.rotate.entry;
                    const auto& wr = apNode->world.rotate.entry;
                    bodies += fmt::format("{}{{\"name\":\"{}\",\"depth\":{},\"readable\":{},\"motionType\":{},\"body\":[{},{},{}],\"node\":[{},{},{}],"
                        "\"localRot\":[{:.4f},{:.4f},{:.4f},{:.4f}],\"worldRot\":[{:.4f},{:.4f},{:.4f},{:.4f}],\"bodyRot\":[{:.4f},{:.4f},{:.4f},{:.4f}]}}",
                        emitted++ ? "," : "", EscapeJson(pName ? pName : ""), aDepth, JsonBool(readable), readable ? state.motionType : -1,
                        state.transform[12] * 70.f, state.transform[13] * 70.f, state.transform[14] * 70.f, apNode->world.translate.x,
                        apNode->world.translate.y, apNode->world.translate.z, lr[0][0], lr[0][1], lr[1][0], lr[2][2], wr[0][0], wr[0][1],
                        wr[1][0], wr[2][2], state.transform[0], state.transform[1], state.transform[4], state.transform[10]);
                }
                if (auto* pAsNode = apNode->AsNode())
                {
                    for (uint16_t i = 0; i < pAsNode->children.length; ++i)
                        walk(pAsNode->children.data[i], aDepth + 1);
                }
            };
            walk(pRef->GetNiNode(), 0);
            bodies += "]";
            return Result(id, fmt::format("\"bodies\":{}", bodies));
        }
        // An actor's 3D root and its parent chain (name, owning reference, world position).
        if (command == "actor_3d_parent")
        {
            const auto form = GetJsonString(acLine, "form_id");
            auto* pActor = form.empty() ? nullptr : Cast<Actor>(TESForm::GetById(std::stoul(form, nullptr, 16)));
            if (!pActor || !pActor->GetNiNode())
                return Error(id, "actor or 3D not found");
            std::string chain = "[";
            const NiAVObject* pNode = pActor->GetNiNode();
            for (int depth = 0; pNode && depth < 12; ++depth, pNode = pNode->parent)
            {
                const auto* pOwner = static_cast<const TESObjectREFR*>(pNode->userData);
                // NiObjectNET::name is the BSFixedString at +0x10.
                const char* pName = *reinterpret_cast<const char* const*>(reinterpret_cast<const uint8_t*>(pNode) + 0x10);
                chain += fmt::format("{}{{\"name\":\"{}\",\"owner\":{},\"world\":[{},{},{}]}}", depth ? "," : "",
                    EscapeJson(pName ? pName : ""), pOwner ? pOwner->formID : 0,
                    pNode->world.translate.x, pNode->world.translate.y, pNode->world.translate.z);
            }
            chain += "]";
            return Result(id, fmt::format("\"position\":[{},{},{}],\"chain\":{}", pActor->position.x, pActor->position.y,
                pActor->position.z, chain));
        }
        // Host-driven bone playback: stats, and "enabled":"false" to compare against local animation.
        if (command == "pose_authority")
        {
            const auto enabled = GetJsonString(acLine, "enabled");
            if (!enabled.empty())
                PoseCopyAuthority::SetEnabled(enabled != "false");
            return Result(id, PoseCopyAuthority::StatsJson());
        }
        if (command == "cart_physics")
        {
            const bool enabled = GetJsonString(acLine, "enabled") != "false";
            ObjectService::SetCartPhysicsEnabled(enabled);
            return Result(id, fmt::format("\"enabled\":{}", enabled));
        }
        if (command == "visual_lag_frame")
        {
            const bool enabled = GetJsonString(acLine, "enabled") != "false";
            ObjectService::SetVisualLagFrameEnabled(enabled);
            return Result(id, fmt::format("\"enabled\":{}", enabled));
        }
        if (command == "body_velocity")
        {
            const bool enabled = GetJsonString(acLine, "enabled") != "false";
            ObjectService::SetBodyVelocityEnabled(enabled);
            return Result(id, fmt::format("\"enabled\":{}", enabled));
        }
        if (command == "cart_smoothing")
        {
            const bool enabled = GetJsonString(acLine, "enabled") != "false";
            ObjectService::SetCartSmoothingEnabled(enabled);
            return Result(id, fmt::format("\"enabled\":{}", enabled));
        }
        if (command == "hermite_playback")
        {
            const bool enabled = GetJsonString(acLine, "enabled") != "false";
            ObjectService::SetHermitePlaybackEnabled(enabled);
            return Result(id, fmt::format("\"enabled\":{}", enabled));
        }
        if (command == "physics_stamp")
        {
            const bool enabled = GetJsonString(acLine, "enabled") != "false";
            ObjectService::SetPhysicsStampEnabled(enabled);
            return Result(id, fmt::format("\"enabled\":{}", enabled));
        }
        if (command == "main_frame_capture")
        {
            const bool enabled = GetJsonString(acLine, "enabled") != "false";
            ObjectService::SetMainFrameCaptureEnabled(enabled);
            return Result(id, fmt::format("\"enabled\":{}", enabled));
        }
        if (command == "main_frame_playback")
        {
            const bool enabled = GetJsonString(acLine, "enabled") != "false";
            ObjectService::SetMainFramePlaybackEnabled(enabled);
            return Result(id, fmt::format("\"enabled\":{}", enabled));
        }
        if (command == "host_driven_playback")
        {
            const bool enabled = GetJsonString(acLine, "enabled") != "false";
            ObjectService::SetHostDrivenPlaybackEnabled(enabled);
            return Result(id, fmt::format("\"enabled\":{}", enabled));
        }
        if (command == "root_body_write")
        {
            const bool enabled = GetJsonString(acLine, "enabled") != "false";
            ObjectService::SetRootBodyWriteEnabled(enabled);
            return Result(id, fmt::format("\"enabled\":{}", enabled));
        }
        if (command == "cell_handoff")
        {
            const bool enabled = GetJsonString(acLine, "enabled") != "false";
            ObjectService::SetCellHandoffEnabled(enabled);
            return Result(id, fmt::format("\"enabled\":{}", enabled));
        }
        // Plays an idle (form ID, hex) on the local player, e.g. 10C00D IdleWalkingCameraEnd.
        if (command == "player_idle")
        {
            auto* pPlayer = PlayerCharacter::Get();
            const auto form = GetJsonString(acLine, "form_id");
            auto* pIdle = form.empty() ? nullptr : Cast<TESIdleForm>(TESForm::GetById(std::stoul(form, nullptr, 16)));
            if (!pPlayer || !pIdle)
                return Error(id, "player or idle form not found");
            return Result(id, fmt::format("\"played\":{}", pPlayer->PlayIdle(pIdle)));
        }
        if (command == "set_session_open")
        {
            if (!m_world.GetPartyService().IsLeader())
                return Error(id, "host party is not ready yet");
            const bool open = GetJsonString(acLine, "open") != "false";
            m_world.GetPartyService().SetSessionSettings(open, {});
            return Result(id, fmt::format("\"open\":{}", open));
        }
        if (command == "join_friend")
        {
            const auto steamId = GetJsonString(acLine, "steamId");
            if (steamId.empty())
                return Error(id, "steamId is required");
            m_world.GetSteamLobbyService().QueueJoinFriend(std::stoull(steamId));
            return Result(id, fmt::format("\"steamId\":\"{}\"", EscapeJson(steamId)));
        }
        // Steam session tests: the lobby as the UI sees it, direct invites, answering invites.
        if (command == "steam_state")
            return Result(id, fmt::format("\"steam\":{}", m_world.GetSteamLobbyService().TestStateJson()));
        if (command == "steam_invite")
        {
            const auto steamId = GetJsonString(acLine, "steamId");
            if (steamId.empty())
                return Error(id, "steamId is required");
            m_world.GetSteamLobbyService().QueueInviteFriendDirect(std::stoull(steamId));
            return Result(id, fmt::format("\"steamId\":\"{}\"", EscapeJson(steamId)));
        }
        if (command == "steam_answer_invite")
        {
            const auto lobby = GetJsonString(acLine, "lobby");
            if (lobby.empty())
                return Error(id, "lobby is required");
            const bool accept = GetJsonString(acLine, "accept") != "false";
            m_world.GetSteamLobbyService().QueueAnswerInvite(std::stoull(lobby), accept);
            return Result(id, fmt::format("\"lobby\":\"{}\",\"accept\":{}", EscapeJson(lobby), accept));
        }
        if (command == "set_ready")
        {
            const bool ready = GetJsonString(acLine, "ready") != "false";
            m_world.GetPartyService().SetReady(ready);
            return Result(id, fmt::format("\"ready\":{}", ready));
        }
        if (command == "start_new_campaign")
        {
            auto& party = m_world.GetPartyService();
            if (!party.IsLeader())
                return Error(id, "only the party leader can start a campaign");
            if (party.GetPartyMembers().size() < 2 ||
                party.GetReadyPlayerCount() != party.GetPartyMembers().size() ||
                party.GetSessionState() != 0)
                return Error(id, "party needs two or more members, all ready, and an idle session");
            party.SelectCampaign(PartyStartRequest::kNew);
            party.StartTogether(PartyStartRequest::kNew);
            return Result(id, "\"campaignMode\":1");
        }
        if (command == "start_continue_campaign")
        {
            auto& party = m_world.GetPartyService();
            if (!party.IsLeader())
                return Error(id, "only the party leader can start a campaign");
            if (party.GetPartyMembers().size() < 2 ||
                party.GetReadyPlayerCount() != party.GetPartyMembers().size() ||
                party.GetSessionState() != 0)
                return Error(id, "party needs two or more members, all ready, and an idle session");
            party.SelectCampaign(PartyStartRequest::kContinue);
            party.StartTogether(PartyStartRequest::kContinue);
            return Result(id, "\"campaignMode\":2");
        }
        if (command == "close_options")
        {
            m_world.GetOverlayService().SetActive(false);
            return Result(id, "\"action\":\"close_options\"");
        }
        if (command == "controller")
        {
            const auto button = GetJsonString(acLine, "button");
            if (!m_world.GetOverlayService().InjectTestControllerButton(button))
                return Error(id, "unknown controller button");
            return Result(id, fmt::format("\"button\":\"{}\"", EscapeJson(button)));
        }
        if (command == "race_menu_state")
        {
            auto* pUI = UI::Get();
            auto* pPlayer = PlayerCharacter::Get();
            auto* pNpc = pPlayer ? Cast<TESNPC>(pPlayer->baseForm) : nullptr;
            const std::string name = pNpc && pNpc->fullName.value.data ?
                pNpc->fullName.value.AsAscii() : "";
            const auto directInput = TiltedPhoques::DInputHook::GetDiagnostic();
            return Result(id, fmt::format(
                "\"raceMenuOpen\":{},\"messageBoxOpen\":{},\"playerName\":\"{}\","
                "\"overlayActive\":{},\"directInputSuppressed\":{},"
                "\"inputPoll\":{{\"calls\":{},\"forwarded\":{},\"unfocused\":{},"
                "\"overlay\":{},\"resumeDelay\":{}}},"
                "\"directInput\":{{\"keyboardStateCalls\":{},\"mouseStateCalls\":{},"
                "\"keyboardDataCalls\":{},\"mouseDataCalls\":{},"
                "\"keyboardEvents\":{},\"mouseEvents\":{}}}",
                JsonBool(pUI && pUI->GetMenuOpen(BSFixedString("RaceSex Menu"))),
                JsonBool(pUI && pUI->GetMenuOpen(BSFixedString("MessageBoxMenu"))),
                EscapeJson(name), JsonBool(m_world.GetOverlayService().GetActive()),
                JsonBool(TiltedPhoques::DInputHook::Get().IsEnabled()),
                g_inputPollDiagnostic.Calls.load(std::memory_order_relaxed),
                g_inputPollDiagnostic.Forwarded.load(std::memory_order_relaxed),
                g_inputPollDiagnostic.Unfocused.load(std::memory_order_relaxed),
                g_inputPollDiagnostic.OverlayActive.load(std::memory_order_relaxed),
                g_inputPollDiagnostic.ResumeDelay.load(std::memory_order_relaxed),
                directInput.KeyboardStateCalls, directInput.MouseStateCalls,
                directInput.KeyboardDataCalls, directInput.MouseDataCalls,
                directInput.KeyboardEvents, directInput.MouseEvents));
        }
        if (command == "confirm_character_native")
        {
            auto* pUI = UI::Get();
            if (!pUI || !pUI->GetMenuOpen(BSFixedString("RaceSex Menu")) ||
                !pUI->GetMenuOpen(BSFixedString("MessageBoxMenu")))
                return Error(id, "character confirmation is not open");
            if (m_nativeCreatorConfirmRequested.load(std::memory_order_acquire))
                return Error(id, "native confirmation already pending");
            {
                std::scoped_lock lock(m_snapshotMutex);
                m_nativeCreatorConfirmComplete = false;
                m_nativeCreatorConfirmSucceeded = false;
            }
            m_nativeCreatorConfirmRequested.store(true, std::memory_order_release);
            return Result(id, "\"queued\":true");
        }
        if (command == "confirm_character_native_status")
        {
            std::scoped_lock lock(m_snapshotMutex);
            return Result(id, fmt::format("\"complete\":{},\"nativeSelected\":{}",
                JsonBool(m_nativeCreatorConfirmComplete),
                JsonBool(m_nativeCreatorConfirmSucceeded)));
        }
        if (command == "race_menu_key" || command == "gameplay_key")
        {
            const bool gameplayKey = command == "gameplay_key";
            const auto key = GetJsonString(acLine, "key");
            if (gameplayKey ? (key != "quicksave" && key != "forward") :
                (key != "done" && key != "confirm" && key != "left" &&
                key != "right" && key != "click" && key != "type" &&
                key != "accept_name"))
                return Error(id, "unsupported key action");
            const auto name = GetJsonString(acLine, "name");
            if (key == "type" && name != "Host" && name != "Follower 1" &&
                name != "Follower 2" && name != "Follower 3" && name != "Follower 4")
                return Error(id, "type requires a party test character name");
            const auto x = GetJsonString(acLine, "x");
            const auto y = GetJsonString(acLine, "y");
            if (key == "click" && (x.empty() || y.empty() ||
                x.find_first_not_of("0123456789") != std::string::npos ||
                y.find_first_not_of("0123456789") != std::string::npos))
                return Error(id, "click requires decimal client x and y");

            auto* pUI = UI::Get();
            auto* pWindow = BSGraphics::GetMainWindow();
            if (!pUI || !pWindow || !pWindow->hWnd || !IsWindowVisible(pWindow->hWnd))
                return Error(id, "Skyrim window is not visible");
            if (gameplayKey)
            {
                auto* pPlayer = PlayerCharacter::Get();
                // The Loading Menu instance stays alive between loads, so GetMenuOpen is not a
                // loading test; a loaded player (cell and 3D) is.
                if (m_world.GetPartyService().GetSessionState() != 3 ||
                    !pPlayer || !pPlayer->parentCell || !pPlayer->GetNiNode() ||
                    pUI->GetMenuOpen(BSFixedString("RaceSex Menu")))
                    return Error(id, fmt::format("gameplay input requires shared gameplay, a loaded cell, and no creator menu "
                        "(session {}, cell {}, 3D {}, creator {})", m_world.GetPartyService().GetSessionState(),
                        pPlayer && pPlayer->parentCell, pPlayer && pPlayer->GetNiNode(),
                        pUI->GetMenuOpen(BSFixedString("RaceSex Menu"))));
            }
            else if (!pUI->GetMenuOpen(BSFixedString("RaceSex Menu")))
                return Error(id, "RaceSex Menu is not visible");
            const bool confirmationWasOpen = pUI->GetMenuOpen(BSFixedString("MessageBoxMenu"));
            if (!gameplayKey && (key == "done" || key == "type" || key == "accept_name") &&
                confirmationWasOpen)
                return Error(id, "character confirmation is open");
            if (!gameplayKey && (key == "confirm" || key == "left" || key == "right" ||
                key == "click") &&
                !confirmationWasOpen)
                return Error(id, "character confirmation is not open");

            // The pipe caller may be an SSH process in a different session.
            // Launch the actuator as Skyrim's child on the same interactive
            // desktop, and fail closed if the game cannot obtain focus.
            if (GetForegroundWindow() != pWindow->hWnd)
            {
                SetForegroundWindow(pWindow->hWnd);
                if (GetForegroundWindow() != pWindow->hWnd)
                    return Error(id, "Skyrim could not become foreground");
            }

            wchar_t modulePath[MAX_PATH]{};
            const auto pathLength = GetModuleFileNameW(nullptr, modulePath, MAX_PATH);
            if (!pathLength || pathLength >= MAX_PATH)
                return Error(id, "could not resolve Skyrim executable path");
            // The launcher spoofs GetModuleFileNameW(nullptr) to SkyrimSE.exe,
            // so modulePath resolves to the game root, not our binary folder.
            const auto module = std::filesystem::path(modulePath);
            std::filesystem::path helper;
            if (_wcsicmp(module.filename().c_str(), L"SkyrimSE.exe") == 0)
                helper = module.parent_path() / L"Data" / L"SkyrimTogetherReborn" / L"GameTestKeyHelper.exe";
            else if (_wcsicmp(module.filename().c_str(), L"SkyrimTogether.exe") == 0)
                helper = module.parent_path() / L"GameTestKeyHelper.exe";
            else
                return Error(id, "unexpected Skyrim process image path");
            if (!std::filesystem::is_regular_file(helper))
                return Error(id, fmt::format("GameTestKeyHelper.exe is not installed at {}",
                    helper.string()));
            const auto helperKey = key == "accept_name" ? "confirm" : key;
            std::wstring commandLine = L"\"" + helper.wstring() + L"\" " +
                std::to_wstring(reinterpret_cast<uintptr_t>(pWindow->hWnd)) + L" " +
                std::to_wstring(GetCurrentProcessId()) + L" " +
                std::wstring(helperKey.begin(), helperKey.end());
            if (key == "type")
                commandLine += L" \"" + std::wstring(name.begin(), name.end()) + L"\"";
            else if (key == "click")
                commandLine += L" " + std::wstring(x.begin(), x.end()) + L" " +
                    std::wstring(y.begin(), y.end());
            STARTUPINFOW startup{};
            startup.cb = sizeof(startup);
            PROCESS_INFORMATION child{};
            if (!CreateProcessW(helper.c_str(), commandLine.data(), nullptr, nullptr,
                FALSE, CREATE_NO_WINDOW, nullptr, helper.parent_path().c_str(), &startup, &child))
                return Error(id, fmt::format("could not start key helper (Win32 {})", GetLastError()));
            const auto wait = WaitForSingleObject(child.hProcess, 3000);
            if (wait == WAIT_TIMEOUT)
                TerminateProcess(child.hProcess, 8);
            DWORD childExit = 8;
            if (wait == WAIT_OBJECT_0)
                GetExitCodeProcess(child.hProcess, &childExit);
            CloseHandle(child.hThread);
            CloseHandle(child.hProcess);
            if (wait != WAIT_OBJECT_0 || childExit != 0)
                return Error(id, fmt::format("key helper failed (wait {}, exit {})", wait, childExit));
            return Result(id, fmt::format("\"key\":\"{}\",\"helperExit\":{}",
                EscapeJson(key), childExit));
        }
        if (command == "toggle_window")
        {
            m_world.GetGameSettingsService().ToggleWindowMode();
            return Result(id, "\"action\":\"toggle_window\"");
        }
        if (command == "confirm_display")
        {
            m_world.GetGameSettingsService().ConfirmDisplaySettings();
            return Result(id, "\"action\":\"confirm_display\"");
        }
        if (command == "setting")
        {
            const auto name = GetJsonString(acLine, "name");
            const auto value = GetJsonString(acLine, "value");
            if (name.empty())
                return Error(id, "setting name is required");
            m_world.GetGameSettingsService().PreviewSetting(name.c_str(), value.c_str());
            return Result(id, fmt::format("\"name\":\"{}\",\"value\":\"{}\"",
                EscapeJson(name), EscapeJson(value)));
        }
        if (command == "screenshot")
        {
            const auto path = m_world.GetGameSettingsService().CaptureTestScreenshot();
            if (path.empty())
                return Error(id, "screenshot failed");
            return Result(id, fmt::format("\"path\":\"{}\"", EscapeJson(path.string())));
        }
        if (command == "snapshot")
        {
            auto* pWindow = BSGraphics::GetMainWindow();
            auto* pRenderer = BSGraphics::GetRendererData();
            if (!pWindow || !pWindow->hWnd || !pRenderer)
                return Error(id, "renderer is not initialized");

            RECT client{}, outer{};
            GetClientRect(pWindow->hWnd, &client);
            GetWindowRect(pWindow->hWnd, &outer);
            CURSORINFO cursor{sizeof(cursor)};
            GetCursorInfo(&cursor);
            DXGI_SWAP_CHAIN_DESC swap{};
            const bool haveSwap = pWindow->pSwapChain && SUCCEEDED(pWindow->pSwapChain->GetDesc(&swap));

            uint32_t overlayWidth = 0, overlayHeight = 0;
            uint16_t cursorX = 0, cursorY = 0;
            bool cefCursor = false;
            if (auto* pApp = m_world.GetOverlayService().GetOverlayApp(); pApp && pApp->GetClient())
                if (auto handler = pApp->GetClient()->GetOverlayRenderHandler())
                {
                    std::tie(overlayWidth, overlayHeight) = handler->GetRenderSize();
                    std::tie(cursorX, cursorY) = handler->GetCursorLocation();
                    cefCursor = handler->IsCursorVisible();
                }

            bool mainMenu = false;
            if (auto* pUI = UI::Get())
                mainMenu = pUI->GetMenuOpen(BSFixedString("Main Menu"));

            const auto style = static_cast<uint64_t>(GetWindowLongPtrW(pWindow->hWnd, GWL_STYLE));
            return Result(id, fmt::format(
                "\"window\":{{\"hwnd\":{},\"foreground\":{},\"style\":{},\"clientWidth\":{},\"clientHeight\":{},"
                "\"outerWidth\":{},\"outerHeight\":{},\"x\":{},\"y\":{}}},"
                "\"renderer\":{{\"width\":{},\"height\":{},\"fullscreen\":{},\"borderless\":{},"
                "\"swapWidth\":{},\"swapHeight\":{},\"swapWindowed\":{}}},"
                "\"overlay\":{{\"active\":{},\"titleScreen\":{},\"width\":{},\"height\":{},"
                "\"cursorVisible\":{},\"cursorX\":{},\"cursorY\":{}}},"
                "\"systemCursor\":{{\"visible\":{}}},\"mainMenuOpen\":{}",
                reinterpret_cast<uintptr_t>(pWindow->hWnd), GetForegroundWindow() == pWindow->hWnd,
                style, client.right, client.bottom,
                outer.right - outer.left, outer.bottom - outer.top, outer.left, outer.top,
                pRenderer->RenderWindowA[0].uiWindowWidth, pRenderer->RenderWindowA[0].uiWindowHeight,
                pRenderer->bAppFullScreen, pRenderer->bBorderlessWindow,
                haveSwap ? swap.BufferDesc.Width : 0, haveSwap ? swap.BufferDesc.Height : 0,
                haveSwap ? swap.Windowed != FALSE : false,
                m_world.GetOverlayService().GetActive(), m_world.GetOverlayService().GetTitleScreen(),
                overlayWidth, overlayHeight, cefCursor, cursorX, cursorY,
                (cursor.flags & CURSOR_SHOWING) != 0, mainMenu));
        }
        return Error(id, "unknown command");
    }
    catch (const std::exception& exception)
    {
        return Error(id, exception.what());
    }
    catch (...)
    {
        return Error(id, "native exception");
    }
}
