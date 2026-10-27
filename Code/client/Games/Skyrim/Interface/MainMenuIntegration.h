#pragma once

#include <string>

struct IMenu;

void PollMainMenuOptions(IMenu* apMainMenu) noexcept;
void PollPauseMenuOptions() noexcept;
void SetMainMenuOverlayActive(bool aActive) noexcept;
// Logs the Main Menu movie's state once a second for aSeconds (the "Launch watch" lines).
void WatchMainMenuState(int aSeconds) noexcept;
// Live lobby: a dark vignette drawn inside the Main Menu movie on its lowest layer, so it sits over the world and
// under the menu's own text (the overlay draws above every menu and would dim the text with it).
void AddMainMenuVignette(void* apMovie) noexcept;
// Stretches the loaded vignette over the visible stage; false until the texture has loaded.
bool SizeMainMenuVignette(void* apMovie) noexcept;
// Test probe: which of the likely news-panel (message of the day) paths exist in the Main Menu movie, with their text.
std::string ProbeMainMenuMotd(void* apMovie) noexcept;
// Test probe: frame and child clips of the intro title movie (Titles.swf).
std::string ProbeTitleMovie(void* apMovie) noexcept;
// The news panel (message of the day): its heading (MessageOfTheDay_mc.Header_tf) and its body (Menu_mc.SetMotd,
// the call the game makes with Bethesda's news). Returns the heading as it reads afterwards.
std::string SetMainMenuMotd(void* apMovie, const char* apHeader, const char* apBody) noexcept;
void SetMainMenuMouseState(float aX, float aY) noexcept;
void RenderNativeCursorOnTop() noexcept;
void RefreshMainMenuLayout() noexcept;
void RefreshMainMenu3DCamera() noexcept;
void LaunchSharedCampaignFromMainMenu(uint8_t aCampaignMode) noexcept;
// True for a few seconds after LaunchSharedCampaignFromMainMenu pressed NEW or CONTINUE for the party's start: the
// lobby lets that press through instead of treating it as the host asking to start.
[[nodiscard]] bool IsSharedCampaignLaunching() noexcept;
void DumpMainMenuState(uint32_t aOverlayWidth, uint32_t aOverlayHeight) noexcept;
