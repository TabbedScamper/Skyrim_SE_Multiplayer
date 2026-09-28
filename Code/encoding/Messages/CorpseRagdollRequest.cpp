#include <Messages/CorpseRagdollRequest.h>
#include <TiltedCore/Serialization.hpp>
#include <Structs/CheckedRead.h>
#include <cmath>
#include <limits>

namespace CorpseRagdollEncoding
{
void WriteBodies(TiltedPhoques::Buffer::Writer& aWriter, const TiltedPhoques::Vector<CorpseRagdollBody>& acBodies) noexcept
{
    const auto count = static_cast<uint32_t>(acBodies.size());
    TiltedPhoques::Serialization::WriteVarInt(aWriter, count);
    if (count > CorpseRagdollRequest::kMaxBodies)
        return;
    for (uint32_t i = 0; i < count; ++i)
    {
        for (const float value : acBodies[i].Position)
            TiltedPhoques::Serialization::WriteFloat(aWriter, value);
        for (const float value : acBodies[i].Rotation)
            TiltedPhoques::Serialization::WriteFloat(aWriter, value);
        for (const float value : acBodies[i].LinearVelocity)
            TiltedPhoques::Serialization::WriteFloat(aWriter, value);
        for (const float value : acBodies[i].AngularVelocity)
            TiltedPhoques::Serialization::WriteFloat(aWriter, value);
        aWriter.WriteBits(acBodies[i].MotionType, 8);
    }
}

bool ReadBodies(TiltedPhoques::Buffer::Reader& aReader, TiltedPhoques::Vector<CorpseRagdollBody>& aBodies) noexcept
{
    uint64_t count{};
    try { count = CheckedRead::VarInt(aReader); }
    catch (...) { return false; }
    aBodies.clear();
    if (count > CorpseRagdollRequest::kMaxBodies || CheckedRead::RemainingBits(aReader) < count * (13 * 32 + 8))
        return false;
    aBodies.resize(count);
    for (auto& body : aBodies)
    {
        for (float& value : body.Position)
            value = TiltedPhoques::Serialization::ReadFloat(aReader);
        for (float& value : body.Rotation)
            value = TiltedPhoques::Serialization::ReadFloat(aReader);
        for (float& value : body.LinearVelocity)
            value = TiltedPhoques::Serialization::ReadFloat(aReader);
        for (float& value : body.AngularVelocity)
            value = TiltedPhoques::Serialization::ReadFloat(aReader);
        uint64_t motion{};
        aReader.ReadBits(motion, 8);
        body.MotionType = static_cast<uint8_t>(motion);
        const auto finiteVelocity = [](float v) { return std::isfinite(v) && std::abs(v) < 10000.f; };
        if (motion < 1 || motion > 6 ||
            !std::all_of(std::begin(body.LinearVelocity), std::end(body.LinearVelocity), finiteVelocity) ||
            !std::all_of(std::begin(body.AngularVelocity), std::end(body.AngularVelocity), finiteVelocity))
            return false;
        float norm = 0.f;
        for (float value : body.Rotation)
            norm += value * value;
        if (!std::isfinite(norm) || std::abs(norm - 1.f) > 0.01f ||
            !std::all_of(std::begin(body.Position), std::end(body.Position),
                [](float v) { return std::isfinite(v) && std::abs(v) < 10'000'000.f; }))
            return false;
    }
    return true;
}
} // namespace CorpseRagdollEncoding

void CorpseRagdollRequest::SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    TiltedPhoques::Serialization::WriteVarInt(aWriter, ServerId);
    aWriter.WriteBits(Tick, 64);
    TiltedPhoques::Serialization::WriteVarInt(aWriter, Limb);
    aWriter.WriteBits(DismemberTick, 64);
    aWriter.WriteBits(Settled, 1);
    aWriter.WriteBits(Active, 1);
    aWriter.WriteBits(Dying, 1);
    for (const float value : Origin)
        TiltedPhoques::Serialization::WriteFloat(aWriter, value);
    CorpseRagdollEncoding::WriteBodies(aWriter, Bodies);
}

void CorpseRagdollRequest::DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    m_valid = false;
    Bodies.clear();
    try
    {
        const auto id = CheckedRead::VarInt(aReader);
        CheckedRead::Bits(aReader, Tick, 64);
        const auto limb = CheckedRead::VarInt(aReader);
        CheckedRead::Bits(aReader, DismemberTick, 64);
        Settled = CheckedRead::Bool(aReader);
        Active = CheckedRead::Bool(aReader);
        Dying = CheckedRead::Bool(aReader);
        if (id > (std::numeric_limits<uint32_t>::max)() || limb > 1 || !Tick)
            return;
        ServerId = static_cast<uint32_t>(id);
        Limb = static_cast<uint32_t>(limb);
        if (CheckedRead::RemainingBits(aReader) < 3 * 32)
            return;
        for (float& value : Origin)
        {
            value = TiltedPhoques::Serialization::ReadFloat(aReader);
            if (!std::isfinite(value) || std::abs(value) >= 10'000'000.f)
                return;
        }
        m_valid = CorpseRagdollEncoding::ReadBodies(aReader, Bodies) &&
            (Active ? !Bodies.empty() : Bodies.empty()) &&
            (Limb ? DismemberTick && DismemberTick <= Tick && (!Active || Bodies.size() == 1) : !DismemberTick);
    }
    catch (...)
    {
        m_valid = false;
    }
}
