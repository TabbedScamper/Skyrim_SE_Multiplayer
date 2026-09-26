#include <Messages/RequestQuestAliasFills.h>

void RequestQuestAliasFills::SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    Fills.Serialize(aWriter);
}

void RequestQuestAliasFills::DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    m_valid = Fills.Deserialize(aReader);
}
