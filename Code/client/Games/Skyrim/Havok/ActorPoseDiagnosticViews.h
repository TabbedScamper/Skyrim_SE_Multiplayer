#pragma once

#include <cstddef>
#include <cstdint>

// Read-only layout views used by the in-game diagnostic bridge. These do not
// own native objects and deliberately expose no mutating methods. Offsets are
// pinned to CommonLibSSE-NG b93280e832f263dbef44e44cbe2936622a02f91a and
// remain runtime candidates until exercised on Skyrim AE 1.7.104.
namespace ActorPoseDiagnosticViews
{
template <class T> struct HavokArray
{
    T* data;                  // 00
    std::int32_t size;        // 08
    std::int32_t capacityAndFlags; // 0C
};

static_assert(sizeof(HavokArray<void*>) == 0x10);

// hkbCharacter::poseLocal points to contiguous hkQsTransform values. The
// 0x30-byte translation/quaternion/scale shape is also documented by the
// open-source Active Ragdoll SKSE research; validate count and full range at
// runtime before reading it on Skyrim AE 1.7.104.
struct alignas(16) QsTransform
{
    float translation[4];
    float rotation[4];
    float scale[4];
};
static_assert(sizeof(QsTransform) == 0x30);

struct Character
{
    void* vtable;                         // 00
    std::uint8_t pad08[0x10 - 0x08];
    HavokArray<Character*> nearbyCharacters; // 10
    std::int16_t currentLOD;              // 20
    std::int16_t numTracksInLOD;          // 22
    std::uint32_t pad24;
    const char* name;                     // 28 (hkStringPtr)
    void* ragdollDriver;                  // 30 (hkRefPtr<hkbRagdollDriver>)
    void* characterControllerDriver;      // 38 (hkRefVariant)
    void* footIkDriver;                   // 40 (hkRefVariant)
    void* handIkDriver;                   // 48 (hkRefVariant)
    void* setup;                          // 50 (hkRefPtr<hkbCharacterSetup>)
    void* behaviorGraph;                  // 58 (hkRefPtr<hkbBehaviorGraph>)
    void* projectData;                    // 60
    void* animationBindingSet;            // 68
    void* raycastInterface;               // 70
    void* world;                          // 78
    void* eventQueue;                     // 80
    void* worldFromModel;                 // 88
    const QsTransform* poseLocal;          // 90
    std::int32_t numPoseLocal;            // 98
    bool deleteWorldFromModel;            // 9C
    bool deletePoseLocal;                 // 9D
    std::uint16_t pad9E;
};

static_assert(offsetof(Character, ragdollDriver) == 0x30);
static_assert(offsetof(Character, setup) == 0x50);
static_assert(offsetof(Character, behaviorGraph) == 0x58);
static_assert(offsetof(Character, worldFromModel) == 0x88);
static_assert(offsetof(Character, poseLocal) == 0x90);
static_assert(sizeof(Character) == 0xA0);

struct BoneNodeEntry
{
    void* node; // NiNode*
    std::uint32_t unk08;
    std::uint32_t unk0C;
};

static_assert(sizeof(BoneNodeEntry) == 0x10);

struct AnimationGraph
{
    std::uint8_t base[0xC0];
    Character characterInstance;                 // 0C0
    struct GameArrayView
    {
        BoneNodeEntry* data;
        std::uint32_t capacity;
        std::uint32_t pad0C;
        std::uint32_t length;
        std::uint32_t pad14;
    } boneNodes;                                  // 160 (BSTArray)
    std::uint8_t pad178[0x1F0 - 0x178];
    const char* projectName;                      // 1F0 (BSFixedString)
    void* projectResource;                        // 1F8
    void* projectDBData;                          // 200
    void* behaviorGraph;                          // 208
    void* holder;                                 // 210 (Actor*)
    void* rootNode;                               // 218 (BSFadeNode*)
    void* generatorOutputs[2];                    // 220
    float interpolationAmounts[2];                // 230
    void* physicsWorld;                           // 238 (bhkWorld*)
    std::uint16_t numAnimBones;                   // 240
    std::uint8_t pad242[0x250 - 0x242];
};

static_assert(offsetof(AnimationGraph, characterInstance) == 0xC0);
static_assert(offsetof(AnimationGraph, boneNodes) == 0x160);
static_assert(offsetof(AnimationGraph, projectName) == 0x1F0);
static_assert(offsetof(AnimationGraph, behaviorGraph) == 0x208);
static_assert(offsetof(AnimationGraph, rootNode) == 0x218);
static_assert(offsetof(AnimationGraph, physicsWorld) == 0x238);
static_assert(offsetof(AnimationGraph, numAnimBones) == 0x240);
static_assert(sizeof(AnimationGraph) == 0x250);

struct BehaviorGraph
{
    std::uint8_t pad00[0x80];
    void* rootGenerator;                           // 080 (hkRefPtr<hkbGenerator>)
    std::uint8_t pad88[0x90 - 0x88];
    void* rootGeneratorClone;                      // 090 (hkRefVariant)
    void* activeNodes;                             // 098 (NodeList* / hkArray<hkbNodeInfo>*)
    std::uint8_t padA0[0x100 - 0xA0];
    std::int32_t numIntermediateOutputs;           // 100
    std::uint8_t pad104[0x128 - 0x104];
    std::int16_t numStaticNodes;                   // 128
    std::int16_t nextUniqueID;                     // 12A
    bool isActive;                                 // 12C
    bool isLinked;                                 // 12D
    bool updateActiveNodes;                        // 12E
    bool stateOrTransitionChanged;                 // 12F
};

static_assert(offsetof(BehaviorGraph, rootGenerator) == 0x80);
static_assert(offsetof(BehaviorGraph, rootGeneratorClone) == 0x90);
static_assert(offsetof(BehaviorGraph, activeNodes) == 0x98);
static_assert(offsetof(BehaviorGraph, isActive) == 0x12C);
static_assert(sizeof(BehaviorGraph) == 0x130);

struct ActiveNodeList
{
    void* data;                                    // 00 (hkbNodeInfo*)
    std::int32_t size;                             // 08
    std::uint32_t unknown0C;                       // 0C (hkArray capacity/flags in CommonLib)
};
static_assert(sizeof(ActiveNodeList) == 0x10);

struct ActiveNodeInfo
{
    std::uint8_t pad00[0x50];
    void* nodeTemplate;                            // 50
    void* nodeClone;                               // 58
    void* behavior;                                // 60
    std::uint8_t pad68[0x84 - 0x68];
    std::uint8_t byte84;                           // 84 (existing event-loop gate)
    std::uint8_t byte85;                           // 85 (existing event dispatch branch)
    std::uint8_t pad86[0x90 - 0x86];
};
static_assert(offsetof(ActiveNodeInfo, nodeTemplate) == 0x50);
static_assert(offsetof(ActiveNodeInfo, nodeClone) == 0x58);
static_assert(offsetof(ActiveNodeInfo, byte84) == 0x84);
static_assert(sizeof(ActiveNodeInfo) == 0x90);

struct StateMachine
{
    std::uint8_t pad00[0x38];
    const char* name;                              // 038 (inherited hkbNode::name)
    std::uint16_t nodeID;                          // 040
    std::uint8_t pad42[0x80 - 0x42];
    std::int32_t currentStateID;                   // 080
    std::uint8_t pad84[0x88 - 0x84];
    bool isActive;                                 // 088
    std::uint8_t pad89[0xF0 - 0x89];
    float timeInState;                             // 0F0
    float lastLocalTime;                           // 0F4
    std::int32_t previousStateID;                  // 0F8
    std::int32_t nextStartStateIndexOverride;      // 0FC
    bool stateOrTransitionChanged;                 // 100
    bool echoNextUpdate;                           // 101
    std::uint16_t currentStateIndexAndEntered;     // 102
    std::uint32_t pad104;
};

static_assert(offsetof(StateMachine, currentStateID) == 0x80);
static_assert(offsetof(StateMachine, timeInState) == 0xF0);
static_assert(sizeof(StateMachine) == 0x108);

struct RagdollDriver
{
    std::uint8_t pad00[0x80];
    void* character;                               // 080
    void* ragdoll;                                 // 088 (hkaRagdollInstance*)
};

static_assert(offsetof(RagdollDriver, ragdoll) == 0x88);

struct RagdollInstance
{
    std::uint8_t pad00[0x10];
    HavokArray<void*> rigidBodies;                 // 10 (hkpRigidBody*)
    HavokArray<void*> constraints;                 // 20
    HavokArray<std::int32_t> boneToRigidBodyMap;   // 30
    const void* skeleton;                          // 40
};

static_assert(offsetof(RagdollInstance, rigidBodies) == 0x10);
static_assert(offsetof(RagdollInstance, constraints) == 0x20);
static_assert(offsetof(RagdollInstance, boneToRigidBodyMap) == 0x30);
static_assert(sizeof(RagdollInstance) == 0x48);

// hkpRigidBody derives hkpEntity. Only source-backed read fields required by
// the checksum are represented; everything else remains opaque padding.
struct RigidBody
{
    std::uint8_t pad00[0x10];
    void* world;                                   // 010 (hkpWorldObject::world)
    std::uint8_t pad18[0x13C - 0x18];
    std::uint32_t uid;                             // 13C
    std::uint8_t pad140[0x160 - 0x140];
    std::uint8_t motionType;                       // 160 (hkpEntity::motion + 0x10)
    std::uint8_t pad161[0x170 - 0x161];
    float transform[16];                           // 170 (hkMotionState::transform)
    std::uint8_t pad1B0[0x230 - 0x1B0];
    float linearVelocity[4];                       // 230
    float angularVelocity[4];                      // 240
};

static_assert(offsetof(RigidBody, world) == 0x10);
static_assert(offsetof(RigidBody, motionType) == 0x160);
static_assert(offsetof(RigidBody, transform) == 0x170);
static_assert(offsetof(RigidBody, linearVelocity) == 0x230);
static_assert(sizeof(RigidBody) == 0x250);
}
