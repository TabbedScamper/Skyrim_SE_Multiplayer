#include <Messages/NotifyRevive.h>

void NotifyRevive::SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    SerializeData(aWriter);
}

void NotifyRevive::DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    m_valid = false;
    try
    {
        ServerMessage::DeserializeRaw(aReader);
        DeserializeData(aReader);
        m_valid = Action == ReviveAction::State || Action == ReviveAction::Grant;
    }
    catch (...) {}
}
