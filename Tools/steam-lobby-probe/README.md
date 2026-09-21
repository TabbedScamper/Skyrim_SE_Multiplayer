# Steam lobby feasibility probe

This Windows-only probe dynamically loads the `steam_api64.dll` shipped with a
legitimate Skyrim Special Edition installation. It verifies that Steam can be
initialized for Skyrim's App ID and reports whether the matchmaking and modern
P2P socket interfaces required by the proposed lobby transport are available.

By default it does not create or join a lobby, open a socket, modify Steam, or
persist credentials. Passing `--create-private-lobby` performs an explicit
integration test by creating a private two-member lobby and immediately leaving
it. It never publishes the lobby or invites another account.
