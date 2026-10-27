#include <TiltedCore/Stl.hpp>
#include <TiltedCore/Allocator.hpp>
#include <TiltedCore/Buffer.hpp>
#include <TiltedCore/Serialization.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <catch2/catch.hpp>
#include <iterator>

#include <AuthorityEpoch.h>
#include <Opcodes.h>
#include <Messages/NotifyPartyInfo.h>
#include <Messages/ServerMessageFactory.h>

using namespace TiltedPhoques;

TEST_CASE("Foundation opcode reservations follow the roster baseline", "[encoding.foundation]")
{
    constexpr ClientOpcode client[] = {
        kTimeSkipRequest, kRequestQuestResync, kRequestQuestSnapshot,
        kRequestQuestCursor, kRequestQuestHandoverDone, kScriptOutcomeRequest,
        kScriptOutcomeResultRequest, kTopicInfoFragmentRunRequest,
        kActivateResultRequest, kScriptEventForwardRequest,
        kTimerHandoverRequest, kRequestGlobalValue};
    constexpr ServerOpcode server[] = {
        kNotifyTimeSkip, kNotifyQuestCommitAck, kNotifyQuestSnapshotRequest,
        kNotifyQuestSnapshot, kNotifyQuestCursor, kNotifyQuestHandover,
        kNotifyScriptOutcome, kNotifyScriptOutcomeResult,
        kNotifyTopicInfoFragmentRun, kNotifyActivateResult,
        kNotifyScriptEventForward, kNotifyTimerHandover,
        kNotifyRegionAuthority, kNotifyGlobalValue};
    REQUIRE(kRequestCheckpointRoster == 85);
    REQUIRE(kNotifyCheckpointRoster == 83);
    for (size_t i = 0; i < std::size(client); ++i)
        REQUIRE(client[i] == static_cast<ClientOpcode>(86 + i));
    for (size_t i = 0; i < std::size(server); ++i)
        REQUIRE(server[i] == static_cast<ServerOpcode>(84 + i));
    // CM's block follows the design table directly (INTEGRATION.md section 3), then the b10d hand-off messages.
    REQUIRE(kContentManifestRequest == 98);
    REQUIRE(kContentChunkUpload == 102);
    REQUIRE(kRequestHandoffAck == 103);
    REQUIRE(kNotifyContentPlan == 98);
    REQUIRE(kNotifyContentStatus == 100);
}

TEST_CASE("Party info carries the full authority epoch", "[encoding.foundation]")
{
    NotifyPartyInfo sent;
    sent.PlayerIds = {7, 8};
    sent.ReadyPlayerIds = {7};
    sent.IsLeader = true;
    sent.LeaderPlayerId = 7;
    sent.SessionState = 3;
    sent.StartEpoch = 0x1122334455667788ull;
    sent.LeaderGeneration = 0xABCDEF12u;
    Buffer buffer(256);
    Buffer::Writer writer(&buffer);
    sent.Serialize(writer);
    Buffer::Reader reader(&buffer);
    auto decoded = ServerMessageFactory{}.Extract(reader);
    REQUIRE(decoded);
    const auto& received = static_cast<const NotifyPartyInfo&>(*decoded);
    REQUIRE(received == sent);
    REQUIRE(received.StartEpoch == 0x1122334455667788ull);
    REQUIRE(received.LeaderGeneration == 0xABCDEF12u);
    const AuthorityEpoch epoch{received.StartEpoch, received.LeaderPlayerId,
                               received.LeaderGeneration};
    const AuthorityEpoch expected{sent.StartEpoch, 7, 0xABCDEF12u};
    REQUIRE(epoch == expected);
}
