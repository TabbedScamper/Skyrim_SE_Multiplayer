#include <Messages/NotifyBusyLock.h>

void NotifyBusyLock::SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    SerializeData(aWriter);
}

void NotifyBusyLock::DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    m_valid = false;
    try
    {
        ServerMessage::DeserializeRaw(aReader);
        DeserializeData(aReader);
        m_valid = Action >= BusyLockAction::Granted;
    }
    catch (...) {}
}
