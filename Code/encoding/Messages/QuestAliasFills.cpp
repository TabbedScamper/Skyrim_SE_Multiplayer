#include <Messages/QuestAliasFills.h>
#include <Structs/CheckedRead.h>

bool QuestAliasFills::IsValid() const noexcept
{
    if (!Id.BaseId || Id.ModId == ServerReference || !Sequence || Entries.size() > MaxAliases ||
        (HasStage && (Status > 2 || !TransactionId)) || HasSeq != HasStage ||
        (!FillsIncluded && !Entries.empty()) || HasObjectives != !Objectives.empty() ||
        Objectives.size() > MaxObjectives)
        return false;

    if (HasSeq)
    {
        const bool runKind = RecordKind <= CompleteAll;
        if (!QuestSeq || RecordKind > Skipped || RecordKind == Reconcile ||
            HasRun != runKind || (HasRun && !RunCount) ||
            (!HasRun && (Forced || MaskOverflow || ItemCount || ItemMask || RunCount || StageFlagsBefore)) ||
            (Status == 0 && !HasRun && RecordKind != Reset && RecordKind != State && RecordKind != Skipped) ||
            (RecordKind == Started && Status != 1) || (RecordKind == Stopped && Status != 2) ||
            (runKind && Status != 0))
            return false;
    }
    else if (HasRun || HasObjectives || QuestSeq || RecordKind)
        return false;

    for (size_t i = 0; i < Entries.size(); ++i)
    {
        const auto& entry = Entries[i];
        if ((i && Entries[i - 1].AliasId >= entry.AliasId) ||
            (entry.Reference && !entry.Reference.BaseId))
            return false;
    }
    for (size_t i = 1; i < Objectives.size(); ++i)
        if (Objectives[i - 1].Index >= Objectives[i].Index)
            return false;
    return true;
}

void QuestAliasFills::Serialize(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    aWriter.WriteBits(Id.ModId, 32);
    aWriter.WriteBits(Id.BaseId, 32);
    aWriter.WriteBits(AuthorityEpoch, 64);
    aWriter.WriteBits(Sequence, 64);
    aWriter.WriteBits(Running, 1);
    aWriter.WriteBits(HasStage, 1);
    if (HasStage)
    {
        aWriter.WriteBits(Stage, 16);
        aWriter.WriteBits(Status, 8);
        aWriter.WriteBits(ClientQuestType, 8);
        aWriter.WriteBits(TransactionId, 64);
    }
    aWriter.WriteBits(Entries.size(), 16);
    for (const auto& entry : Entries)
    {
        aWriter.WriteBits(entry.AliasId, 32);
        aWriter.WriteBits(entry.Reference.ModId, 32);
        aWriter.WriteBits(entry.Reference.BaseId, 32);
    }
    aWriter.WriteBits(FillsIncluded, 1);
    aWriter.WriteBits(HasSeq, 1);
    if (HasSeq)
    {
        aWriter.WriteBits(RecordKind, 4);
        aWriter.WriteBits(QuestSeq, 32);
        aWriter.WriteBits(QuestFlags, 16);
        aWriter.WriteBits(CurrentStageAfter, 16);
        aWriter.WriteBits(HasRun, 1);
        if (HasRun)
        {
            aWriter.WriteBits(Forced, 1);
            aWriter.WriteBits(MaskOverflow, 1);
            aWriter.WriteBits(StageFlagsBefore, 8);
            aWriter.WriteBits(ItemCount, 8);
            aWriter.WriteBits(ItemMask, 32);
            aWriter.WriteBits(RunCount, 16);
        }
        aWriter.WriteBits(HasObjectives, 1);
        if (HasObjectives)
        {
            aWriter.WriteBits(Objectives.size(), 8);
            for (const auto& objective : Objectives)
            {
                aWriter.WriteBits(objective.Index, 16);
                aWriter.WriteBits(objective.State, 8);
            }
        }
    }
}

bool QuestAliasFills::Deserialize(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    try
    {
        uint64_t value{};
        CheckedRead::Bits(aReader, value, 32);
        Id.ModId = static_cast<uint32_t>(value);
        CheckedRead::Bits(aReader, value, 32);
        Id.BaseId = static_cast<uint32_t>(value);
        CheckedRead::Bits(aReader, AuthorityEpoch, 64);
        CheckedRead::Bits(aReader, Sequence, 64);
        Running = CheckedRead::Bool(aReader);
        HasStage = CheckedRead::Bool(aReader);
        if (HasStage)
        {
            CheckedRead::Bits(aReader, value, 16);
            Stage = static_cast<uint16_t>(value);
            CheckedRead::Bits(aReader, value, 8);
            Status = static_cast<uint8_t>(value);
            CheckedRead::Bits(aReader, value, 8);
            ClientQuestType = static_cast<uint8_t>(value);
            CheckedRead::Bits(aReader, TransactionId, 64);
        }
        CheckedRead::Bits(aReader, value, 16);
        if (value > MaxAliases || CheckedRead::RemainingBits(aReader) < value * 96)
            return false;
        Entries.resize(static_cast<size_t>(value));
        for (auto& entry : Entries)
        {
            CheckedRead::Bits(aReader, value, 32);
            entry.AliasId = static_cast<uint32_t>(value);
            CheckedRead::Bits(aReader, value, 32);
            entry.Reference.ModId = static_cast<uint32_t>(value);
            CheckedRead::Bits(aReader, value, 32);
            entry.Reference.BaseId = static_cast<uint32_t>(value);
        }
        FillsIncluded = CheckedRead::Bool(aReader);
        HasSeq = CheckedRead::Bool(aReader);
        RecordKind = 0;
        QuestSeq = 0;
        QuestFlags = 0;
        CurrentStageAfter = 0;
        HasRun = false;
        Forced = false;
        MaskOverflow = false;
        StageFlagsBefore = 0;
        ItemCount = 0;
        ItemMask = 0;
        RunCount = 0;
        HasObjectives = false;
        Objectives.clear();
        if (HasSeq)
        {
            CheckedRead::Bits(aReader, value, 4);
            RecordKind = static_cast<uint8_t>(value);
            CheckedRead::Bits(aReader, value, 32);
            QuestSeq = static_cast<uint32_t>(value);
            CheckedRead::Bits(aReader, value, 16);
            QuestFlags = static_cast<uint16_t>(value);
            CheckedRead::Bits(aReader, value, 16);
            CurrentStageAfter = static_cast<uint16_t>(value);
            HasRun = CheckedRead::Bool(aReader);
            if (HasRun)
            {
                Forced = CheckedRead::Bool(aReader);
                MaskOverflow = CheckedRead::Bool(aReader);
                CheckedRead::Bits(aReader, value, 8);
                StageFlagsBefore = static_cast<uint8_t>(value);
                CheckedRead::Bits(aReader, value, 8);
                ItemCount = static_cast<uint8_t>(value);
                CheckedRead::Bits(aReader, value, 32);
                ItemMask = static_cast<uint32_t>(value);
                CheckedRead::Bits(aReader, value, 16);
                RunCount = static_cast<uint16_t>(value);
            }
            HasObjectives = CheckedRead::Bool(aReader);
            if (HasObjectives)
            {
                CheckedRead::Bits(aReader, value, 8);
                if (value > MaxObjectives || CheckedRead::RemainingBits(aReader) < value * 24)
                    return false;
                Objectives.resize(static_cast<size_t>(value));
                for (auto& objective : Objectives)
                {
                    CheckedRead::Bits(aReader, value, 16);
                    objective.Index = static_cast<uint16_t>(value);
                    CheckedRead::Bits(aReader, value, 8);
                    objective.State = static_cast<uint8_t>(value);
                }
            }
        }
        return IsValid();
    }
    catch (...)
    {
        return false;
    }
}
