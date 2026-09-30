#include <Messages/DoorVoteRequest.h>

void DoorVoteRequest::SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    SerializeData(aWriter);
}

void DoorVoteRequest::DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    m_valid = false;
    try
    {
        ClientMessage::DeserializeRaw(aReader);
        DeserializeData(aReader);
        m_valid = Action <= DoorVoteAction::Failed || Action == DoorVoteAction::TestCell;
    }
    catch (...) {}
}
