#include <Messages/ClientReferencesMoveRequest.h>
#include <TiltedCore/Serialization.hpp>
#include <Structs/CheckedRead.h>

void ClientReferencesMoveRequest::SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    Serialization::WriteVarInt(aWriter, Tick);
    Serialization::WriteVarInt(aWriter, Updates.size());

    for (const auto& kvp : Updates)
    {
        Serialization::WriteVarInt(aWriter, kvp.first);
        kvp.second.Serialize(aWriter);
    }
}

void ClientReferencesMoveRequest::DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    m_valid = false;
    Updates.clear();
    try
    {
        ClientMessage::DeserializeRaw(aReader);

        Tick = CheckedRead::VarInt(aReader);
        const auto count = CheckedRead::VarInt(aReader);
        if (count > 4096 ||
            count > CheckedRead::RemainingBits(aReader) / 8)
            throw std::runtime_error("reference update count exceeds limit");

        for (auto i = 0u; i < count; ++i)
        {
            uint32_t serverId = CheckedRead::VarInt(aReader) & 0xFFFFFFFF;
            Updates[serverId].Deserialize(aReader);
        }
        m_valid = true;
    }
    catch (...)
    {
        Updates.clear();
    }
}
