#include <Messages/NotifyDoorVote.h>

void NotifyDoorVote::SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    SerializeData(aWriter);
}

void NotifyDoorVote::DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    m_valid = false;
    try
    {
        ServerMessage::DeserializeRaw(aReader);
        DeserializeData(aReader);
        m_valid = Action >= DoorVoteAction::State && ReadyCount <= TotalCount;
    }
    catch (...) {}
}
