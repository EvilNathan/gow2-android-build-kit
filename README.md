# God of War II — native Android build kit

This kit turns **your own** copy of *God of War II* (USA, `SCUS_974.81`) into an
Android arm64 app. It is a **static-recompilation** port, not an emulator
frontend: the game's Emotion Engine code is translated ahead of time to C++,
VU1 microcode is compiled AOT, and the Graphics Synthesizer is implemented by a
native WebGPU/Vulkan renderer through `wgpu-native`. SDL3 provides the Android
window, audio device, gamepads, and lifecycle handling.

The kit contains **no game code, ELF, ISO, generated C++, or APK**. You must
supply the disc image. Everything derived from it stays on your machine.

> **Status:** the app builds and boots the USA DVD9 image to the title,
> difficulty menu, intro cutscenes, and the opening Loom Chamber sequence with
> 3D characters, textures, and animation. The APK has not been benchmarked on a
> physical Mali-G68/G77 device in this build environment. The SPU2 audio path
> produces output, but 989SND bank loading is incomplete (see *Known
> limitations*). Read the notes before judging performance.

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

Put the ISO anywhere outside this folder (the path may contain spaces), then
from the kit root:

```powershell
Set-ExecutionPolicy -Scope Process Bypass
.\scripts\build-all.ps1 -Iso "D:\Games\God of War II (USA).iso"
```

The first run downloads the pinned engine and two dependencies, builds the
recompilers, extracts the boot files, translates the game, records VU1 programs,
and builds a signed debug APK. Expect roughly 20-90 minutes depending on CPU
and the exploration length. The exploration run is CPU-heavy because this
build environment has no Vulkan device; on a PC with a supported GPU the
native runner is much faster.

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
.\scripts\recompile.ps1 -Iso "...\God of War II (USA).iso" -Frames 2400
.\scripts\build-android.ps1         # dist\gow2-debug.apk
```

`recompile.ps1` accepts `-SkipExplore` for a fast build without VU recording.
Missing VU programs are interpreted at runtime; the game is correct but slower.
Increase `-Frames` to capture more VU programs and more of the intro.

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

- **Widescreen (16:9)** — passes `--widescreen`; default is the game's 4:3.
- **Render scale 1x / 2x** — sets `PS2_SCALE` for the internal GS buffers.
  1x is strongly recommended on Mali-G68/G77. Changes apply at the next start.
- **Dim touch controls** — lowers the overlay opacity.

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

The Android app is built for arm64 only. `libmain.so` is linked with
`-Wl,-z,max-page-size=16384` and `-Wl,-z,common-page-size=16384`, and Gradle
extracts native libraries at install time (`useLegacyPackaging = true`). This
keeps the app compatible with 16 KB-page Android 15+ devices without requiring
the APK's compressed ZIP entries to be 16 KB aligned.

## Performance notes

- The game runs its original 60 Hz vblank cadence; frame pacing follows the
  audio device when it has queued samples, with a 60/50 Hz timer fallback.
- The internal GS resolution is 512×448. `PS2_SCALE=2` raises it and costs
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
- No benchmark result is claimed for Mali-G68/G77. The target is a stable
  30 fps at 1x; if the device cannot reach 60 Hz, the intended configuration is
  1x with background apps closed. Performance remains unverified without a
  physical device.
- The desktop runner accepts `--audio-out file.pcm` to write raw stereo
  signed-16-bit PCM for diagnostics. It is not used by the Android app.

## Known limitations

1. **Audio banks.** The game's Sony `989NOMID` sound driver asks for
   `host1:sound/general.bnk`, `general2.bnk`, `SHELL.bnk`, `RHOD10.bnk`,
   `RHSOLD00.bnk`, `COLOS00.bnk`, and level banks. Those names are not plain
   ISO9660/TOC files on the retail disc; the equivalent sample data is embedded
   in the WAD `SNDX_*` groups. The current `host1:` service exposes WAD/VAG TOC
   entries but does not synthesize these BNK files. The SPU2 path still runs and
   was verified to produce non-silent stereo output during the intro (audio from
   WAD sample groups), but bank-driven music, voice, and some effects can be
   missing. This is the main compatibility item left.
2. **Performance validation.** There was no physical Android device or local
   Vulkan adapter in the build environment. The APK's runtime behavior and Mali
   performance are unverified.
3. **Dynamic resolution.** The app exposes fixed 1x/2x scaling. It does not
   change scale dynamically based on frame time.
4. **Region/version.** Only the USA DVD9 `SCUS_974.81` image was tested. PAL and
   other revisions are not supported by this kit.
5. **Release signing.** `build-android.ps1` produces a debug-signed APK. For
   Play distribution, add your own signing config and keystore.

## Audio work (for contributors)

The useful starting point is the WAD format. A GoW2 WAD has a metadata heap,
a `PopHeap` marker, and a data heap. `SNDX_*` groups in the metadata heap carry
a small header; their sample data lives in the data heap (for example the
`SNDX_default` group in `R_SHELLA.WAD` points at raw sample bytes). The 989SND
bank loader expects a BNK container, so serving `sound/*.bnk` requires either
converting the SNDX/group data into the bank format or intercepting the loader
at a higher level. The current `runtime/src/iop/io.cpp` already knows how to
resolve `host1:` WAD/VAG entries through `GODOFWAR.TOC` and is the place to add
that mapping.

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

## Legal

*God of War* and *God of War II* are trademarks of Sony Interactive
Entertainment. This project is not affiliated with or endorsed by Sony,
Santa Monica Studio, or the upstream engine authors.

You must own the disc image you use. The generated C++, the boot ELF, the ISO,
the memory card image, and the APK are derivative works of the game and **must
not** be published, committed, or shared. This kit deliberately contains none
of them. The engine and dependency licenses are in `LICENSE-ENGINE.txt` and
`NOTICE.md`.
