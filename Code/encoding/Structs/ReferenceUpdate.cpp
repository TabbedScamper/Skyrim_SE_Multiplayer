#include <Structs/ReferenceUpdate.h>
#include <TiltedCore/Serialization.hpp>
#include <stdexcept>

using TiltedPhoques::Serialization;

bool ReferenceUpdate::operator==(const ReferenceUpdate& acRhs) const noexcept
{
    return UpdatedMovement == acRhs.UpdatedMovement &&
        CombatTargetServerId == acRhs.CombatTargetServerId && ActionEvents == acRhs.ActionEvents &&
        EvaluatedPose == acRhs.EvaluatedPose && VisualBones == acRhs.VisualBones;
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
}

void ReferenceUpdate::Deserialize(TiltedPhoques::Buffer::Reader& aReader)
{
    UpdatedMovement.Deserialize(aReader);
    CombatTargetServerId = static_cast<uint32_t>(Serialization::ReadVarInt(aReader));

    const auto count = Serialization::ReadVarInt(aReader);

    ActionEvents.resize(count);

    for (auto i = 0u; i < count; ++i)
    {
        ActionEvents[i].ApplyDifferential(aReader);
    }
    EvaluatedPose.Deserialize(aReader);
    VisualBones.Deserialize(aReader);
}
