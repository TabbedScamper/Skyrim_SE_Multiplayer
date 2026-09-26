#pragma once

#include <Structs/GameId.h>
#include <TiltedCore/Buffer.hpp>
#include <TiltedCore/Stl.hpp>

struct DialogueListenTopic
{
    uint32_t Index{}; // Native index, including entries omitted from the visible list.
    TiltedPhoques::String Text;
    bool Said{};
    bool operator==(const DialogueListenTopic&) const = default;
};

struct DialogueListenState
{
    static constexpr uint32_t None = UINT32_MAX;
    static constexpr size_t MaxTopics = 64;
    static constexpr size_t MaxText = 512;
    uint64_t Epoch{};
    uint64_t Session{};
    uint64_t Revision{};
    GameId Npc;
    uint32_t NpcServerId{None};
    bool Active{};
    TiltedPhoques::Vector<DialogueListenTopic> Topics;
    uint32_t Highlighted{None};
    uint32_t Chosen{None};
    uint64_t ChoiceSerial{};
    TiltedPhoques::String ChosenText;
    TiltedPhoques::String Subtitle;
    uint64_t LineSerial{};
    uint32_t DurationMs{}; // Native voice timer at line start. Hide events end skipped lines.
    uint64_t LineStartedTick{};
    uint32_t Fov{65};

    bool operator==(const DialogueListenState&) const = default;
    bool Valid() const noexcept;
    void Serialize(TiltedPhoques::Buffer::Writer&) const noexcept;
    bool Deserialize(TiltedPhoques::Buffer::Reader&) noexcept;
};
