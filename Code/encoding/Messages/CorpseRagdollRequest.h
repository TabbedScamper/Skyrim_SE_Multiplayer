#pragma once

#include "Message.h"

// One ragdoll rigid body: position relative to the actor's reference
// position (game units) and world rotation as a quaternion (x, y, z, w).
struct CorpseRagdollBody
{
    float Position[3]{};
    float Rotation[4]{0.f, 0.f, 0.f, 1.f};
    // Native Havok units/s and radians/s; needed for feed-forward between samples.
    float LinearVelocity[3]{};
    float AngularVelocity[3]{};
    uint8_t MotionType{1};

    bool operator==(const CorpseRagdollBody& acRhs) const noexcept
    {
        return std::equal(std::begin(Position), std::end(Position), std::begin(acRhs.Position)) &&
               std::equal(std::begin(Rotation), std::end(Rotation), std::begin(acRhs.Rotation)) &&
               std::equal(std::begin(LinearVelocity), std::end(LinearVelocity), std::begin(acRhs.LinearVelocity)) &&
               std::equal(std::begin(AngularVelocity), std::end(AngularVelocity), std::begin(acRhs.AngularVelocity)) &&
               MotionType == acRhs.MotionType;
    }
};

// Owner -> server: a ragdoll or detached part, every body in native array order.
struct CorpseRagdollRequest final : ClientMessage
{
    static constexpr ClientOpcode Opcode = kCorpseRagdollRequest;
    static constexpr uint32_t kMaxBodies = 64;
    CorpseRagdollRequest() : ClientMessage(Opcode) {}

    void SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept override;
    void DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept override;
    bool operator==(const CorpseRagdollRequest& acRhs) const noexcept
    {
        return GetOpcode() == acRhs.GetOpcode() && ServerId == acRhs.ServerId && Tick == acRhs.Tick && Bodies == acRhs.Bodies &&
               Limb == acRhs.Limb && DismemberTick == acRhs.DismemberTick && Settled == acRhs.Settled && Active == acRhs.Active && Dying == acRhs.Dying &&
               std::equal(std::begin(Origin), std::end(Origin), std::begin(acRhs.Origin));
    }

    uint32_t ServerId{};
    // Shared-clock tick of the owner frame the bodies were read in.
    uint64_t Tick{};
    // 0 is the actor ragdoll, 1 is the detached head. Detached streams name their reliable event.
    uint32_t Limb{};
    uint64_t DismemberTick{};
    bool Settled{};
    bool Active{true};
    bool Dying{};
    // The owner's actor position; body positions are offsets from it. Placing them from the
    // receiver's own actor position followed that copy wherever its local ragdoll dragged it.
    float Origin[3]{};
    TiltedPhoques::Vector<CorpseRagdollBody> Bodies{};
};

namespace CorpseRagdollEncoding
{
void WriteBodies(TiltedPhoques::Buffer::Writer& aWriter, const TiltedPhoques::Vector<CorpseRagdollBody>& acBodies) noexcept;
bool ReadBodies(TiltedPhoques::Buffer::Reader& aReader, TiltedPhoques::Vector<CorpseRagdollBody>& aBodies) noexcept;
} // namespace CorpseRagdollEncoding
