#include <Messages/BusyLockRequest.h>

void BusyLockRequest::SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    SerializeData(aWriter);
}

void BusyLockRequest::DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    m_valid = false;
    try
    {
        ClientMessage::DeserializeRaw(aReader);
        DeserializeData(aReader);
        m_valid = Action <= BusyLockAction::Release && Holder.empty();
    }
    catch (...) {}
}
