#include <papyrus-vm/Reader.h>

#include <exception>
#include <iostream>
#include <map>
#include <string>
#include <vector>

namespace
{
std::string EscapeJson(const std::string& acValue)
{
    std::string result;
    result.reserve(acValue.size());
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
}

int main(int argc, char** argv)
{
    if (argc < 2)
    {
        std::cerr << "usage: SkyrimPapyrusProbe <script.pex> [script.pex...]\n";
        return 1;
    }

    try
    {
        std::vector<std::string> paths;
        for (int index = 1; index < argc; ++index)
            paths.emplace_back(argv[index]);

        Reader reader(paths);
        const auto scripts = reader.GetSourceStructures();
        std::cout << "{\"passed\":true,\"scriptCount\":" << scripts.size() << ",\"scripts\":[";
        bool firstScript = true;
        for (const auto& script : scripts)
        {
            if (!firstScript) std::cout << ',';
            firstScript = false;
            std::cout << "{\"source\":\"" << EscapeJson(script->source) << "\",\"objects\":[";
            bool firstObject = true;
            for (const auto& object : script->objectTable)
            {
                if (!firstObject) std::cout << ',';
                firstObject = false;
                size_t functionCount = 0;
                size_t nativeFunctionCount = 0;
                size_t instructionCount = 0;
                std::map<std::string, size_t> calls;
                for (const auto& state : object.states)
                {
                    for (const auto& function : state.functions)
                    {
                        ++functionCount;
                        nativeFunctionCount += function.function.IsNative() ? 1 : 0;
                        instructionCount += function.function.code.instructions.size();
                        for (const auto& instruction : function.function.code.instructions)
                        {
                            if ((instruction.op == FunctionCode::kOp_CallMethod ||
                                    instruction.op == FunctionCode::kOp_CallParent) &&
                                !instruction.args.empty())
                            {
                                const auto& name = instruction.args[0];
                                if (name.GetType() == VarValue::kType_String ||
                                    name.GetType() == VarValue::kType_Identifier)
                                    ++calls[static_cast<const char*>(name)];
                            }
                            else if (instruction.op == FunctionCode::kOp_CallStatic &&
                                instruction.args.size() >= 2)
                            {
                                const auto& className = instruction.args[0];
                                const auto& functionName = instruction.args[1];
                                if ((className.GetType() == VarValue::kType_String ||
                                        className.GetType() == VarValue::kType_Identifier) &&
                                    (functionName.GetType() == VarValue::kType_String ||
                                        functionName.GetType() == VarValue::kType_Identifier))
                                {
                                    ++calls[std::string(static_cast<const char*>(className)) + "." +
                                        static_cast<const char*>(functionName)];
                                }
                            }
                        }
                    }
                }
                std::cout << "{\"name\":\"" << EscapeJson(object.NameIndex)
                          << "\",\"parent\":\"" << EscapeJson(object.parentClassName)
                          << "\",\"stateCount\":" << object.states.size()
                          << ",\"functionCount\":" << functionCount
                          << ",\"nativeFunctionCount\":" << nativeFunctionCount
                          << ",\"instructionCount\":" << instructionCount
                          << ",\"calls\":{";
                bool firstCall = true;
                for (const auto& [name, count] : calls)
                {
                    if (!firstCall) std::cout << ',';
                    firstCall = false;
                    std::cout << '"' << EscapeJson(name) << "\":" << count;
                }
                std::cout << "}}";
            }
            std::cout << "]}";
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
