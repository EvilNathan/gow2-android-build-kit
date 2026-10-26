# overrides/

GoW II-specific patches: game functions that are replaced with hand-written C++ (`PS2_REGISTER_GAME_OVERRIDE`). `recompile.ps1` copies the `*.cpp` and `*.h` files from here to the runner.

The model is `referencia/gow1/overrides/gow_overrides.cpp` (Waffle565, via KIexster): for example `gowIpuInit` (the runtime's generic `sceIpuInit` carries another game's addresses), boot, the CD or the controller. Each one has to be redone with **GoW II's addresses**, and only when a blocker calls for it.
