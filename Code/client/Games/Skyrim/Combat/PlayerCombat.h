#pragma once
#include <Messages/PlayerCombatState.h>
#include <vector>

struct Actor;

namespace PlayerCombat
{
// Runner event: damage is recorded on the simulation thread and sent on the world thread.
struct Impact
{
    uint32_t AttackerFormId{};
    uint32_t VictimFormId{};
    float Damage{};
    bool KillMove{};
    bool ThreatOnly{};
    uint64_t SessionEpoch{};
};

bool Capture(Actor* apActor, PlayerCombatState& aState);
void Publish(Actor* apActor, const PlayerCombatState& aState);
void SetTeammate(Actor* apActor, bool aValue);
void Forget(uint32_t aFormId);
void Clear();
int32_t DetectionLevel(Actor* apObserver, Actor* apTarget);
std::vector<Actor*> Observers();
// Own the current snapshot while borrowing only empty vector capacity between
// calls. Nested calls and other threads never overwrite a live snapshot.
class ObserverSnapshot
{
public:
    ObserverSnapshot();
    ~ObserverSnapshot();
    ObserverSnapshot(const ObserverSnapshot&) = delete;
    ObserverSnapshot& operator=(const ObserverSnapshot&) = delete;
    const std::vector<Actor*>& Get() const noexcept { return m_actors; }

private:
    std::vector<Actor*> m_actors;
};
bool HasSnapshot(Actor* apActor);
void GetAwareness(Actor* apTarget, int32_t& aLevel, uint32_t& aLOSCount, const std::vector<Actor*>& acObservers);
uint8_t GetMeterLevel(Actor* apTarget, int32_t aDetectionLevel);
void SetMeter(int32_t aLevel, uint32_t aLOSCount);
void ApplyDamage(Actor* apVictim, Actor* apAttacker, float aDamage, bool aKillMove);
}
