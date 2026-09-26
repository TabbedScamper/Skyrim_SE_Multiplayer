#include <Services/DiagnosticsService.h>

#include <World.h>
#include <Components.h>
#include <GameServer.h>
#include <Setting.h>

#include <ChatMessageTypes.h>
#include <Messages/NotifyChatMessageBroadcast.h>

#include <filesystem>
#include <fstream>
#include <sstream>

namespace
{
Console::StringSetting sDiagnosticsDirectory{
    "Diagnostics:sDirectory",
    "Directory shared with the local diagnostics MCP. Empty disables the bridge.",
    ""};

String JsonEscape(const String& acValue)
{
    String result;
    result.reserve(acValue.size() + 8);

    constexpr char cHex[] = "0123456789ABCDEF";
    for (const unsigned char c : acValue)
    {
        switch (c)
        {
        case '"': result += "\\\""; break;
        case '\\': result += "\\\\"; break;
        case '\b': result += "\\b"; break;
        case '\f': result += "\\f"; break;
        case '\n': result += "\\n"; break;
        case '\r': result += "\\r"; break;
        case '\t': result += "\\t"; break;
        default:
            if (c < 0x20)
            {
                result += "\\u00";
                result += cHex[(c >> 4) & 0xF];
                result += cHex[c & 0xF];
            }
            else
            {
                result += static_cast<char>(c);
            }
            break;
        }
    }

    return result;
}

Vector<String> SplitCommand(const String& acLine)
{
    Vector<String> fields;
    size_t start = 0;
    while (fields.size() < 3)
    {
        const auto separator = acLine.find('\t', start);
        if (separator == String::npos)
            break;

        fields.emplace_back(acLine.substr(start, separator - start));
        start = separator + 1;
    }
    fields.emplace_back(acLine.substr(start));
    return fields;
}

uint64_t UnixMilliseconds() noexcept
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}
} // namespace

DiagnosticsService::DiagnosticsService(World& aWorld, entt::dispatcher& aDispatcher) noexcept
    : m_world(aWorld)
    , m_updateConnection(aDispatcher.sink<UpdateEvent>().connect<&DiagnosticsService::OnUpdate>(this))
{
}

void DiagnosticsService::RecordPlayerMessage(const Player& acPlayer, const String& acMessage) const noexcept
{
    const String directory = sDiagnosticsDirectory.value();
    if (directory.empty())
        return;

    try
    {
        const auto timestamp = UnixMilliseconds();
        const auto standardRequestId = "player-" + std::to_string(acPlayer.GetId()) + "-" + std::to_string(timestamp);
        const String requestId = standardRequestId.c_str();
        const std::filesystem::path runtimeDirectory(directory.c_str());
        std::filesystem::create_directories(runtimeDirectory);

        std::ofstream output(runtimeDirectory / "telemetry.jsonl", std::ios::binary | std::ios::app);
        if (output)
        {
            output << "{\"type\":\"player_message\",\"request_id\":\"" << JsonEscape(requestId)
                   << "\",\"timestamp_ms\":" << timestamp << ",\"player_id\":" << acPlayer.GetId()
                   << ",\"player_name\":\"" << JsonEscape(acPlayer.GetUsername()) << "\",\"message\":\""
                   << JsonEscape(acMessage.substr(0, 500)) << "\"}\n";
            output.flush();
        }

        spdlog::info("Player report from {} ({}): {}", acPlayer.GetUsername(), acPlayer.GetId(), acMessage);
        BroadcastMessage("Report captured from " + acPlayer.GetUsername());
        WriteSnapshot(requestId, "player report: " + acMessage.substr(0, 300));
    }
    catch (const std::exception& exception)
    {
        spdlog::error("Diagnostics player report failed: {}", exception.what());
    }
}

void DiagnosticsService::OnUpdate(const UpdateEvent&) noexcept
{
    const auto now = std::chrono::steady_clock::now();
    if (now < m_nextPoll)
        return;

    m_nextPoll = now + std::chrono::milliseconds(100);
    ProcessMailbox();
}

void DiagnosticsService::ProcessMailbox() noexcept
{
    const String directory = sDiagnosticsDirectory.value();
    if (directory.empty())
        return;

    try
    {
        const std::filesystem::path runtimeDirectory(directory.c_str());
        std::filesystem::create_directories(runtimeDirectory);
        const auto commandPath = runtimeDirectory / "commands.tsv";

        if (!std::filesystem::exists(commandPath))
            return;

        const auto fileSize = static_cast<std::streamoff>(std::filesystem::file_size(commandPath));
        if (fileSize < m_commandOffset)
            m_commandOffset = 0;

        std::ifstream input(commandPath, std::ios::binary);
        if (!input)
            return;

        input.seekg(m_commandOffset);
        String line;
        while (std::getline(input, line))
        {
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            if (!line.empty())
                ProcessCommand(line);
        }

        m_commandOffset = fileSize;
    }
    catch (const std::exception& exception)
    {
        spdlog::error("Diagnostics mailbox failed: {}", exception.what());
    }
}

void DiagnosticsService::ProcessCommand(const String& acLine) noexcept
{
    const auto fields = SplitCommand(acLine);
    if (fields.size() != 4)
    {
        spdlog::warn("Ignored malformed diagnostics command");
        return;
    }

    const auto& command = fields[0];
    const auto& requestId = fields[1];
    const auto& value = fields[3];

    if (command == "message")
    {
        BroadcastMessage(value);
        spdlog::info("Diagnostics message delivered, request {}", requestId);
    }
    else if (command == "snapshot")
    {
        WriteSnapshot(requestId, value);
    }
    else if (command == "mark")
    {
        BroadcastMessage("Bug marked: " + value);
        WriteSnapshot(requestId, "bug: " + value);
    }
    else
    {
        spdlog::warn("Ignored unsupported diagnostics command '{}'", command);
    }
}

void DiagnosticsService::BroadcastMessage(const String& acMessage) const noexcept
{
    NotifyChatMessageBroadcast message{};
    message.MessageType = ChatMessageType::kSystemMessage;
    message.PlayerName = "[Report]";
    message.ChatMessage = "[Report] " + acMessage.substr(0, 400);
    GameServer::Get()->SendToPlayers(message);
}

void DiagnosticsService::WriteSnapshot(const String& acRequestId, const String& acReason) const noexcept
{
    const String directory = sDiagnosticsDirectory.value();
    if (directory.empty())
        return;

    try
    {
        const std::filesystem::path runtimeDirectory(directory.c_str());
        std::filesystem::create_directories(runtimeDirectory);
        std::ofstream output(runtimeDirectory / "telemetry.jsonl", std::ios::binary | std::ios::app);
        if (!output)
            return;

        std::ostringstream json;
        json << "{\"type\":\"snapshot\",\"request_id\":\"" << JsonEscape(acRequestId)
             << "\",\"reason\":\"" << JsonEscape(acReason) << "\",\"timestamp_ms\":" << UnixMilliseconds()
             << ",\"server_tick\":" << GameServer::Get()->GetTick() << ",\"players\":[";

        bool firstPlayer = true;
        for (Player* pPlayer : m_world.GetPlayerManager())
        {
            if (!firstPlayer)
                json << ',';
            firstPlayer = false;

            const auto& party = pPlayer->GetParty();
            const auto& cell = pPlayer->GetCellComponent();
            const auto* pParty = m_world.GetPartyService().GetPlayerParty(pPlayer);

            json << "{\"id\":" << pPlayer->GetId() << ",\"name\":\"" << JsonEscape(pPlayer->GetUsername())
                 << "\",\"level\":" << pPlayer->GetLevel() << ",\"is_leader\":"
                 << (m_world.GetPartyService().IsPlayerLeader(pPlayer) ? "true" : "false") << ",\"party_id\":";

            if (party.JoinedPartyId)
                json << *party.JoinedPartyId;
            else
                json << "null";

            json << ",\"party_leader_id\":";
            if (pParty)
                json << pParty->LeaderPlayerId;
            else
                json << "null";

            json << ",\"cell\":{\"mod\":" << cell.Cell.ModId << ",\"base\":" << cell.Cell.BaseId
                 << "},\"worldspace\":{\"mod\":" << cell.WorldSpaceId.ModId << ",\"base\":"
                 << cell.WorldSpaceId.BaseId << "},\"quests\":[";

            bool firstQuest = true;
            for (const auto& quest : pPlayer->GetQuestLogComponent().QuestContent.Entries)
            {
                if (!firstQuest)
                    json << ',';
                firstQuest = false;
                json << "{\"mod\":" << quest.Id.ModId << ",\"base\":" << quest.Id.BaseId << ",\"stage\":"
                     << quest.Stage << '}';
            }
            json << ']';

            if (const auto character = pPlayer->GetCharacter())
            {
                if (const auto* pMovement = m_world.try_get<MovementComponent>(*character))
                {
                    json << ",\"position\":[" << pMovement->Position.x << ',' << pMovement->Position.y << ','
                         << pMovement->Position.z << ']';
                }
            }
            json << '}';
        }

        json << "],\"actors\":[";
        bool firstActor = true;
        auto actors = m_world.view<CharacterComponent>();
        for (const auto entity : actors)
        {
            if (!firstActor)
                json << ',';
            firstActor = false;

            const auto& character = actors.get<CharacterComponent>(entity);
            json << "{\"server_id\":" << World::ToInteger(entity) << ",\"base\":{\"mod\":"
                 << character.BaseId.Id.ModId << ",\"base\":" << character.BaseId.Id.BaseId << "},\"dead\":"
                 << (character.IsDead() ? "true" : "false") << ",\"player\":"
                 << (character.IsPlayer() ? "true" : "false");

            if (const auto* pOwner = m_world.try_get<OwnerComponent>(entity))
            {
                json << ",\"owner_player_id\":";
                if (const auto* pPlayer = pOwner->GetOwner())
                    json << pPlayer->GetId();
                else
                    json << "null";
                json << ",\"ownership_epoch\":" << pOwner->OwnershipEpoch;
            }

            if (const auto* pCell = m_world.try_get<CellIdComponent>(entity))
            {
                json << ",\"cell\":{\"mod\":" << pCell->Cell.ModId << ",\"base\":" << pCell->Cell.BaseId << '}';
            }

            if (const auto* pMovement = m_world.try_get<MovementComponent>(entity))
            {
                json << ",\"position\":[" << pMovement->Position.x << ',' << pMovement->Position.y << ','
                     << pMovement->Position.z << ']';
            }
            json << '}';
        }
        json << "]}";

        output << json.str() << '\n';
        output.flush();
        spdlog::info("Diagnostics snapshot written, request {}", acRequestId);
    }
    catch (const std::exception& exception)
    {
        spdlog::error("Diagnostics snapshot failed: {}", exception.what());
    }
}
