#include <Messages/QuestItemState.h>
#include <Structs/CheckedRead.h>

bool QuestItemState::SameKey(const QuestItemState& aOther) const noexcept
{
    return BaseId == aOther.BaseId && QuestId == aOther.QuestId && AliasId == aOther.AliasId && QuestInstance == aOther.QuestInstance;
}

bool QuestItemState::IsValid() const noexcept
{
    return BaseId.BaseId && QuestId.BaseId && BaseId.ModId != UINT32_MAX &&
        QuestId.ModId != UINT32_MAX && ReferenceId.ModId != UINT32_MAX &&
        (ReferenceId.BaseId || !ReferenceId.ModId) && Count > 0 && Count <= 65535;
}

void QuestItemState::Serialize(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    for (const auto& id : {BaseId, QuestId, ReferenceId})
    {
        aWriter.WriteBits(id.ModId, 32);
        aWriter.WriteBits(id.BaseId, 32);
    }
    aWriter.WriteBits(AliasId, 32);
    aWriter.WriteBits(Count, 32);
    aWriter.WriteBits(QuestObject, 1);
    aWriter.WriteBits(Active, 1);
    aWriter.WriteBits(Revision, 64);
    aWriter.WriteBits(QuestInstance, 32);
}

bool QuestItemState::Deserialize(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    try
    {
        uint64_t value{};
        for (auto* id : {&BaseId, &QuestId, &ReferenceId})
        {
            CheckedRead::Bits(aReader, value, 32);
            id->ModId = static_cast<uint32_t>(value);
            CheckedRead::Bits(aReader, value, 32);
            id->BaseId = static_cast<uint32_t>(value);
        }
        CheckedRead::Bits(aReader, value, 32);
        AliasId = static_cast<uint32_t>(value);
        CheckedRead::Bits(aReader, value, 32);
        Count = static_cast<uint32_t>(value);
        QuestObject = CheckedRead::Bool(aReader);
        Active = CheckedRead::Bool(aReader);
        CheckedRead::Bits(aReader, Revision, 64);
        CheckedRead::Bits(aReader, value, 32);
        QuestInstance = static_cast<uint32_t>(value);
        return IsValid();
    }
    catch (...) { return false; }
}
