# VERIFICATION

Evidence for the milestones in `TASKS.md`. Commands are run from the kit root.

## M0 — toolchain and translation

| Check | Command | Result |
| --- | --- | --- |
| Engine pin | `git -C PS2Recomp rev-parse HEAD` | `379fa48b1a292eb7f688ccdc202d97f5ffc09c6d` |
| Kit patches apply | `.\scripts\setup.ps1` | both patches applied |
| Tools build | `.\scripts\build-tools.ps1` | `ps2_analyzer.exe`, `ps2_recomp.exe` produced |
| Boot ELF SHA1 | `Get-FileHash games\gow2\disc\SCUS_974.81 -Algorithm SHA1` | `E6A62A0F3E781C29513F563F4625B9B3A9103C56` (matches reference) |
| Translation | `.\scripts\recompile.ps1` | 6,609 C++ units + `register_functions.cpp` |
| Runner sync | file count in `PS2Recomp/ps2xRuntime/src/runner` | 6,610 files |

The ISO is read directly (`IoPaths.cdImage`); the full 8 GB disc is not copied.

## M1 — Android boot

| Gate | Check | Result |
| --- | --- | --- |
| G1 | `.\scripts\build-android.ps1 -Configuration Debug` completes | pending (first build running) |
| G2 | APK contains `lib/arm64-v8a/libps2EntryRunner.so` | pending |
| G3 | `adb logcat -s ps2x` shows EE/IOP boot and ≥300 frames without a fatal signal | pending |

Expected M1 characteristics: no-op GS (no pixels), interpreter VU1/IOP, arm64
only. G2/G3 cannot pass before G1.

## Gates for later milestones

- M2: first frame submitted through `GSVkBackend`, screenshot compared with the
  PC OpenGL renderer; no GL symbols in the Android path.
- M3: 30 FPS target measured on the reference device; native VU1/IOP generated
  from a PC capture and verified against the interpreter.
