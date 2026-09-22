#include <papyrus-vm/Reader.h>
#include <papyrus-vm/VirtualMachine.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <iostream>
#include <map>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

#include <spdlog/sinks/callback_sink.h>
#include <spdlog/spdlog.h>

namespace
{
std::string Lower(std::string aValue)
{
    std::transform(aValue.begin(), aValue.end(), aValue.begin(),
        [](unsigned char aCharacter) { return static_cast<char>(std::tolower(aCharacter)); });
    return aValue;
}

std::string EscapeJson(const std::string& acValue)
{
    std::string result;
    for (const char value : acValue)
    {
        switch (value)
        {
        case '\\': result += "\\\\"; break;
        case '"': result += "\\\""; break;
        case '\n': result += "\\n"; break;
        case '\r': result += "\\r"; break;
        case '\t': result += "\\t"; break;
        default: result += value; break;
        }
    }
    return result;
}

struct ModelObject final : IGameObject
{
    ModelObject(std::string aId, std::string aNativeType)
        : Id(std::move(aId))
        , NativeType(std::move(aNativeType))
    {
    }

    const char* GetStringID() override { return Id.c_str(); }
    const char* GetParentNativeScript() override { return NativeType.c_str(); }
    bool EqualsByValue(const IGameObject& acOther) const override
    {
        const auto* pOther = dynamic_cast<const ModelObject*>(&acOther);
        return pOther && pOther->Id == Id;
    }
    const std::vector<std::shared_ptr<ActivePexInstance>>& ListActivePexInstances() const override
    {
        return Scripts;
    }
    void AddScript(std::shared_ptr<ActivePexInstance> aScript) noexcept override
    {
        Scripts.push_back(std::move(aScript));
    }

    std::string Id;
    std::string NativeType;
    bool Enabled{true};
    bool Open{};
    bool Locked{};
    int32_t LockLevel{};
    double NumericValue{};
    std::string Position;
    std::map<std::string, int32_t> Inventory;
    std::vector<int32_t> Stages;
    std::map<int32_t, std::string> Objectives;
    std::shared_ptr<ModelObject> OwnedLinkedReference;
    ModelObject* LinkedReference{};
    std::vector<std::shared_ptr<ActivePexInstance>> Scripts;
};

class VariableStore final : public IVariablesHolder
{
public:
    explicit VariableStore(const PexScript& acScript)
    {
        Values.emplace(Lower("::State"), VarValue(std::string{}));
        for (const auto& object : acScript.objectTable)
        {
            for (const auto& variable : object.variables)
                Values.insert_or_assign(Lower(variable.name), variable.value);
            for (const auto& property : object.properties)
            {
                if (property.autoVarName.empty()) continue;
                const auto nativeType = NormalizeNativeType(property.type);
                auto modeled = std::make_shared<ModelObject>(property.name, nativeType);
                if (Lower(nativeType) == "referencealias" || Lower(nativeType) == "locationalias")
                {
                    modeled->OwnedLinkedReference = std::make_shared<ModelObject>(
                        property.name + ".Ref", Lower(nativeType) == "referencealias" ? "Actor" : "ObjectReference");
                    modeled->LinkedReference = modeled->OwnedLinkedReference.get();
                }
                PropertyObjects.emplace(Lower(property.name), modeled);
                VarValue value(modeled);
                value.objectType = property.type;
                Values.insert_or_assign(Lower(property.autoVarName), value);
            }
        }
        Values.insert_or_assign(Lower("::State"), VarValue(std::string{}));
    }

    VarValue* GetVariableByName(const char* acName, const PexScript&) override
    {
        auto iterator = Values.find(Lower(acName ? acName : ""));
        return iterator == Values.end() ? nullptr : &iterator->second;
    }

    static std::string NormalizeNativeType(const std::string& acType)
    {
        const auto lower = Lower(acType);
        if (lower == "referencealias" || lower == "locationalias") return acType;
        if (lower == "actor") return "Actor";
        if (lower == "objectreference") return "ObjectReference";
        if (lower == "quest") return "Quest";
        if (lower == "scene") return "Scene";
        return "Form";
    }

    std::map<std::string, VarValue> Values;
    std::map<std::string, std::shared_ptr<ModelObject>> PropertyObjects;
};

struct Simulation
{
    std::vector<std::string> Events;
    std::shared_ptr<ModelObject> Player = std::make_shared<ModelObject>("PlayerRef", "Actor");
    bool PlayerControlsEnabled{true};
};

ModelObject* AsObject(const VarValue& acValue)
{
    return dynamic_cast<ModelObject*>(static_cast<IGameObject*>(acValue));
}

std::string ObjectId(const VarValue& acValue)
{
    if (const auto* pObject = AsObject(acValue)) return pObject->Id;
    return "None";
}

void AddEvent(Simulation& aSimulation, const std::string& acEvent)
{
    aSimulation.Events.push_back(acEvent);
}

void RegisterNatives(VirtualMachine& aVm, Simulation& aSimulation)
{
    const auto questStage = [&aSimulation](VarValue aSelf, std::vector<VarValue> aArguments) {
        auto* pQuest = AsObject(aSelf);
        const int32_t stage = aArguments.empty() ? 0 : static_cast<int>(aArguments[0]);
        if (pQuest) pQuest->Stages.push_back(stage);
        AddEvent(aSimulation, "quest.setStage:" + ObjectId(aSelf) + ":" + std::to_string(stage));
        return VarValue(true);
    };
    aVm.RegisterFunction("Quest", "SetStage", FunctionType::Method, questStage);
    aVm.RegisterFunction("Quest", "SetCurrentStageID", FunctionType::Method, questStage);
    aVm.RegisterFunction("Quest", "GetStageDone", FunctionType::Method,
        [&aSimulation](VarValue aSelf, std::vector<VarValue> aArguments) {
            const auto stage = aArguments.empty() ? 0 : static_cast<int>(aArguments[0]);
            const auto* pQuest = AsObject(aSelf);
            const bool done = pQuest && std::find(pQuest->Stages.begin(), pQuest->Stages.end(), stage) != pQuest->Stages.end();
            AddEvent(aSimulation, "quest.getStageDone:" + ObjectId(aSelf) + ":" + std::to_string(stage));
            return VarValue(done);
        });
    const auto objective = [&aSimulation](const std::string& acState, VarValue aSelf,
                               const std::vector<VarValue>& acArguments) {
        auto* pQuest = AsObject(aSelf);
        const auto id = acArguments.empty() ? 0 : static_cast<int>(acArguments[0]);
        if (pQuest) pQuest->Objectives[id] = acState;
        AddEvent(aSimulation, "quest.objective:" + ObjectId(aSelf) + ":" + std::to_string(id) + ":" + acState);
        return VarValue::None();
    };
    aVm.RegisterFunction("Quest", "SetObjectiveDisplayed", FunctionType::Method,
        [objective](VarValue aSelf, std::vector<VarValue> aArguments) { return objective("displayed", aSelf, aArguments); });
    aVm.RegisterFunction("Quest", "SetObjectiveCompleted", FunctionType::Method,
        [objective](VarValue aSelf, std::vector<VarValue> aArguments) { return objective("completed", aSelf, aArguments); });

    aVm.RegisterFunction("Game", "GetPlayer", FunctionType::GlobalFunction,
        [&aSimulation](VarValue, std::vector<VarValue>) {
            AddEvent(aSimulation, "game.getPlayer");
            VarValue result(aSimulation.Player);
            result.objectType = "Actor";
            return result;
        });
    aVm.RegisterFunction("Game", "DisablePlayerControls", FunctionType::GlobalFunction,
        [&aSimulation](VarValue, std::vector<VarValue>) {
            aSimulation.PlayerControlsEnabled = false;
            AddEvent(aSimulation, "game.disablePlayerControls");
            return VarValue::None();
        });
    aVm.RegisterFunction("Game", "EnablePlayerControls", FunctionType::GlobalFunction,
        [&aSimulation](VarValue, std::vector<VarValue>) {
            aSimulation.PlayerControlsEnabled = true;
            AddEvent(aSimulation, "game.enablePlayerControls");
            return VarValue::None();
        });
    aVm.RegisterFunction("Game", "UsingGamePad", FunctionType::GlobalFunction,
        [&aSimulation](VarValue, std::vector<VarValue>) {
            AddEvent(aSimulation, "game.usingGamePad:false");
            return VarValue(false);
        });
    for (const std::string name : {"SetHudCartMode", "RequestSave", "SetInChargen",
             "SetPlayerAIDriven", "FadeOutGame", "ShowRaceMenu", "ShowFirstPersonGeometry",
             "ShowTitleSequenceMenu", "ShakeCamera", "ShakeController", "RequestAutoSave",
             "SetSittingRotation", "PrecacheCharGen", "AddAchievement"})
    {
        aVm.RegisterFunction("Game", name, FunctionType::GlobalFunction,
            [&aSimulation, name](VarValue, std::vector<VarValue>) {
                AddEvent(aSimulation, "conservative.game." + name);
                return VarValue::None();
            });
    }
    aVm.RegisterFunction("Utility", "Wait", FunctionType::GlobalFunction,
        [&aSimulation](VarValue, std::vector<VarValue> aArguments) {
            const auto duration = aArguments.empty() ? 0.0 : static_cast<double>(aArguments[0]);
            AddEvent(aSimulation, "utility.wait:" + std::to_string(duration));
            return VarValue::None();
        });
    aVm.RegisterFunction("Form", "SetValue", FunctionType::Method,
        [&aSimulation](VarValue aSelf, std::vector<VarValue> aArguments) {
            const auto value = aArguments.empty() ? 0.0 : static_cast<double>(aArguments[0]);
            if (auto* pForm = AsObject(aSelf)) pForm->NumericValue = value;
            AddEvent(aSimulation, "global.setValue:" + ObjectId(aSelf) + ":" + std::to_string(value));
            return VarValue::None();
        });

    const auto addItem = [&aSimulation](VarValue aSelf, std::vector<VarValue> aArguments) {
        auto* pTarget = AsObject(aSelf);
        const std::string item = aArguments.empty() ? "None" : ObjectId(aArguments[0]);
        const int32_t quantity = aArguments.size() < 2 ? 1 : static_cast<int>(aArguments[1]);
        if (pTarget) pTarget->Inventory[item] += quantity;
        AddEvent(aSimulation, "inventory.add:" + ObjectId(aSelf) + ":" + item + ":" + std::to_string(quantity));
        return VarValue::None();
    };
    const auto equipItem = [&aSimulation](VarValue aSelf, std::vector<VarValue> aArguments) {
        AddEvent(aSimulation, "inventory.equip:" + ObjectId(aSelf) + ":" +
            (aArguments.empty() ? "None" : ObjectId(aArguments[0])));
        return VarValue::None();
    };
    const auto removeItem = [&aSimulation](VarValue aSelf, std::vector<VarValue> aArguments) {
        auto* pTarget = AsObject(aSelf);
        const std::string item = aArguments.empty() ? "None" : ObjectId(aArguments[0]);
        const int32_t quantity = aArguments.size() < 2 ? 1 : static_cast<int>(aArguments[1]);
        if (pTarget) pTarget->Inventory[item] = std::max(0, pTarget->Inventory[item] - quantity);
        AddEvent(aSimulation, "inventory.remove:" + ObjectId(aSelf) + ":" + item + ":" + std::to_string(quantity));
        return VarValue::None();
    };
    const auto moveTo = [&aSimulation](VarValue aSelf, std::vector<VarValue> aArguments) {
        auto* pTarget = AsObject(aSelf);
        const std::string destination = aArguments.empty() ? "None" : ObjectId(aArguments[0]);
        if (pTarget) pTarget->Position = destination;
        AddEvent(aSimulation, "reference.moveTo:" + ObjectId(aSelf) + ":" + destination);
        return VarValue::None();
    };
    const auto evaluatePackage = [&aSimulation](VarValue aSelf, std::vector<VarValue>) {
        AddEvent(aSimulation, "actor.evaluatePackage:" + ObjectId(aSelf));
        return VarValue::None();
    };
    const auto playIdle = [&aSimulation](VarValue aSelf, std::vector<VarValue> aArguments) {
        AddEvent(aSimulation, "actor.playIdle:" + ObjectId(aSelf) + ":" +
            (aArguments.empty() ? "None" : ObjectId(aArguments[0])));
        return VarValue(true);
    };
    for (const std::string type : {"Actor", "ObjectReference"})
    {
        aVm.RegisterFunction(type, "AddItem", FunctionType::Method, addItem);
        aVm.RegisterFunction(type, "EquipItem", FunctionType::Method, equipItem);
        aVm.RegisterFunction(type, "RemoveItem", FunctionType::Method, removeItem);
        aVm.RegisterFunction(type, "MoveTo", FunctionType::Method, moveTo);
        aVm.RegisterFunction(type, "EvaluatePackage", FunctionType::Method, evaluatePackage);
        aVm.RegisterFunction(type, "PlayIdle", FunctionType::Method, playIdle);
        aVm.RegisterFunction(type, "Enable", FunctionType::Method,
            [&aSimulation](VarValue aSelf, std::vector<VarValue>) {
                if (auto* pObject = AsObject(aSelf)) pObject->Enabled = true;
                AddEvent(aSimulation, "reference.enable:" + ObjectId(aSelf));
                return VarValue::None();
            });
        aVm.RegisterFunction(type, "Disable", FunctionType::Method,
            [&aSimulation](VarValue aSelf, std::vector<VarValue>) {
                if (auto* pObject = AsObject(aSelf)) pObject->Enabled = false;
                AddEvent(aSimulation, "reference.disable:" + ObjectId(aSelf));
                return VarValue::None();
            });
        aVm.RegisterFunction(type, "EnableNoWait", FunctionType::Method,
            [&aSimulation](VarValue aSelf, std::vector<VarValue>) {
                if (auto* pObject = AsObject(aSelf)) pObject->Enabled = true;
                AddEvent(aSimulation, "reference.enableNoWait:" + ObjectId(aSelf));
                return VarValue::None();
            });
        aVm.RegisterFunction(type, "DisableNoWait", FunctionType::Method,
            [&aSimulation](VarValue aSelf, std::vector<VarValue>) {
                if (auto* pObject = AsObject(aSelf)) pObject->Enabled = false;
                AddEvent(aSimulation, "reference.disableNoWait:" + ObjectId(aSelf));
                return VarValue::None();
            });
        aVm.RegisterFunction(type, "SetOpen", FunctionType::Method,
            [&aSimulation](VarValue aSelf, std::vector<VarValue> aArguments) {
                const bool open = !aArguments.empty() && static_cast<bool>(aArguments[0]);
                if (auto* pObject = AsObject(aSelf)) pObject->Open = open;
                AddEvent(aSimulation, "reference.setOpen:" + ObjectId(aSelf) + ":" + (open ? "true" : "false"));
                return VarValue::None();
            });
        aVm.RegisterFunction(type, "Lock", FunctionType::Method,
            [&aSimulation](VarValue aSelf, std::vector<VarValue>) {
                if (auto* pObject = AsObject(aSelf)) pObject->Locked = true;
                AddEvent(aSimulation, "reference.lock:" + ObjectId(aSelf));
                return VarValue::None();
            });
        aVm.RegisterFunction(type, "SetLockLevel", FunctionType::Method,
            [&aSimulation](VarValue aSelf, std::vector<VarValue> aArguments) {
                const int level = aArguments.empty() ? 0 : static_cast<int>(aArguments[0]);
                if (auto* pObject = AsObject(aSelf)) pObject->LockLevel = level;
                AddEvent(aSimulation, "reference.setLockLevel:" + ObjectId(aSelf) + ":" + std::to_string(level));
                return VarValue::None();
            });
        for (const std::string name : {"ExitCart", "Activate", "BlockActivation",
                 "MoveToMyEditorLocation", "RemoveAllItems", "SetRestrained", "SetActorValue",
                 "StartCombat", "SetHeadTracking", "IgnoreFriendlyHits", "SetNotShowOnStealthMeter",
                 "SetProtected", "SetOutfit", "UnEquipItem", "PlayAnimation", "SetVehicle",
                 "TetherToHorse", "SetMotionType", "ResetHealthAndLimbs"})
        {
            aVm.RegisterFunction(type, name, FunctionType::Method,
                [&aSimulation, name](VarValue aSelf, std::vector<VarValue>) {
                    AddEvent(aSimulation, "conservative.actor." + name + ":" + ObjectId(aSelf));
                    return VarValue::None();
                });
        }
        aVm.RegisterFunction(type, "GetActorBase", FunctionType::Method,
            [&aSimulation](VarValue aSelf, std::vector<VarValue>) {
                AddEvent(aSimulation, "actor.getActorBase:" + ObjectId(aSelf));
                return aSelf;
            });
        aVm.RegisterFunction(type, "Is3DLoaded", FunctionType::Method,
            [&aSimulation](VarValue aSelf, std::vector<VarValue>) {
                AddEvent(aSimulation, "reference.is3DLoaded:" + ObjectId(aSelf) + ":true");
                return VarValue(true);
            });
    }

    const auto aliasMove = [&aSimulation](VarValue aSelf, std::vector<VarValue> aArguments) {
        AddEvent(aSimulation, "alias.moveTo:" + ObjectId(aSelf) + ":" +
            (aArguments.empty() ? "None" : ObjectId(aArguments[0])));
        return VarValue(true);
    };
    const auto aliasEnable = [&aSimulation](VarValue aSelf, std::vector<VarValue>) {
        if (auto* pAlias = AsObject(aSelf)) pAlias->Enabled = true;
        AddEvent(aSimulation, "alias.enable:" + ObjectId(aSelf));
        return VarValue(true);
    };
    const auto aliasDisable = [&aSimulation](VarValue aSelf, std::vector<VarValue>) {
        if (auto* pAlias = AsObject(aSelf)) pAlias->Enabled = false;
        AddEvent(aSimulation, "alias.disable:" + ObjectId(aSelf));
        return VarValue(true);
    };
    for (const std::string type : {"ReferenceAlias", "LocationAlias"})
    {
        aVm.RegisterFunction(type, "TryToMoveTo", FunctionType::Method, aliasMove);
        aVm.RegisterFunction(type, "TryToEnable", FunctionType::Method, aliasEnable);
        aVm.RegisterFunction(type, "TryToDisable", FunctionType::Method, aliasDisable);
        const auto getReference = [&aSimulation](VarValue aSelf, std::vector<VarValue>) {
            auto* pAlias = AsObject(aSelf);
            AddEvent(aSimulation, "alias.getReference:" + ObjectId(aSelf));
            if (!pAlias || !pAlias->LinkedReference) return VarValue::None();
            VarValue result(pAlias->LinkedReference);
            result.objectType = pAlias->LinkedReference->NativeType;
            return result;
        };
        aVm.RegisterFunction(type, "GetRef", FunctionType::Method, getReference);
        aVm.RegisterFunction(type, "GetReference", FunctionType::Method, getReference);
        aVm.RegisterFunction(type, "GetActorRef", FunctionType::Method, getReference);
        aVm.RegisterFunction(type, "GetActorReference", FunctionType::Method, getReference);
        aVm.RegisterFunction(type, "ForceRefTo", FunctionType::Method,
            [&aSimulation](VarValue aSelf, std::vector<VarValue> aArguments) {
                auto* pAlias = AsObject(aSelf);
                if (pAlias && !aArguments.empty()) pAlias->LinkedReference = AsObject(aArguments[0]);
                AddEvent(aSimulation, "alias.forceRefTo:" + ObjectId(aSelf) + ":" +
                    (aArguments.empty() ? "None" : ObjectId(aArguments[0])));
                return VarValue::None();
            });
        aVm.RegisterFunction(type, "Clear", FunctionType::Method,
            [&aSimulation](VarValue aSelf, std::vector<VarValue>) {
                if (auto* pAlias = AsObject(aSelf)) pAlias->LinkedReference = nullptr;
                AddEvent(aSimulation, "alias.clear:" + ObjectId(aSelf));
                return VarValue::None();
            });
    }

    const auto sceneStart = [&aSimulation](VarValue aSelf, std::vector<VarValue>) {
        AddEvent(aSimulation, "scene.start:" + ObjectId(aSelf));
        return VarValue::None();
    };
    const auto sceneStop = [&aSimulation](VarValue aSelf, std::vector<VarValue>) {
        AddEvent(aSimulation, "scene.stop:" + ObjectId(aSelf));
        return VarValue::None();
    };
    aVm.RegisterFunction("Scene", "Start", FunctionType::Method, sceneStart);
    aVm.RegisterFunction("Scene", "Stop", FunctionType::Method, sceneStop);
    aVm.RegisterFunction("Form", "ShowAsHelpMessage", FunctionType::Method,
        [&aSimulation](VarValue aSelf, std::vector<VarValue>) {
            AddEvent(aSimulation, "message.showAsHelp:" + ObjectId(aSelf));
            return VarValue::None();
        });
    aVm.RegisterFunction("Form", "Play", FunctionType::Method,
        [&aSimulation](VarValue aSelf, std::vector<VarValue>) {
            AddEvent(aSimulation, "sound.play:" + ObjectId(aSelf));
            return VarValue(1);
        });
    for (const std::string name : {"Start", "Stop"})
    {
        aVm.RegisterFunction("Form", name, FunctionType::Method,
            [&aSimulation, name](VarValue aSelf, std::vector<VarValue>) {
                AddEvent(aSimulation, "conservative.form." + name + ":" + ObjectId(aSelf));
                return VarValue::None();
            });
    }
    for (const std::string type : {"Form", "Quest", "Actor", "ObjectReference", "ReferenceAlias"})
    {
        for (const std::string name : {"SetAlly", "SetEnemy", "Add", "Remove", "Show",
                 "ForceActive", "AddRaceSpells", "PlayerFurnitureAnimation", "PlayerImodAnimation",
                 "RegisterForSingleUpdate"})
        {
            aVm.RegisterFunction(type, name, FunctionType::Method,
                [&aSimulation, name](VarValue aSelf, std::vector<VarValue>) {
                    AddEvent(aSimulation, "conservative.native." + name + ":" + ObjectId(aSelf));
                    return VarValue::None();
                });
        }
        aVm.RegisterFunction(type, "RegisterForAnimationEvent", FunctionType::Method,
            [&aSimulation](VarValue aSelf, std::vector<VarValue>) {
                AddEvent(aSimulation, "conservative.native.RegisterForAnimationEvent:" + ObjectId(aSelf));
                return VarValue(true);
            });
    }
    aVm.RegisterFunction("Weather", "ReleaseOverride", FunctionType::GlobalFunction,
        [&aSimulation](VarValue, std::vector<VarValue>) {
            AddEvent(aSimulation, "conservative.weather.releaseOverride");
            return VarValue::None();
        });
    aVm.RegisterFunction("Sound", "StopInstance", FunctionType::GlobalFunction,
        [&aSimulation](VarValue, std::vector<VarValue>) {
            AddEvent(aSimulation, "conservative.sound.stopInstance");
            return VarValue::None();
        });
}

const FunctionInfo* FindFunction(const PexScript& acScript, const std::string& acName)
{
    for (const auto& object : acScript.objectTable)
        for (const auto& state : object.states)
            for (const auto& function : state.functions)
                if (Lower(function.name) == Lower(acName)) return &function.function;
    return nullptr;
}

std::set<std::string> FindUnsupportedCalls(const FunctionInfo& acFunction)
{
    static const std::set<std::string> supported = {
        "setstage", "setcurrentstageid", "getstagedone", "setobjectivedisplayed", "setobjectivecompleted",
        "game.getplayer", "game.disableplayercontrols", "game.enableplayercontrols", "game.usinggamepad",
        "game.sethudcartmode", "game.requestsave", "game.setinchargen", "game.setplayeraidriven",
        "game.fadeoutgame", "game.showracemenu", "game.showfirstpersongeometry", "game.showtitlesequencemenu",
        "game.shakecamera", "game.shakecontroller", "game.requestautosave", "game.setsittingrotation",
        "game.precachechargen", "game.addachievement",
        "utility.wait", "weather.releaseoverride",
        "additem", "removeitem", "removeallitems", "equipitem", "unequipitem", "moveto",
        "evaluatpackage", "evaluatepackage", "playidle", "enable", "disable", "enablenowait", "disablenowait",
        "setopen", "lock", "setlocklevel", "exitcart", "activate", "blockactivation", "movetomyeditorlocation",
        "setrestrained", "setactorvalue", "startcombat", "setheadtracking", "ignorefriendlyhits",
        "setnotshowonstealthmeter", "getactorbase", "setprotected", "setoutfit",
        "playanimation", "is3dloaded", "setvehicle", "tethertohorse", "setmotiontype", "resethealthandlimbs",
        "trytomoveto", "trytoenable", "trytodisable",
        "getref", "getreference", "getactorref", "getactorreference", "forcerefto", "clear",
        "start", "stop", "setvalue", "showashelpmessage", "play", "setally", "setenemy", "add", "remove",
        "show", "forceactive", "addracespells", "playerfurnitureanimation", "playerimodanimation",
        "registerforanimationevent", "registerforsingleupdate", "sound.stopinstance"
    };
    std::set<std::string> result;
    for (const auto& instruction : acFunction.code.instructions)
    {
        std::string call;
        if ((instruction.op == FunctionCode::kOp_CallMethod || instruction.op == FunctionCode::kOp_CallParent) &&
            !instruction.args.empty())
        {
            const auto& name = instruction.args[0];
            if (name.GetType() == VarValue::kType_String || name.GetType() == VarValue::kType_Identifier)
                call = static_cast<const char*>(name);
        }
        else if (instruction.op == FunctionCode::kOp_CallStatic && instruction.args.size() >= 2)
        {
            call = std::string(static_cast<const char*>(instruction.args[0])) + "." +
                static_cast<const char*>(instruction.args[1]);
        }
        if (!call.empty() && !supported.contains(Lower(call))) result.insert(call);
    }
    return result;
}
}

int main(int argc, char** argv)
{
    if (argc != 4)
    {
        std::cerr << "usage: SkyrimPapyrusSim <pex-directory> <script-name> <function-name>\n";
        return 1;
    }

    try
    {
        const std::filesystem::path pexDirectory = argv[1];
        const std::string scriptName = argv[2];
        const std::string functionName = argv[3];
        const std::vector<std::string> paths = {
            (pexDirectory / (Lower(scriptName) + ".pex")).string(),
            (pexDirectory / "quest.pex").string(),
            (pexDirectory / "form.pex").string(),
            (pexDirectory / "actor.pex").string(),
            (pexDirectory / "objectreference.pex").string(),
            (pexDirectory / "alias.pex").string(),
            (pexDirectory / "referencealias.pex").string(),
        };
        for (const auto& path : paths)
            if (!std::filesystem::exists(path)) throw std::runtime_error("missing PEX: " + path);

        std::vector<std::string> runtimeErrors;
        auto sink = std::make_shared<spdlog::sinks::callback_sink_mt>(
            [&runtimeErrors](const spdlog::details::log_msg& acMessage) {
                if (acMessage.level >= spdlog::level::err)
                    runtimeErrors.emplace_back(acMessage.payload.data(), acMessage.payload.size());
            });
        auto logger = std::make_shared<spdlog::logger>("papyrus-sim", sink);
        logger->set_level(spdlog::level::err);
        spdlog::set_default_logger(logger);

        Reader reader(paths);
        auto scripts = reader.GetSourceStructures();
        auto script = std::find_if(scripts.begin(), scripts.end(), [&](const auto& aScript) {
            return Lower(aScript->source) == Lower(scriptName);
        });
        if (script == scripts.end()) throw std::runtime_error("target script was not parsed");
        const auto* pFunction = FindFunction(**script, functionName);
        if (!pFunction) throw std::runtime_error("target function was not found");
        const auto unsupported = FindUnsupportedCalls(*pFunction);
        if (!unsupported.empty())
        {
            std::string names;
            for (const auto& name : unsupported) names += (names.empty() ? "" : ",") + name;
            throw std::runtime_error("unimplemented direct calls: " + names);
        }

        VirtualMachine vm(scripts);
        Simulation simulation;
        RegisterNatives(vm, simulation);
        auto variables = std::make_shared<VariableStore>(**script);
        auto quest = std::make_shared<ModelObject>("Skyrim.esm:0003372B", "Quest");
        vm.AddObject(quest, {{scriptName, variables}});
        std::vector<VarValue> arguments;
        vm.CallMethod(quest.get(), functionName.c_str(), arguments);

        if (!runtimeErrors.empty())
        {
            std::cout << "{\"passed\":false,\"error\":\"Papyrus VM reported runtime errors\",\"runtimeErrors\":[";
            for (size_t index = 0; index < runtimeErrors.size(); ++index)
            {
                if (index) std::cout << ',';
                std::cout << '"' << EscapeJson(runtimeErrors[index]) << '"';
            }
            std::cout << "]}\n";
            return 1;
        }

        std::cout << "{\"passed\":true,\"confidence\":\"mixed-executable-model\",\"script\":\"" << EscapeJson(scriptName)
                  << "\",\"function\":\"" << EscapeJson(functionName) << "\",\"questStages\":[";
        for (size_t index = 0; index < quest->Stages.size(); ++index)
        {
            if (index) std::cout << ',';
            std::cout << quest->Stages[index];
        }
        std::cout << "],\"playerInventory\":{";
        bool firstItem = true;
        for (const auto& [item, quantity] : simulation.Player->Inventory)
        {
            if (!firstItem) std::cout << ',';
            firstItem = false;
            std::cout << '"' << EscapeJson(item) << "\":" << quantity;
        }
        std::cout << "},\"playerControlsEnabled\":"
                  << (simulation.PlayerControlsEnabled ? "true" : "false") << ",\"events\":[";
        for (size_t index = 0; index < simulation.Events.size(); ++index)
        {
            if (index) std::cout << ',';
            std::cout << '"' << EscapeJson(simulation.Events[index]) << '"';
        }
        std::cout << "]}\n";
        return 0;
    }
    catch (const std::exception& exception)
    {
        std::cout << "{\"passed\":false,\"error\":\"" << EscapeJson(exception.what()) << "\"}\n";
        return 1;
    }
}
