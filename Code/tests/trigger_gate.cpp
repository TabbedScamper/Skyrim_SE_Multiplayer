// Already registered by TPTests' add_files("*.cpp"). Standalone compilation
// with TRIGGER_GATE_STANDALONE_TEST runs these tests without building the game.
#ifdef TRIGGER_GATE_STANDALONE_TEST
#define CATCH_CONFIG_MAIN
#endif
#include <catch2/catch.hpp>
#include "../client/Games/Skyrim/TriggerCapabilities.h"
#include "../client/Games/Skyrim/TriggerPartyContact.h"

using namespace TriggerCapabilities;

TEST_CASE("Audited actor wake helpers never wait for the party", "[trigger]")
{
    for (const auto name : {"defaultForceEvaluatePackageTrigger", "DEFAULTlinkRefStartCombatPlayer",
             "defaultSetLinkAVVar", "defaultSetMultiAVTriggerScript"})
    {
        REQUIRE(Declaration(name) == Bucket::ActorWake);
        REQUIRE_FALSE(Hold(Declaration(name)));
    }
}

TEST_CASE("Generic activation declarations use the activation bucket", "[trigger]")
{
    REQUIRE(Declaration("DEFAULTACTIVATESELF") == Bucket::Activation);
    REQUIRE(Declaration("defaultActivateLinkDoOnceSCRIPT") == Bucket::Activation);
    REQUIRE_FALSE(Hold(Declaration("defaultActivateActivateLinkedRefOnce")));
}

TEST_CASE("Door and unbounded declarations retain the gate", "[trigger]")
{
    REQUIRE(Hold(Declaration("defaultActivateOpenLinkedRef")));
    REQUIRE(Hold(Declaration("defaultBlockActivation")));
    REQUIRE(Hold(Declaration("defaultSetStageTRIGSCRIPT")));
    REQUIRE(Hold(Declaration("CustomAmbushTeleportPlayer")));
    REQUIRE(Hold(Declaration("defaultActivateSelfChild")));
    REQUIRE(Hold(Declaration("")));
}

TEST_CASE("Actor enable helpers require target classification", "[trigger]")
{
    REQUIRE(Declaration("defaultEnableEncLinkedRef") == Bucket::ActorEnable);
    REQUIRE(Declaration("defaultEnableDisableLinkedRef") == Bucket::ActorEnable);
    REQUIRE(Declaration("defaultPlayerEnableDisableLinkedRef") == Bucket::ActorEnable);
    REQUIRE(Hold(Merge(Bucket::ActorEnable, Bucket::DoorOrMovement)));
    REQUIRE(Hold(Merge(Bucket::Activation, Bucket::Unknown)));
    REQUIRE_FALSE(Hold(Merge(Bucket::Activation, Bucket::ActorWake)));
}

TEST_CASE("All scripts and inherited property capabilities contribute", "[trigger]")
{
    REQUIRE(Merge(Bucket::ActorWake, PropertyType("Scene")) == Bucket::QuestOrScene);
    REQUIRE(Merge(Bucket::Activation, PropertyType("Quest")) == Bucket::QuestOrScene);
    REQUIRE(Hold(Merge(Bucket::ActorWake, PropertyType("ReferenceAlias"))));
    REQUIRE(Hold(Merge(Bucket::ActorWake, Bucket::Unknown)));
    REQUIRE(Hold(Merge(Bucket::Unknown, Bucket::ActorWake)));
    REQUIRE_FALSE(Hold(Merge(Bucket::None, Bucket::ActorWake)));
    REQUIRE(PropertyType("Actor") == Bucket::None);
    REQUIRE(PropertyType("QuestLikeCustomName") == Bucket::None);
}

TEST_CASE("Five player occupancy produces one enter and one final leave", "[trigger]")
{
    using namespace TriggerPartyContact;
    Occupancy contact;
    REQUIRE(contact.Observe(Enter, 2, 100)); // Remote arrives first.
    REQUIRE_FALSE(contact.Observe(Enter, 1, 100)); // Host joins.
    for (uint32_t id = 3; id <= 5; ++id)
        REQUIRE_FALSE(contact.Observe(Enter, id, 100));
    REQUIRE(contact.Members.size() == 5);
    REQUIRE_FALSE(contact.Observe(Enter, 2, 100)); // Duplicate native body.
    for (uint32_t id = 1; id < 5; ++id)
        REQUIRE_FALSE(contact.Observe(Leave, id, 101));
    REQUIRE(contact.Observe(Leave, 5, 101));
    REQUIRE_FALSE(contact.Observe(Leave, 5, 101));
    REQUIRE(contact.Observe(Enter, 4, 102));
}

TEST_CASE("Repeat events are deduplicated across party members per update", "[trigger]")
{
    using namespace TriggerPartyContact;
    Occupancy contact;
    REQUIRE(contact.Observe(Enter, 1, 0));
    REQUIRE(contact.Observe(Trigger, 1, 0));
    REQUIRE_FALSE(contact.Observe(Trigger, 2, 0));
    REQUIRE_FALSE(contact.Observe(Trigger, 1, 0));
    REQUIRE(contact.Observe(Trigger, 2, 1));
    REQUIRE_FALSE(contact.Observe(Leave, 99, 1));
    REQUIRE(contact.Members.size() == 2);
}
