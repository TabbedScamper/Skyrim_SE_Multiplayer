#include <Messages/ReviveRequest.h>

void ReviveRequest::SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    SerializeData(aWriter);
}

void ReviveRequest::DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    m_valid = false;
    try
    {
        ClientMessage::DeserializeRaw(aReader);
        DeserializeData(aReader);
        m_valid = Action != ReviveAction::Grant;
    }
    catch (...) {}
}
