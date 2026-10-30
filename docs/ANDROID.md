# Android build and install guide

The kit turns your own *God of War II* (USA, `SCUS_974.81` v1.01) into a native
arm64 app. Nothing derived from the disc is committed; every step below runs on
your machine.

## 1. Prerequisites

| Tool | Version | Notes |
| --- | --- | --- |
| Windows 10/11 x64 | — | desktop tools (analyzer/recompiler) are MSVC-only |
| Visual Studio | 2022 or 2026, C++ workload | `build-tools.ps1` and the desktop runner |
| CMake | 3.24+ | the Android build uses the SDK's 3.31.5 instead |
| Git, Python | 3.10+ | ISO/ELF helpers |
| JDK | 17 | AGP 8.6.1 |
| Android SDK | platform 35, build-tools 34+ | `ANDROID_HOME` |
| Android NDK | `27.3.13750724` | pinned by `android-runner/app/build.gradle` |
| Device | arm64, Android 9+ (API 28) | 16 KB page compatible |

## 2. Build everything

```powershell
.\scripts\setup.ps1                      # engine submodule + kit patches
.\scripts\build-tools.ps1                # ps2_analyzer, ps2_recomp
.\scripts\extract-disc.ps1 -Iso "D:\Games\God of War II (USA).iso"
.\scripts\recompile.ps1                  # generated/gow2 -> runner sources
.\scripts\build-android.ps1              # dist/gow2-android-debug.apk
.\scripts\verify-android.ps1             # static APK checks
```

`build-all.ps1 -Iso <iso>` chains the same steps. The Android build is ~20 min
on four cores; the generated units dominate.

`setup.ps1`/`build-android.ps1` re-apply `patches/*.patch` on top of the pinned
engine revision, discarding tracked engine edits first (the patches are the
source of truth). Untracked outputs under `PS2Recomp/ps2xRuntime/src/runner/`
are kept.

## 3. Get the data onto the device

The app can do it by itself — this is the recommended path:

1. Install the APK (`build-android.ps1 -Install`, or `adb install -r`).
2. Open **God of War II**.
3. **Impor ISO** → pick your ISO with the system file picker. The image is
   copied to the app files dir as `gow2.iso` (needs ~8 GB free).
4. The launcher extracts `SCUS_974.81` from the image (ISO9660 + SHA1 check) and
   starts the game.

The scripted alternative pushes the same files over adb:

```powershell
.\scripts\push-game-data.ps1             # ELF + ISO to /sdcard/Android/data/com.gow2.android/files
.\scripts\run-headless.ps1 -Android -Seconds 60
```

## 4. Logs and diagnostics

- In-app: three-finger tap toggles the HUD (frame time, host stats).
- `adb logcat -s ps2x:V` follows the host/engine log.
- `NativeBridge.nativeLogPath()` (`adb shell run-as` not needed: the file lives
  in the app files dir) points at the session log.
- The native crash handler writes a report next to that log on SIGSEGV/SIGABRT.

## 5. Current limitations

- M1 ships the **no-op GS**: the CPU/IOP core boots, but no pixels are drawn
  until `android-runner/native/gs_vk/` lands (`TASKS.md`, M2).
- No save-state/memory-card UI yet; the engine's regular memory card path
  applies.
- Only `arm64-v8a`; 32-bit and x86 devices are rejected by the APK.
