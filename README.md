# God of War II — Android arm64 build kit

Turns **your own** *God of War II* (USA, `SCUS_974.81` v1.01) into a native
Android arm64 app by **static recompilation**: the Emotion Engine code is
translated ahead of time to C++, the IOP runs the game's original modules, and
the Graphics Synthesizer is served by a native backend. It is not an emulator
frontend.

The engine is [`DaniSBR1/PS2Recomp`](https://github.com/DaniSBR1/PS2Recomp)
(branch `godofwar2`, pinned at `379fa48`) — the same runtime as the working PC
port — fetched as the `PS2Recomp/` submodule.

> This repository contains **no game code**: no ISO, no ELF, no generated C++,
> no APK. Everything derived from the disc stays on your machine.

## Status

| Milestone | State |
| --- | --- |
| **M0** — rebase the kit onto PS2Recomp, tools build, ELF verified | done |
| **M1** — Android arm64 boots headless (no-op GS, ISO read directly) | in progress |
| **M2** — raw Vulkan GS backend, first pixels | planned |
| **M3** — input, audio, performance, device matrix | planned |

The engine's PC renderer is OpenGL 4.5 (desktop). Android needs a new
`GSVkBackend` implementing `GSRasterBackend`
(`PS2Recomp/ps2xRuntime/include/runtime/gs/gs_backend.h`). During M1 the GS is a
no-op backend and the Android build only proves the CPU/IOP core boots.

## Requirements

- Windows 10/11 x64 with Visual Studio 2022/2026 + C++ workload (desktop tools).
- CMake 3.24+, Git, Python 3.10+.
- JDK 17, Android SDK platform 35, build-tools 34+, **NDK 27.3.13750724**,
  CMake 3.31.5 (SDK package).
- An arm64 Android 9+ (API 28) device with adb.
- Your own God of War II (USA) ISO.

## Quick start

```powershell
# 1. Engine submodule + kit patches
.\scripts\setup.ps1

# 2. Recompiler tools (ps2_analyzer, ps2_recomp)
.\scripts\build-tools.ps1

# 3. Read the boot ELF + IOP modules straight from your ISO (~4 MB, no 8 GB copy)
.\scripts\extract-disc.ps1 -Iso "D:\Games\God of War II (USA).iso"

# 4. Translate the ELF (writes generated/gow2/ and syncs the runner)
.\scripts\recompile.ps1

# 5. Android APK (arm64-v8a, 16 KB pages)
.\scripts\build-android.ps1 -Install

# 6. Static APK checks (ABI, launcher, 16 KB LOAD alignment)
.\scripts\verify-android.ps1

# 7. Push ELF + ISO to the device, then run and capture logcat
.\scripts\push-game-data.ps1
.\scripts\run-headless.ps1 -Android -Seconds 60
```

`build-all.ps1 -Iso <iso>` chains steps 1–5. The desktop runner can also be run
with `run-headless.ps1 -Build`; it reads the same ISO through `GOW2_ISO`
(no extracted disc needed). Install and troubleshooting notes live in
[`docs/ANDROID.md`](docs/ANDROID.md).

## How it works

1. `tools/iso_extract.py` lists the ISO9660 tree and copies just
   `SYSTEM.CNF`, the boot ELF, and the root `*.IRX` modules.
2. `ps2_recomp` translates the ELF to ~6.6k C++ units using
   `config/funcmap.csv` + `overrides/gow2_overrides.cpp`.
3. The kit copies generated units into `PS2Recomp/ps2xRuntime/src/runner/`
   (gitignored inside the submodule) and builds them into `ps2EntryRunner`.
4. At runtime the disc is read **directly from the ISO**:
   `IoPaths.cdImage` is set from `GOW2_ISO`, or from `gow2.iso` next to the
   boot ELF (the Android app files dir).
5. On Android, the `android-runner/` app is a `MainActivity` launcher
   (ISO import + boot-ELF extraction) plus a `GameActivity` that loads
   `libps2EntryRunner.so`; `compat/arm64/` maps leftover x86 intrinsic header
   names onto sse2neon, `patches/0002` installs the no-op GS for M1 and
   `patches/0004` routes window/audio/pad/log through the Java host.

## Layout

| Path | Purpose |
| --- | --- |
| `PS2Recomp/` | engine submodule (GPL-3.0) |
| `android-runner/` | Gradle + NativeActivity host, arm64-v8a |
| `compat/arm64/` | `immintrin.h`-style shims over sse2neon |
| `patches/` | ISO-direct and null-GS patches applied to the engine |
| `config/`, `overrides/` | recompiler inputs for GoW II |
| `tools/` | ISO/ELF/IRX/VU dump helpers (game-derived output stays local) |
| `scripts/` | setup, tool build, extract, recompile, Android build, push, run |
| `generated/`, `build/`, `games/`, `dist/` | local outputs, never committed |

## Legal

GPL-3.0 (see `LICENSE`). No game assets are distributed; you must own the disc.
See `NOTICE.md` for third-party licenses and the game-content notice. The old
wgpu-based attempt is archived under `docs/legacy/`.
