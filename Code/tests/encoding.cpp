#include <TiltedCore/Stl.hpp>
#include <TiltedCore/Allocator.hpp>
#include <TiltedCore/Buffer.hpp>
#include <TiltedCore/Serialization.hpp>

#include <optional>
#include <limits>

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include "StringCache.h"
#include "Messages/StringCacheUpdate.h"

#include <catch2/catch.hpp>

#include <Messages/ClientMessageFactory.h>
#include <Messages/ServerMessageFactory.h>
#include <Structs/Vector2_NetQuantize.h>
#include <Structs/AnimationGraphDescriptorManager.h>
#include <Structs/AnimationVariables.h>
#include <Structs/PhysicsReferenceUpdate.h>
#include <Structs/ServerSettings.h>
#include <Structs/CharacterSnapshot.h>
#include <Messages/DropIn.h>
#include <Structs/Skyrim/AnimationGraphDescriptor_Master_Behavior.h>

#include <TiltedCore/Math.hpp>
#include <TiltedCore/Platform.hpp>

using namespace TiltedPhoques;

TEST_CASE("Character snapshot roundtrips every field and refuses a foreign version or oversized list", "[encoding.character]")
{
    CharacterSnapshot sent;
    sent.Name = "Rex";
    sent.ChangeFlags = 0x1000800;
    sent.AppearanceBuffer = String("\x01\x02\x00\x7F", 4);
    sent.Level = 31;
    sent.Xp = 412.5f;
    sent.LevelThreshold = 1600.f;
    sent.PerkPoints = 3;
    for (uint32_t i = 0; i < CharacterSnapshot::kSkillCount; ++i)
        sent.Skills.push_back({15.f + i, 0.25f * i, 100.f + i, i % 3});
    sent.BaseValues = {{24, 250.f}, {25, 180.f}, {26, 210.f}};
    sent.DragonSouls = 4.f;
    sent.Perks = {{GameId(0, 0xBABE4), 1}, {GameId(0, 0xBCD2A), 5}};
    sent.Spells = {GameId(0, 0x12FCD), GameId(1, 0x800)};
    sent.Shouts = {GameId(0, 0x13E07)};
    sent.Words = {{GameId(0, 0x602A3), true}, {GameId(0, 0x602A4), false}};
    Inventory::Entry gold;
    gold.BaseId = GameId(0, 0xF);
    gold.Count = 1234;
    sent.Items.Entries.push_back(gold);
    sent.ExcludedQuestItems = {GameId(0, 0x2BE4A)};

    Buffer buffer(1 << 14);
    Buffer::Writer writer(&buffer);
    sent.Serialize(writer);
    writer.WriteBits(0xBEEF, 16);
    Buffer::Reader reader(&buffer);
    CharacterSnapshot received;
    REQUIRE(received.Deserialize(reader));
    uint64_t sentinel{};
    reader.ReadBits(sentinel, 16);
    REQUIRE(sentinel == 0xBEEF);
    REQUIRE(received == sent);

    // A snapshot from another format version is refused rather than misread.
    Buffer foreign(64);
    Buffer::Writer foreignWriter(&foreign);
    Serialization::WriteVarInt(foreignWriter, CharacterSnapshot::kVersion + 1);
    Buffer::Reader foreignReader(&foreign);
    CharacterSnapshot unusable;
    REQUIRE_FALSE(unusable.Deserialize(foreignReader));

    // A skill list longer than the game's 18 skills marks the snapshot corrupt.
    CharacterSnapshot oversized = sent;
    oversized.Skills.push_back({});
    Buffer big(1 << 14);
    Buffer::Writer bigWriter(&big);
    oversized.Serialize(bigWriter);
    Buffer::Reader bigReader(&big);
    CharacterSnapshot rejected;
    REQUIRE_FALSE(rejected.Deserialize(bigReader));
}

TEST_CASE("A hostile inventory count stops at the data instead of allocating without end", "[encoding.inventory]")
{
    // Muse 2026-09-30: an entry count of 2^32-1 pushed default entries until the game ran out of memory.
    Buffer buffer(64);
    Buffer::Writer writer(&buffer);
    Serialization::WriteVarInt(writer, 0xFFFFFFFFull);
    Buffer::Reader reader(&buffer);
    Inventory inventory;
    inventory.Deserialize(reader);
    REQUIRE(inventory.Entries.size() <= 16384);
}

TEST_CASE("A drop-in chunk offset near 2^64 does not wrap past the size check", "[encoding.dropin]")
{
    DropInData chunk;
    chunk.Attempt = 1;
    chunk.Op = DropInOp::Chunk;
    chunk.Bytes = "x";
    chunk.Offset = 0;
    REQUIRE(chunk.Valid());
    chunk.Offset = std::numeric_limits<uint64_t>::max();
    REQUIRE_FALSE(chunk.Valid());
    chunk.Offset = DropInData::MaxFile;
    REQUIRE_FALSE(chunk.Valid());
}

TEST_CASE("Player camera look survives movement encoding", "[encoding.movement]")
{
    // Include both pitch poles, yaw wrap endpoints, and every bit set. Packed
    // integers must survive exactly, without a second float quantization.
    for (const uint32_t packed : {0u, 0xFFFFFFFFu, 0x80000000u, 0x8000FFFFu, 0x1234ABCDu})
    {
        Movement sent;
        sent.HasLookDirection = true;
        sent.LookDirection = packed;
        Movement npc;
        Movement received;
        Movement receivedNpc;
        receivedNpc.HasLookDirection = true;
        receivedNpc.LookDirection = 0xFFFFFFFFu;
        Buffer buffer(4096);
        Buffer::Writer writer(&buffer);
        sent.Serialize(writer);
        npc.Serialize(writer);
        writer.WriteBits(0xBEEF, 16);

        Buffer::Reader reader(&buffer);
        received.Deserialize(reader);
        receivedNpc.Deserialize(reader);
        uint64_t sentinel{};
        reader.ReadBits(sentinel, 16);
        REQUIRE(received.HasLookDirection);
        REQUIRE(received.LookDirection == packed);
        REQUIRE(received == sent);
        REQUIRE_FALSE(receivedNpc.HasLookDirection);
        REQUIRE(receivedNpc.LookDirection == 0);
        REQUIRE(receivedNpc == npc);
        REQUIRE(sentinel == 0xBEEF);

        received.LookDirection ^= 1u;
        REQUIRE(received != sent);
        received = sent;
        received.LookDirection ^= 1u << 16;
        REQUIRE(received != sent); // pitch alone must trigger a movement update
        received = sent;
        received.HasLookDirection = false;
        REQUIRE(received != sent);
    }
}

TEST_CASE("Both movement message formats preserve independent player look directions", "[encoding.movement]")
{
    ClientReferencesMoveRequest sent;
    sent.Tick = 12345;
    for (uint32_t player = 1; player <= 5; ++player)
    {
        auto& movement = sent.Updates[player].UpdatedMovement;
        movement.HasLookDirection = player != 3; // native dialogue/disabled look
        // Five independent pitches, including straight up/down, at the same
        // yaw. Player 3 yields to native targeting instead of sending a look.
        const uint32_t pitch = (player - 1) * 65535u / 4u;
        movement.LookDirection = movement.HasLookDirection ? (pitch << 16) | 0x4000u : 0u;
    }
    sent.Updates[42].UpdatedMovement.Direction = 0.25f; // NPC without camera look
    Buffer clientBuffer(8192);
    Buffer::Writer clientWriter(&clientBuffer);
    sent.Serialize(clientWriter);
    Buffer::Reader clientReader(&clientBuffer);
    uint64_t opcode{};
    clientReader.ReadBits(opcode, 8);
    ClientReferencesMoveRequest received;
    received.DeserializeRaw(clientReader);
    REQUIRE(received.Updates.size() == sent.Updates.size());

    ServerReferencesMoveRequest relayed;
    relayed.Tick = received.Tick;
    relayed.Updates = received.Updates;
    // The relay carries each owner sample's age (Lokir jitter, 2026-09-30): 0, one byte, and multi-byte values.
    std::vector<uint32_t> ids;
    for (const auto& entry : relayed.Updates)
        ids.push_back(entry.first);
    for (size_t i = 0; i < ids.size(); ++i)
        relayed.Updates[ids[i]].SampleAge = i == 0 ? 0 : 999;
    relayed.Updates[42].SampleAge = 17;
    Buffer serverBuffer(8192);
    Buffer::Writer serverWriter(&serverBuffer);
    relayed.Serialize(serverWriter);
    Buffer::Reader serverReader(&serverBuffer);
    serverReader.ReadBits(opcode, 8);
    ServerReferencesMoveRequest presented;
    presented.DeserializeRaw(serverReader);
    REQUIRE(presented.Tick == sent.Tick);
    REQUIRE(presented.Updates.size() == sent.Updates.size());
    for (const auto& [id, update] : sent.Updates)
        REQUIRE(presented.Updates.at(id).UpdatedMovement == update.UpdatedMovement);
    for (const auto& [id, update] : relayed.Updates)
        REQUIRE(presented.Updates.at(id).SampleAge == update.SampleAge);
    REQUIRE(presented.Updates.at(42).SampleAge == 17);
}

TEST_CASE("Every server setting participates in equality", "[encoding.settings]")
{
    ServerSettings source{};
    source.Difficulty = 4;
    source.DeathSystemEnabled = true;
    ServerSettings changed = source;
    changed.SyncPlayerCalendar = true;
    REQUIRE(source != changed);

    Buffer buffer(64);
    Buffer::Writer writer(&buffer);
    changed.Serialize(writer);
    Buffer::Reader reader(&buffer);
    ServerSettings restored{};
    restored.Deserialize(reader);
    REQUIRE(restored == changed);
}

TEST_CASE("Dynamic physics stream detects body-only motion", "[encoding.physics]")
{
    std::array<float, 16> sent{};
    sent[0] = sent[5] = sent[10] = sent[15] = 1.f;
    auto current = sent;
    REQUIRE_FALSE(PhysicsBodyMotionChanged(sent, {}, current, {}));
    current[12] = 0.01f;
    REQUIRE_FALSE(PhysicsBodyMotionChanged(sent, {}, current, {}));
    current[12] = 0.02f;
    REQUIRE(PhysicsBodyMotionChanged(sent, {}, current, {}));
    current = sent;
    current[0] = 0.97f;
    REQUIRE(PhysicsBodyMotionChanged(sent, {}, current, {}));
    current = sent;
    REQUIRE(PhysicsBodyMotionChanged(sent, {}, current,
        glm::vec3{0.2f, 0.f, 0.f}));
}

TEST_CASE("Humanoid head-tracking graph inputs are synchronized", "[encoding.animation]")
{
    const auto* descriptor = AnimationGraphDescriptorManager::Get().GetDescriptor(
        AnimationGraphDescriptor_Master_Behavior::m_key);
    REQUIRE(descriptor);
    REQUIRE(descriptor->BooleanLookUpTable.size() == 66);
    for (const uint32_t index : {151u, 178u, 185u, 221u, 257u, 271u, 272u, 283u})
        REQUIRE(std::find(descriptor->BooleanLookUpTable.begin(),
            descriptor->BooleanLookUpTable.end(), index) !=
            descriptor->BooleanLookUpTable.end());
    for (const uint32_t index : {47u, 127u, 184u, 191u, 192u, 193u, 194u})
        REQUIRE(std::find(descriptor->FloatLookupTable.begin(),
            descriptor->FloatLookupTable.end(), index) !=
            descriptor->FloatLookupTable.end());
    REQUIRE(std::find(descriptor->IntegerLookupTable.begin(),
        descriptor->IntegerLookupTable.end(), 229u) !=
        descriptor->IntegerLookupTable.end());
}

TEST_CASE("Animation booleans beyond bit 63 roundtrip", "[encoding.animation]")
{
    AnimationVariables source;
    AnimationVariables previous;
    source.Booleans.assign(66, false);
    source.Booleans[64] = true;
    source.Booleans[65] = true;
    Buffer buffer(256);
    Buffer::Writer writer(&buffer);
    source.GenerateDiff(previous, writer);
    Buffer::Reader reader(&buffer);
    AnimationVariables received;
    received.ApplyDiff(reader);
    REQUIRE(received.Booleans.size() == 66);
    REQUIRE(received.Booleans[64]);
    REQUIRE(received.Booleans[65]);
}

TEST_CASE("Humanoid turn graph inputs are synchronized", "[encoding.animation]")
{
    const auto* descriptor = AnimationGraphDescriptorManager::Get().GetDescriptor(
        AnimationGraphDescriptor_Master_Behavior::m_key);
    REQUIRE(descriptor);
    for (const uint32_t index : {2u, 13u, 155u})
        REQUIRE(descriptor->IsSynced(index));
}

TEST_CASE("Encoding factory", "[encoding.factory]")
{
    Buffer buff(1000);

    {
        AuthenticationRequest request;
        request.Token = "TesSt";

        Buffer::Writer writer(&buff);
        request.Serialize(writer);

        Buffer::Reader reader(&buff);

        const ClientMessageFactory factory;
        auto pMessage = factory.Extract(reader);

        REQUIRE(pMessage);
        REQUIRE(pMessage->GetOpcode() == request.GetOpcode());

        auto pRequest = CastUnique<AuthenticationRequest>(std::move(pMessage));
        REQUIRE(pRequest->Token == request.Token);
    }

    {
        PartyAcceptInviteRequest request;
        request.InviterId = 123456;

        Buffer::Writer writer(&buff);
        request.Serialize(writer);

        Buffer::Reader reader(&buff);

        const ClientMessageFactory factory;
        auto pMessage = factory.Extract(reader);

        REQUIRE(pMessage);
        REQUIRE(pMessage->GetOpcode() == request.GetOpcode());

        auto pRequest = CastUnique<PartyAcceptInviteRequest>(std::move(pMessage));
        REQUIRE(pRequest->InviterId == request.InviterId);
    }

    {
        PartySessionSettingsRequest request;
        request.Open = true;
        request.Password = "dragonborn";

        Buffer::Writer writer(&buff);
        request.Serialize(writer);
        Buffer::Reader reader(&buff);
        const ClientMessageFactory factory;
        auto pMessage = factory.Extract(reader);

        REQUIRE(pMessage);
        auto pRequest = CastUnique<PartySessionSettingsRequest>(std::move(pMessage));
        REQUIRE(pRequest->Open);
        REQUIRE(pRequest->Password == request.Password);
    }

    {
        PhysicsReferencesMoveRequest request;
        request.Tick = 4242;
        request.Updates.push_back({GameId{1, 0xB9DF3}, {1.f, 2.f, 3.f}, {0.1f, 0.2f, 0.3f}, 3, {1.1f, -2.2f, 0.5f},
            {1.f, 0.f, 0.f, 0.f, 0.f, 1.f, 0.f, 0.f, 0.f, 0.f, 1.f, 0.f, 4.f, 5.f, 6.f, 1.f}});

        Buffer::Writer writer(&buff);
        request.Serialize(writer);
        Buffer::Reader reader(&buff);
        const ClientMessageFactory factory;
        auto pMessage = factory.Extract(reader);

        REQUIRE(pMessage);
        auto pRequest = CastUnique<PhysicsReferencesMoveRequest>(std::move(pMessage));
        REQUIRE(pRequest->Tick == request.Tick);
        REQUIRE(pRequest->Updates.size() == 1);
        REQUIRE(pRequest->Updates[0].Id == request.Updates[0].Id);
        REQUIRE(pRequest->Updates[0].Position == request.Updates[0].Position);
        REQUIRE(pRequest->Updates[0].Rotation == request.Updates[0].Rotation);
        REQUIRE(pRequest->Updates[0].MotionType == 3);
        REQUIRE(pRequest->Updates[0].LinearVelocity == request.Updates[0].LinearVelocity);
        REQUIRE(pRequest->Updates[0].BodyTransform == request.Updates[0].BodyTransform);
    }

    {
        NotifyPhysicsReferencesMove notify;
        notify.Tick = 5150;
        notify.AuthorityEpoch = 7;
        notify.Updates.push_back({GameId{2, 0xBB970}, {-4.f, 5.f, 6.f}, {0.4f, 0.5f, 0.6f}, 3, {-1.f, 2.f, 0.f},
            {0.f, 1.f, 0.f, 0.f, -1.f, 0.f, 0.f, 0.f, 0.f, 0.f, 1.f, 0.f, -4.f, 5.f, 6.f, 1.f}});

        Buffer::Writer writer(&buff);
        notify.Serialize(writer);
        Buffer::Reader reader(&buff);
        const ServerMessageFactory factory;
        auto pMessage = factory.Extract(reader);

        REQUIRE(pMessage);
        auto pNotify = CastUnique<NotifyPhysicsReferencesMove>(std::move(pMessage));
        REQUIRE(pNotify->Tick == notify.Tick);
        REQUIRE(pNotify->AuthorityEpoch == notify.AuthorityEpoch);
        REQUIRE(pNotify->Updates.size() == 1);
        REQUIRE(pNotify->Updates[0].Id == notify.Updates[0].Id);
        REQUIRE(pNotify->Updates[0].Position == notify.Updates[0].Position);
        REQUIRE(pNotify->Updates[0].Rotation == notify.Updates[0].Rotation);
        REQUIRE(pNotify->Updates[0].MotionType == 3);
        REQUIRE(pNotify->Updates[0].LinearVelocity == notify.Updates[0].LinearVelocity);
        REQUIRE(pNotify->Updates[0].BodyTransform == notify.Updates[0].BodyTransform);
    }

    {
        PhysicsReferencesMoveRequest request;
        request.Tick = 5151;
        request.Updates.push_back({GameId{1, 0x1234}, {1.f, 2.f, 3.f}, {}, 0, {}});
        Buffer::Writer writer(&buff);
        request.Serialize(writer);
        Buffer::Reader reader(&buff);
        const ClientMessageFactory factory;
        auto pMessage = factory.Extract(reader);
        REQUIRE(pMessage);
        auto pRequest = CastUnique<PhysicsReferencesMoveRequest>(std::move(pMessage));
        REQUIRE(pRequest->Updates.size() == 1);
        REQUIRE(pRequest->Updates[0].MotionType == 0);
        REQUIRE(pRequest->Updates[0].BodyTransform == std::array<float, 16>{});
    }

    {
        CameraStateRequest request;
        request.Snapshot.Tick = 9001;
        request.Snapshot.AuthorityEpoch = 12;
        request.Snapshot.Position = {123.5f, -456.25f, 789.f};
        request.Snapshot.Rotation = {1.f, 0.f, 0.f, 0.f, 0.f, -1.f, 0.f, 1.f, 0.f};
        request.Snapshot.Scale = 1.f;
        request.Snapshot.Fov = 80.f;
        request.Snapshot.StateId = 8;
        REQUIRE(request.Snapshot.IsValid());

        Buffer::Writer writer(&buff);
        request.Serialize(writer);
        Buffer::Reader reader(&buff);
        const ClientMessageFactory factory;
        auto pMessage = factory.Extract(reader);

        REQUIRE(pMessage);
        auto pRequest = CastUnique<CameraStateRequest>(std::move(pMessage));
        REQUIRE(*pRequest == request);
        REQUIRE(pRequest->Snapshot.IsValid());
    }

    {
        NotifyCameraState notify;
        notify.Snapshot.Tick = 9100;
        notify.Snapshot.AuthorityEpoch = 13;
        notify.Snapshot.Position = {-10.f, 20.f, 30.f};
        notify.Snapshot.Rotation = {0.f, -1.f, 0.f, 1.f, 0.f, 0.f, 0.f, 0.f, 1.f};
        notify.Snapshot.Scale = 1.f;
        notify.Snapshot.Fov = 65.f;
        notify.Snapshot.StateId = 7;

        Buffer::Writer writer(&buff);
        notify.Serialize(writer);
        Buffer::Reader reader(&buff);
        const ServerMessageFactory factory;
        auto pMessage = factory.Extract(reader);

        REQUIRE(pMessage);
        auto pNotify = CastUnique<NotifyCameraState>(std::move(pMessage));
        REQUIRE(*pNotify == notify);
        REQUIRE(pNotify->Snapshot.IsValid());
    }

    {
        CharacterSpawnRequest spawn;
        spawn.ServerId = 101;
        spawn.OwnershipEpoch = 7;
        spawn.MountedOnServerId = 202;
        Buffer::Writer writer(&buff);
        spawn.Serialize(writer);
        Buffer::Reader reader(&buff);
        const ServerMessageFactory factory;
        auto decoded = CastUnique<CharacterSpawnRequest>(factory.Extract(reader));
        REQUIRE(decoded);
        REQUIRE(decoded->MountedOnServerId == 202);
        REQUIRE(*decoded == spawn);
    }

    {
        AssignCharacterResponse assignment;
        assignment.Cookie = 42;
        assignment.ServerId = 101;
        assignment.OwnershipEpoch = 7;
        assignment.InventoryAuthoritative = true;
        assignment.MountedOnServerId = 202;
        Buffer::Writer writer(&buff);
        assignment.Serialize(writer);
        Buffer::Reader reader(&buff);
        const ServerMessageFactory factory;
        auto decoded = CastUnique<AssignCharacterResponse>(factory.Extract(reader));
        REQUIRE(decoded);
        REQUIRE(decoded->MountedOnServerId == 202);
        REQUIRE(*decoded == assignment);
    }

    {
        MountRequest dismount;
        dismount.RiderId = 101;
        dismount.RiderOwnershipEpoch = 7;
        dismount.MountId = 0;
        dismount.MountOwnershipEpoch = 0;
        Buffer::Writer writer(&buff);
        dismount.Serialize(writer);
        Buffer::Reader reader(&buff);
        const ClientMessageFactory factory;
        auto decoded = CastUnique<MountRequest>(factory.Extract(reader));
        REQUIRE(decoded);
        REQUIRE(*decoded == dismount);
    }

    {
        NotifyMount dismount;
        dismount.RiderId = 101;
        dismount.MountId = 0;
        Buffer::Writer writer(&buff);
        dismount.Serialize(writer);
        Buffer::Reader reader(&buff);
        const ServerMessageFactory factory;
        auto decoded = CastUnique<NotifyMount>(factory.Extract(reader));
        REQUIRE(decoded);
        REQUIRE(decoded->RiderId == 101);
        REQUIRE(decoded->MountId == 0);
    }

    {
        CameraStateSnapshot invalid{};
        invalid.Rotation = {1.f, 0.f, 0.f, 0.f, 1.f, 0.f, 0.f, 0.f, 1.f};
        invalid.Fov = std::numeric_limits<float>::quiet_NaN();
        REQUIRE_FALSE(invalid.IsValid());
        invalid.Fov = 75.f;
        invalid.Rotation.fill(0.f);
        REQUIRE_FALSE(invalid.IsValid());
    }

    {
        DialogueRequest request;
        request.ServerId = 42;
        request.Tick = 123456789;
        request.SoundFilename = "Voice\\MQ101\\line.fuz";
        Buffer::Writer writer(&buff);
        request.Serialize(writer);
        Buffer::Reader reader(&buff);
        const ClientMessageFactory factory;
        auto decoded = CastUnique<DialogueRequest>(factory.Extract(reader));
        REQUIRE(decoded);
        REQUIRE(*decoded == request);
    }

    {
        NotifyDialogue notify;
        notify.ServerId = 42;
        notify.Tick = 123456789;
        notify.SoundFilename = "Voice\\MQ101\\line.fuz";
        Buffer::Writer writer(&buff);
        notify.Serialize(writer);
        Buffer::Reader reader(&buff);
        const ServerMessageFactory factory;
        auto decoded = CastUnique<NotifyDialogue>(factory.Extract(reader));
        REQUIRE(decoded);
        REQUIRE(*decoded == notify);
    }

    {
        SubtitleRequest request;
        request.ServerId = 42;
        request.Tick = 123456790;
        request.TopicFormId = 0x1234;
        request.Text = "Wake up.";
        Buffer::Writer writer(&buff);
        request.Serialize(writer);
        Buffer::Reader reader(&buff);
        const ClientMessageFactory factory;
        auto decoded = CastUnique<SubtitleRequest>(factory.Extract(reader));
        REQUIRE(decoded);
        REQUIRE(*decoded == request);
    }

    {
        NotifySubtitle notify;
        notify.ServerId = 42;
        notify.Tick = 123456790;
        notify.TopicFormId = 0x1234;
        notify.Text = "Wake up.";
        Buffer::Writer writer(&buff);
        notify.Serialize(writer);
        Buffer::Reader reader(&buff);
        const ServerMessageFactory factory;
        auto decoded = CastUnique<NotifySubtitle>(factory.Extract(reader));
        REQUIRE(decoded);
        REQUIRE(*decoded == notify);
    }
}

TEST_CASE("Static structures", "[encoding.static]")
{
    GIVEN("GameId")
    {
        GameId sendObjects, recvObjects;
        sendObjects.ModId = 1456987;
        sendObjects.BaseId = 0x789654;

        {
            Buffer buff(1000);
            Buffer::Writer writer(&buff);

            sendObjects.Serialize(writer);

            Buffer::Reader reader(&buff);
            recvObjects.Deserialize(reader);

            REQUIRE(sendObjects == recvObjects);
        }
    }

    GIVEN("Vector3_NetQuantize")
    {
        Vector3_NetQuantize sendObjects, recvObjects;
        sendObjects.x = 142.56f;
        sendObjects.y = 45687.7f;
        sendObjects.z = -142.56f;

        {
            Buffer buff(1000);
            Buffer::Writer writer(&buff);

            sendObjects.Serialize(writer);

            Buffer::Reader reader(&buff);
            recvObjects.Deserialize(reader);

            REQUIRE(sendObjects == recvObjects);
        }
    }

    GIVEN("Vector2_NetQuantize")
    {
        Vector2_NetQuantize sendObjects, recvObjects;
        sendObjects.x = 1000.89f;
        sendObjects.y = -485632.75f;

        {
            Buffer buff(1000);
            Buffer::Writer writer(&buff);

            sendObjects.Serialize(writer);

            Buffer::Reader reader(&buff);
            recvObjects.Deserialize(reader);

            REQUIRE(sendObjects == recvObjects);
        }
    }

    GIVEN("Rotator2_NetQuantize")
    {
        Rotator2_NetQuantize sendObjects, recvObjects;
        sendObjects.x = 1.89f;
        sendObjects.y = TiltedPhoques::Pi * 2.0f;

        {
            Buffer buff(1000);
            Buffer::Writer writer(&buff);

            sendObjects.Serialize(writer);

            Buffer::Reader reader(&buff);
            recvObjects.Deserialize(reader);

            REQUIRE(sendObjects == recvObjects);
        }
    }

    GIVEN("Rotator2_NetQuantize needing wrap")
    {
        // This test is a bit dangerous as floating errors can lead to sendObjects != recvObjects but the difference is minuscule so we don't care abut such cases
        Rotator2_NetQuantize sendObjects, recvObjects;
        sendObjects.x = -1.87f;
        sendObjects.y = static_cast<float>(TiltedPhoques::Pi) * 18.0f + 3.6f;

        {
            Buffer buff(1000);
            Buffer::Writer writer(&buff);

            sendObjects.Serialize(writer);

            Buffer::Reader reader(&buff);
            recvObjects.Deserialize(reader);

            REQUIRE(sendObjects == recvObjects);
        }
    }
}

TEST_CASE("Differential structures", "[encoding.differential]")
{
    GIVEN("Full ActionEvent")
    {
        ActionEvent sendAction, recvAction;

        sendAction.ActionId = 42;
        sendAction.State1 = 6547;
        sendAction.Tick = 48;
        sendAction.ActorId = 12345678;
        sendAction.EventName = "test";
        sendAction.IdleId = 87964;
        sendAction.State2 = 8963;
        sendAction.TargetEventName = "toast";
        sendAction.TargetId = 963741;
        sendAction.Type = 4;

        {
            Buffer buff(1000);
            Buffer::Writer writer(&buff);

            sendAction.GenerateDifferential(recvAction, writer);

            Buffer::Reader reader(&buff);
            recvAction.ApplyDifferential(reader);

            REQUIRE(sendAction == recvAction);
        }

        {
            Buffer buff(1000);
            Buffer::Writer writer(&buff);

            sendAction.EventName = "Plot twist !";

            sendAction.GenerateDifferential(recvAction, writer);

            Buffer::Reader reader(&buff);
            recvAction.ApplyDifferential(reader);

            REQUIRE(sendAction == recvAction);
        }
    }

    GIVEN("A single cached event name")
    {
        ActionEvent sendAction, recvAction;

        TP_UNUSED(StringCache::Get().Add("test"))

        sendAction.ActionId = 42;
        sendAction.State1 = 6547;
        sendAction.Tick = 48;
        sendAction.ActorId = 12345678;
        sendAction.EventName = "test";
        sendAction.IdleId = 87964;
        sendAction.State2 = 8963;
        sendAction.TargetEventName = "toast";
        sendAction.TargetId = 963741;
        sendAction.Type = 4;

        {
            Buffer buff(1000);
            Buffer::Writer writer(&buff);

            sendAction.GenerateDifferential(recvAction, writer);

            Buffer::Reader reader(&buff);
            recvAction.ApplyDifferential(reader);

            REQUIRE(sendAction == recvAction);
        }

        {
            Buffer buff(1000);
            Buffer::Writer writer(&buff);

            sendAction.EventName = "Plot twist !";

            sendAction.GenerateDifferential(recvAction, writer);

            Buffer::Reader reader(&buff);
            recvAction.ApplyDifferential(reader);

            REQUIRE(sendAction == recvAction);
        }
    }

    GIVEN("Full Mods")
    {
        Mods sendMods, recvMods;

        Buffer buff(1000);
        Buffer::Writer writer(&buff);

        sendMods.ModList.push_back({"Hello", 42});
        sendMods.ModList.push_back({"Hi", 14});
        sendMods.ModList.push_back({"Test", 8});
        sendMods.ModList.push_back({"Toast", 49});

        sendMods.Serialize(writer);

        Buffer::Reader reader(&buff);
        recvMods.Deserialize(reader);

        REQUIRE(sendMods == recvMods);
    }

    GIVEN("AnimationVariables")
    {
        AnimationVariables vars, recvVars;

        vars.Booleans.resize(76);
        String testString(
            "\xDE\xAD\xBE\xEF"
            "\xDE\xAD\xBE\xEF\x76\xB");
        vars.String_to_VectorBool(testString, vars.Booleans);

        vars.Floats.push_back(1.f);
        vars.Floats.push_back(7.f);
        vars.Floats.push_back(12.f);
        vars.Floats.push_back(0.f);
        vars.Floats.push_back(145.f);
        vars.Floats.push_back(100.f);
        vars.Floats.push_back(-1.f);

        vars.Integers.push_back(0);
        vars.Integers.push_back(12000);
        vars.Integers.push_back(06);
        vars.Integers.push_back(7778);
        vars.Integers.push_back(41104539);

        Buffer buff(1000);
        {
            Buffer::Writer writer(&buff);

            vars.GenerateDiff(recvVars, writer);

            Buffer::Reader reader(&buff);
            recvVars.ApplyDiff(reader);

            REQUIRE(vars.Booleans == recvVars.Booleans);
            REQUIRE(vars.Floats == recvVars.Floats);
            REQUIRE(vars.Integers == recvVars.Integers);
        }

        vars.Booleans.resize(33);
        vars.Booleans[16] = false;
        vars.Booleans[17] = false;
        vars.Booleans[18] = false;
        vars.Booleans[19] = false;
        vars.Floats[3] = 42.f;
        vars.Integers[0] = 18;
        vars.Integers[3] = 0;

        {
            Buffer::Writer writer(&buff);

            vars.GenerateDiff(recvVars, writer);

            Buffer::Reader reader(&buff);
            recvVars.ApplyDiff(reader);

            REQUIRE(vars.Booleans == recvVars.Booleans);
            REQUIRE(vars.Floats == recvVars.Floats);
            REQUIRE(vars.Integers == recvVars.Integers);
        }
    }
}

TEST_CASE("Packets", "[encoding.packets]")
{
    SECTION("AuthenticationRequest")
    {
        Buffer buff(1000);

        AuthenticationRequest sendMessage, recvMessage;
        sendMessage.Token = "TesSt";
        sendMessage.UserMods.ModList.push_back({"Hello", 42});
        sendMessage.UserMods.ModList.push_back({"Hi", 14});
        sendMessage.UserMods.ModList.push_back({"Test", 8});
        sendMessage.UserMods.ModList.push_back({"Toast", 49});
        sendMessage.UserMods.ModList[0].ContentSize = 123456;
        sendMessage.UserMods.ModList[0].ContentSha256.fill(0xA5);
        sendMessage.UserMods.ModList[0].HasFingerprint = true;

        Buffer::Writer writer(&buff);
        sendMessage.Serialize(writer);

        Buffer::Reader reader(&buff);

        uint64_t trash;
        reader.ReadBits(trash, 8); // pop opcode

        recvMessage.DeserializeRaw(reader);

        REQUIRE(sendMessage == recvMessage);
    }

    SECTION("AuthenticationResponse")
    {
        Buffer buff(1000);

        AuthenticationResponse sendMessage, recvMessage;
        sendMessage.Type = AuthenticationResponse::ResponseType::kAccepted;
        sendMessage.CampaignId = "ad9dc044-d8d7-4da2-a857-079c436f202a";
        sendMessage.CampaignRevision = 0x1020304050607080ULL;
        sendMessage.AuthorityEpoch = 12;
        sendMessage.UserMods.ModList.push_back({"Hello", 42});
        sendMessage.UserMods.ModList.push_back({"Hi", 14});
        sendMessage.UserMods.ModList.push_back({"Test", 8});
        sendMessage.UserMods.ModList.push_back({"Toast", 49});

        Buffer::Writer writer(&buff);
        sendMessage.Serialize(writer);

        Buffer::Reader reader(&buff);

        uint64_t trash;
        reader.ReadBits(trash, 8); // pop opcode

        recvMessage.DeserializeRaw(reader);

        REQUIRE(sendMessage == recvMessage);
    }

    SECTION("Exact plugin manifest comparison")
    {
        auto makeEntry = [](const char* acName, const uint16_t aId, const bool aIsLite, const uint64_t aSize, const uint8_t aHashByte)
        {
            Mods::Entry entry;
            entry.Filename = acName;
            entry.Id = aId;
            entry.IsLite = aIsLite;
            entry.ContentSize = aSize;
            entry.ContentSha256.fill(aHashByte);
            entry.HasFingerprint = true;
            return entry;
        };

        Mods expected;
        expected.ModList.push_back(makeEntry("Same.esp", 1, false, 100, 0x11));
        expected.ModList.push_back(makeEntry("Missing.esm", 2, false, 200, 0x22));
        expected.ModList.push_back({"Unverifiable.esl", 3, true});

        Mods actual;
        actual.ModList.push_back(makeEntry("same.ESP", 9, true, 101, 0x33));
        actual.ModList.push_back({"Unverifiable.esl", 3, true});
        actual.ModList.push_back(makeEntry("Unexpected.esp", 4, false, 400, 0x44));

        const auto differences = Mods::Compare(expected, actual);
        REQUIRE(differences.size() == 4);

        const auto changed = std::find_if(
            differences.begin(), differences.end(), [](const Mods::Difference& acDifference) { return acDifference.HasExpected && acDifference.Expected.Filename == "Same.esp"; });
        REQUIRE(changed != differences.end());
        REQUIRE((changed->MismatchFlags & Mods::kPluginType) != 0);
        REQUIRE((changed->MismatchFlags & Mods::kLoadOrder) != 0);
        REQUIRE((changed->MismatchFlags & Mods::kContentSize) != 0);
        REQUIRE((changed->MismatchFlags & Mods::kContentHash) != 0);

        const auto missing =
            std::find_if(differences.begin(), differences.end(), [](const Mods::Difference& acDifference) { return acDifference.MismatchFlags == Mods::kMissing; });
        REQUIRE(missing != differences.end());
        REQUIRE(missing->Expected.Filename == "Missing.esm");

        const auto unverifiable =
            std::find_if(differences.begin(), differences.end(), [](const Mods::Difference& acDifference) { return acDifference.MismatchFlags == Mods::kUnverifiable; });
        REQUIRE(unverifiable != differences.end());
        REQUIRE(unverifiable->Expected.Filename == "Unverifiable.esl");

        const auto unexpected =
            std::find_if(differences.begin(), differences.end(), [](const Mods::Difference& acDifference) { return acDifference.MismatchFlags == Mods::kUnexpected; });
        REQUIRE(unexpected != differences.end());
        REQUIRE(unexpected->Actual.Filename == "Unexpected.esp");
    }

    SECTION("Revisioned quest messages")
    {
        Buffer requestBuffer(1000);
        RequestQuestUpdate sendRequest, recvRequest;
        sendRequest.Id = GameId(42, 0x123456);
        sendRequest.Stage = 160;
        sendRequest.Status = RequestQuestUpdate::StageUpdate;
        sendRequest.ClientQuestType = 4;
        sendRequest.TransactionId = 0x1122334455667788ULL;

        Buffer::Writer requestWriter(&requestBuffer);
        sendRequest.Serialize(requestWriter);
        Buffer::Reader requestReader(&requestBuffer);
        uint64_t opcode;
        requestReader.ReadBits(opcode, 8);
        recvRequest.DeserializeRaw(requestReader);
        REQUIRE(sendRequest == recvRequest);

        Buffer notifyBuffer(1000);
        NotifyQuestUpdate sendNotify, recvNotify;
        sendNotify.Id = sendRequest.Id;
        sendNotify.Stage = sendRequest.Stage;
        sendNotify.Status = NotifyQuestUpdate::StageUpdate;
        sendNotify.ClientQuestType = sendRequest.ClientQuestType;
        sendNotify.TransactionId = sendRequest.TransactionId;
        sendNotify.Revision = 9876543210ULL;
        sendNotify.AuthorityEpoch = 7;

        Buffer::Writer notifyWriter(&notifyBuffer);
        sendNotify.Serialize(notifyWriter);
        Buffer::Reader notifyReader(&notifyBuffer);
        notifyReader.ReadBits(opcode, 8);
        recvNotify.DeserializeRaw(notifyReader);
        REQUIRE(sendNotify == recvNotify);
    }

    SECTION("AssignCharacterRequest")
    {
        Buffer buff(1000);

        ActionEvent sendAction;
        sendAction.ActionId = 42;
        sendAction.State1 = 6547;
        sendAction.Tick = 48;
        sendAction.ActorId = 12345678;
        sendAction.EventName = "test";
        sendAction.IdleId = 87964;
        sendAction.State2 = 8963;
        sendAction.TargetEventName = "toast";
        sendAction.TargetId = 963741;
        sendAction.Type = 4;

        AssignCharacterRequest sendMessage, recvMessage;
        sendMessage.Cookie = 14523698;
        sendMessage.AppearanceBuffer = "toto";
        sendMessage.CellId.BaseId = 45;
        sendMessage.FormId.ModId = 48;
        sendMessage.ReferenceId.BaseId = 456799;
        sendMessage.ReferenceId.ModId = 4079;
        sendMessage.LatestAction = sendAction;
        sendMessage.Position.x = -452.4f;
        sendMessage.Position.y = 452.4f;
        sendMessage.Position.z = 125452.4f;
        sendMessage.Rotation.x = -1.87f;
        sendMessage.Rotation.y = 45.35f;

        Buffer::Writer writer(&buff);
        sendMessage.Serialize(writer);

        Buffer::Reader reader(&buff);

        uint64_t trash;
        reader.ReadBits(trash, 8); // pop opcode

        recvMessage.DeserializeRaw(reader);

        REQUIRE(sendMessage == recvMessage);
    }

    GIVEN("ClientReferencesMoveRequest")
    {
        ClientReferencesMoveRequest sendMessage, recvMessage;
        auto& update = sendMessage.Updates[1];
        update.CombatTargetServerId = 42;
        auto& move = update.UpdatedMovement;

        AnimationVariables vars;
        vars.Booleans.resize(76);
        String testString("\xDE\xAD\xBE\xEF\x76\xB");
        vars.String_to_VectorBool(testString, vars.Booleans);

        vars.Floats.push_back(1.f);
        vars.Floats.push_back(7.f);
        vars.Floats.push_back(12.f);
        vars.Floats.push_back(0.f);
        vars.Floats.push_back(145.f);
        vars.Floats.push_back(100.f);
        vars.Floats.push_back(-1.f);

        vars.Integers.push_back(0);
        vars.Integers.push_back(12000);
        vars.Integers.push_back(06);
        vars.Integers.push_back(7778);
        vars.Integers.push_back(41104539);

        move.Variables = vars;

        auto& pose = update.EvaluatedPose;
        pose.GraphDescriptor = 0xAABBCCDDEEFF0011ULL;
        pose.SourceTick = 7878;
        EvaluatedPoseSnapshot::Bone bone{};
        bone.Translation = {12.5f, -3.25f, 0.f};
        bone.Rotation = {0.f, 0.f, 0.70710677f, 0.70710677f};
        bone.Scale = {1.f, 1.f, 1.f};
        pose.Bones.push_back(bone);

        auto& visual = update.VisualBones;
        visual.GraphDescriptor = pose.GraphDescriptor;
        visual.SourceTick = pose.SourceTick;
        visual.RootWorld.Present = true;
        visual.RootWorld.Rotation = {1.f, 0.f, 0.f, 0.f, 1.f, 0.f, 0.f, 0.f, 1.f};
        visual.RootWorld.Translation = {200.f, 300.f, 400.f};
        visual.RootWorld.Scale = 1.f;
        VisualBoneSnapshot::Bone visualBone{};
        visualBone.Present = true;
        visualBone.Rotation = {1.f, 0.f, 0.f, 0.f, 1.f, 0.f, 0.f, 0.f, 1.f};
        visualBone.Translation = {12.5f, -3.25f, 7.f};
        visualBone.Scale = 1.f;
        visual.Bones.push_back(visualBone);
        visual.Bones.push_back({}); // no live NiNode at this graph index

        Buffer buff(2000);
        Buffer::Writer writer(&buff);
        sendMessage.Serialize(writer);

        Buffer::Reader reader(&buff);

        uint64_t trash;
        reader.ReadBits(trash, 8); // pop opcode

        recvMessage.DeserializeRaw(reader);

        REQUIRE(recvMessage.Updates[1].UpdatedMovement == sendMessage.Updates[1].UpdatedMovement);
        REQUIRE(recvMessage.Updates[1].CombatTargetServerId == 42);
        // The pose is packed (0.01-unit translation, 15-bit smallest-three rotation): compare within that.
        const auto& received = recvMessage.Updates[1].EvaluatedPose;
        REQUIRE(received.GraphDescriptor == pose.GraphDescriptor);
        REQUIRE(received.SourceTick == pose.SourceTick);
        REQUIRE(received.Bones.size() == pose.Bones.size());
        for (int i = 0; i < 3; ++i)
            REQUIRE(std::abs(received.Bones[0].Translation[i] - pose.Bones[0].Translation[i]) <= 0.005f);
        float dot = 0.f;
        for (int i = 0; i < 4; ++i)
            dot += received.Bones[0].Rotation[i] * pose.Bones[0].Rotation[i];
        REQUIRE(std::abs(dot) > 0.99999f);
        REQUIRE(received.Bones[0].Scale == pose.Bones[0].Scale);
        REQUIRE(recvMessage.Updates[1].VisualBones == visual);
        REQUIRE(recvMessage.Updates[1].VisualBones.Checksum() == visual.Checksum());
    }
}

TEST_CASE("Evaluated pose transport bounds", "[encoding.pose]")
{
    EvaluatedPoseSnapshot pose;
    pose.Bones.resize(EvaluatedPoseSnapshot::MaxBones + 1);
    REQUIRE_FALSE(pose.IsValid());
    pose.Bones.resize(1);
    pose.Bones[0].Scale = {1.f, 1.f, 1.f};
    pose.Bones[0].Rotation = {0.f, 0.f, 0.f, 1.f};
    REQUIRE(pose.IsValid());
    pose.Bones[0].Rotation[0] = std::numeric_limits<float>::infinity();
    REQUIRE_FALSE(pose.IsValid());
}

TEST_CASE("Evaluated pose carries a dying owner's bone-0 frame", "[encoding.pose]")
{
    // Lokir's flash at death (2026-09-30): receivers rebase bone 0 onto the owner's frame at capture.
    EvaluatedPoseSnapshot pose;
    pose.GraphDescriptor = 7;
    pose.SourceTick = 1234;
    pose.Bones.resize(2);
    for (auto& bone : pose.Bones)
    {
        bone.Translation = {1.f, 2.f, 3.f};
        bone.Rotation = {0.f, 0.f, 0.f, 1.f};
        bone.Scale = {1.f, 1.f, 1.f};
    }
    for (const bool withParent : {false, true})
    {
        pose.HasParent = withParent;
        pose.ParentTranslation = {16106.f, -82438.f, 8200.f};
        pose.ParentRotation = {0.866f, -0.5f, 0.f, 0.5f, 0.866f, 0.f, 0.f, 0.f, 1.f};
        pose.ParentScale = 1.f;
        Buffer buffer(4096);
        Buffer::Writer writer(&buffer);
        pose.Serialize(writer);
        Buffer::Reader reader(&buffer);
        EvaluatedPoseSnapshot received;
        received.Deserialize(reader);
        REQUIRE(received.HasParent == withParent);
        REQUIRE(received.Bones.size() == 2);
        if (withParent)
        {
            REQUIRE(received.ParentTranslation == pose.ParentTranslation);
            REQUIRE(received.ParentRotation == pose.ParentRotation);
            REQUIRE(received.ParentScale == pose.ParentScale);
        }
    }
}

TEST_CASE("Evaluated pose packing keeps every bone within tolerance", "[encoding.pose]")
{
    EvaluatedPoseSnapshot pose;
    pose.GraphDescriptor = 1;
    pose.SourceTick = 99;
    for (int i = 0; i < 99; ++i)
    {
        EvaluatedPoseSnapshot::Bone bone{};
        const float a = static_cast<float>(i) * 0.37f;
        bone.Translation = {std::sin(a) * 40.f, std::cos(a) * 12.f, i == 0 ? 500.f : -3.f};
        const float half = a * 0.5f;
        const float axis[3]{0.26726124f, 0.53452248f, 0.80178373f}; // unit length
        bone.Rotation = {axis[0] * std::sin(half), axis[1] * std::sin(half), axis[2] * std::sin(half), std::cos(half)};
        bone.Scale = i == 7 ? std::array<float, 3>{1.1f, 1.1f, 1.1f} : std::array<float, 3>{1.f, 1.f, 1.f};
        pose.Bones.push_back(bone);
    }
    Buffer buffer(8000);
    Buffer::Writer writer(&buffer);
    pose.Serialize(writer);
    REQUIRE(writer.GetBytePosition() < 99 * 14 + 32);
    Buffer::Reader reader(&buffer);
    EvaluatedPoseSnapshot received;
    received.Deserialize(reader);
    REQUIRE(received.Bones.size() == 99);
    for (size_t b = 0; b < 99; ++b)
    {
        for (int i = 0; i < 3; ++i)
            REQUIRE(std::abs(received.Bones[b].Translation[i] - pose.Bones[b].Translation[i]) <= 0.005f);
        float dot = 0.f;
        for (int i = 0; i < 4; ++i)
            dot += received.Bones[b].Rotation[i] * pose.Bones[b].Rotation[i];
        REQUIRE(std::abs(dot) > 0.99999f);
        REQUIRE(received.Bones[b].Scale == pose.Bones[b].Scale);
    }
}

TEST_CASE("Visual bone transport bounds", "[encoding.visual_pose]")
{
    VisualBoneSnapshot visual;
    visual.Bones.resize(VisualBoneSnapshot::MaxBones + 1);
    REQUIRE_FALSE(visual.IsValid());
    visual.Bones.resize(1);
    visual.Bones[0].Present = true;
    visual.Bones[0].Rotation = {1.f, 0.f, 0.f, 0.f, 1.f, 0.f, 0.f, 0.f, 1.f};
    visual.Bones[0].Scale = 1.f;
    REQUIRE(visual.IsValid());
    visual.RootWorld.Present = true;
    visual.RootWorld.Rotation = {1.f, 0.f, 0.f, 0.f, 1.f, 0.f, 0.f, 0.f, 1.f};
    visual.RootWorld.Translation = {100.f, 200.f, 300.f};
    REQUIRE(visual.IsValid());
    visual.RootWorld.Translation[0] = std::numeric_limits<float>::infinity();
    REQUIRE_FALSE(visual.IsValid());
    visual.RootWorld.Translation[0] = 100.f;
    visual.Bones[0].Rotation[0] = std::numeric_limits<float>::infinity();
    REQUIRE_FALSE(visual.IsValid());
}

TEST_CASE("StringCache", "[encoding.string_cache]")
{
    SECTION("Messages")
    {
        StringCacheUpdate update;
        update.Values.push_back("Hello");
        update.Values.push_back("Bye");

        Buffer buff(1000);
        Buffer::Writer writer(&buff);
        update.Serialize(writer);

        Buffer::Reader reader(&buff);

        uint64_t trash;
        reader.ReadBits(trash, 8); // pop opcode

        StringCacheUpdate recvUpdate;
        recvUpdate.DeserializeRaw(reader);

        REQUIRE(update == recvUpdate);
    }
}
