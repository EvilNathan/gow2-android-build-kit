# TASKS - v2.3 "VU1 AOT hotfix" (all Vulkan vendors)

> Execution tracker. Updated as work lands. Statuses: `[ ]` todo, `[~]` in progress, `[x]` done, `[!]` blocked on hardware.

## Crash evidence (2026-10-22, binary diff of the APKs)

The v2.0/v2.1/v2.2 crash is now tied to the only **game-code** change between
the working v1.x builds and the crashing v2.x builds:

| APK | VU1 AOT programs in `libmain.so` |
| --- | --- |
| v1.0.0 (works) | 112 |
| v1.1.0 (works) | 112 |
| v2.0.0-v2.2.0 (crash) | 117 |

The five programs added in v2.0 are:

```
4176D02B93877C36  54781DABBA793231  758DC1364D0BF01B
E344F110E1EBEEDC  EF764AF45D6D7189
```

v2.0 shipped a larger table ("117 from 86 recorded blocks"). A miscompiled AOT
program crashes only when the game executes it; v1.x used the interpreter for
those five, so the same scenes were safe. v2.3 therefore **skips the five by
default** and exposes a per-program diagnostic switch in Settings. The exact
offending program still needs a device test (no phone attached to this build
machine).

## Status snapshot (2026-10-22, fifth pass — v2.3)
- Android APK builds: **yes** — `dist/gow2-debug.apk`, versionCode 6 /
  versionName 2.3, arm64-v8a, with `libmain.so` + libadrenotools hook libs.
  `dlopen`/`dlclose` are plain imports (nothing exported).
  `extractNativeLibs=true`; all five `.so` files have `LOAD` align `0x4000`.
  13,192,938 bytes, sha256
  `B612E1BA300053041C08358E6C9ECE04384296A5F12D9BEE61C3EF65AFBF577F`.
  Uploaded as release `v2.3.0` in `EvilNathan/gow2-android-apk`; download-back
  hash matches.
- v2.3 VU1 AOT hotfix: **built, device confirmation pending** — the five
  programs added in 2.0 are skipped by default (112/117 registered), with a
  Settings master switch and one toggle per program. Native runner verified in
  all three modes (`117/117`, `112/117`, all-interpreter).
- UI: **new launcher** — two-column landscape layout, bronze/gold theme, adaptive
  omega icon, cards for controls and diagnostics, ripple buttons.
- Logs: **`/sdcard/gow2/log.txt`** — native `[boot]`/`[gpu]`/`[window]`/`[ee]`
  lines plus Java events and crash traces, flushed per line, 4 MiB rotate.
  Android 11+ needs all-files access; the launcher shows the active path and has
  a grant button, otherwise the fallback is the app external folder.
- v2.0 device crash: **mitigated** — v2.0 exported `dlopen`/`dlclose` from
  `libmain.so` (process-wide interposition); v2.1 uses
  `-Wl,--wrap=dlopen -Wl,--wrap=dlclose` and exports nothing. Device limit
  request now comes from `wgpuAdapterGetLimits` instead of desktop defaults.
  DRS is opt-in again, Safe mode disables every optional GPU path, and `[boot]`
  breadcrumbs are written to logcat. Root cause still needs the user's logcat.
- Desktop runner builds: **yes** — a 60-frame run after the v2.1 changes
  completed (297,861,626 cycles).
- Disc reads: **read-ahead window implemented** — 256 KiB aligned window in
  `runtime/src/disc.cpp`, shared by the plain-file and SAF descriptor readers.
  `PS2_DISC_STATS=1` prints a summary: the 300-frame run made **533 host reads
  for 14.7 MiB, 75 % served from the window**.
- Frame cadence: **30 Hz mode implemented** — `--fps-cap` / `PS2_FPS_CADENCE`,
  Settings checkbox. It halves the vblank rate so logic, audio and presentation
  stay consistent instead of dropping frames at 60. Verified headless:
  9,830,411 EE cycles per frame (EE clock / 30) and 48 kHz audio output.
- Dynamic resolution: **implemented (swapchain-only)** — opt-in on every
  platform since 2.1 (Settings toggle off by default), `PS2_DRS` env, 0.6–1.0.
- Adreno driver/turbo: **implemented, opt-in** — `ps2_android_vulkan_prepare()`
  runs before wgpu init; Mali and other vendors always use the stock driver.
  Loader errors now log and fall back instead of aborting.
- Turnip fetch: **`scripts/fetch-turnip.ps1` added**, pinned to
  `K11MCH1/AdrenoToolsDrivers` `v26.0.0-rc08` / `Turnip_v26.0.0_R8.zip`
  (sha256 `e634db0f…`), downloaded and hash-verified locally. Not loaded on a
  phone yet.
- 989SND: **implemented** — WAD `SBP_*` parser and virtual `.bnk` service;
  trace shows SMPD loading `SBP_general` resources; no `.bnk` requests observed.
- Hardware validation: **pending** — no Android device attached to this
  machine; driver/turbo/perf claims remain unverified.
- Raw Vulkan backend: **not implemented** — wgpu remains the renderer on all
  platforms. No fake completion.

## Fase 0 — Instrumentasi & diagnostik
- [x] Per-frame stats v2: EE/GS/VU busy, present ms, GPU ms, upload MB, Vuint, dispatches, backend, driver, fps
- [!] `adb logcat -d -s GoW2Stats` captured on the reporter's 8 GB Adreno device
- [!] Perfetto capture + bottleneck verdict (CPU vs GS vs present vs driver)
- [x] Consolidate engine submodule changes into `engine.patch` (canonical diff; verified against a fresh checkout at `6526f48`)

## Fase 1 — CPU & build (win cepat)
- [x] Thin-LTO across `ps2game` + `ps2runtime` + `main` (Android)
- [x] `-ffunction-sections -fdata-sections -Wl,--gc-sections`, arm64 tuning
- [x] Thread affinity: EE → fastest cluster, render → fastest cluster, VU → all cores; no-op on desktop
- [x] CDVD read-ahead: 256 KiB aligned window shared by the file and SAF readers; `PS2_DISC_STATS` reports the hit rate
- [~] VU capture presets (`-Preset quick|standard|full`) done; `Vuint ≈ 0` still needs a device measurement. Native builds have no VU JIT by design (`jit::mode()` returns 0), so the fallback is AOT coverage plus the interpreter's decoded-instruction cache.

## Fase 2 — Driver Adreno (libadrenotools) + kompatibilitas Mali
- [x] Vendor `libadrenotools` (BSD-2) and build hooks with NDK
- [x] Vulkan bootstrap through `adrenotools_open_libvulkan` with stock fallback
- [x] GPU Driver Manager UI (SAF zip → internal driver dir), active-driver display
- [x] Turbo clock toggle (`adrenotools_set_turbo`), default OFF
- [x] Adreno-only gating; Mali/other vendors keep stock driver path (never load turnip)
- [x] `scripts/fetch-turnip.ps1` with a pinned release + SHA256 check (`v26.0.0-rc08`, `Turnip_v26.0.0_R8.zip`). The revision is pinned but **not hardware-tested**; the script prints that warning.

## Fase 3 - Backend Vulkan raw + jalur graphics (engine native baru)

> Research, repo/license matrix and the Tier 1 phase plan are in
> [`docs/RENDERER-MIGRATION.md`](docs/RENDERER-MIGRATION.md). Nothing below is
> implemented yet; wgpu remains the renderer.

- [ ] Backend abstraction over `gs::gpu` (wgpu | raw Vulkan), wgpu stays default on desktop/web
- [ ] Raw Vulkan context: instance/device/queue/ANativeWindow surface/swapchain/FIFO/letterbox
- [ ] Persistent-mapped host-visible buffers (VRAM/CLUT/arena/params) — no per-frame queue writes
- [ ] Compute path port (gs.wgsl → SPIR-V offline), feature parity with wgpu backend
- [ ] Graphics-pipeline path for common primitives (textured/blended/fog/alpha), compute fallback per draw
- [ ] Adreno/Mali per-chip profiles; no Adreno-specific extension in core path
- [ ] Present fence pacing, 2–3 frames in flight

## Fase 4 — Dynamic resolution & adaptive fps
- [x] Swapchain-only frame-budget controller (GPU ms + present ms), hysteresis, 0.6–1.0 range
- [x] Settings toggle (`Dynamic resolution`, default ON; env `PS2_DRS`) and `DRS` in stats
- [x] 30 Hz cadence: `--fps-cap 30` / `PS2_FPS_CADENCE=30`, Settings checkbox; halves the vblank rate (EE cycles / 30) so game logic, SPU2 and presentation stay in step. Verified headless; device feel unverified.
- [x] ISO via `ParcelFileDescriptor` + `pread` (this landed with the first kit commit: `GameActivity` passes `--iso-fd`, `host/main.cpp` calls `ps2::disc::open_fd`, `runtime/src/disc.cpp` reads with `pread`). RSS headroom on an 8 GB phone is still unmeasured.
- [ ] Internal GS render-scale step (1.0 → 0.85 → 0.7). `g_scale` sizes VRAM, scratch and readback buffers at `init_graphics()`, so a live step needs buffer reallocation plus pipeline rebinding; swapchain-only DRS covers the fill-rate goal for now.

## Fase 5 — Audio 989SND (BNK)
- [x] WAD `SBP_*` resource parser (989SND SBP/SBlk format) in `runtime/src/iop/io.cpp`
- [x] Serve `host1:sound/*.bnk` from loaded WADs + TOC fallback (`R_PERMA.WAD` for `general`)
- [x] Runtime trace: SMPD receives `SBP_general`/`SBP_general2` via SIF RPC and loads them as WAD resources; normal boot audio is non-silent
- [!] No `sound/*.bnk` request observed in headless traces, so the virtual `.bnk` service is dormant until a menu/gameplay path asks for one
- [!] `989 ERROR 84`/`55` lines appear in scripted runs and are non-fatal; root cause still needs a hardware logcat capture.
- [x] Regression: existing WAD/VAG audio path unchanged (300-frame and 120-frame headless runs produced non-silent stereo PCM; the 300-frame 30 Hz run wrote 474,976 stereo frames = 10 s at 48 kHz)

## Fase 6 — Verifikasi & rilis
- [x] Desktop/headless regression (wgpu path unaffected; 300-frame 30 Hz + 120-frame 60 Hz runs completed)
- [x] Android build: single `libmain.so`, 16 KB LOAD alignment, `extractNativeLibs=true`
- [x] Clean/repeatable APK packaging; `build-android.ps1` removes the variant output first to avoid zipflinger stale padding
- [x] APK version bumped to `versionCode 3` / `versionName "2.0"` (manifest + Gradle)
- [x] APK version bumped to `versionCode 6` / `versionName "2.3"` for the VU1 hotfix
- [x] 16 KB linker flags applied to libadrenotools hook `.so` targets
- [x] `ps2_android_vulkan_prepare()` called from Android `main()` before wgpu creates its Vulkan instance
- [x] README / VERIFICATION.md v2.0
- [x] `engine.patch` regenerated after the read-ahead and cadence changes; `git apply --check` passes on a pristine worktree at `6526f48`
- [!] Frame-time plots (Perfetto) on Adreno 6xx-class target
- [!] Mali device smoke test (stock driver, no turnip)

## Fase 7 - Crash v2.0-v2.2: VU1 AOT bisect

- [x] Binary evidence: v1.x = 112 programs, v2.x = 117; five new hashes isolated
- [x] Runtime filter `PS2_VU_AOT=0` (all-interpreter) and `PS2_VU_SKIP=<hashes>` in `runtime/src/vu_micro.cpp`
- [x] Android defaults: five new programs skipped; saver settings checkbox `Compiled VU1 microcode (AOT)` plus one toggle per program
- [x] `[app] game start: ... vu_aot=... vu_skip=...` written to `/sdcard/gow2/log.txt`
- [x] v2.3 APK built and uploaded: 13,192,938 bytes, sha256 `B612E1BA…577F`,
  release `v2.3.0`; download-back hash matches
- [ ] **Device confirmation**: install v2.3, boot, then enable one program at a time from Settings; record the hash that crashes
- [ ] Fix `vurecomp`/codegen for the offending program (or keep it on the interpreter permanently; cost is small)
- [ ] Long headless run (3,500+ frames) with all five enabled under ASan/UBSan
- [ ] If v2.3 still crashes with all five skipped, bisect the v2.0 engine changes: read-ahead, SBP scanner, affinity, ThinLTO/ICF, adrenotools

## Findings - 989SND bank layout (retail USA ISO)
- `GODOFWAR.TOC` has no `.BNK` entries. SMPD contains an override loader that
  can build `host1:sound/<bank>.bnk`, but normal bank loads use the named
  `SBP_*` resource path instead. The SIF trace shows the EE sending
  `SBP_general` / `SBP_general2` to SMPD; no `.bnk` open is issued.
- The bank image is a named `SBP_*` resource in a WAD's raw data heap:
  - `R_PERMA.WAD` -> `SBP_general` (295 sounds, pseudo `GEN1`) and
    `SBP_general2` (21 sounds, pseudo `GEN2`)
  - `R_SHELLA.WAD` -> `SBP_SHELL` (8 sounds, pseudo `TL01`)
  - `R_RHSOLD00.WAD` -> `SBP_RHSOLD00` (13 sounds, pseudo `RH0`)
- Resource layout: `u32 type=1`, `u32 size`, `char name[24]` (`SBP_...`),
  then the 989SND bank: `u32 magic=0x15`, `u32 soundCount`,
  `soundCount * 28` sound entries, 24-byte bank info, then `SBlk` header /
  ADPCM stream at the offsets in the bank info.
- `io.cpp` scans loaded WADs for `SBP_*`, validates the SBlk header, matches
  the request against the resource suffix / pseudo name / `R_<name>` WAD
  name, and serves the resource bytes as the virtual `.bnk` file.
