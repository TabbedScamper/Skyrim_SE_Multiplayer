#include <Messages/DialogueListenState.h>
#include <Structs/CheckedRead.h>
#include <algorithm>

namespace
{
uint32_t Read32(TiltedPhoques::Buffer::Reader& aReader)
{
    const auto value = CheckedRead::VarInt(aReader);
    if (value > UINT32_MAX) throw std::runtime_error("dialogue integer overflow");
    return static_cast<uint32_t>(value);
}
void Text(TiltedPhoques::Buffer::Writer& aWriter, const TiltedPhoques::String& aText)
{
    TiltedPhoques::Serialization::WriteVarInt(aWriter, aText.size());
    aWriter.WriteBytes(reinterpret_cast<const uint8_t*>(aText.data()), aText.size());
}
void Text(TiltedPhoques::Buffer::Reader& aReader, TiltedPhoques::String& aText, size_t aMax)
{
    const auto count = CheckedRead::VarInt(aReader);
    if (count > aMax || count > CheckedRead::RemainingBits(aReader) / 8)
        throw std::runtime_error("dialogue text limit");
    aText.resize(static_cast<size_t>(count));
    CheckedRead::Bytes(aReader, reinterpret_cast<uint8_t*>(aText.data()), aText.size());
}
}

bool DialogueListenState::Valid() const noexcept
{
    if (Topics.size() > MaxTopics || Subtitle.size() > 4096 || ChosenText.size() > MaxText ||
        DurationMs > 300000 || Fov < 1 || Fov > 179 || (Active && (!Session || NpcServerId == None)))
        return false;
    uint32_t previous = None;
    for (const auto& topic : Topics)
    {
        if (topic.Text.size() > MaxText || topic.Index == None ||
            (previous != None && topic.Index <= previous)) return false;
        previous = topic.Index;
    }
    return Highlighted == None || std::any_of(Topics.begin(), Topics.end(),
        [&](const auto& topic) { return topic.Index == Highlighted; });
}

void DialogueListenState::Serialize(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    using S = TiltedPhoques::Serialization;
    S::WriteVarInt(aWriter, Epoch); S::WriteVarInt(aWriter, Session); S::WriteVarInt(aWriter, Revision);
    Npc.Serialize(aWriter); S::WriteVarInt(aWriter, NpcServerId); S::WriteBool(aWriter, Active);
    S::WriteVarInt(aWriter, Topics.size());
    for (const auto& topic : Topics)
    {
        S::WriteVarInt(aWriter, topic.Index); Text(aWriter, topic.Text); S::WriteBool(aWriter, topic.Said);
    }
    S::WriteVarInt(aWriter, Highlighted); S::WriteVarInt(aWriter, Chosen);
    S::WriteVarInt(aWriter, ChoiceSerial); Text(aWriter, ChosenText); Text(aWriter, Subtitle);
    S::WriteVarInt(aWriter, LineSerial); S::WriteVarInt(aWriter, DurationMs);
    S::WriteVarInt(aWriter, LineStartedTick); S::WriteVarInt(aWriter, Fov);
}

bool DialogueListenState::Deserialize(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    try
    {
        Epoch = CheckedRead::VarInt(aReader); Session = CheckedRead::VarInt(aReader);
        Revision = CheckedRead::VarInt(aReader);
        Npc.BaseId = Read32(aReader); Npc.ModId = Read32(aReader);
        NpcServerId = Read32(aReader); Active = CheckedRead::Bool(aReader);
        const auto count = Read32(aReader);
        if (count > MaxTopics) return false;
        Topics.clear();
        for (uint32_t i = 0; i < count; ++i)
        {
            DialogueListenTopic topic;
            topic.Index = Read32(aReader); Text(aReader, topic.Text, MaxText);
            topic.Said = CheckedRead::Bool(aReader); Topics.push_back(std::move(topic));
        }
        Highlighted = Read32(aReader); Chosen = Read32(aReader);
        ChoiceSerial = CheckedRead::VarInt(aReader); Text(aReader, ChosenText, MaxText);
        Text(aReader, Subtitle, 4096); LineSerial = CheckedRead::VarInt(aReader);
        DurationMs = Read32(aReader); LineStartedTick = CheckedRead::VarInt(aReader); Fov = Read32(aReader);
        return Valid();
    }
    catch (...) { return false; }
}
