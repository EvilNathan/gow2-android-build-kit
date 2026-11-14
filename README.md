# God of War II — Android arm64 build kit

Turn **your own** *God of War II* (USA, `SCUS_974.81` v1.01) into a native
Android arm64 app by **static recompilation**: the Emotion Engine code is
translated ahead of time to C++, the IOP runs the game's original modules, and
the Graphics Synthesizer is served by a raw Vulkan backend. It is not an
emulator frontend.

The engine is [`DaniSBR1/PS2Recomp`](https://github.com/DaniSBR1/PS2Recomp)
(branch `godofwar2`, pinned at `379fa48`) — the same runtime as the working PC
port — fetched as the `PS2Recomp/` submodule. The kit patches it with a small
stack of `-p1` patches (`patches/0001`–`0009`) applied in order by
`scripts/setup.ps1`.

> This repository contains **no game code**: no ISO, no ELF, no generated C++,
> no APK. Everything derived from the disc stays on your machine.

## Status

**M3 device bring-up is well advanced (2026-11-14): the game boots, plays its
opening movie, and renders the first playable area on real hardware** — on a
Redfinger cloud phone (Galaxy S25+, Adreno; earlier runs on a Galaxy Note20,
Mali-G77). Native VU1/IOP code is generated and linked (VU1 ≈10× faster than
interpretation), the GS composition path produces correct pixels on screen,
and the guest's mode state machine has been mapped end to end: after the
movie the game waits in mode 11, and the pad sequence **SELECT → X → START**
walks it through modes 12 → 11 → 13, where the character-update branch runs
continuously. One gate remains before this is *playable*: in mode 13 the
character does not yet move under analog-stick input. The 2026-11-14
diagnostic run (diag119) narrowed it further: the stick *does* reach the
guest pad struct (cooked analog float reads exactly ±1.0 in both directions)
and a forced player gate (`+0x394`) did not move the character, so the block
is downstream of pad delivery — the current work item is a call-profile run
(diag120) to see which update functions actually execute in mode 13 (see
`TASKS.md` and `VERIFICATION.md`).

The working tree also carries TEMP-DIAG instrumentation (guest-state probes,
GS counters, dump facilities in `gs_vk_backend.cpp` / `gow2_overrides.cpp` /
`jni_bridge.cpp`) that will be removed once playability is established; the
permanent fixes from this phase (GPU wedge fix, MAILBOX swapchain, channel
shuffle relaxations, CT32 present path, native VU1/IOP wiring) are the parts
that stay.

| Milestone | State |
| --- | --- |
| **M0** — rebase the kit onto PS2Recomp, tools build, ELF verified | **done** 2026-10-26 |
| **M1** — Android arm64 host layer, app shell, APK end to end | **done** — device boot verified 2026-11-09 |
| **M2** — raw Vulkan GS backend (`GSVkBackend`) | **done** — correct pixels on device 2026-11-11 |
| **M3** — playability: native VU1/IOP, input & audio tuning, performance | **in progress** — native VU1/IOP done; movie + first area render; mode machine mapped (SELECT → X → START reaches mode 13); stick delivery to the guest pad struct proven in both directions (diag119, 2026-11-14); character locomotion under stick input still gated — next: diag120 call profile of mode 13 |

Current deliverable: `gow2-android-debug.apk` (arm64-v8a, Vulkan GS), built
with `scripts/build-android.sh` / `.ps1`. Stable releases are published to the
private APK archive
([`v0.4.0-m2`](https://github.com/EvilNathan/gow2-android-apk/releases/tag/v0.4.0-m2)
is the latest tagged one; M3 test builds circulate as debug APKs only).

## Requirements

- Windows 10/11 x64 with Visual Studio 2022/2026 + C++ workload (desktop tools).
- CMake 3.24+, Git, Python 3.10+.
- JDK 17, Android SDK platform 35, build-tools 34+, **NDK 27.3.13750724**
  (with `shader-tools` for `glslc`), CMake 3.31.5 (SDK package).
- An arm64 Android 9+ (API 28) device with adb.
- Your own God of War II (USA) ISO.

## Quick start

```powershell
# 1. Engine submodule + kit patch stack
.\scripts\setup.ps1

# 2. Recompiler tools (ps2_analyzer, ps2_recomp)
.\scripts\build-tools.ps1

# 3. Read the boot ELF + IOP modules straight from your ISO (~4 MB, no 8 GB copy)
.\scripts\extract-disc.ps1 -Iso "D:\Games\God of War II (USA).iso"

# 4. Translate the ELF (writes generated/gow2/ and syncs the runner)
.\scripts\recompile.ps1

# 5. Android APK — arm64-v8a, 16 KB pages, Vulkan GS on
.\scripts\build-android.ps1 -Install

# 6. Static APK checks (package, launcher, ABI, minSdk, 16 KB LOAD alignment)
.\scripts\verify-android.ps1

# 7. Push ELF + ISO to the device, then run and capture logcat
.\scripts\push-game-data.ps1
.\scripts\run-headless.ps1 -Android -Seconds 60
```

`build-all.ps1 -Iso <iso>` chains steps 1–5. Install and troubleshooting notes
live in [`docs/ANDROID.md`](docs/ANDROID.md).

By default `build-android.ps1` compiles the **Vulkan GS** backend (pass
`-NullGs:$true` for the headless no-op-GS build used by M1 boot tests).

### Linux

Every script also ships as a Bash port next to the `.ps1` (`scripts/*.sh`,
same steps, guards and outputs; the PowerShell scripts remain the Windows
entry points). Prerequisites are the same as above minus Visual Studio:
JDK 17, Android SDK platform 35, **NDK 27.3.13750724**, CMake 3.24+,
Ninja (or Make), clang (only for `build-gs-probe.sh`), Git, Python 3.10+.
The SDK is located via `ANDROID_HOME`/`ANDROID_SDK_ROOT`, defaulting to
`~/Android/Sdk`.

```bash
./scripts/setup.sh
./scripts/build-tools.sh
./scripts/extract-disc.sh --iso "/path/to/God of War II (USA).iso"
./scripts/recompile.sh
./scripts/build-android.sh --install
./scripts/verify-android.sh
./scripts/push-game-data.sh
./scripts/run-headless.sh --android --seconds 60
```

`./scripts/build-all.sh --iso <iso>` chains the same steps, and the flags
mirror the PowerShell parameters (`--configuration`, `--null-gs`, `--clean`,
`--sync-only`, `--no-run`, …). Host tools build with CMake + Ninja/Make
instead of MSVC, so executables land in `build/<subdir>/` with no `Release/`
folder and no `.exe` suffix.

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
   names onto sse2neon and `patches/0004` routes window/audio/pad/log through
   the Java host.
6. The Graphics Synthesizer is rendered by **`GSVkBackend`**
   (`android-runner/native/gs_vk/`, selected by
   `patches/0003`): see below.

## The Vulkan GS backend (M2)

`GSVkBackend` is a port of the desktop render-target renderer
(`PS2Recomp/ps2xRuntime/src/lib/gs/gpu/gs_hw_backend.cpp`, OpenGL 4.5) to a raw
Vulkan pipeline at 1x internal scale. The engine calls it behind the standard
`GSThreadedBackend` wrapper — every Vulkan call runs on the GS worker thread,
the loader is `dlopen`ed (no link dependency, no API-level requirement).

| Piece | Where | Notes |
| --- | --- | --- |
| Backend + loader | `gs_vk_backend.{h,cpp}`, `gs_vk_loader.{h,cpp}` | instance/device/swapchain are created from `ps2x::host::NativeWindow()`; without a window (offscreen probe) `Present` only produces readback pixels |
| Shaders | `shaders/*.vert|frag` | compiled by the NDK `glslc` (`compile_shaders.cmake`), embedded as SPIR-V via `cmake/embed_spirv.cmake` and reached through `gs_vk_spv::Get("name")` |
| Render targets | keyed by `FRAME.FBP`, RGBA8 + D32 | the reinterpreting fetch reads them through pure address math in the fragment shader; target memory that a CLUT points into is read back on the CPU instead |
| Textures | `GSTextureStore` (engine) | direct-colour textures are CPU-decoded (swizzle tables + TEXA); paletted ones upload indices + a 256×1 palette so animated CLUTs are cheap |
| GS semantics kept | — | destination alpha test, Z aliasing (Z in colour memory), Z inversion, AFAIL two-pass alpha fail, fixed-function + software blend split, sprite snapping and shared-seam clamps, channel shuffles, present circuit composition (PMODE blending) |
| Tests | `tests/gs_probe/` + `scripts/build-gs-probe.ps1` | self-provisioning offscreen harness (copies NDK Vulkan headers, runs `glslc`, embeds SPIR-V) against the SwiftShader ICD; drives init, render targets, pipelines and draw dispatch. Pixel-exact cases need real hardware — the CI SwiftShader builds trap internally (see `VERIFICATION.md`) |

`build-android.ps1` toggles: `-NullGs:$false` (default) builds the Vulkan
backend, `-NullGs:$true` builds the M1 no-op GS.

## Layout

| Path | Purpose |
| --- | --- |
| `PS2Recomp/` | engine submodule (GPL-3.0), patched by the kit |
| `android-runner/` | Gradle app: `MainActivity` (ISO import, settings) + `GameActivity` (SurfaceView session) |
| `android-runner/native/gs_vk/` | raw Vulkan GS backend + shaders (M2) |
| `android-runner/native/host/` | `ps2x_host.h` contract implementation: window, AAudio, touch/gamepad, storage, crash capture |
| `android-runner/native/iso/` | in-app ISO9660 reader + SHA1 boot-ELF extractor |
| `compat/arm64/` | `immintrin.h`-style shims over sse2neon |
| `patches/` | `0001` read ISO · `0002` null GS · `0003` Vulkan GS select · `0004` Android host · `0005` GS CSR event count · `0006` IOP diagnostics · `0007` libsd tracing · `0008` SMPD status slots · `0009` EE status readback |
| `config/`, `overrides/` | recompiler inputs for GoW II |
| `tests/gs_probe/` | offscreen Vulkan backend tests (SwiftShader or any desktop ICD) |
| `tools/` | ISO/ELF/IRX/VU dump helpers (game-derived output stays local) |
| `scripts/` | setup, tool build, extract, recompile, Android build, probe, push, run |
| `generated/`, `build/`, `games/`, `dist/` | local outputs, never committed |

## Roadmap

See [`TASKS.md`](TASKS.md) for the full list. Device bring-up, the capture
pass and native VU1/IOP generation are done; what remains is the playability
pass:

1. **Mode-13 locomotion gate**: find why the character does not move under
   stick input once the mode machine reaches mode 13 (pad delivery, the
   mode-13 character branch and the action-request chain are all verified
   live; the missing link is inside the mode-13 update).
2. **Present-path performance**: double-buffer the frame resources so
   `Present()` no longer waits on the previous frame's fence (the heavy
   phases run at ~8 guest flips/s largely because of this serialization).
3. **Cleanup + release**: strip the TEMP-DIAG instrumentation, hard-code the
   verified rotation behaviour, fix the crash handler's ring-dump copy, then
   publish an M3 release to the private APK archive.

## Legal

GPL-3.0 (see `LICENSE`). No game assets are distributed; you must own the disc.
See `NOTICE.md` for third-party licenses and the game-content notice. The old
wgpu-based attempt is archived under `docs/legacy/`.
