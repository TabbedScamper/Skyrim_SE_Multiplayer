#include <chrono>
#include <mutex>
#include <optional>

#include <TiltedCore/Stl.hpp>
#include <TiltedCore/Allocator.hpp>
#include <TiltedCore/Buffer.hpp>
#include <TiltedCore/Serialization.hpp>
#include <TiltedCore/ScratchAllocator.hpp>
#include <TiltedCore/ViewBuffer.hpp>

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include <BuildInfo.h>
#include <Client.hpp>
#include <Packet.hpp>

#include <Messages/AuthenticationRequest.h>
#include <Messages/AuthenticationResponse.h>
#include <Messages/NotifyPartyInfo.h>
#include <Messages/NotifyPartyJoined.h>
#include <Messages/NotifyQuestUpdate.h>
#include <Messages/PartyCreateRequest.h>
#include <Messages/PartyReadyRequest.h>
#include <Messages/PartyStartRequest.h>
#include <Messages/PartySessionSettingsRequest.h>
#include <Messages/RequestQuestUpdate.h>
#include <Messages/ServerMessageFactory.h>

#include <functional>
#include <iostream>
#include <string>
#include <string_view>
#include <thread>

using namespace std::chrono_literals;
using TiltedPhoques::Buffer;
using TiltedPhoques::Packet;
using TiltedPhoques::PacketView;
using TiltedPhoques::ScopedAllocator;
using TiltedPhoques::ScratchAllocator;
using TiltedPhoques::ViewBuffer;

namespace
{
struct ProtocolBot final : TiltedPhoques::Client
{
    explicit ProtocolBot(
        std::string aName, const bool aUseSyntheticManifest = false, const uint8_t aSyntheticPluginHashByte = 0x5A,
        const uint8_t aSyntheticDeploymentHashByte = 0x6A, std::string aPassword = {})
        : Name(std::move(aName))
        , UseSyntheticManifest(aUseSyntheticManifest)
        , SyntheticPluginHashByte(aSyntheticPluginHashByte)
        , SyntheticDeploymentHashByte(aSyntheticDeploymentHashByte)
        , Password(std::move(aPassword))
    {
    }

    bool SendMessage(const ClientMessage& acMessage) const noexcept
    {
        static thread_local ScratchAllocator allocator(1 << 18);
        {
            ScopedAllocator scopedAllocator{allocator};
            Buffer buffer(1 << 16);
            Buffer::Writer writer(&buffer);
            writer.WriteBits(0, 8);
            acMessage.Serialize(writer);
            PacketView packet(reinterpret_cast<char*>(buffer.GetWriteData()), writer.Size());
            Send(&packet);
        }
        allocator.Reset();
        return true;
    }

    void OnConnected() override
    {
        TransportConnected = true;
        AuthenticationRequest request{};
        request.Version = BUILD_COMMIT;
        request.Username = Name;
        request.Token = Password;
        request.Level = 1;
        request.PlayerTime.TimeScale = 20.f;
        if (UseSyntheticManifest)
        {
            auto& entry = request.UserMods.ModList.emplace_back();
            entry.Filename = "ProtocolTest.esp";
            entry.Id = 0;
            entry.IsLite = false;
            entry.ContentSize = 17;
            entry.ContentSha256.fill(SyntheticPluginHashByte);
            entry.HasFingerprint = true;

            auto& deployment = request.UserMods.Deployment;
            deployment.Complete = true;
            deployment.AllFiles.FileCount = 1;
            deployment.AllFiles.TotalSize = 17;
            deployment.AllFiles.Root.fill(SyntheticDeploymentHashByte);
            for (size_t i = 0; i < deployment.Layers.size(); ++i)
                deployment.Layers[i].Root.fill(static_cast<uint8_t>(0xA0 + i));
            auto& assets = deployment.Layers[static_cast<size_t>(DeploymentManifest::Layer::Assets)];
            assets.FileCount = 1;
            assets.TotalSize = 17;
            assets.Root.fill(SyntheticDeploymentHashByte);
        }
        SendMessage(request);
    }

    void OnDisconnected(EDisconnectReason aReason) override
    {
        Disconnected = true;
        DisconnectReason = aReason;
    }

    void OnUpdate() override {}

    void OnConsume(const void* apData, uint32_t aSize) override
    {
        ServerMessageFactory factory;
        ViewBuffer buffer(static_cast<uint8_t*>(const_cast<void*>(apData)), aSize);
        Buffer::Reader reader(&buffer);
        auto message = factory.Extract(reader);
        if (!message)
        {
            ParseFailure = true;
            return;
        }

        switch (message->GetOpcode())
        {
        case AuthenticationResponse::Opcode:
        {
            const auto& response = static_cast<const AuthenticationResponse&>(*message);
            AuthenticationType = response.Type;
            PlayerId = response.PlayerId;
            CampaignId = response.CampaignId.c_str();
            CampaignRevision = response.CampaignRevision;
            AuthorityEpoch = response.AuthorityEpoch;
            Authenticated = response.Type == AuthenticationResponse::ResponseType::kAccepted;
            break;
        }
        case NotifyPartyJoined::Opcode:
        {
            const auto& joined = static_cast<const NotifyPartyJoined&>(*message);
            PartyJoined = true;
            IsLeader = joined.IsLeader;
            LeaderPlayerId = joined.LeaderPlayerId;
            PartySize = joined.PlayerIds.size();
            break;
        }
        case NotifyPartyInfo::Opcode:
        {
            const auto& info = static_cast<const NotifyPartyInfo&>(*message);
            IsLeader = info.IsLeader;
            LeaderPlayerId = info.LeaderPlayerId;
            PartySize = info.PlayerIds.size();
            ReadyCount = info.ReadyPlayerIds.size();
            CampaignMode = info.CampaignMode;
            SessionState = info.SessionState;
            StartEpoch = info.StartEpoch;
            LobbyOpen = info.LobbyOpen;
            PasswordProtected = info.PasswordProtected;
            break;
        }
        case NotifyQuestUpdate::Opcode:
        {
            const auto& update = static_cast<const NotifyQuestUpdate&>(*message);
            LastQuestStage = update.Stage;
            LastQuestTransactionId = update.TransactionId;
            LastQuestRevision = update.Revision;
            LastQuestAuthorityEpoch = update.AuthorityEpoch;
            QuestUpdateCount++;
            break;
        }
        default: break;
        }
    }

    std::string Name;
    std::string Password;
    bool UseSyntheticManifest{};
    uint8_t SyntheticPluginHashByte{};
    uint8_t SyntheticDeploymentHashByte{};
    bool TransportConnected{};
    bool Authenticated{};
    bool Disconnected{};
    bool ParseFailure{};
    bool PartyJoined{};
    bool IsLeader{};
    EDisconnectReason DisconnectReason{kNormal};
    AuthenticationResponse::ResponseType AuthenticationType{AuthenticationResponse::ResponseType::kWrongVersion};
    uint32_t PlayerId{};
    std::string CampaignId;
    uint64_t CampaignRevision{};
    uint64_t AuthorityEpoch{};
    uint32_t LeaderPlayerId{};
    size_t PartySize{};
    size_t ReadyCount{};
    uint8_t CampaignMode{};
    uint8_t SessionState{};
    uint64_t StartEpoch{};
    bool LobbyOpen{};
    bool PasswordProtected{};
    uint16_t LastQuestStage{};
    uint64_t LastQuestTransactionId{};
    uint64_t LastQuestRevision{};
    uint64_t LastQuestAuthorityEpoch{};
    uint32_t QuestUpdateCount{};
};

bool PumpUntil(std::initializer_list<ProtocolBot*> aBots, const std::function<bool()>& acPredicate, std::chrono::milliseconds aTimeout)
{
    const auto deadline = std::chrono::steady_clock::now() + aTimeout;
    while (std::chrono::steady_clock::now() < deadline)
    {
        for (auto* pBot : aBots)
            pBot->Update();
        if (acPredicate())
            return true;
        std::this_thread::sleep_for(1ms);
    }
    return false;
}

void PrintResult(const char* acScenario, bool aPassed, const std::string& acDetail)
{
    std::cout << "{\"scenario\":\"" << acScenario << "\",\"passed\":" << (aPassed ? "true" : "false") << ",\"detail\":\"" << acDetail << "\"}\n";
}
} // namespace

int main(int argc, char** argv)
{
    const std::string endpoint = argc > 1 ? argv[1] : "127.0.0.1:10578";
    const std::string scenario = argc > 2 ? argv[2] : "join";
    const bool useSyntheticManifest = argc > 3 && std::string_view(argv[3]) == "synthetic-plugin";

    ProtocolBot leader("HeadlessLeader", useSyntheticManifest);
    ProtocolBot follower(
        "HeadlessFollower", useSyntheticManifest, scenario == "mod-mismatch" ? 0x5B : 0x5A, scenario == "deployment-mismatch" ? 0x6B : 0x6A);
    if (!leader.Connect(endpoint) || !follower.Connect(endpoint))
    {
        PrintResult(scenario.c_str(), false, "transport connection could not be started");
        return 1;
    }

    if (scenario == "mod-mismatch")
    {
        const bool rejected =
            PumpUntil({&leader, &follower}, [&] { return leader.Authenticated && follower.AuthenticationType == AuthenticationResponse::ResponseType::kModsMismatch; }, 10s);
        PrintResult(
            "mod-mismatch", rejected,
            rejected ? "server rejected a same-name plugin with a different SHA-256 fingerprint" : "server accepted or failed to classify a different plugin fingerprint");
        return rejected ? 0 : 1;
    }

    if (scenario == "deployment-mismatch")
    {
        const bool rejected =
            PumpUntil({&leader, &follower}, [&] { return leader.Authenticated && follower.AuthenticationType == AuthenticationResponse::ResponseType::kModsMismatch; }, 10s);
        PrintResult(
            "deployment-mismatch", rejected,
            rejected ? "server rejected a different effective Data asset fingerprint" : "server accepted or failed to classify a different effective Data fingerprint");
        return rejected ? 0 : 1;
    }

    if (!PumpUntil({&leader, &follower}, [&] { return leader.Authenticated && follower.Authenticated; }, 10s))
    {
        PrintResult(scenario.c_str(), false, "both clients did not authenticate");
        return 1;
    }

    if (leader.CampaignId.empty() || leader.CampaignId != follower.CampaignId || leader.CampaignRevision != follower.CampaignRevision || leader.AuthorityEpoch == 0 ||
        leader.AuthorityEpoch != follower.AuthorityEpoch)
    {
        PrintResult(scenario.c_str(), false, "clients did not authenticate against one campaign watermark");
        return 1;
    }

    PartyCreateRequest createParty{};
    leader.SendMessage(createParty);
    if (!PumpUntil(
            {&leader, &follower},
            [&] { return leader.PartySize == 2 && follower.PartySize == 2 && leader.LeaderPlayerId == leader.PlayerId && follower.LeaderPlayerId == leader.PlayerId; }, 5s))
    {
        PrintResult(scenario.c_str(), false, "two-player auto-party state did not converge");
        return 1;
    }

    if (scenario == "lobby-ready-barrier")
    {
        PartyReadyRequest ready;
        ready.Ready = true;
        leader.SendMessage(ready);
        follower.SendMessage(ready);
        if (!PumpUntil({&leader, &follower}, [&] { return leader.ReadyCount == 2 && follower.ReadyCount == 2; }, 5s))
        {
            PrintResult("lobby-ready-barrier", false, "server did not converge both ready states");
            return 1;
        }

        PartyStartRequest unauthorized;
        unauthorized.Mode = PartyStartRequest::kContinue;
        unauthorized.Launch = true;
        unauthorized.CheckpointId = "forbidden-follower-checkpoint";
        follower.SendMessage(unauthorized);

        PartyStartRequest selection;
        selection.Mode = PartyStartRequest::kNew;
        leader.SendMessage(selection);
        if (!PumpUntil(
                {&leader, &follower},
                [&] { return leader.CampaignMode == PartyStartRequest::kNew && follower.CampaignMode == PartyStartRequest::kNew && leader.SessionState == 0 && follower.SessionState == 0; }, 5s))
        {
            PrintResult("lobby-ready-barrier", false, "follower could launch or campaign selection did not converge");
            return 1;
        }

        selection.Launch = true;
        leader.SendMessage(selection);
        const bool started = PumpUntil(
            {&leader, &follower},
            [&] { return leader.SessionState == 1 && follower.SessionState == 1 && leader.StartEpoch > 0 && leader.StartEpoch == follower.StartEpoch; }, 5s);
        PrintResult(
            "lobby-ready-barrier", started,
            started ? "server enforced host-only launch and released both ready clients on one start epoch" : "ready clients did not converge on one start epoch");
        return started ? 0 : 1;
    }

    if (scenario == "session-access-authority")
    {
        PartySessionSettingsRequest settings;
        settings.Open = true;
        settings.Password = "headless-secret";
        leader.SendMessage(settings);
        if (!PumpUntil({&leader, &follower}, [&] { return leader.LobbyOpen && follower.LobbyOpen && leader.PasswordProtected && follower.PasswordProtected; }, 5s))
        {
            PrintResult("session-access-authority", false, "leader settings did not converge");
            return 1;
        }

        ProtocolBot wrongPassword("WrongPassword", useSyntheticManifest, 0x5A, 0x6A, "wrong");
        if (!wrongPassword.Connect(endpoint) || !PumpUntil({&leader, &follower, &wrongPassword},
                [&] { return wrongPassword.AuthenticationType == AuthenticationResponse::ResponseType::kWrongPassword; }, 5s))
        {
            PrintResult("session-access-authority", false, "server did not reject an incorrect session password");
            return 1;
        }
        wrongPassword.Close();

        ProtocolBot correctPassword("CorrectPassword", useSyntheticManifest, 0x5A, 0x6A, "headless-secret");
        if (!correctPassword.Connect(endpoint) || !PumpUntil({&leader, &follower, &correctPassword}, [&] { return correctPassword.Authenticated; }, 5s))
        {
            PrintResult("session-access-authority", false, "server did not accept the correct session password");
            return 1;
        }
        correctPassword.Close();

        settings.Open = false;
        settings.Password.clear();
        follower.SendMessage(settings);
        std::this_thread::sleep_for(100ms);
        leader.Update();
        follower.Update();
        const bool rejectedFollower = leader.LobbyOpen && follower.LobbyOpen && leader.PasswordProtected && follower.PasswordProtected;

        leader.SendMessage(settings); // restore the shared test server for following scenarios
        const bool reset = PumpUntil({&leader, &follower}, [&] { return !leader.LobbyOpen && !follower.LobbyOpen && !leader.PasswordProtected && !follower.PasswordProtected; }, 5s);
        const bool passed = rejectedFollower && reset;
        PrintResult("session-access-authority", passed,
            passed ? "host-only access changes converged; wrong password was rejected and correct password accepted" : "session access authority or password enforcement failed");
        return passed ? 0 : 1;
    }

    if (scenario == "join")
    {
        PrintResult("join", true, "two clients authenticated and converged on one leader-owned party");
        return 0;
    }

    if (scenario == "leader-handoff")
    {
        leader.Close();
        const bool reassigned = PumpUntil({&leader, &follower}, [&] { return follower.PartySize == 1 && follower.IsLeader && follower.LeaderPlayerId == follower.PlayerId; }, 5s);
        PrintResult("leader-handoff", reassigned, reassigned ? "remaining player became leader after host disconnected" : "party did not converge after host disconnected");
        return reassigned ? 0 : 1;
    }

    if (scenario == "follower-reconnect")
    {
        follower.Close();
        if (!PumpUntil({&leader, &follower}, [&] { return leader.PartySize == 1; }, 5s))
        {
            PrintResult("follower-reconnect", false, "leader did not observe follower departure");
            return 1;
        }

        ProtocolBot replacement("HeadlessFollowerReconnect", useSyntheticManifest);
        if (!replacement.Connect(endpoint) ||
            !PumpUntil(
                {&leader, &replacement},
                [&] { return replacement.Authenticated && leader.PartySize == 2 && replacement.PartySize == 2 && replacement.LeaderPlayerId == leader.PlayerId; }, 10s))
        {
            PrintResult("follower-reconnect", false, "reconnected follower did not autojoin the existing party");
            return 1;
        }
        PrintResult("follower-reconnect", true, "replacement client reauthenticated and rejoined the leader party");
        return 0;
    }

    if (scenario == "quest-authority-audit")
    {
        RequestQuestUpdate leaderUpdate{};
        leaderUpdate.Id = {0, 0x3372B};
        leaderUpdate.Stage = 10;
        leaderUpdate.Status = RequestQuestUpdate::StageUpdate;
        leaderUpdate.ClientQuestType = 1;
        leaderUpdate.TransactionId = 1;
        leader.SendMessage(leaderUpdate);
        if (!PumpUntil(
                {&leader, &follower},
                [&]
                {
                    return follower.LastQuestStage == 10 && follower.LastQuestTransactionId == leaderUpdate.TransactionId &&
                           follower.LastQuestRevision > follower.CampaignRevision && follower.LastQuestAuthorityEpoch == follower.AuthorityEpoch;
                },
                3s))
        {
            PrintResult("quest-authority-audit", false, "leader quest update did not reach follower");
            return 1;
        }

        RequestQuestUpdate followerUpdate = leaderUpdate;
        followerUpdate.Stage = 20;
        followerUpdate.TransactionId = 2;
        follower.SendMessage(followerUpdate);
        const bool unauthorizedAccepted = PumpUntil({&leader, &follower}, [&] { return leader.LastQuestStage == 20; }, 1500ms);
        PrintResult(
            "quest-authority-audit", !unauthorizedAccepted,
            unauthorizedAccepted ? "GAP: server accepted and broadcast a follower-authored quest stage" : "server rejected the follower-authored quest stage");
        return unauthorizedAccepted ? 2 : 0;
    }

    PrintResult(scenario.c_str(), false, "unknown scenario");
    return 1;
}
