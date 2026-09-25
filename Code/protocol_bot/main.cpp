#include <chrono>
#include <array>
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
#include <Messages/NotifyCameraState.h>
#include <Messages/CameraStateRequest.h>
#include <Messages/NotifySceneTimeline.h>
#include <Messages/SceneTimelineRequest.h>
#include <Messages/NotifyPhysicsReferencesMove.h>
#include <Messages/PhysicsReferencesMoveRequest.h>
#include <Messages/EnterInteriorCellRequest.h>
#include <Messages/AssignCharacterRequest.h>
#include <Messages/AssignCharacterResponse.h>
#include <Messages/CharacterSpawnRequest.h>
#include <Messages/RequestOwnershipClaim.h>
#include <Messages/NotifyOwnershipTransfer.h>
#include <Messages/PartyCreateRequest.h>
#include <Messages/PartyReadyRequest.h>
#include <Messages/PartyStartRequest.h>
#include <Messages/PartySessionSettingsRequest.h>
#include <Messages/RequestQuestUpdate.h>
#include <Messages/ServerMessageFactory.h>

#include <functional>
#include <iostream>
#include <memory>
#include <limits>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

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
        case NotifyCameraState::Opcode:
        {
            const auto& update = static_cast<const NotifyCameraState&>(*message);
            LastCameraSnapshot = update.Snapshot;
            CameraUpdateCount++;
            break;
        }
        case NotifySceneTimeline::Opcode:
        {
            const auto& update = static_cast<const NotifySceneTimeline&>(*message);
            LastSceneSnapshot = update.Snapshot;
            SceneUpdateCount++;
            break;
        }
        case NotifyPhysicsReferencesMove::Opcode:
        {
            const auto& update = static_cast<const NotifyPhysicsReferencesMove&>(*message);
            LastPhysicsTick = update.Tick;
            LastPhysicsEpoch = update.AuthorityEpoch;
            LastPhysicsCount = update.Updates.size();
            if (!update.Updates.empty())
                LastPhysicsBodyTransform = update.Updates.front().BodyTransform;
            PhysicsUpdateCount++;
            break;
        }
        case AssignCharacterResponse::Opcode:
        {
            const auto& response = static_cast<const AssignCharacterResponse&>(*message);
            LastAssignedServerId = response.ServerId;
            LastAssignedCookie = response.Cookie;
            LastAssignedOwner = response.Owner;
            LastAssignedEpoch = response.OwnershipEpoch;
            AssignmentCount++;
            break;
        }
        case NotifyOwnershipTransfer::Opcode:
        {
            const auto& transfer = static_cast<const NotifyOwnershipTransfer&>(*message);
            LastTransferServerId = transfer.ServerId;
            LastTransferOwnerId = transfer.OwnerPlayerId;
            LastTransferEpoch = transfer.OwnershipEpoch;
            OwnershipTransferCount++;
            break;
        }
        case CharacterSpawnRequest::Opcode:
        {
            const auto& spawn = static_cast<const CharacterSpawnRequest&>(*message);
            LastSpawnServerId = spawn.ServerId;
            LastSpawnEpoch = spawn.OwnershipEpoch;
            SpawnCount++;
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
    CameraStateSnapshot LastCameraSnapshot{};
    uint32_t CameraUpdateCount{};
    SceneTimelineSnapshot LastSceneSnapshot{};
    uint32_t SceneUpdateCount{};
    uint64_t LastPhysicsTick{};
    uint64_t LastPhysicsEpoch{};
    size_t LastPhysicsCount{};
    std::array<float, 16> LastPhysicsBodyTransform{};
    uint32_t PhysicsUpdateCount{};
    uint32_t LastAssignedServerId{};
    uint32_t LastAssignedCookie{};
    bool LastAssignedOwner{};
    uint32_t LastAssignedEpoch{};
    uint32_t AssignmentCount{};
    uint32_t LastTransferServerId{};
    uint32_t LastTransferOwnerId{};
    uint32_t LastTransferEpoch{};
    uint32_t OwnershipTransferCount{};
    uint32_t LastSpawnServerId{};
    uint32_t LastSpawnEpoch{};
    uint32_t SpawnCount{};
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

    if (scenario == "five-player-smoke")
    {
        std::vector<std::unique_ptr<ProtocolBot>> bots;
        bots.reserve(5);
        for (int i = 0; i < 5; ++i)
        {
            auto bot = std::make_unique<ProtocolBot>("ScalePlayer" + std::to_string(i + 1), useSyntheticManifest);
            if (!bot->Connect(endpoint))
            {
                PrintResult("five-player-smoke", false, "one of five transport connections failed");
                return 1;
            }
            bots.push_back(std::move(bot));
        }
        const auto pumpFive = [&](const std::function<bool()>& acPredicate, std::chrono::milliseconds aTimeout)
        {
            const auto deadline = std::chrono::steady_clock::now() + aTimeout;
            while (std::chrono::steady_clock::now() < deadline)
            {
                for (auto& bot : bots)
                    bot->Update();
                if (acPredicate())
                    return true;
                std::this_thread::sleep_for(1ms);
            }
            return false;
        };
        if (!pumpFive([&] {
                for (const auto& bot : bots)
                    if (!bot->Authenticated) return false;
                return true;
            }, 10s))
        {
            PrintResult("five-player-smoke", false, "five clients did not authenticate");
            return 1;
        }
        PartyCreateRequest createParty;
        bots.front()->SendMessage(createParty);
        if (!pumpFive([&] {
                for (const auto& bot : bots)
                    if (bot->PartySize != 5 || bot->LeaderPlayerId != bots.front()->PlayerId) return false;
                return true;
            }, 10s))
        {
            PrintResult("five-player-smoke", false, "five clients did not converge in one party");
            return 1;
        }
        PartyReadyRequest ready;
        ready.Ready = true;
        for (auto& bot : bots)
            bot->SendMessage(ready);
        if (!pumpFive([&] {
                for (const auto& bot : bots)
                    if (bot->ReadyCount != 5) return false;
                return true;
            }, 10s))
        {
            PrintResult("five-player-smoke", false, "five-player ready barrier did not converge");
            return 1;
        }
        PartyStartRequest start;
        start.Mode = PartyStartRequest::kNew;
        start.Launch = true;
        bots.front()->SendMessage(start);
        const bool launched = pumpFive([&] {
            const auto epoch = bots.front()->StartEpoch;
            if (epoch == 0) return false;
            for (const auto& bot : bots)
                if (bot->SessionState != 1 || bot->StartEpoch != epoch) return false;
            return true;
        }, 10s);
        PrintResult("five-player-smoke", launched,
            launched ? "five clients joined, readied, and entered one shared start epoch" :
                "five clients did not enter one shared start epoch");
        return launched ? 0 : 1;
    }

    if (scenario == "distributed-host" || scenario == "distributed-follower")
    {
        const bool isHost = scenario == "distributed-host";
        ProtocolBot bot(isHost ? "PhysicalRigHost" : "PhysicalRigFollower", useSyntheticManifest);
        if (!bot.Connect(endpoint) || !PumpUntil({&bot}, [&] { return bot.Authenticated; }, 10s))
        {
            PrintResult(scenario.c_str(), false, "physical-rig client did not authenticate");
            return 1;
        }

        if (isHost)
        {
            PartyCreateRequest createParty{};
            bot.SendMessage(createParty);
            if (!PumpUntil({&bot}, [&] { return bot.PartySize == 1 && bot.IsLeader; }, 5s))
            {
                PrintResult(scenario.c_str(), false, "host did not create the physical-rig party");
                return 1;
            }
        }

        if (!PumpUntil({&bot}, [&] { return bot.PartySize == 2; }, 30s))
        {
            PrintResult(scenario.c_str(), false, "two physical-rig clients did not converge in one party");
            return 1;
        }

        PartyReadyRequest ready;
        ready.Ready = true;
        bot.SendMessage(ready);
        if (isHost)
        {
            if (!PumpUntil({&bot}, [&] { return bot.ReadyCount == 2; }, 15s))
            {
                PrintResult(scenario.c_str(), false, "host did not observe both physical rigs ready");
                return 1;
            }
            PartyStartRequest start;
            start.Mode = PartyStartRequest::kNew;
            start.Launch = true;
            bot.SendMessage(start);
        }

        const bool started = PumpUntil({&bot}, [&] { return bot.SessionState == 1 && bot.StartEpoch > 0; }, 15s);
        // Keep the authoritative host connected long enough for the remote
        // machine to consume the start broadcast. A real Skyrim process stays
        // alive here; exiting immediately resets the party during teardown.
        if (started && isHost)
        {
            const auto lingerUntil = std::chrono::steady_clock::now() + 2s;
            while (std::chrono::steady_clock::now() < lingerUntil)
            {
                bot.Update();
                std::this_thread::sleep_for(1ms);
            }
        }
        PrintResult(scenario.c_str(), started,
            started ? "physical-rig client reached the authoritative shared start epoch" : "physical-rig client did not reach shared start");
        return started ? 0 : 1;
    }

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

    if (scenario == "camera-authority")
    {
        PartyReadyRequest ready;
        ready.Ready = true;
        leader.SendMessage(ready);
        follower.SendMessage(ready);
        if (!PumpUntil({&leader, &follower}, [&] { return leader.ReadyCount == 2 && follower.ReadyCount == 2; }, 5s))
        {
            PrintResult("camera-authority", false, "party did not reach the ready barrier");
            return 1;
        }

        PartyStartRequest start;
        start.Mode = PartyStartRequest::kNew;
        start.Launch = true;
        leader.SendMessage(start);
        if (!PumpUntil({&leader, &follower}, [&] {
                return leader.SessionState == 1 && follower.SessionState == 1 &&
                    leader.StartEpoch > 0 && leader.StartEpoch == follower.StartEpoch;
            }, 5s))
        {
            PrintResult("camera-authority", false, "party did not enter the loading epoch");
            return 1;
        }

        // The same ready request is the world-loaded barrier while a campaign
        // is launching. Both peers must cross it before presentation packets
        // become eligible for relay.
        leader.SendMessage(ready);
        follower.SendMessage(ready);
        if (!PumpUntil({&leader, &follower}, [&] {
                return leader.SessionState == 2 && follower.SessionState == 2;
            }, 5s))
        {
            PrintResult("camera-authority", false, "party did not reach the running session state");
            return 1;
        }

        CameraStateRequest camera;
        camera.Snapshot.Tick = 100;
        camera.Snapshot.AuthorityEpoch = leader.StartEpoch;
        camera.Snapshot.Position = {12.f, -34.f, 56.f};
        camera.Snapshot.Rotation = {1.f, 0.f, 0.f, 0.f, 1.f, 0.f, 0.f, 0.f, 1.f};
        camera.Snapshot.Scale = 1.f;
        camera.Snapshot.Fov = 70.f;
        camera.Snapshot.StateId = 8;
        leader.SendMessage(camera);
        const bool relayed = PumpUntil({&leader, &follower}, [&] {
            return follower.CameraUpdateCount == 1 && follower.LastCameraSnapshot == camera.Snapshot;
        }, 5s);

        camera.Snapshot.Tick = 101;
        camera.Snapshot.Position.x = 999.f;
        follower.SendMessage(camera);
        const auto rejectionDeadline = std::chrono::steady_clock::now() + 250ms;
        while (std::chrono::steady_clock::now() < rejectionDeadline)
        {
            leader.Update();
            follower.Update();
            std::this_thread::sleep_for(1ms);
        }
        const bool rejectedFollower = leader.CameraUpdateCount == 0;
        leader.SendMessage(ready);
        const auto firstReadyDeadline = std::chrono::steady_clock::now() + 250ms;
        while (std::chrono::steady_clock::now() < firstReadyDeadline)
        {
            leader.Update();
            follower.Update();
            std::this_thread::sleep_for(1ms);
        }
        const bool heldForFollower = leader.SessionState == 2 && follower.SessionState == 2;
        follower.SendMessage(ready);
        const bool gameplayReleased = PumpUntil({&leader, &follower}, [&] {
            return leader.SessionState == 3 && follower.SessionState == 3;
        }, 5s);
        camera.Snapshot.Tick = 102;
        leader.SendMessage(camera);
        const auto gameplayDeadline = std::chrono::steady_clock::now() + 250ms;
        while (std::chrono::steady_clock::now() < gameplayDeadline)
        {
            leader.Update();
            follower.Update();
            std::this_thread::sleep_for(1ms);
        }
        const bool cameraStopped = follower.CameraUpdateCount == 1;
        const bool passed = relayed && rejectedFollower && heldForFollower &&
            gameplayReleased && cameraStopped;
        PrintResult("camera-authority", passed,
            passed ? "leader cinematic camera relayed, then all-member gameplay barrier stopped camera authority" :
                "camera authority or gameplay handoff barrier failed");
        return passed ? 0 : 1;
    }

    if (scenario == "scene-authority")
    {
        PartyReadyRequest ready;
        ready.Ready = true;
        leader.SendMessage(ready);
        follower.SendMessage(ready);
        if (!PumpUntil({&leader, &follower}, [&] { return leader.ReadyCount == 2 && follower.ReadyCount == 2; }, 5s))
        {
            PrintResult("scene-authority", false, "party did not reach the ready barrier");
            return 1;
        }

        PartyStartRequest start;
        start.Mode = PartyStartRequest::kNew;
        start.Launch = true;
        leader.SendMessage(start);
        if (!PumpUntil({&leader, &follower}, [&] {
                return leader.SessionState == 1 && follower.SessionState == 1 &&
                    leader.StartEpoch > 0 && leader.StartEpoch == follower.StartEpoch;
            }, 5s))
        {
            PrintResult("scene-authority", false, "party did not enter the loading epoch");
            return 1;
        }
        leader.SendMessage(ready);
        follower.SendMessage(ready);
        if (!PumpUntil({&leader, &follower}, [&] {
                return leader.SessionState == 2 && follower.SessionState == 2;
            }, 5s))
        {
            PrintResult("scene-authority", false, "party did not reach the running session state");
            return 1;
        }

        SceneTimelineRequest scene;
        scene.Snapshot.Tick = 100;
        scene.Snapshot.AuthorityEpoch = leader.StartEpoch;
        scene.Snapshot.TransactionId = 1;
        scene.Snapshot.SceneId = GameId{1, 0xBECD4};
        scene.Snapshot.QuestId = GameId{1, 0x3372B};
        scene.Snapshot.RawPhaseWord = 1;
        scene.Snapshot.Playing = true;
        leader.SendMessage(scene);
        const bool relayed = PumpUntil({&leader, &follower}, [&] {
            return follower.SceneUpdateCount == 1 &&
                follower.LastSceneSnapshot.SceneId == scene.Snapshot.SceneId &&
                follower.LastSceneSnapshot.QuestId == scene.Snapshot.QuestId &&
                follower.LastSceneSnapshot.ServerSequence > 0;
        }, 5s);

        // Test each rejected write separately; pump after each so an accepted
        // packet cannot be hidden by a later, valid packet.
        const auto pumpBriefly = [&] {
            const auto deadline = std::chrono::steady_clock::now() + 250ms;
            while (std::chrono::steady_clock::now() < deadline)
            {
                leader.Update();
                follower.Update();
                std::this_thread::sleep_for(1ms);
            }
        };
        follower.SendMessage(scene);
        pumpBriefly();
        const bool rejectedFollower = leader.SceneUpdateCount == 0 && follower.SceneUpdateCount == 1;

        scene.Snapshot.TransactionId = 2;
        scene.Snapshot.AuthorityEpoch = leader.StartEpoch - 1;
        leader.SendMessage(scene);
        pumpBriefly();
        const bool rejectedStaleEpoch = follower.SceneUpdateCount == 1;

        scene.Snapshot.AuthorityEpoch = leader.StartEpoch;
        scene.Snapshot.TransactionId = 1;
        leader.SendMessage(scene);
        pumpBriefly();
        const bool rejectedDuplicate = follower.SceneUpdateCount == 1;

        scene.Snapshot.TransactionId = 2;
        scene.Snapshot.RawPhaseWord = 2;
        leader.SendMessage(scene);
        const bool nextRelayed = PumpUntil({&leader, &follower}, [&] {
            return follower.SceneUpdateCount == 2 && follower.LastSceneSnapshot.RawPhaseWord == 2 &&
                follower.LastSceneSnapshot.ServerSequence > 1;
        }, 5s);
        const bool passed = relayed && rejectedFollower && rejectedStaleEpoch && rejectedDuplicate && nextRelayed;
        PrintResult("scene-authority", passed,
            passed ? "leader scenes relayed in sequence; follower, stale epoch, and duplicate writes rejected" :
                "scene relay or authority validation failed");
        return passed ? 0 : 1;
    }

    if (scenario == "physics-authority")
    {
        PartyReadyRequest ready;
        ready.Ready = true;
        leader.SendMessage(ready);
        follower.SendMessage(ready);
        if (!PumpUntil({&leader, &follower}, [&] { return leader.ReadyCount == 2 && follower.ReadyCount == 2; }, 5s))
        {
            PrintResult("physics-authority", false, "party did not reach the ready barrier");
            return 1;
        }
        PartyStartRequest start;
        start.Mode = PartyStartRequest::kNew;
        start.Launch = true;
        leader.SendMessage(start);
        if (!PumpUntil({&leader, &follower}, [&] {
                return leader.SessionState == 1 && follower.SessionState == 1 &&
                    leader.StartEpoch > 0 && leader.StartEpoch == follower.StartEpoch;
            }, 5s))
        {
            PrintResult("physics-authority", false, "party did not enter the loading epoch");
            return 1;
        }
        leader.SendMessage(ready);
        follower.SendMessage(ready);
        if (!PumpUntil({&leader, &follower}, [&] {
                return leader.SessionState == 2 && follower.SessionState == 2;
            }, 5s))
        {
            PrintResult("physics-authority", false, "party did not reach the running session state");
            return 1;
        }

        PhysicsReferencesMoveRequest physics;
        physics.Tick = 100;
        PhysicsReferenceUpdate pose;
        pose.Id = GameId{1, 0x1234};
        pose.Position = {12.f, -34.f, 56.f};
        pose.MotionType = 3;
        pose.BodyTransform = {1.f, 0.f, 0.f, 0.f,
            0.f, 1.f, 0.f, 0.f, 0.f, 0.f, 1.f, 0.f,
            12.f / 70.f, -34.f / 70.f, 56.f / 70.f, 1.f};
        physics.Updates.push_back(pose);

        EnterInteriorCellRequest leaderCell;
        leaderCell.CellId = GameId{1, 0x100};
        EnterInteriorCellRequest followerCell;
        followerCell.CellId = GameId{1, 0x200};
        leader.SendMessage(leaderCell);
        follower.SendMessage(followerCell);
        const auto cellUpdateDeadline = std::chrono::steady_clock::now() + 250ms;
        while (std::chrono::steady_clock::now() < cellUpdateDeadline)
        {
            leader.Update();
            follower.Update();
            std::this_thread::sleep_for(1ms);
        }
        leader.SendMessage(physics);
        const auto separationDeadline = std::chrono::steady_clock::now() + 250ms;
        while (std::chrono::steady_clock::now() < separationDeadline)
        {
            leader.Update();
            follower.Update();
            std::this_thread::sleep_for(1ms);
        }
        const bool suppressedAcrossCells = follower.PhysicsUpdateCount == 0;

        follower.SendMessage(leaderCell);
        const auto reentryDeadline = std::chrono::steady_clock::now() + 250ms;
        while (std::chrono::steady_clock::now() < reentryDeadline)
        {
            leader.Update();
            follower.Update();
            std::this_thread::sleep_for(1ms);
        }
        physics.Tick = 101;
        leader.SendMessage(physics);
        const bool relayed = PumpUntil({&leader, &follower}, [&] {
            return follower.PhysicsUpdateCount == 1 && follower.LastPhysicsTick == 101 &&
                follower.LastPhysicsEpoch == leader.StartEpoch && follower.LastPhysicsCount == 1 &&
                follower.LastPhysicsBodyTransform == pose.BodyTransform;
        }, 5s);
        physics.Tick = 102;
        follower.SendMessage(physics);
        const auto rejectionDeadline = std::chrono::steady_clock::now() + 250ms;
        while (std::chrono::steady_clock::now() < rejectionDeadline)
        {
            leader.Update();
            follower.Update();
            std::this_thread::sleep_for(1ms);
        }
        const bool rejectedFollower = leader.PhysicsUpdateCount == 0;
        const bool passed = suppressedAcrossCells && relayed && rejectedFollower;
        PrintResult("physics-authority", passed,
            passed ? "distant cell suppressed, nearby leader snapshot relayed, follower write rejected" :
                "physics cell scoping, relay, or leader-only authority validation failed");
        return passed ? 0 : 1;
    }

    if (scenario == "temporary-actor-identity")
    {
        EnterInteriorCellRequest cell;
        cell.CellId = GameId{1, 0x100};
        leader.SendMessage(cell);
        follower.SendMessage(cell);
        const auto settleUntil = std::chrono::steady_clock::now() + 250ms;
        while (std::chrono::steady_clock::now() < settleUntil)
        {
            leader.Update();
            follower.Update();
            std::this_thread::sleep_for(1ms);
        }

        AssignCharacterRequest native;
        native.Cookie = 501;
        native.ReferenceId = GameId{std::numeric_limits<uint32_t>::max(), 0x1001};
        native.FormId = GameId{1, 0x23456};
        native.CellId = cell.CellId;
        native.Position = glm::vec3{100.f, 200.f, 300.f};
        leader.SendMessage(native);
        if (!PumpUntil({&leader, &follower}, [&] { return leader.AssignmentCount == 1; }, 5s))
        {
            PrintResult("temporary-actor-identity", false, "leader temporary actor was not assigned");
            return 1;
        }
        const auto firstServerId = leader.LastAssignedServerId;
        const auto firstEpoch = leader.LastAssignedEpoch;

        native.Cookie = 502;
        native.ReferenceId.BaseId = 0x2001; // Another process's FF reference.
        // A scripted actor may travel well beyond the old 64-unit gate
        // before its native counterpart is registered by the other client.
        native.Position = glm::vec3{250.f, 200.f, 300.f};
        follower.SendMessage(native);
        const bool reconciled = PumpUntil({&leader, &follower}, [&] {
            return follower.AssignmentCount == 1;
        }, 5s) && follower.LastAssignedServerId == firstServerId &&
            follower.LastAssignedEpoch == firstEpoch && !follower.LastAssignedOwner;

        native.Cookie = 503;
        native.ReferenceId.BaseId = 0x1002;
        leader.SendMessage(native); // Same source may have two genuine actors.
        const bool distinctSameSource = PumpUntil({&leader, &follower}, [&] {
            return leader.AssignmentCount == 2;
        }, 5s) && leader.LastAssignedServerId != firstServerId && leader.LastAssignedOwner;

        native.Cookie = 504;
        native.ReferenceId.BaseId = 0x2002;
        native.FormId.BaseId = 0x23457; // A different NPC base at the same marker.
        follower.SendMessage(native);
        const bool distinctBase = PumpUntil({&leader, &follower}, [&] {
            return follower.AssignmentCount == 2;
        }, 5s) && follower.LastAssignedServerId != firstServerId && follower.LastAssignedOwner;

        const bool passed = reconciled && distinctSameSource && distinctBase;
        PrintResult("temporary-actor-identity", passed,
            passed ? "cross-client FF references reconciled; same-source and different-base NPCs remained distinct" :
                "temporary actor identity or distinct-actor separation failed");
        return passed ? 0 : 1;
    }

    if (scenario == "mount-leader-affinity")
    {
        EnterInteriorCellRequest leaderCell;
        leaderCell.CellId = GameId{1, 0x100};
        EnterInteriorCellRequest followerCell;
        followerCell.CellId = GameId{1, 0x200};
        leader.SendMessage(leaderCell);
        follower.SendMessage(followerCell);
        const auto settleUntil = std::chrono::steady_clock::now() + 250ms;
        while (std::chrono::steady_clock::now() < settleUntil)
        {
            leader.Update();
            follower.Update();
            std::this_thread::sleep_for(1ms);
        }

        AssignCharacterRequest mount;
        mount.Cookie = 64;
        mount.ReferenceId = GameId{1, 0x12364};
        mount.FormId = GameId{1, 0x23464};
        mount.CellId = followerCell.CellId;
        mount.IsMount = true;
        follower.SendMessage(mount);
        if (!PumpUntil({&leader, &follower}, [&] { return follower.AssignmentCount > 0; }, 5s) ||
            !follower.LastAssignedOwner || follower.LastAssignedEpoch == 0)
        {
            PrintResult("mount-leader-affinity", false, "follower did not receive the separated-cell mount lease");
            return 1;
        }

        RequestOwnershipClaim claim;
        claim.ServerId = follower.LastAssignedServerId;
        claim.ExpectedOwnershipEpoch = follower.LastAssignedEpoch;
        leader.SendMessage(followerCell);
        const auto enterUntil = std::chrono::steady_clock::now() + 250ms;
        while (std::chrono::steady_clock::now() < enterUntil)
        {
            leader.Update();
            follower.Update();
            std::this_thread::sleep_for(1ms);
        }
        leader.SendMessage(claim);
        const bool leaderClaimed = PumpUntil({&leader, &follower}, [&] {
            return follower.OwnershipTransferCount == 1 &&
                follower.LastTransferServerId == claim.ServerId &&
                follower.LastTransferOwnerId == leader.PlayerId &&
                follower.LastTransferEpoch > claim.ExpectedOwnershipEpoch;
        }, 5s);

        claim.ExpectedOwnershipEpoch = follower.LastTransferEpoch;
        follower.SendMessage(claim);
        const auto rejectionUntil = std::chrono::steady_clock::now() + 250ms;
        while (std::chrono::steady_clock::now() < rejectionUntil)
        {
            leader.Update();
            follower.Update();
            std::this_thread::sleep_for(1ms);
        }
        const bool followerRejected = follower.OwnershipTransferCount == 1;
        const bool passed = leaderClaimed && followerRejected;
        PrintResult("mount-leader-affinity", passed,
            passed ? "leader claimed nearby mount; follower could not reclaim it" :
                "mount authority failed to converge on the leader");
        return passed ? 0 : 1;
    }

    if (scenario == "separated-cell-lease")
    {
        EnterInteriorCellRequest leaderCell;
        leaderCell.CellId = GameId{1, 0x100};
        EnterInteriorCellRequest followerCell;
        followerCell.CellId = GameId{1, 0x200};
        leader.SendMessage(leaderCell);
        follower.SendMessage(followerCell);
        const auto settleUntil = std::chrono::steady_clock::now() + 250ms;
        while (std::chrono::steady_clock::now() < settleUntil)
        {
            leader.Update();
            follower.Update();
            std::this_thread::sleep_for(1ms);
        }

        AssignCharacterRequest discovered;
        discovered.Cookie = 42;
        discovered.ReferenceId = GameId{1, 0x12345};
        discovered.FormId = GameId{1, 0x23456};
        discovered.CellId = followerCell.CellId;
        follower.SendMessage(discovered);
        if (!PumpUntil({&leader, &follower}, [&] { return follower.AssignmentCount > 0; }, 5s) ||
            follower.LastAssignedCookie != discovered.Cookie || !follower.LastAssignedOwner ||
            follower.LastAssignedEpoch == 0)
        {
            PrintResult("separated-cell-lease", false,
                "follower assignment count=" + std::to_string(follower.AssignmentCount) +
                " cookie=" + std::to_string(follower.LastAssignedCookie) +
                " owner=" + std::to_string(follower.LastAssignedOwner) +
                " server=" + std::to_string(follower.LastAssignedServerId) +
                " epoch=" + std::to_string(follower.LastAssignedEpoch) +
                " parseFailure=" + std::to_string(follower.ParseFailure));
            return 1;
        }

        RequestOwnershipClaim claim;
        claim.ServerId = follower.LastAssignedServerId;
        claim.ExpectedOwnershipEpoch = follower.LastAssignedEpoch;
        leader.SendMessage(claim);
        const auto rejectedUntil = std::chrono::steady_clock::now() + 250ms;
        while (std::chrono::steady_clock::now() < rejectedUntil)
        {
            leader.Update();
            follower.Update();
            std::this_thread::sleep_for(1ms);
        }
        const bool distantHostRejected = follower.OwnershipTransferCount == 0;

        leader.SendMessage(followerCell);
        const auto enterUntil = std::chrono::steady_clock::now() + 250ms;
        while (std::chrono::steady_clock::now() < enterUntil)
        {
            leader.Update();
            follower.Update();
            std::this_thread::sleep_for(1ms);
        }
        leader.SendMessage(claim);
        const bool reclaimed = PumpUntil({&leader, &follower}, [&] {
            return follower.OwnershipTransferCount > 0 &&
                follower.LastTransferServerId == claim.ServerId &&
                follower.LastTransferOwnerId == leader.PlayerId &&
                follower.LastTransferEpoch > claim.ExpectedOwnershipEpoch;
        }, 5s);

        follower.SendMessage(claim);
        const auto staleUntil = std::chrono::steady_clock::now() + 250ms;
        while (std::chrono::steady_clock::now() < staleUntil)
        {
            leader.Update();
            follower.Update();
            std::this_thread::sleep_for(1ms);
        }
        const bool staleRejected = follower.OwnershipTransferCount == 1;
        discovered.Cookie = 43;
        discovered.ReferenceId = GameId{1, 0x12346};
        const uint32_t priorSpawns = leader.SpawnCount;
        follower.SendMessage(discovered);
        const bool sameCellSpawned = PumpUntil({&leader, &follower}, [&] {
            return follower.AssignmentCount == 2 && leader.SpawnCount > priorSpawns &&
                leader.LastSpawnServerId == follower.LastAssignedServerId &&
                leader.LastSpawnEpoch == follower.LastAssignedEpoch && follower.LastAssignedOwner;
        }, 5s);
        if (sameCellSpawned)
        {
            claim.ServerId = leader.LastSpawnServerId;
            claim.ExpectedOwnershipEpoch = leader.LastSpawnEpoch;
            leader.SendMessage(claim);
        }
        const bool nearbyHostReclaimed = sameCellSpawned && PumpUntil({&leader, &follower}, [&] {
            return follower.LastTransferServerId == claim.ServerId &&
                follower.LastTransferOwnerId == leader.PlayerId &&
                follower.LastTransferEpoch > claim.ExpectedOwnershipEpoch;
        }, 5s);
        const bool passed = distantHostRejected && reclaimed && staleRejected && nearbyHostReclaimed;
        PrintResult("separated-cell-lease", passed,
            passed ? "distant follower lease and nearby host spawn/claim both converged; stale epoch rejected" :
                "cell lease, initial host spawn/claim, or stale epoch validation failed");
        return passed ? 0 : 1;
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
