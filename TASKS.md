# TASKS — v2.0 "Adreno Edition" (Adreno + Mali)

> Execution tracker. Updated as work lands. Statuses: `[ ]` todo, `[~]` in progress, `[x]` done, `[!]` blocked on hardware.

## Status snapshot (2026-10-22)
- Android APK builds: **yes** — `dist/gow2-debug.apk`, versionCode 3 / versionName
  2.0, arm64-v8a, 13,177,473 bytes, with `libmain.so` + libadrenotools hook libs.
  `extractNativeLibs=true`; all five `.so` files have `LOAD` align `0x4000`.
  The build script now clears the variant APK directory before packaging so an
  incremental zipflinger pass cannot leave stale padding (the 26.1 MB anomaly).
- Desktop runner builds: **yes** — diagnostics/LTO/affinity/DRS/audio changes
  link and complete a 1000-frame headless run.
- Dynamic resolution: **implemented (swapchain-only)** — default ON on Android,
  Settings toggle, `PS2_DRS` env, 0.6–1.0 range.
- Adreno driver/turbo: **implemented, opt-in** — `ps2_android_vulkan_prepare()`
  runs before wgpu init; Mali always uses the stock driver.
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
- [ ] CDVD read-ahead from SAF
- [ ] VU capture presets + faster interpreter fallback; target `Vuint ≈ 0` in test scene

## Fase 2 — Driver Adreno (libadrenotools) + kompatibilitas Mali
- [x] Vendor `libadrenotools` (BSD-2) and build hooks with NDK
- [x] Vulkan bootstrap through `adrenotools_open_libvulkan` with stock fallback
- [x] GPU Driver Manager UI (SAF zip → internal driver dir), active-driver display
- [x] Turbo clock toggle (`adrenotools_set_turbo`), default OFF
- [x] Adreno-only gating; Mali/other vendors keep stock driver path (never load turnip)
- [ ] `scripts/fetch-turnip.ps1` + documented tested driver revision

## Fase 3 — Backend Vulkan raw + jalur graphics (engine native baru)
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
- [ ] Internal GS render-scale step (1.0 → 0.85 → 0.7); currently swapchain-only by design
- [ ] Stable 30 fps cadence when 60 is unreachable (no judder), runtime switch
- [ ] Memory: ISO via `ParcelFileDescriptor` pread path, keep RSS headroom on 8 GB

## Fase 5 — Audio 989SND (BNK)
- [x] WAD `SBP_*` resource parser (989SND SBP/SBlk format) in `runtime/src/iop/io.cpp`
- [x] Serve `host1:sound/*.bnk` from loaded WADs + TOC fallback (`R_PERMA.WAD` for `general`)
- [x] Runtime trace: SMPD receives `SBP_general`/`SBP_general2` via SIF RPC and loads them as WAD resources; normal boot audio is non-silent
- [!] No `sound/*.bnk` request observed in headless traces, so the virtual `.bnk` service is dormant until a menu/gameplay path asks for one
- [!] One `989 ERROR 55 84010002` appeared in a scripted Start-press run, but not in a 1000-frame scripted trace run; it is non-fatal and no bank request precedes it. Root cause still needs a hardware logcat capture.
- [x] Regression: existing WAD/VAG audio path unchanged (600-frame and 1000-frame headless runs produced non-silent stereo PCM)

## Fase 6 — Verifikasi & rilis
- [x] Desktop/headless regression (wgpu path unaffected; 600/1000-frame runs completed)
- [x] Android build: single `libmain.so`, 16 KB LOAD alignment, `extractNativeLibs=true`
- [x] Clean/repeatable APK packaging: 13,177,473 bytes; `build-android.ps1` removes the variant output first to avoid zipflinger stale padding
- [x] APK version bumped to `versionCode 3` / `versionName "2.0"` (manifest + Gradle)
- [x] 16 KB linker flags applied to libadrenotools hook `.so` targets
- [x] `ps2_android_vulkan_prepare()` called from Android `main()` before wgpu creates its Vulkan instance
- [x] README / VERIFICATION.md v2.0
- [!] Frame-time plots (Perfetto) on Adreno 6xx-class target
- [!] Mali device smoke test (stock driver, no turnip)

## Findings — 989SND bank layout (retail USA ISO)
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
