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
        // Hold/Cancel tell the downed player who is reviving them (the server forwards the reviver's hold).
        m_valid = Action == ReviveAction::State || Action == ReviveAction::Grant || Action == ReviveAction::Hold ||
            Action == ReviveAction::Cancel || Action == ReviveAction::Raise ||
            Action == ReviveAction::Wipe;
    }
    catch (...) {}
}
