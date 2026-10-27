#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

// Live preview for the volume sliders. While a slider is being adjusted, the
// other mixer categories are muted and its own category plays continuously
// (random samples from a pool of Skyrim.esm sound descriptors), all through
// Skyrim's audio engine so the slider's level is exactly what is heard.
// Window/main thread only.
namespace AudioPreview
{
// Timer driving playback; handled in InputService::WndProc.
inline constexpr uintptr_t kTimerId = 0x5C1A;

// Starts or extends the preview for acChannel ("master", "effects",
// "footsteps", "voice", "music"). Stops by itself ~1.5 s after the last call.
void KeepAlive(const std::string& acChannel) noexcept;
// Stops playback and restores every muted category to its previous volume.
void Stop() noexcept;
// Timer callback.
void Tick() noexcept;

// Tells the audio thread to re-apply category volumes to sounds already
// playing. Setting a BGSSoundCategory volume only stores it; the vanilla
// Journal OptionChange handler (ID 53310, default case) follows every
// category change with BSAudioManager message 0x10 (ID 67716). Without it a
// playing track (e.g. the menu music) keeps its old level.
void NotifyCategoryVolumesChanged() noexcept;

// Sets a BGSSoundCategory's volume through the vanilla Journal OptionChange
// handler (ID 53310): its default case takes the category form ID as the
// option number. Same path the vanilla Audio menu uses; master uses option 27.
void SetCategoryVolumeVanilla(uint32_t aCategoryFormId, float aValue) noexcept;

// Category volume only reaches sounds created after a change, so the music
// track that is already playing (the title theme, a long exploration track)
// keeps its old level. This rescales the playing track's own sound handles to
// the current Music level (or 0 while another channel is being previewed).
void SyncMusicVolume() noexcept;

// Live lobby (LobbyService): the title theme keeps playing in the lobby world and the world's own music tracks are
// held silent, without touching the saved Music level. Main thread.
// Restarts the Main Menu theme (ID 52185) when it is not playing.
void EnsureTitleTheme() noexcept;
// The lobby soundtrack: every named Skyrim, Dawnguard and Dragonborn track (LobbyPlaylist.inl), shuffled each time
// the lobby opens, played the way the Main Menu plays its theme. Tick returns the playing track's official title.
void StartLobbyPlaylist() noexcept;
[[nodiscard]] std::string TickLobbyPlaylist() noexcept;
void StopLobbyPlaylist(uint16_t aFadeMilliseconds) noexcept;
[[nodiscard]] bool LobbyPlaylistActive() noexcept;
// The lobby soundtrack's tracks by official title, in soundtrack order, with whether each is on (Options > Audio).
// Off titles are kept in Data\SkyrimTogetherReborn\lobby_music.txt; without that file "Blood and Steel" starts off.
[[nodiscard]] std::vector<std::pair<std::string, bool>> LobbyPlaylistTitles() noexcept;
void SetLobbyTrackEnabled(const std::string& acTitle, bool aEnabled) noexcept;
// Fades the title theme out (BSSoundHandle::FadeOutAndRelease) over aMilliseconds.
void FadeOutTitleTheme(uint16_t aMilliseconds) noexcept;
// Holds the world's music tracks at zero volume while set.
void SetWorldMusicMuted(bool aMuted) noexcept;
} // namespace AudioPreview
