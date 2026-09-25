# Project research rule

Before designing or patching a Skyrim, SKSE, Scaleform, rendering, input, save, networking, or multiplayer feature:

1. Search the web for prior open-source Skyrim/SKSE/CommonLib mods and engine research implementing the same or a closely related feature.
2. Prefer primary sources: source repositories, official SDK/documentation, and original issue/commit discussions.
3. Inspect the relevant source code, not only feature descriptions or mod pages.
4. Record the useful references and the reason each approach was adopted or rejected in `docs/REFERENCE_RESEARCH.md`.
5. Do not claim an in-game issue is fixed until the changed build has been exercised and the observed result confirms it.

# Read the executable rule

The whole of `SkyrimSE.exe` 1.7.104 is decompiled and queryable offline: see `docs/reverse-engineering/EXE_CORPUS.md` and use `C:\Tools\skyrim_re\sk.cmd` (`id`, `find`, `fn`, `callers`, `callees`, `str`, `grep`, `vt`). Before adding a live probe, diagnostic counter, or offset guess for engine behavior, read the relevant native functions there first. If a live measurement has not settled a question after two attempts, stop measuring and read the code. Use live probes to confirm what the code says. Cite the function VA and Address Library ID in `docs/REFERENCE_RESEARCH.md`.

# Correctness and performance rule

For multiplayer changes, first make the behavior correct across game states and validate it with protocol tests and paired in-game observations. Once a path works, profile and optimize its CPU, memory, and network costs. Treat five simultaneous players as the initial performance baseline; design authority and transport state so larger parties can be evaluated later without assuming two-player-only behavior. Do not trade correctness for an unmeasured speedup or claim a five-player performance target from a two-player test.

# Independent review rule

For the remainder of the 1:1 multiplayer build, ask the locally available Reviewer A and Reviewer B assistants to independently check significant diagnoses and proposed changes, and to suggest extensions. Record disagreements and resolve them against source inspection, measurements, tests, and paired in-game observations; their agreement alone is not validation. If either assistant is unavailable, state that limitation rather than implying it reviewed the work.
