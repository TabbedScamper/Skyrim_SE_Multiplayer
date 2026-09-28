#pragma once
#include <Messages/WorldState.h>
#include <string_view>

// Synthetic AnimationStream values. This is a protocol fixture, not a captured
// native wall state or evidence that the native loader generated a matching pose.
inline constexpr std::vector<uint8_t> WorldAnimationFixture()
{
    using namespace WorldAnimationData;
    std::vector<uint8_t> data;
    auto value = [&](Type type, uint32_t word) {
        data.push_back(type);
        for (size_t i = 0; i < Sizes[type]; ++i) data.push_back(static_cast<uint8_t>(word >> (8 * i)));
    };
    auto text = [&](const char* name) {
        const auto size = std::string_view{name}.size();
        data.push_back(WorldAnimationData::String);
        data.push_back(static_cast<uint8_t>(size)); data.push_back(0);
        data.insert(data.end(), name, name + size);
    };
    value(UInt32, 1); text("BShkbAnimationGraph");
    value(UInt32, 2);
    for (const auto& state : {std::pair{"Behavior00", 5u}, std::pair{"Behavior01", 2u}})
    {
        text(state.first); value(Int32, state.second);
        text(state.second == 5 ? "State02" : "Opened");
        value(UInt32, 0); value(UInt32, 0);
    }
    value(UInt32, 0); value(UInt32, 0);
    text("$-NoMoreVariables-$");
    for (unsigned i = 0; i < 10; ++i) value(UInt32, 0);
    return data;
}
