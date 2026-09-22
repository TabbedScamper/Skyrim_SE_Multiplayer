#pragma once

struct IMenu;

void PollMainMenuOptions(IMenu* apMainMenu) noexcept;
void SetMainMenuOverlayActive(bool aActive) noexcept;
void SetMainMenuMouseState(float aX, float aY) noexcept;
void RenderNativeCursorOnTop() noexcept;
void RefreshMainMenuLayout() noexcept;
void RefreshMainMenu3DCamera() noexcept;
void LaunchSharedCampaignFromMainMenu(uint8_t aCampaignMode) noexcept;
void DumpMainMenuState(uint32_t aOverlayWidth, uint32_t aOverlayHeight) noexcept;
