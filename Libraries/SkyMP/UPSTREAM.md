# SkyMP-derived libraries

The `papyrus-vm` and `viet` directories were imported from
[`skyrim-multiplayer/skymp`](https://github.com/skyrim-multiplayer/skymp) at
commit `f926944b18e3aed4bc3864ce668626c05ec2545f` on 2026-09-22.

Both imported directories retain their upstream MIT `LICENSE` files and
copyright notices. The compatibility `MakeID.h` is Emil Persson's explicitly
public-domain implementation, sourced from the historical standalone
`skyrim-multiplayer/papyrus-vm` repository at commit
`54500907281a0862e7dba854bbb763b16733fbe8`.

The AGPL-licensed SkyMP server-native implementations were not copied. They
remain architectural references for later native-function shims and condition
evaluation unless this project deliberately adopts the corresponding AGPL
obligations.
