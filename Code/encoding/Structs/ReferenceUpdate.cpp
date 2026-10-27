#include <Structs/ReferenceUpdate.h>
#include <TiltedCore/Serialization.hpp>
#include <Structs/CheckedRead.h>

using TiltedPhoques::Serialization;

bool ReferenceUpdate::operator==(const ReferenceUpdate& acRhs) const noexcept
{
    return UpdatedMovement == acRhs.UpdatedMovement &&
        CombatTargetServerId == acRhs.CombatTargetServerId && ActionEvents == acRhs.ActionEvents &&
        EvaluatedPose == acRhs.EvaluatedPose && VisualBones == acRhs.VisualBones && SampleAge == acRhs.SampleAge;
}

bool ReferenceUpdate::operator!=(const ReferenceUpdate& acRhs) const noexcept
{
    return !this->operator==(acRhs);
}

void ReferenceUpdate::Serialize(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    UpdatedMovement.Serialize(aWriter);
    Serialization::WriteVarInt(aWriter, CombatTargetServerId);

    Serialization::WriteVarInt(aWriter, ActionEvents.size());

    for (auto& entry : ActionEvents)
    {
        entry.GenerateDifferential(ActionEvent{}, aWriter);
    }
    EvaluatedPose.Serialize(aWriter);
    VisualBones.Serialize(aWriter);
    Serialization::WriteVarInt(aWriter, SampleAge);
}

void ReferenceUpdate::Deserialize(TiltedPhoques::Buffer::Reader& aReader)
{
    UpdatedMovement.Deserialize(aReader);
    CombatTargetServerId = static_cast<uint32_t>(CheckedRead::VarInt(aReader));

    const auto count = CheckedRead::VarInt(aReader);

    if (count > 4096 || count > CheckedRead::RemainingBits(aReader) / 16)
        throw std::runtime_error("action event count exceeds limit");
    ActionEvents.resize(static_cast<size_t>(count));

    for (auto i = 0u; i < count; ++i)
    {
        ActionEvents[i].ApplyDifferential(aReader);
    }
    EvaluatedPose.Deserialize(aReader);
    VisualBones.Deserialize(aReader);
    const auto age = CheckedRead::VarInt(aReader);
    if (age > 60000)
        throw std::runtime_error("reference update sample age exceeds limit");
    SampleAge = static_cast<uint32_t>(age);
}
