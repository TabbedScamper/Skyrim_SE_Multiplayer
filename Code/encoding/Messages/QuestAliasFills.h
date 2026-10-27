#pragma once

#include <Structs/GameId.h>
#include <TiltedCore/Stl.hpp>

struct QuestAliasFill
{
    uint32_t AliasId{};
    // Zero is empty. UINT32_MAX as ModId identifies a replicated entity, not a local FF form.
    GameId Reference{};

    bool operator==(const QuestAliasFill&) const noexcept = default;
};

struct QuestAliasFills
{
    static constexpr size_t MaxAliases = 1024;
    static constexpr size_t MaxObjectives = 64;
    static constexpr uint32_t ServerReference = UINT32_MAX;

    enum Kind : uint8_t
    {
        SetStage, Startup, Shutdown, CompleteAll, Reconcile, Reset, State,
        Started, Stopped, Skipped, Unknown = 15
    };

    struct Objective
    {
        uint16_t Index{};
        uint8_t State{};
        bool operator==(const Objective&) const noexcept = default;
    };

    void Serialize(TiltedPhoques::Buffer::Writer& aWriter) const noexcept;
    bool Deserialize(TiltedPhoques::Buffer::Reader& aReader) noexcept;
    bool IsValid() const noexcept;
    bool operator==(const QuestAliasFills&) const noexcept = default;

    GameId Id{};
    uint64_t AuthorityEpoch{};
    uint64_t Sequence{};
    TiltedPhoques::Vector<QuestAliasFill> Entries;
    bool FillsIncluded{true};
    bool Running{true};
    bool HasStage{};
    uint16_t Stage{};
    uint8_t Status{};
    uint8_t ClientQuestType{};
    uint64_t TransactionId{};
    bool HasSeq{};
    uint8_t RecordKind{};
    uint32_t QuestSeq{};
    uint16_t QuestFlags{};
    uint16_t CurrentStageAfter{};
    bool HasRun{};
    bool Forced{};
    bool MaskOverflow{};
    uint8_t StageFlagsBefore{};
    uint8_t ItemCount{};
    uint32_t ItemMask{};
    uint16_t RunCount{};
    bool HasObjectives{};
    TiltedPhoques::Vector<Objective> Objectives;
};
