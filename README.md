# God of War II — Android arm64 build kit

Turn **your own** *God of War II* (USA, `SCUS_974.81` v1.01) into a native
Android arm64 app by **static recompilation**: the Emotion Engine code is
translated ahead of time to C++, the IOP runs the game's original modules, and
the Graphics Synthesizer is served by a raw Vulkan backend. It is not an
emulator frontend.

The engine is [`DaniSBR1/PS2Recomp`](https://github.com/DaniSBR1/PS2Recomp)
(branch `godofwar2`, pinned at `379fa48`) — the same runtime as the working PC
port — fetched as the `PS2Recomp/` submodule. The kit patches it with a small
stack of `-p1` patches (`patches/0001`–`0004`) applied in order by
`scripts/setup.ps1`.

> This repository contains **no game code**: no ISO, no ELF, no generated C++,
> no APK. Everything derived from the disc stays on your machine.

## Status

**M2 build side is done (2026-10-31): the raw Vulkan GS backend exists, compiles
for arm64, and the APK now ships with it** — the null-GS switch is off. What
remains is device work: boot, pixels, input, audio, performance.

| Milestone | State |
| --- | --- |
| **M0** — rebase the kit onto PS2Recomp, tools build, ELF verified | **done** 2026-10-26 |
| **M1** — Android arm64 host layer, app shell, APK end to end | **build side done** 2026-10-30 (device bring-up pending) |
| **M2** — raw Vulkan GS backend (`GSVkBackend`) | **build side done** 2026-10-31 — pixels pending on real hardware |
| **M3** — playability: native VU1/IOP, input & audio tuning, performance | sensor/touch/audio code in place, untested; capture pass pending |

Current deliverable: `gow2-android-debug.apk` (56 MB, arm64-v8a, Vulkan GS) —
also published as
[`v0.3.0-m2`](https://github.com/EvilNathan/gow2-android-apk/releases/tag/v0.3.0-m2)
in the private APK archive.

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
| `patches/` | `0001` read ISO · `0002` null GS · `0003` Vulkan GS select · `0004` Android host |
| `config/`, `overrides/` | recompiler inputs for GoW II |
| `tests/gs_probe/` | offscreen Vulkan backend tests (SwiftShader or any desktop ICD) |
| `tools/` | ISO/ELF/IRX/VU dump helpers (game-derived output stays local) |
| `scripts/` | setup, tool build, extract, recompile, Android build, probe, push, run |
| `generated/`, `build/`, `games/`, `dist/` | local outputs, never committed |

## Roadmap

Remaining work needs hardware — see [`TASKS.md`](TASKS.md) for the full list:

1. **Device bring-up** (any arm64 Android 9+): install the APK, import the
   ISO, capture logcat with `run-headless.ps1 -Android`, fix what the real
   Adreno/Mali path disagrees with.
2. **PC capture pass** (a machine with a GPU): boot the desktop runner to the
   menu / first area with `GOW_VU1_CAPTURA` + the IOP dump env vars, then
   `scripts/gen-native.ps1` for the native VU1/IOP generators and measure the
   FPS gain (~20 FPS menu / ~10 FPS first area are the interpreter baselines).
3. Then the playability pass: input/audio tuning on device, performance and
   the Android 15 16 KB-page matrix.

## Legal

GPL-3.0 (see `LICENSE`). No game assets are distributed; you must own the disc.
See `NOTICE.md` for third-party licenses and the game-content notice. The old
wgpu-based attempt is archived under `docs/legacy/`.
