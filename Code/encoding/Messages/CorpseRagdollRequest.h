#pragma once

#include "Message.h"

// One ragdoll rigid body of a settled corpse: position relative to the actor's reference
// position (game units) and world rotation as a quaternion (x, y, z, w).
struct CorpseRagdollBody
{
    float Position[3]{};
    float Rotation[4]{0.f, 0.f, 0.f, 1.f};

    bool operator==(const CorpseRagdollBody& acRhs) const noexcept
    {
        return std::equal(std::begin(Position), std::end(Position), std::begin(acRhs.Position)) &&
               std::equal(std::begin(Rotation), std::end(Rotation), std::begin(acRhs.Rotation));
    }
};

// Owner -> server: the settled ragdoll of a corpse the sender owns, every body in array order.
struct CorpseRagdollRequest final : ClientMessage
{
    static constexpr ClientOpcode Opcode = kCorpseRagdollRequest;
    static constexpr uint32_t kMaxBodies = 64;
    CorpseRagdollRequest() : ClientMessage(Opcode) {}

    void SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept override;
    void DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept override;
    bool operator==(const CorpseRagdollRequest& acRhs) const noexcept
    {
        return GetOpcode() == acRhs.GetOpcode() && ServerId == acRhs.ServerId && Bodies == acRhs.Bodies;
    }

    uint32_t ServerId{};
    TiltedPhoques::Vector<CorpseRagdollBody> Bodies{};
};

namespace CorpseRagdollEncoding
{
void WriteBodies(TiltedPhoques::Buffer::Writer& aWriter, const TiltedPhoques::Vector<CorpseRagdollBody>& acBodies) noexcept;
void ReadBodies(TiltedPhoques::Buffer::Reader& aReader, TiltedPhoques::Vector<CorpseRagdollBody>& aBodies) noexcept;
} // namespace CorpseRagdollEncoding
