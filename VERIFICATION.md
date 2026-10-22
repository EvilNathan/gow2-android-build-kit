# Verification log

This file records what was actually tested while the kit was assembled. It is
not a guarantee for your machine, but it tells you which parts have been run
end to end and which parts have not.

Date: 2026-10-22. Host: Windows 11, LLVM/Clang, Ninja, Python 3, JDK 17,
Android SDK platform 35, NDK 27.3.13750724, CMake 3.30.5.

## Passed

- **Disc image.** The USA DVD9 image reports `SCUS_974.81;1`, `VER = 1.01`,
  `VMODE = NTSC`; both layers and the root directory parse correctly.
- **Translation.** `eerecomp` translated 477,499 instructions into 4,444
  functions and 34,933 entry points, writing 118 C++ units for this disc.
- **VU AOT.** A headless run captured 78 VU1 programs; `vurecomp` produced
  7 translation units plus a table, and no new entry points or VU programs
  appeared after frame 3,000.
- **Runtime progression (software GS, headless).** The runner reached the
  SCEA presents screen, the main menu, the difficulty menu, the intro
  cutscenes, and the opening Loom Chamber sequence with 3D characters,
  textures, and animation (screenshot captured at frame 3,500). The run used
  the AOT VU1 programs and the generated EE code.
- **Audio.** A 420-frame run with `--audio-out` produced 333,344 stereo
  frames (6.9 seconds) of signed-16-bit PCM. The output was non-silent
  (RMS above 1,200, peak 8,547) after the initial logo silence. The SPU2
  path is live; see the bank limitation in `README.md`.
- **Patch portability.** `engine.patch` was applied with `git apply` to a
  fresh checkout of `snuri00/ps2-web-recomp` at
  `6526f4803d98e10de50bea3af104a4d26e9df033`, and `eerecomp`, `vurecomp`,
  and the `ps2run` target all built from that checkout.
- **Clean-kit workflow.** A copy of this kit with no engine and no build
  outputs was run through `setup.ps1`, `build-tools.ps1`,
  `extract-disc.ps1`, `recompile.ps1 -SkipExplore`, and `build-android.ps1`.
  The result was an installable debug APK.
- **APK structure.** The APK contains `lib/arm64-v8a/libmain.so` and the four
  libadrenotools hook libraries (`libhook_impl.so`, `libmain_hook.so`,
  `libfile_redirect_hook.so`, `libgsl_alloc_hook.so`). It exports `SDL_main`,
  `ps2_android_vulkan_prepare`, and
  `Java_com_gow2_android_TouchOverlayView_nativeSetPad`; every `.so` has ELF
  `LOAD` alignment `0x4000` (16 KB compatible); the merged manifest sets
  `extractNativeLibs=true`; the APK is debug-signed and does not package
  `libwgpu_native.so`.

## Performance update (2026-10-21, APK v1.1 / kit v0.1.1)

The first APK booted on a mid-range phone (8 GB RAM) and reached gameplay,
but the frame rate was too low to play. Only the outcome was reported; no
profile numbers were captured. This update targets the present/pacing layer
and adds instrumentation for the next round:

- Android selects the FIFO surface present mode (frames align with the panel
  vsync instead of arriving in bursts when a frame finishes early);
- the frame pacing loop now spins in the final millisecond after a coarse
  sleep instead of relying on one `SDL_Delay` call with 1-2 ms scheduler
  slack;
- on Android the swapchain surface is capped to about 1.4 M pixels (aspect
  preserved; the compositor upscales), cutting the final blit fill cost
  several times on 1080p/1440p-class panels;
- the deinterlace weave (blending the previous display field) moved from a
  compile-time constant to a runtime switch (initial state from
  `PS2_NO_DEINTERLACE`), so a weak device can drop the extra blend pass;
- `PS2_PRESENT_MODE` (immediate/mailbox/fifo) can force the present mode;
- one logcat line per second with tag `GoW2Stats`: present fps, CPU %, GPU
  %, RSS, audio queue depth, VU1 interpreter fallback calls per second, plus
  the chosen adapter and surface size at startup.

Verified for this update: `ps2run` (Release, MSVC/clang-cl) rebuilt cleanly;
the APK rebuilt with NDK 27.3.13750724 and the same structure checks passed
(single `lib/arm64-v8a/libmain.so`, LOAD align `0x4000`, `SDL_main` and the
touch JNI exported). `engine.patch` re-applies to a fresh checkout at
`6526f4803d98e10de50bea3af104a4d26e9df033` (`git apply --ignore-whitespace`)
and the tools build from it.

## v2.0 Adreno Edition (2026-10-22)

Build and packaging checks passed:

- Desktop `ps2run` rebuilt with the v2.0 diagnostics, CPU-affinity, DRS, and
  989SND changes; a 1000-frame scripted headless run completed with 797,344
  stereo audio frames and no crash.
- Android APK rebuilt from a clean `android/app/build` as `versionCode 3` /
  `versionName "2.0"`, arm64-v8a, 13,177,473 bytes. `aapt2 dump badging`
  reports `sdkVersion 26`, `targetSdkVersion 35`; `aapt2 dump xmltree` reports
  `extractNativeLibs=true`. A previous incremental package was 26.1 MB because
  zipflinger left stale padding in place; `build-android.ps1` now removes the
  variant output directory before packaging. A clean package and a repeated
  incremental package both produced 13,177,473 bytes.
- All five native libraries in the APK have `LOAD` align `0x4000`.
  `libmain.so` contains `ps2_android_vulkan_prepare`, `adrenotools_open_libvulkan`,
  `adrenotools_set_turbo`, and the `dlopen`/`dlclose` interposer. Android
  `main()` calls `ps2_android_vulkan_prepare()` before wgpu creates its Vulkan
  instance.
- Android builds use ThinLTO, `--gc-sections`, and `--icf=all` by default.
  `PS2_ANDROID_LTO=OFF` is available for fast iteration.
- CPU affinity reads the maximum-frequency table and pins the EE/render
  threads to the fastest cluster; the VU thread is allowed on all cores.
  The code is Android-only and fails open if the sysfs table is unreadable.
- DRS is swapchain-only: `show_speed()` runs immediately before `present()`
  on the render thread, adjusts `0.6–1.0` using GPU/present frame time, and
  logs `DRS %.2fx` in `GoW2Stats`. It is on by default on Android and can be
  disabled in Settings (`PS2_DRS=0`).
- 989SND trace: SMPD receives `SBP_general`/`SBP_general2` over SIF RPC and
  loads them as WAD resources; boot audio is non-silent. `io.cpp` now scans
  WADs for `SBP_*`, validates the `SBlk` header, and can serve a virtual
  `host1:sound/*.bnk`. No actual `.bnk` request was observed in the headless
  traces, and one scripted Start-press run printed a non-fatal
  `989 ERROR 55 84010002` without a preceding bank request. That error is not
  yet root-caused; it needs a hardware logcat capture.
- `scripts/build-android.ps1` completed successfully. The APK is at
  `dist/gow2-debug.apk`.

## Maintenance pass (2026-10-22, second pass)

This pass corrected `TASKS.md` against the code and landed the remaining
software-side items. Verified here:

- **Tracker correction.** `TASKS.md` marked "ISO via `ParcelFileDescriptor`
  pread path" as todo, but it shipped with the first kit commit: `GameActivity`
  passes `--iso-fd`/`--iso-size`, `host/main.cpp` calls `ps2::disc::open_fd`,
  and `runtime/src/disc.cpp` reads with `pread`. The row is now checked.
- **Disc read-ahead.** `runtime/src/disc.cpp` keeps a 256 KiB aligned window in
  front of the image and serves each CDVD burst from it. The window is shared by
  the plain-file reader (exercised on Windows) and the SAF descriptor reader, so
  the same code path was tested headless. A 300-frame run with
  `PS2_DISC_STATS=1` reported **533 host reads, 398 cache hits (75 %),
  14.7 MiB served**.
- **30 Hz cadence.** `--fps-cap 30` / `PS2_FPS_CADENCE=30` (and a Settings
  checkbox) halves the vblank rate instead of skipping presented frames.
  Verified headless: **9,830,411 EE cycles per frame** (EE clock / 30, exactly
  double the 60 Hz 4,939,815) and **474,976 stereo frames = 10 s at 48 kHz** from
  the SPU2 path, so audio and presentation stay aligned. A 120-frame run without
  the cap still reports 4,939,815 cycles per frame.
- **Capture presets.** `recompile.ps1 -Preset quick|standard|full` maps to
  no-replay / 2,400 / 4,200 frames; explicit `-Frames`/`-ExploreRounds` still
  win. `-VuCache .\games\gow2\vu1` recompiled **117 VU1 programs from 86
  records** without replaying the game, and a 60-frame run with the restored
  table completed.
- **Turnip fetch.** `scripts/fetch-turnip.ps1` downloaded
  `K11MCH1/AdrenoToolsDrivers` `v26.0.0-rc08` / `Turnip_v26.0.0_R8.zip`
  (3.3 MB), verified the pinned SHA256, and unpacked `vulkan.ad07xx.so`.
- **Patch portability.** `engine.patch` was regenerated from the working tree;
  `git apply --check` passes on a pristine worktree at `6526f48`, and
  `git apply --reverse --check` passes against the patched tree.
- **APK.** Rebuilt after the changes: **13,182,613 bytes**, `versionCode 3` /
  `versionName "2.0"`, arm64-v8a, all five `.so` files with `LOAD` align
  `0x4000`. `libmain.so` contains the new `--fps-cap`, `PS2_FPS_CADENCE`,
  `PS2_DISC_STATS` and read-ahead strings.

Not verified in this pass: the read-ahead benefit on a real SAF descriptor, the
30 Hz mode on a phone, loading the Turnip package through libadrenotools, and
every performance number (no device attached).

## v2.1 Safety & Universal (2026-10-22, third pass)

Trigger: the v2.0 APK closed as soon as Play was pressed on the reporter's
phone, while v1.1 (no AOT VU table, no driver loader, DRS default OFF) booted.
No device is attached to this machine, so the pass is static analysis plus a
build/test cycle; the fixes target the two Android-only regressions with the
largest blast radius.

- **`dlopen` interposition removed.** `readelf --dyn-syms` on the v2.0
  `libmain.so` showed `dlopen` and `dlclose` exported `GLOBAL DEFAULT`; v1.1 had
  them as undefined imports. That made every `dlopen` in the app process funnel
  through `android_vulkan_loader.cpp`. 2.1 builds `main` with
  `-Wl,--wrap=dlopen -Wl,--wrap=dlclose`, so only references linked into
  `libmain.so` reach `__wrap_*`; the symbols stay hidden and the rest of the
  process uses bionic.
- **Adapter limits.** `gpu::init()` now requests `WGPULimits` filled by
  `wgpuAdapterGetLimits()` (falling back to `WGPU_LIMITS_INIT` if the call
  fails) rather than desktop defaults with two fields overwritten. Mobile
  drivers that report smaller values can no longer fail device creation.
- **DRS opt-in.** `PS2_DRS` is now `true` only when set (any platform); the
  Settings default is unchecked and Safe mode forces it off. The first
  `set_present_scale()` call is also skipped when DRS is off.
- **Safe mode.** `PS2_SAFE=1` (Settings checkbox) skips the custom driver and
  turbo, forces the stock driver, DRS off and 60 Hz cadence in `GameActivity`.
- **Boot breadcrumbs.** `[boot]` logcat lines from `main.cpp` (args, ISO handle,
  disc open, ELF extraction, window/audio) and `window.cpp`/`gpu.cpp` (adapter,
  limits, device failure, graphics mode, first frame). `[driver]` lines cover
  the Vulkan loader path.
- **Universal UI.** The driver/turbo section is rendered on every device; on
  non-Adreno it is disabled with a vendor-specific explanation. DRS, cadence,
  render scale and stats are vendor-neutral.
- **Regression run.** The native runner still completes 60 frames:
  **297,861,626 cycles**, frame limit reached.
- **Patch.** `engine.patch` regenerated (107,920 bytes); reverse check passes.

Still unverified: the exact v2.0 crash signature (needs `adb logcat -d -s
GoW2Stats AndroidRuntime DEBUG` from the phone) and every v2.1 change on a real
GPU.

## v2.2 UI & Logs (2026-10-22, fourth pass)

- **Adaptive icon.** `mipmap-anydpi-v26/ic_launcher(.round).xml` with vector
  `ic_launcher_background` (radial bronze/red) and `ic_launcher_foreground`
  (omega medallion). Manifest keeps the PNG mipmaps as the pre-26 fallback and
  adds `roundIcon`.
- **Launcher.** Rewritten programmatic layout: two columns, gradient accent bar,
  gradient title, status dot, ripple buttons, cards for controls and
  diagnostics, version footer. `AppTheme` is now Material dark with the bronze
  accent and the launcher gradient as `windowBackground`.
- **Log file.** New `LogFile` helper + `GoW2App` application class install the
  Java crash handler and set `PS2_LOG_FILE` before any activity. Native
  `ps2/log.hpp` writes every line to logcat (Android) and the file, flushing
  after each line; `main.cpp`, `window.cpp`, `gpu.cpp` and `system.cpp` now use
  it, so the boot breadcrumbs, adapter/limits, frame stats and the `[ee]`
  summary land in the same file. Verified on the desktop runner: a 60-frame run
  with `PS2_LOG_FILE` produced the ELF/stop/finish lines in order.
- **Location.** `/sdcard/gow2/log.txt` when writable (Android <= 9 or after the
  all-files grant); otherwise `Android/data/com.gow2.android/files/gow2/log.txt`.
  The launcher shows which one is active, has VIEW LOG / COPY PATH, and opens
  the system all-files-access page when needed. Rotation keeps `log.txt.old`
  at 4 MiB.
- **Regression run.** Native runner still completes 60 frames
  (**297,861,626 cycles**), and the new logger wrote the expected lines.

Not verified in this pass: icon rendering on a device launcher, the launcher
layout on a real phone, and whether the all-files grant flow is accepted by the
reporter's OEM.

## Not verified

- **Performance update on hardware.** The v1.1/v2.0/v2.1 APKs have not run on the
  reporter's phone yet. The expected workflow is `adb logcat -d -s
  GoW2Stats AndroidRuntime DEBUG` during play, after which the
  present-mode/quality knobs can be adjusted against real numbers.
- **v2.1 crash fix.** Static analysis names two
  Android-only regressions (exported `dlopen`, desktop-default limits) plus DRS
  being on by default; all three are addressed, but the actual 2.0 crash
  signature is unknown until the phone returns a logcat.
- **Dynamic resolution on hardware.** The controller and swapchain resize are
  implemented and compile, but the actual scale changes, visual quality, and
  frame-time response need a device.
- **Adreno driver replacement and turbo.** The APK contains libadrenotools and
  the interposer, and the opt-in path is wired to the launcher, but no custom
  driver has been loaded on a phone. Turbo's KGSL ioctl may be denied without
  root; it silently no-ops in that case. Mali/other vendors always use the
  stock driver and were not device-tested either.
- **Internal GS render-scale step.** Not implemented; the shipped controller is
  swapchain-only by design, because `PS2_SCALE` sizes VRAM/scratch/readback
  buffers at `init_graphics()` and a live step needs buffer reallocation plus
  pipeline rebinding.
- **Full VU exploration in the clean kit.** Now covered: a fresh clone at
  `3cffd78` ran `build-all.ps1` end to end and recorded 86 VU1 programs
  (117 compiled) in 2,400 frames, then built the APK.
- **Disc read-ahead on a real SAF descriptor, 30 Hz feel on a phone, and loading
  the pinned Turnip package** — all implemented and exercised headless only; no
  device was attached.
- **Complete audio.** The `SBP_*` parser/virtual `.bnk` service is implemented,
  but the runtime `.bnk` request names have not been captured and the menu
  `989 ERROR 55` is not root-caused. Bank-driven music, voice, or effects may
  still be missing on specific screens.
- **Raw Vulkan backend.** Not implemented; wgpu-native remains the renderer.
- **Release signing.** The kit builds a debug-signed APK.
