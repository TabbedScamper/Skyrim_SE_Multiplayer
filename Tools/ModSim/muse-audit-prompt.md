Act as a read-only second reviewer for this Skyrim SE Multiplayer repository.

Inspect the existing quest audit tools, `docs/reverse-engineering`, the server
quest and party services, `Code/protocol_bot`, and
`docs/HEADLESS_MULTIPLAYER_TESTING.md`.

Propose a concrete ModSim architecture that can run Skyrim quest scenarios for
two to eight multiplayer participants without launching Skyrim. Separate:

1. What can be simulated exactly from plugin records and production protocol
   code.
2. What can only be conservatively modeled from decompiled Papyrus.
3. What must remain an in-engine, GPU-backed test.

Identify the first five high-value scenario families and the data/schema each
needs. Do not edit files. Return a concise engineering memo with risks and
acceptance criteria.
