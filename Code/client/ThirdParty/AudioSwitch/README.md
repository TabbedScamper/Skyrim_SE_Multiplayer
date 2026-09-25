# Auto Audio Output Switch (vendored)

Moves Skyrim's XAudio2 2.7 engine to another output device without restarting the game. It follows the Windows
default device, or the device chosen on the options page while that device is connected.

- Upstream: https://github.com/ApocryphaRealm/ApocryphaAutoAudioOutputSwitch
  (Nexus: https://www.nexusmods.com/skyrimspecialedition/mods/191770)
- Vendored commit: `ab235c2ddb15f4b550d7ae7fa7c53ecb8d71cb3a` (2026-09-18)
- License: GPL-3.0-or-later (`LICENSE.txt`, `NOTICE.md`). Third-party notices, including the MIT notice for
  Live Audio Output Switching SE by Maarten Harms that the switch procedure is based on: `THIRD_PARTY_NOTICES.md`.
- Credit: ApocryphaRealm (Auto Audio Output Switch), Maarten Harms (Live Audio Output Switching SE), Parapets
  (original concept), as recorded upstream.

## Files

| File | Origin |
|---|---|
| `AudioSwitch.cpp`, `AudioSwitch.h`, `XAudio27.h` | upstream, unmodified |
| `PCH.h`, `Settings.h`, `utils/Logger.h`, `AudioSwitchCompat.cpp` | Skyrim Together glue |

The glue replaces what upstream got from CommonLibSSE-NG and SKSE:

- `REL::RelocationID` resolves the AE-family ID through this client's `VersionDb` (1.7.104 only).
- `SKSE::GetTrampoline().write_call<5>` rewrites the rel32 call directly. The client image sits within rel32 range
  of the game, so no trampoline block is needed. Out-of-range targets are refused and logged, never truncated.
- `RE::BSAudioManager::GetSingleton` calls Address Library ID 67652.
- `logger::*` goes to spdlog with an `[AudioSwitch]` prefix.
- Settings: upstream's INI is replaced by the options page. `settings::GetPreferredDevice()` reads
  `Games/Skyrim/Audio/AudioDeviceSelection` (stored as `sAudioDevice` in `[SkyrimTogether]` of SkyrimPrefs.ini).
  Switching on a default-device change and on newly connected devices is always on.
- `Install()` and `StartWatcher()` run from a `TiltedPhoques::Initializer`, before the game creates its engine,
  matching upstream's `SKSEPluginLoad`.

The Address Library IDs used upstream were cross-checked against the 1.7.104 exe corpus
(`docs/reverse-engineering/EXE_CORPUS.md`): 67746, 67725, 68003, 388390, 388393, 410149, 236527, 67955 and 67952
all resolve to the functions and globals the upstream comments describe.

To update: replace the three upstream files from a newer commit, rebuild, and fix any new CommonLib/SKSE symbols
in the glue.
