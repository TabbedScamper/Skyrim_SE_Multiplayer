#pragma once

struct TESWeather;

struct Sky
{
    static Sky* Get() noexcept;

    static bool s_shouldUpdateWeather;

    virtual ~Sky();

    void SetWeather(TESWeather* apWeather) noexcept;
    void ForceWeather(TESWeather* apWeather) noexcept;
    void ReleaseWeatherOverride() noexcept;
    void ResetWeather() noexcept;

    TESWeather* GetWeather() const noexcept;

    uint8_t unk8[0x40 - 0x8];
    void* pClimate;                    // 0x40
    TESWeather* pCurrentWeather;
    // Offsets from CommonLibSSE-NG RE/S/Sky.h, confirmed in 1.7.104: the wind update (IDs 26229/26234) writes
    // +0x18C = current.windSpeed * pct + last.windSpeed * (1 - pct) and rolls +0x190 with the RNG on a weather
    // change; ForceWeather (26243) resets +0x50 and +0x1B8; +0x1DC bit 0x100000 is kUpdateWind.
    TESWeather* pLastWeather;           // 0x50
    uint8_t unk58[0x60 - 0x58];
    TESWeather* pOverrideWeather;       // 0x60
    uint8_t unk68[0x18C - 0x68];
    float windSpeed;                    // 0x18C
    float windAngle;                    // 0x190
    uint8_t unk194[0x1B4 - 0x194];
    float weatherStartHour;             // 0x1B4
    float currentWeatherPct;            // 0x1B8
    uint32_t mode;                      // 0x1BC
    uint8_t unk1C0[0x1D8 - 0x1C0];
    float weatherAcceleration;          // 0x1D8
    uint32_t flags;                     // 0x1DC
    uint8_t unk1E0[0x2C8 - 0x1E0];
};

static_assert(offsetof(Sky, pLastWeather) == 0x50);
static_assert(offsetof(Sky, pClimate) == 0x40);
static_assert(offsetof(Sky, pOverrideWeather) == 0x60);
static_assert(offsetof(Sky, windSpeed) == 0x18C);
static_assert(offsetof(Sky, windAngle) == 0x190);
static_assert(offsetof(Sky, currentWeatherPct) == 0x1B8);
static_assert(offsetof(Sky, weatherStartHour) == 0x1B4);
static_assert(offsetof(Sky, mode) == 0x1BC);
static_assert(offsetof(Sky, flags) == 0x1DC);

bool Sky::s_shouldUpdateWeather = true;

static_assert(sizeof(Sky) == 0x2C8);
