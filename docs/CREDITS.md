# Credits

SkyrimSeamlessCoop builds on Skyrim Together Reborn (TiltedPhoques) and on the work of the Skyrim
modding community. Fixes below are implemented in this mod's own code, following the behavior the
original mod documents, and are only active where co-op needs them.

## Built-in engine fixes

- **Havok frame-rate step** - follows the HavokFPS fix in
  [SSE Display Tweaks](https://www.nexusmods.com/skyrimspecialedition/mods/34705).
  Above 60 FPS, Skyrim advances physics by a fixed 1/60 s per frame, so physics objects glide and
  scripted physics (the Helgen carts) misbehave; in co-op the host's physics is what every player
  sees. The step is kept at the real frame time when it is shorter than 1/60 s and left untouched
  otherwise. Code: `Code/client/Services/Generic/EngineFixes.cpp`.

## Reference sources

- [CommonLibSSE-NG](https://github.com/CharmedBaryon/CommonLibSSE-NG) - native layouts and IDs.
- [Address Library for SKSE Plugins](https://www.nexusmods.com/skyrimspecialedition/mods/32444) by meh321 - function IDs.
