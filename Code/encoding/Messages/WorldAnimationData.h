#pragma once
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

// Transport for AnimationStream's values, NOT an engine memory image. Strings
// cross the wire by value; no pointers, handles or save-game string-table IDs.
namespace WorldAnimationData
{
inline constexpr size_t MaximumBytes = 32768;
inline constexpr uint32_t MaximumEntries = 1024;
enum Type : uint8_t { Transform = 1, Quaternion, Vector4, Vector3, Float, Bool, Int16, UInt16, Int32, UInt32, String };
inline constexpr size_t Sizes[] = {0, 48, 16, 16, 12, 4, 1, 2, 2, 4, 4, 0};

struct Reader
{
    const std::vector<uint8_t>& Data;
    size_t Position{};
    bool Good{true};
    constexpr bool Token(Type type, size_t size)
    {
        if (!Good || Position >= Data.size() || Data[Position++] != type || size > Data.size() - Position)
            return Good = false;
        Position += size;
        return true;
    }
    bool Read(Type type, void* output, size_t size)
    {
        if (!Token(type, size)) return false;
        if (output) std::memcpy(output, Data.data() + Position - size, size);
        return true;
    }
    constexpr uint32_t Word(Type type, size_t size)
    {
        if (!Token(type, size)) return 0;
        uint32_t value{};
        for (size_t i = 0; i < size; ++i) value |= uint32_t(Data[Position - size + i]) << (8 * i);
        return value;
    }
    constexpr bool Text(std::string& output)
    {
        const auto size = Word(String, 2);
        if (!Good || size > 1024 || size > Data.size() - Position) return Good = false;
        output.clear();
        for (size_t i = 0; i < size; ++i) output.push_back(static_cast<char>(Data[Position++]));
        return Good = output.find('\0') == std::string::npos;
    }
    constexpr bool Skip(Type type)
    {
        if (type == String) { std::string value; return Text(value); }
        if (type == Bool) { const auto value = Word(type, 1); return Good = Good && value <= 1; }
        return Token(type, Sizes[type]);
    }
    constexpr uint32_t Count()
    {
        const auto count = Word(UInt32, 4);
        if (!Good || count > MaximumEntries) { Good = false; return 0; }
        return count;
    }
    constexpr bool Fields(std::initializer_list<Type> types)
    {
        for (auto type : types) if (!Skip(type)) return false;
        return Good;
    }
    constexpr bool Array(std::initializer_list<Type> types)
    {
        const auto count = Count();
        for (uint32_t i = 0; Good && i < count; ++i) Fields(types);
        return Good;
    }
};

// The grammar is N63368/N63601/N63602 (1.7.104), including every nested count.
// Validate BEFORE entering native loading: its stream callbacks do not abort
// allocation/loops on read failure. The engine is never handed unchecked bytes.
template<class VariableCheck>
inline constexpr bool Validate(const std::vector<uint8_t>& bytes, VariableCheck variableCheck)
{
    if (bytes.empty() || bytes.size() > MaximumBytes) return false;
    Reader r{bytes};
    const auto graphs = r.Count();
    if (!graphs || graphs > 16) return false;
    for (uint32_t graph = 0; r.Good && graph < graphs; ++graph)
    {
        std::string graphType;
        if (!r.Text(graphType) || graphType != "BShkbAnimationGraph") return false;
        r.Array({String, Int32, String, UInt32, UInt32}); // state machines
        r.Array({String, Float}); // clip time
        r.Array({String, Float});
        // Variables have a string terminator, followed by the next array count.
        for (uint32_t n = 0; r.Good; ++n)
        {
            std::string name;
            if (n > MaximumEntries || !r.Text(name)) { r.Good = false; break; }
            if (name == "$-NoMoreVariables-$") break;
            const auto word = r.Word(Bool, 1);
            if (!r.Good || word > 1 || !variableCheck(graph, name, word != 0) ||
                !r.Skip(word ? Int32 : Vector4)) { r.Good = false; break; }
        }
        r.Array({String, Bool});
        r.Array({String, String, String, Float, Int32, Int16, Int16, UInt16, Bool, Bool});
        r.Array({String, Float});
        r.Array({String, Float});
        const auto tracks = r.Count();
        for (uint32_t i = 0; r.Good && i < tracks; ++i)
        {
            r.Skip(String);
            const auto first = r.Count(), second = r.Count();
            for (uint32_t j = 0; r.Good && j < first + second; ++j) r.Skip(Vector4);
        }
        r.Array({String, Bool});
        r.Array({String, Transform, Float, Float});
        r.Array({String, Transform, Transform, Transform, Float, Bool, Bool});
        r.Array({String, String}); // queued graph events
        r.Array({String, Int16, Int32, Int32, Float, Bool, Float}); // Gamebryo generator
    }
    return r.Good && r.Position == bytes.size();
}
inline constexpr bool Valid(const std::vector<uint8_t>& bytes)
{
    return Validate(bytes, [](uint32_t, const std::string&, bool) { return true; });
}
}
