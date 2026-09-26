#include <Messages/NotifyCorpseRagdoll.h>
#include <TiltedCore/Serialization.hpp>
#include <Structs/CheckedRead.h>
#include <cmath>
#include <limits>

void NotifyCorpseRagdoll::SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
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

void NotifyCorpseRagdoll::DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept
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
