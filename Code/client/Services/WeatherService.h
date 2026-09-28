#pragma once

struct World;
struct TransportService;
struct UpdateEvent;
struct DisconnectedEvent;
struct PartyJoinedEvent;
struct PartyLeftEvent;
struct NotifyWeatherChange;

/**
 * @brief Responsible for weather changes, which is controlled on a party-per-party basis.
 */
struct WeatherService
{
    WeatherService(World& aWorld, TransportService& aTransport, entt::dispatcher& aDispatcher);
    ~WeatherService() noexcept = default;

    TP_NOCOPYMOVE(WeatherService);

protected:
    void OnUpdate(const UpdateEvent& acEvent) noexcept;
    void OnDisconnected(const DisconnectedEvent& acEvent) noexcept;
    void OnPartyJoinedEvent(const PartyJoinedEvent& acEvent) noexcept;
    void OnPartyLeftEvent(const PartyLeftEvent& acEvent) noexcept;
    void OnWaitingFor3DRemoved(entt::registry& aRegistry, entt::entity aEntity) noexcept;
    void OnPlayerComponentRemoved(entt::registry& aRegistry, entt::entity aEntity) noexcept;
    void OnWeatherChange(const NotifyWeatherChange& acMessage) noexcept;

    void RunWeatherUpdates(const double acDelta) noexcept;

    void ToggleGameWeatherSystem(bool aToggle) noexcept;
    void SetCachedWeather() noexcept;
    // Leader: send weather + sky state when it changes (and every 500 ms during a blend).
    void SendSkyState(uint32_t aWeatherId) noexcept;
    // Follower: hold the local sky on the host's blend and wind.
    void ApplyHostSky() noexcept;

private:
    World& m_world;
    TransportService& m_transport;

    /**
    * This variable has two uses:
    * For the party leader, it is used to detect weather changes.
    * For non-leaders, it is used to reapply the server weather if it changes.
    */
    uint32_t m_cachedWeatherId{};

    // Leader: last sky state sent. Follower: host sky state being held.
    struct SkyState
    {
        bool Valid{};
        uint32_t LastWeatherId{};
        float Percent{1.f};
        float WindSpeed{};
        float WindAngle{};
    };
    SkyState m_sentSky{};
    uint64_t m_nextSkySendMs{};
    SkyState m_hostSky{};

    entt::scoped_connection m_updateConnection;
    entt::scoped_connection m_disconnectConnection;
    entt::scoped_connection m_partyJoinedConnection;
    entt::scoped_connection m_partyLeftConnection;
    entt::scoped_connection m_playerAddedConnection;
    entt::scoped_connection m_playerRemovedConnection;
    entt::scoped_connection m_weatherChangeConnection;
};
