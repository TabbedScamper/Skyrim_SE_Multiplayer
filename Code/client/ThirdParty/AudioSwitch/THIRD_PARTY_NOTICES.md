# Third-party components and their notices

Auto Audio Output Switch as a whole is GPL-3.0-or-later (`LICENSE`, `NOTICE.md`). These components are included under their own
GPL-compatible licences; their notices are reproduced as those licences require.

## CommonLibSSE-NG 7.2.0 - Skyrim 1.7.x build line

https://github.com/alandtse/CommonLibSSE-NG (commit 7a60f4de794095d7b0f8928d1b930a52e9a7da83), GPL-3.0-or-later WITH
Modding Exception AND GPL-3.0 Linking Exception (with Corresponding Source); the exceptions ship as
`CommonLibSSE-NG-EXCEPTIONS.md` beside the 1.7 build.

## CommonLibSSE-NG 3.7.0 - SE 1.5.97 / AE 1.6.1170 build line

MIT License

Copyright (c) 2018 Ryan-rsm-McKenzie

Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated
documentation files (the "Software"), to deal in the Software without restriction, including without limitation the
rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to permit
persons to whom the Software is furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all copies or substantial portions of the
Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE
WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

## DevBench consumer API (`include/DevBench/`, `source/DevBench/`)

MIT - the notice is `include/DevBench/DevBenchAPI.LICENSE.txt`, kept with the files.

## Microsoft XAudio2 2.7 and the Windows audio endpoint API

Not included. The plugin calls XAudio2_7.dll (installed with the DirectX End-User Runtime, which Skyrim requires) and the
Windows MMDevice API at run time. `include/XAudio27.h` declares the XAudio2 2.7 interfaces from their published binary
interface; it contains no Microsoft header text.

## Live Audio Output Switching SE - the device-switch procedure

https://github.com/maartenharms/live-audio-output-switching (v0.1.0) by Maarten Harms, MIT. The procedure in
`source/AudioSwitch.cpp` - rebuild the game's own audio engine on its audio thread, detach and revive every live
sound's voice, clear stale entries from the game's voice lists, and probe a new endpoint before using it - follows that
project's 1.5.97 implementation, ported here to 1.5.97, AE 1.6.1170 and 1.7.x through Address Library IDs. Its
licence notice:

MIT License

Copyright (c) 2026 Maarten Harms

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.

## Media Keys Fix SKSE

Copyright (c) 2023-2026 Emerson Pinter (epinter). https://github.com/epinter/MediaKeysFix - Nexus Mods 92948.
Licensed under the GNU Lesser General Public License, version 3 or later (LGPL-3.0-or-later).

The media keys feature (`include/KeyboardAccess.h`, `source/KeyboardAccess.cpp`) re-implements Media Keys Fix's
approach: the SetCooperativeLevel flags patch in the game's DirectInput keyboard setup (Address Library 67471 / 68781,
offset 0x55, instruction `41 B8 15 00 00 00 48 8B D0 FF`) and the ToUnicode wrap that disables dead keys (67472 +0x20D /
68782 +0x2CB, `FF 15 ?? ?? ?? ?? 83 F8 01 75`). The LGPL-3.0 permits using that work in this GPL-3.0-or-later program;
the GPL-3.0 text shipped with this mod covers it. Thank you to Emerson Pinter for finding and publishing the fix.
