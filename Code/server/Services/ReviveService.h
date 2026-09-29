#pragma once

#include <Events/PacketEvent.h>
#include <Messages/ReviveData.h>
#include <map>
#include <string>

struct World;
struct Player;
struct UpdateEvent;
struct ReviveRequest;

struct ReviveService
{
    ReviveService(World& aWorld, entt::dispatcher& aDispatcher) noexcept;
    // Fallen (bled out, spectating) in its party's current epoch: door votes count it as ready.
    bool IsFallenPlayer(uint32_t aPlayerId) const noexcept;

private:
    struct State
    {
        ReviveData Data;
        uint32_t PartyId{};
        uint64_t Received{};
        uint64_t GrantUntil{};
        // A raise at this revision: a fallen report at or before it was sent before the owner heard of the raise.
        uint64_t RaisedRevision{};
    };
    struct Hold
    {
        uint32_t Target{};
        uint64_t Revision{};
        uint64_t Started{};
        uint64_t Received{};
    };

    void OnRequest(const PacketEvent<ReviveRequest>& aEvent) noexcept;
    void OnUpdate(const UpdateEvent&) noexcept;
    bool CombatAround(uint32_t aReviver, uint32_t aTarget) const noexcept;
    bool Eligible(uint32_t aReviver, const Hold& aHold, uint64_t aNow) const noexcept;
    bool Current(uint32_t aPlayer, const State& aState, uint64_t aNow) const noexcept;
    void Broadcast(Player* aPlayer, const ReviveData& aData) const noexcept;
    void OnRaise(Player* aCaster, const ReviveRequest& aRequest, uint64_t aNow) noexcept;
    // Fallen players by player id (a save reload keeps the connection and the id). A fallen player who disconnects
    // is kept by party and username and restored on rejoin when that name is unique in the party (names can repeat).
    struct Fallen
    {
        uint32_t PartyId{};
        uint64_t Epoch{};
        std::string Username;
    };
    using DepartedKey = std::pair<uint32_t, std::string>;
    bool IsFallen(Player* aPlayer, uint64_t aEpoch) const noexcept;
    void SetFallen(Player* aPlayer, uint64_t aEpoch) noexcept;

    World& m_world;
    std::map<uint32_t, State> m_states;
    std::map<uint32_t, Hold> m_holds;
    std::map<uint32_t, Fallen> m_fallen;
    std::map<DepartedKey, uint64_t> m_departed;
    // Party wipes (nobody standing): announced once per epoch, then the party reloads its checkpoint.
    struct Wipe
    {
        uint64_t Epoch{};
        uint64_t At{};
        bool Done{};
    };
    std::map<uint32_t, Wipe> m_wipes;
    void CheckWipes(uint64_t aNow) noexcept;
    entt::scoped_connection m_requestConnection;
    entt::scoped_connection m_updateConnection;
};
