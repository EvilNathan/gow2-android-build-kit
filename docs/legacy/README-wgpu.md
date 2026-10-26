# God of War II - native Android build kit

![license](https://img.shields.io/badge/license-MIT-blue.svg)
![platform](https://img.shields.io/badge/platform-Android%20arm64-3ddc84.svg)
![engine](https://img.shields.io/badge/engine-ps2--web--recomp%20%406526f48-informational.svg)

This kit turns **your own** copy of *God of War II* (USA, `SCUS_974.81`) into an
Android arm64 app. It is a **static-recompilation** port, not an emulator
frontend: the game's Emotion Engine code is translated ahead of time to C++,
VU1 microcode is compiled AOT, and the Graphics Synthesizer is implemented by a
native WebGPU/Vulkan renderer through `wgpu-native`. SDL3 provides the Android
window, audio device, gamepads, and lifecycle handling.

> **This repository contains no game code.** No ISO, no ELF, no generated C++,
> no APK, no game assets. You supply the disc image; everything derived from it
> stays on your machine.

## Build status

| Item | Status |
| --- | --- |
| Recompiler tools (`eerecomp`, `vurecomp`) build on Windows | ✅ passing |
| Emotion Engine translation of the USA disc | ✅ 477,499 instructions → 118 C++ units |
| VU1 AOT compilation | ✅ 117 programs compiled from 86 captured blocks; the five added in 2.0 are skipped by default in 2.3 |
| Native runner (`ps2run`) build | ✅ passing |
| Boot: title → difficulty menu → intro cutscenes → Loom Chamber | ✅ verified (frame 3,500) |
| 3D rendering with characters, textures, animation | ✅ verified in software-GS headless run |
| SPU2 audio output | ✅ non-silent stereo verified (6.9 s capture) |
| 989SND bank coverage (`sound/*.bnk`) | ⚠️ WAD `SBP_*` scanner + virtual `.bnk` service implemented; runtime `.bnk` capture pending |
| Android APK build (`assembleDebug`) | ✅ passing (version 6 / 2.3, arm64-v8a) |
| 16 KB page compatibility | ✅ `libmain.so` + hook libs `LOAD` align `0x4000`, `extractNativeLibs=true` |
| Memory-card save/load | ⚠️ implemented, in-game save not yet exercised |
| First device run (mid-range phone, 8 GB RAM) | ⚠️ booted to gameplay, but too slow to play on v1.0 |
| Performance update v1.1 (FIFO present, tuned pacing, Android surface cap, runtime deinterlace switch, logcat stats) | ✅ shipped, device re-test pending |
| **v2.0 Adreno Edition** (ThinLTO, gc-sections, ICF, CPU affinity, DRS, optional Adreno driver + turbo) | ⚠️ implemented; crashed on a user device at Play |
| **v2.1 Safety & Universal** (no exported `dlopen`, adapter limits, DRS opt-in, safe mode, boot logcat) | ✅ built; device re-test pending |
| **v2.3 VU1 AOT hotfix** (five 2.0 programs skipped by default, per-program Settings toggles, `PS2_VU_SKIP`/`PS2_VU_AOT`) | ✅ built; device confirmation pending |
| Mali-G68/G77 30 fps target | ❌ not yet measured on hardware |
| Dynamic resolution | ✅ swapchain-only 0.6–1.0, opt-in on every platform, Settings toggle |

## v2.3 VU1 AOT hotfix

Download: [`v2.3.0`](https://github.com/EvilNathan/gow2-android-apk/releases/tag/v2.3.0)
(private repo; asset `gow2-debug.apk`, 13,192,938 bytes, sha256
`B612E1BA300053041C08358E6C9ECE04384296A5F12D9BEE61C3EF65AFBF577F`).

Sixth build. The first evidence-backed fix for the v2.0-v2.2 "closes at Play"
crash:

- **Binary evidence.** v1.0.0 and v1.1.0 `libmain.so` contain 112 of the 117 VU1
  AOT program hashes; v2.0.0-v2.2.0 contain all 117. The five added in 2.0 are
  `4176D02B93877C36`, `54781DABBA793231`, `758DC1364D0BF01B`,
  `E344F110E1EBEEDC`, `EF764AF45D6D7189`. v1.x ran them on the interpreter, so
  the same scenes were safe.
- **Five skipped by default.** The game starts with the v1.x program set
  (`[vu] AOT programs registered: 112/117`); the five fall back to the
  interpreter until each one is verified on hardware.
- **Per-program bisect.** Settings → **VU1 AOT (diagnostic)** has a master
  checkbox plus one checkbox per program, all off after a fresh install. Enable
  them one at a time to find the one that crashes a specific device.
- **Runtime controls.** `PS2_VU_AOT=0` disables AOT entirely;
  `PS2_VU_SKIP=<hash>,<hash>,...` skips individual programs. The active set is
  logged at game start (`vu_aot`, `vu_skip`) and by the native runtime.
- **Version** 6 / 2.3.

If 2.3 still closes, the five were not the cause; the next bisect is the rest of
the v2.0 engine changes (read-ahead, SBP scanner, affinity, ThinLTO/ICF,
adrenotools), documented in `TASKS.md` Fase 7.

## v2.2 UI & logs

Download: [`v2.2.0`](https://github.com/EvilNathan/gow2-android-apk/releases/tag/v2.2.0)
(private repo; asset `gow2-debug.apk`, 13,192,346 bytes, sha256
`0A0F0016756DC1D67A1E5BD67DE23370BE7897BD9C177A122E8953205E66E16A`).

Fifth build. Same engine as 2.1 plus presentation and diagnostics:

- **New app icon.** Adaptive icon (API 26+) with a bronze omega medallion on a
  deep red radial background; round-icon variant included.
- **New launcher.** Two-column landscape layout with gold accent bar, gradient
  title, card panels for controls and diagnostics, state-coloured status dot,
  custom PLAY/ghost buttons with ripple, and a Material dark theme shared with
  the dialog.
- **Log file on external storage.** `PS2_LOG_FILE` is set at application start,
  so the native `[boot]`/`[gpu]`/`[window]`/`[ee]` breadcrumbs and frame stats
  are appended (and flushed) to **`/sdcard/gow2/log.txt`**. Android 11+ needs
  "All files access" for that exact path; the launcher shows the active path,
  can open the log, copy the path, and offers an **ALLOW /SDCARD WRITE** button.
  Without the grant the same file is written to
  `Android/data/com.gow2.android/files/gow2/log.txt`, and the card says so.
  Java crash traces go to the same file, and the log rotates at 4 MiB
  (`log.txt.old`).
- **Version** 5 / 2.2.

## v2.1 Safety & Universal

Download: [`v2.1.0`](https://github.com/EvilNathan/gow2-android-apk/releases/tag/v2.1.0)
(private repo; asset `gow2-debug.apk`, 13,179,189 bytes, sha256
`86BC385313A9A64C46D9B95999D6DF67DCD064998736F685B7682BD38A2EDBAF`).

v2.0 crashed on the user's phone as soon as Play was pressed. Two Android-only
changes were identified as high risk and are fixed or disabled by default in
2.1. The renderer, the game code and the PS2-side behaviour are unchanged.

- **No more process-wide `dlopen` override.** 2.0 exported `dlopen`/`dlclose`
  from `libmain.so`; that interposes those symbols for *every* library in the
  app process. 2.1 keeps the optional custom-driver hook, but wires it with
  `-Wl,--wrap=dlopen -Wl,--wrap=dlclose`, so only the references linked into
  `libmain.so` are redirected and nothing is exported.
- **Device limits instead of desktop defaults.** The device is now requested
  with exactly the limits the adapter reports (`wgpuAdapterGetLimits`) instead of
  WebGPU defaults, which mobile drivers can reject; a rejected device used to
  abort during start-up.
- **Dynamic resolution is opt-in on all platforms** (was default ON on
  Android). A live swapchain reconfigure is the graphics path that varies most
  between drivers, so it now only runs when the Settings box is ticked.
- **Safe mode.** A new Settings checkbox forces the stock driver, no turbo,
  no DRS and a fixed 60 Hz cadence, so a device that crashed on 2.0 still boots.
- **Boot breadcrumbs in logcat.** `[boot]` lines cover args, ISO open, ELF
  extraction, window/audio, adapter/limits, graphics init and the first frame.
  `adb logcat -d -s GoW2Stats AndroidRuntime DEBUG` now pinpoints a failure.
- **Universal by default.** DRS, 30 fps cadence, render scale and stats are
  vendor-neutral (Adreno, Mali, Xclipse, PowerVR). The driver import and turbo
  toggle remain Adreno-only because Turnip/libadrenotools is Adreno-specific;
  the section is now visible on every device with the controls disabled and an
  explanation instead of being hidden.


## v2.0 Adreno Edition (historical)

This release added the mobile performance work on top of v1.1. Its risky parts
were reworked in 2.1 (see above); the list below describes the feature set.

- **Faster native build:** Android uses ThinLTO, `--gc-sections`, and
  `--icf=all`; set `PS2_ANDROID_LTO=OFF` for quick iteration.
- **CPU affinity:** EE/render threads use the fastest CPU cluster; VU can use
  all cores. No-op on desktop and fails open when sysfs is restricted.
- **Dynamic resolution:** swapchain-only 0.6x–1.0x controller. Since 2.1 it is
  opt-in on every platform, with a Settings checkbox and `DRS` stats in logcat.
- **Optional Adreno driver manager:** import an AdrenoTools zip or
  `libvulkan*.so`; load it rootlessly through libadrenotools. **Turbo clock is
  a separate toggle, default OFF.** Mali/other vendors always use the stock
  driver; a failed import/load falls back to it.
- **989SND bank work:** WAD `SBP_*` scanner, `SBlk` validation, and virtual
  `host1:sound/*.bnk` service. Normal SMPD loads use the `SBP_*` resources
  directly; exact `.bnk` request names still need a hardware capture.
- **Disc read-ahead:** a 256 KiB aligned window in front of the ISO descriptor
  serves a CDVD burst with one host read instead of one per sector; on Android
  each read otherwise crosses into the Storage Access Framework provider.
  `PS2_DISC_STATS=1` prints the hit rate.
- **30 Hz cadence:** `--fps-cap 30` (or `PS2_FPS_CADENCE=30`, or the Settings
  checkbox) halves the vblank rate so logic, SPU2 and presentation stay in step
  on a phone that cannot hold 60, instead of dropping frames.
- **Turnip fetch:** `scripts/fetch-turnip.ps1` downloads a pinned, SHA256-checked
  AdrenoTools package from `K11MCH1/AdrenoToolsDrivers`; import it with
  **SELECT DRIVER**.

Everything above is implemented and builds, but the performance and driver
claims are **not hardware-verified**: no Android device was attached to the
build machine. See `VERIFICATION.md`.

## Port progress

Engineering estimate for the USA NTSC-U image, in the scope this repo can test.
Percentages are honest status, not a compatibility promise.

| Area | Progress | Notes |
| --- | ---: | --- |
| Emotion Engine static recompilation | 100% | 4,444 functions, 34,933 entry points, 118 units |
| VU1 AOT compilation | 100% | No new VU programs recorded after frame 3,000 |
| Core runtime HLE (IOP, CDVD, SIF, IPU, timers) | 90% | Modules load; deeper game states untested |
| GS renderer via wgpu / Vulkan | 85% | Works in the software-GS headless run; device Vulkan untested |
| Android app shell (SAF, touch, gamepad, lifecycle) | 95% | Built and statically checked; not run on hardware |
| Saves (MC2_D memory card) | 80% | Card HLE present; in-game save not yet verified |
| Audio: SPU2 output path | 100% | Non-silent stereo capture verified |
| Audio: 989SND bank coverage | 65% | `SBP_*` parser and virtual `.bnk` service implemented; SMPD normal bank loads use WAD resources; capture pending |
| Game compatibility breadth | 60% | Boots into the opening chapter; full game unverified |
| Performance tuning | 70% | ThinLTO/gc-sections/ICF, CPU affinity, swapchain DRS, disc read-ahead, 30 Hz cadence, optional Adreno driver/turbo; no device metrics yet |
| Docs, packaging, legal | 100% | Kit, notices, release, verification log |

**Overall: ~87% engineering complete; hardware validation ~5%** (one phone
reached gameplay but the v1.0 frame rate was too low; the v2.1 safety build
awaits a re-test with `adb logcat -d -s GoW2Stats` numbers). The raw
Vulkan backend (Fase 3 in `TASKS.md`) is the one large piece still unwritten;
the shipped renderer remains wgpu.

## Important files

| Path | What it is |
| --- | --- |
| [`README.md`](README.md) | This document |
| [`VERIFICATION.md`](VERIFICATION.md) | What was actually tested and what was not |
| [`TASKS.md`](TASKS.md) | v2.0 phase tracker and 989SND findings |
| [`NOTICE.md`](NOTICE.md) | Dependency notices and trademark statement |
| [`LICENSE`](LICENSE) | MIT license for this kit |
| [`LICENSE-ENGINE.txt`](LICENSE-ENGINE.txt) | Upstream MIT license (`ps2-web-recomp`) |
| [`third_party/LICENSE-SDL3.txt`](third_party/LICENSE-SDL3.txt) | zlib license for the bundled SDL Java classes |
| [`engine.patch`](engine.patch) | All engine changes, applied by `setup.ps1` |
| [`scripts/common.ps1`](scripts/common.ps1) | Shared paths and helpers |
| [`scripts/setup.ps1`](scripts/setup.ps1) | Clones the pinned engine, fetches SDL3 + wgpu-native + libadrenotools |
| [`scripts/build-tools.ps1`](scripts/build-tools.ps1) | Builds `eerecomp.exe` and `vurecomp.exe` |
| [`scripts/extract-disc.ps1`](scripts/extract-disc.ps1) | Copies the boot files out of your ISO |
| [`scripts/recompile.ps1`](scripts/recompile.ps1) | Translates EE code, records/compiles VU1, builds `ps2run` |
| [`scripts/build-android.ps1`](scripts/build-android.ps1) | Builds the debug APK |
| [`scripts/fetch-turnip.ps1`](scripts/fetch-turnip.ps1) | Downloads a pinned, hash-verified Turnip package for the Adreno driver manager |
| [`scripts/build-all.ps1`](scripts/build-all.ps1) | Runs the whole pipeline |
| [`scripts/run-headless.ps1`](scripts/run-headless.ps1) | Runs the desktop/headless diagnostic runner |
| [`android/app/jni/CMakeLists.txt`](android/app/jni/CMakeLists.txt) | Android native build: SDL3 + wgpu-native + libadrenotools + `libmain.so` |
| [`engine/host/android_vulkan_loader.cpp`](engine/host/android_vulkan_loader.cpp) | Opt-in Adreno driver loader and `dlopen` bridge |
| [`engine/third_party/libadrenotools/`](engine/third_party/libadrenotools) | Vendored BSD-2 rootless driver loader and hooks |
| [`engine/runtime/src/iop/io.cpp`](engine/runtime/src/iop/io.cpp) | IOP file service, WAD `SBP_*` scanner, virtual `.bnk` serving |
| [`android/app/src/main/AndroidManifest.xml`](android/app/src/main/AndroidManifest.xml) | Activities, Vulkan/gamepad features |
| [`android/app/src/main/java/com/gow2/android/`](android/app/src/main/java/com/gow2/android) | Launcher, SAF picker, game activity, touch overlay |
| [`tools/toc.py`](tools/toc.py) | Inspects `GODOFWAR.TOC` entries and partitions |
| [`.gitattributes`](.gitattributes) | Keeps `engine.patch` LF so `git apply` works everywhere |

## How it works

```
your ISO (SCUS_974.81)
        │
        ├── eerecomp ──▶ generated/gow2/unit_*.cpp      (Emotion Engine → C++)
        ├── vurecomp ──▶ generated/gow2/vu1_*.cpp       (VU1 microcode → C++)
        │
        └── runtime + generated code + wgpu-native (Vulkan) + SDL3
                        │
                        └──▶ libmain.so ──▶ app-debug.apk
```

The runtime replaces the PS2 hardware: EE/VU execution, IOP with IRX modules
(`SIO2MAN`, `DBCMAN`, `DS2U_D`, `LIBSD`, `989NOMID`, `SMPD`, `MC2_D`, …), GS
commands, CDVD reads from your ISO, SPU2 audio, and memory cards. On Android
the GS is rendered through `wgpu-native`'s Vulkan backend onto an
`ANativeWindow` surface.

## Requirements

### Windows 10/11

- Git for Windows
- CMake 3.24 or newer, Ninja, and LLVM/Clang 20 on `PATH`
  (MSVC also works if you run from a Developer PowerShell and omit Ninja)
- Python 3 with a `python` command
- JDK 17
- Android SDK with:
  - platform 35
  - build-tools 35.0.0
  - NDK **27.3.13750724**
  - CMake **3.30.5** (the version selected by Gradle)
- About 12 GB free disk space (engine, SDK, generated C++, build outputs)
- An ISO of *God of War II* (USA), `SCUS_974.81`, 8.5 GB DVD9

Set `ANDROID_HOME` (or `ANDROID_SDK_ROOT`) to the SDK folder. Install missing
packages with `sdkmanager`, for example:

```powershell
sdkmanager "platforms;android-35" "build-tools;35.0.0" "ndk;27.3.13750724" "cmake;3.30.5"
```

## Quick start

Get the kit from the
[v0.1.0 release](https://github.com/EvilNathan/gow2-android-build-kit/releases)
or clone this repository. Put the ISO anywhere outside the kit folder (the
path may contain spaces), then from the kit root:

```powershell
Set-ExecutionPolicy -Scope Process Bypass
.\scripts\build-all.ps1 -Iso "D:\Games\God of War II (USA).iso"
```

The first run downloads the pinned engine and three dependencies, builds the
recompilers, extracts the boot files, translates the game, records VU1 programs,
and builds a debug APK. Expect roughly 20-90 minutes depending on CPU and the
exploration length. The exploration run is CPU-heavy because this build
environment has no Vulkan device; on a PC with a supported GPU the native
runner is much faster.

Install it:

```powershell
adb install -r .\dist\gow2-debug.apk
```

On the phone, open **God of War II**, choose the ISO with the system file picker,
and press **PLAY**. The ISO is read through Android's Storage Access Framework;
it is not copied except for the 2.1 MB boot ELF, which is extracted into the
app's private files directory.

### Individual steps

```powershell
.\scripts\setup.ps1                 # engine + SDL3 + wgpu-native
.\scripts\extract-disc.ps1 -Iso "...\God of War II (USA).iso"
.\scripts\build-tools.ps1           # eerecomp.exe and vurecomp.exe
.\scripts\recompile.ps1 -Iso "...\God of War II (USA).iso" -Preset standard
.\scripts\build-android.ps1         # dist\gow2-debug.apk
```

`recompile.ps1` accepts `-SkipExplore` for a fast build without VU recording, or
`-Preset` to pick a capture budget: `quick` (no replay), `standard` (2,400
frames, the default) and `full` (4,200 frames, more of the intro). Missing VU
programs are interpreted at runtime; the game is correct but slower. Increase
`-Frames` to capture more VU programs and more of the intro.

### Faster rebuilds

The expensive step is the VU exploration: it replays the game in the software
GS for `-Frames` frames (~20 minutes for the default 2,400), once per round.
Keep the recording and reuse it instead of replaying:

```powershell
# First run records games\gow2\vu1\*.bin as usual.
.\scripts\recompile.ps1 -Iso "D:\Games\God of War II (USA).iso"

# Later builds reuse the recording; no replay, no ISO reads.
.\scripts\recompile.ps1 -VuCache .\games\gow2\vu1
.\scripts\build-android.ps1
```

`-VuCache` accepts any folder of `*.bin` VU records, including a copy kept
outside `games\`, which the kit's `.gitignore` excludes. The rest of the
pipeline is already incremental: `setup.ps1` skips a checkout that is at the
pinned commit, `build-tools.ps1` and the native CMake build reuse their build
trees, and Gradle reuses `android\app\.cxx` and `android\app\build`.

Everything game-derived — `generated\`, `games\`, and the APK — stays local.
The kit ships none of it, so a fresh clone still has to translate the disc
once.

## Project layout

```
kit/
  engine.patch           changes applied to ps2-web-recomp
  scripts/               setup and build scripts
  android/               Gradle project and SDL Java activity
  tools/                 small Python helpers
  VERIFICATION.md        what was tested and what was not
  engine/                created by setup.ps1 (pinned upstream checkout)
  games/gow2/            created locally: extracted boot files, VU records,
                         memory card, screenshots
  generated/gow2/        created locally: translated EE/VU C++
  build/                 native and tool build trees
  dist/                  APK (local only)
```

The engine is pinned to `snuri00/ps2-web-recomp`
commit `6526f4803d98e10de50bea3af104a4d26e9df033` (MIT).

## Controls

- **Touch:** left side is the analog stick; the right diamond is
  Triangle/Circle/Cross/Square; L1/L2 and R1/R2 are at the edges;
  START/SELECT are at the bottom.
- **Gamepad:** SDL gamepad mapping; South/East/West/North map to
  Cross/Circle/Square/Triangle. A second pad becomes player 2.
- **Keyboard (desktop runner):** arrows, Z/X/A/S, Q/W, 1/2, Enter, Backspace.
- **Android:** system Back leaves the game. The activity pauses emulation and
  audio when it enters the background or loses focus.

## Settings

The launcher has a **SETTINGS** button:

- **Widescreen (16:9)** - passes `--widescreen`; default is the game's 4:3.
- **Render scale 1x / 2x** - sets `PS2_SCALE` for the internal GS buffers.
  1x is strongly recommended on Mali-G68/G77. Changes apply at the next start.
- **Dynamic resolution (recommended)** - default ON. Lowers only the Android
  swapchain scale (0.6x–1.0x) when the GPU misses frame time; the PS2 render
  scale and game geometry stay unchanged. Works on Adreno and Mali. Passes
  `PS2_DRS=1`.
- **30 fps cadence** - default OFF. Runs the console at 30 Hz instead of 60
  (`PS2_FPS_CADENCE=30`). Logic, SPU2 audio and presentation stay in step, so a
  phone that cannot hold 60 shows every frame evenly instead of juddering; the
  game itself runs at half speed.
- **Dim touch controls** - lowers the overlay opacity.
- **GPU driver (Adreno only)** - import an AdrenoTools `.zip` or a
  `libvulkan*.so` package. The custom driver is opt-in; **USE SYSTEM DRIVER**
  removes it. Mali, PowerVR and other vendors always use the stock driver.
- **Turbo clock (Adreno only)** - default **OFF**. Requests the Adreno turbo
  power hint through `adrenotools_set_turbo()`; it can raise power and heat and
  may be denied without the required kernel access. Leave it off unless a
  specific driver package benefits.

Preferences and the memory card survive app updates. Clearing app data removes
both.

## Saves

The app passes `--memcard <files>/gow2.ps2` to the runtime's MC2_D memory-card
implementation. Use the game's normal save points and save/load menus. The card
is a file in the app's private storage; back it up with:

```powershell
adb shell run-as com.gow2.android ls files
adb exec-out run-as com.gow2.android cat files/gow2.ps2 > gow2.ps2
```

## 16 KB page compatibility

The Android app is built for arm64 only. `libmain.so` and the libadrenotools
hook libraries (`libhook_impl.so`, `libmain_hook.so`, `libfile_redirect_hook.so`,
`libgsl_alloc_hook.so`) are linked with `-Wl,-z,max-page-size=16384` and
`-Wl,-z,common-page-size=16384`, and Gradle extracts native libraries at
install time (`useLegacyPackaging = true`). This keeps the app compatible with
16 KB-page Android 15+ devices without requiring the APK's compressed ZIP
entries to be 16 KB aligned.

## Performance notes

- **v2.0 Adreno Edition:** Android builds use ThinLTO, `--gc-sections` and
  identical-code folding to shrink `libmain.so` and improve instruction-cache
  behaviour.
- **CPU affinity:** on Android the EE/render threads are pinned to the fastest
  available CPU cluster and the VU thread is allowed on all cores. This is a
  no-op on desktop and does not assume a specific GPU vendor.
- **Disc read-ahead:** the ISO reader keeps a 256 KiB aligned window, so a CDVD
  burst costs one host read instead of one per sector. On Android the descriptor
  belongs to the Storage Access Framework, where every read crosses a binder
  boundary; a 300-frame headless run made 533 reads for 14.7 MiB with 75 %
  served from the window (`PS2_DISC_STATS=1`).
- **30 Hz cadence:** `--fps-cap 30` doubles the vblank period (EE clock / 30,
  measured at 9,830,411 cycles per frame) and keeps the 48 kHz SPU2 rate, so
  audio and presentation stay aligned at half rate.
- **Dynamic resolution:** enabled by default on Android. It changes only the
  swapchain render target (0.6x–1.0x) based on GPU/present frame time; the
  internal 512x448 GS framebuffer is untouched, so UI and game geometry remain
  correct. Toggle it in Settings (`PS2_DRS`).
- **Optional Adreno driver:** the launcher can import a rootless AdrenoTools
  driver. It is never loaded on Mali/other vendors and falls back to the system
  driver on failure. Turbo clock is a separate opt-in toggle, default OFF.
- The game runs its original 60 Hz vblank cadence; frame pacing follows the
  audio device when it has queued samples, with a 60/50 Hz timer fallback.
- The internal GS resolution is 512x448. `PS2_SCALE=2` raises it and costs
  memory and GPU time; 1x is the default.
- The renderer is wgpu-native's Vulkan backend. The Android device must expose
  a Vulkan 1.1+ adapter and a working driver. Emulators and devices without a
  suitable driver fall through to the SDL renderer only on desktop, not on
  Android.
- The recompiled EE code and AOT VU1 programs are native arm64. Missing VU
  programs fall back to the interpreter.
- In this build environment the software-GS headless runner rendered through
  frame 3,500 with the AOT VU1 programs in use, and a 420-frame run produced
  7 seconds of non-silent stereo SPU2 output.
- No benchmark result is claimed for Mali-G68/G77 or Adreno 6xx. The target is a
  stable 30 fps at 1x; performance remains unverified without a physical device.
- The desktop runner accepts `--audio-out file.pcm` to write raw stereo
  signed-16-bit PCM for diagnostics. It is not used by the Android app.

## Known limitations

1. **Audio banks.** The retail disc has no plain `.bnk` files in
   `GODOFWAR.TOC`. The 989SND bank images are named `SBP_*` resources in the WAD
   data heap (for example `R_PERMA.WAD` contains `SBP_general` with 295 sounds
   and `SBP_general2` with 21). v2.0 scans loaded WADs, validates the `SBlk`
   bank header, and serves a matching `host1:sound/*.bnk` from the resource
   bytes. Normal SMPD bank loads also use these `SBP_*` resources directly.
   The parser and service are implemented, but the exact runtime request names
   and menu/gameplay bank playback still need a hardware/logcat capture.
2. **Performance.** v2.0 adds ThinLTO, dead-section folding, CPU affinity,
   swapchain dynamic resolution, and optional Adreno driver management. The
   first device run (mid-range phone, 8 GB RAM) was unplayable on v1.0; the
   v1.1/v2.0 numbers from real hardware are still needed. Check `adb logcat -d
   -s GoW2Stats`; if `Vuint` is high, more VU1 programs are missing from the AOT
   capture and the interpreter is the bottleneck.
3. **Dynamic resolution scope.** v2.0 lowers only the Android swapchain target.
   It does not change the internal PS2 GS render scale (the 1x/2x setting is
   separate). This is intentional: swapchain-only DRS keeps the game's
   framebuffer geometry and UI stable while cutting fill-rate cost.
4. **Region/version.** Only the USA DVD9 `SCUS_974.81` image was tested. PAL and
   other revisions are not supported by this kit.
5. **Release signing.** `build-android.ps1` produces a debug-signed APK. For
   Play distribution, add your own signing config and keystore.

## Verification

[`VERIFICATION.md`](VERIFICATION.md) logs the runs behind the status tables:
translation counts, VU capture, the frame-3,500 rendering checkpoint, the audio
capture numbers, clean-kit rebuild, and the APK structure checks. It also lists
what was **not** verified, so nothing here is overstated.

## Audio work (for contributors)

The useful starting point is the WAD format. A GoW2 WAD has a metadata heap,
a `PopHeap` marker, and a data heap. The retail disc has **no `.bnk` files in
`GODOFWAR.TOC`**; the 989SND bank image is a named `SBP_*` resource in the data
heap. Verified examples:

| WAD | Resource | Sounds | Pseudo name |
| --- | --- | ---: | --- |
| `R_PERMA.WAD` | `SBP_general` | 295 | `GEN1` |
| `R_PERMA.WAD` | `SBP_general2` | 21 | `GEN2` |
| `R_SHELLA.WAD` | `SBP_SHELL` | 8 | `TL01` |
| `R_RHSOLD00.WAD` | `SBP_RHSOLD00` | 13 | `RH0` |

Resource layout: `u32 type=1`, `u32 size`, `char name[24]` (`SBP_...`), then
the SBP bank: `u32 magic=0x15`, `u32 soundCount`, `soundCount * 28` sound
entries, a 24-byte bank-info block, and the `SBlk` header/ADPCM data at the
offsets in that block. `runtime/src/iop/io.cpp` scans loaded WADs, validates
the `SBlk` header, matches the request against the resource suffix, pseudo name
or `R_<name>` WAD name, and serves the bytes as the virtual `.bnk` file.
`R_PERMA.WAD` is also tried directly for `general`/`GEN1`/`GEN2` because the
front-end bank may load before that WAD has been opened.

## Troubleshooting

| Symptom | Fix |
| --- | --- |
| `engine.patch does not apply` | Delete `engine\` and rerun `setup.ps1`; do not hand-edit the checkout. |
| Gradle cannot find the SDK | Set `ANDROID_HOME`; verify `platforms\android-35` and the NDK version. |
| CMake version error | Install Android SDK CMake 3.30.5; Gradle pins it in `app/build.gradle`. |
| App exits immediately | Re-select the ISO; the SAF grant may have been revoked. Check `adb logcat -s SDL Gow2 AndroidRuntime`. |
| Black screen or no Vulkan adapter | Update the GPU driver/OS; the device needs a working Vulkan 1.1+ implementation. |
| Missing music or voices | See the audio-bank limitation; check media volume first. |
| Build is very slow | Use `-SkipExplore`; the software GS exploration is CPU-bound. |

## Credits

| Component | Author / project | License |
| --- | --- | --- |
| Recompilation engine, runtime, GS/IOP/SPU2 HLE | [`snuri00/ps2-web-recomp`](https://github.com/snuri00/ps2-web-recomp) (Samir Nuri), pinned at `6526f48` | MIT |
| Window, audio device, gamepads, lifecycle | [SDL3](https://github.com/libsdl-org/SDL) 3.4.18 | zlib |
| Vulkan/WebGPU renderer backend | [wgpu-native](https://github.com/gfx-rs/wgpu-native) 29.0.1.1, built on [`wgpu`](https://github.com/gfx-rs/wgpu) / `naga` | MIT OR Apache-2.0 |
| Optional Adreno driver loading | [libadrenotools](https://github.com/bylaws/libadrenotools) + linkernsbypass, pinned at `8fae8ce` | BSD-2-Clause |
| Android Gradle wrapper | Gradle project | Apache-2.0 |
| Kit scripts, Android glue, docs (`engine.patch` changes) | This repository | MIT |

The `engine.patch` modifications remain under the upstream MIT license; see
[`LICENSE-ENGINE.txt`](LICENSE-ENGINE.txt) and [`NOTICE.md`](NOTICE.md).

## Copyright and legal

*God of War* and *God of War II* are trademarks of Sony Interactive
Entertainment. The game, its characters, music, voices, and other assets are
copyright Sony Interactive Entertainment and Santa Monica Studio. This project
is not affiliated with or endorsed by them, and it ships **no game content** of
any kind.

You must own the disc image you use. The generated C++, the boot ELF, the ISO,
the memory card image, screenshots, captured audio, and any APK you build are
derivative works of the game and **must not** be published, committed, or
shared. This repository deliberately contains none of them.
