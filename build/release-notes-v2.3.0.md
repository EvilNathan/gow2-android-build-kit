# APK 2.3 (debug) - VU1 AOT hotfix, private test build

Sixth build. Same engine and UI as 2.2, plus the first evidence-backed fix for
the v2.0-v2.2 "closes on Play" crash.

## Build details

| Field | Value |
| --- | --- |
| Package | `com.gow2.android` |
| versionCode / versionName | 6 / 2.3 |
| ABI | arm64-v8a only |
| minSdk / targetSdk | 26 / 35 |
| Signing | Android debug key |
| Size | 13,192,938 bytes |
| SHA-256 | `B612E1BA300053041C08358E6C9ECE04384296A5F12D9BEE61C3EF65AFBF577F` |
| Engine patch | `1F047AC3E186136D9CC26AFD52DC18A2FF89A7F7CD59DD016F4F0AE1A0623A05` (113,680 bytes) against `snuri00/ps2-web-recomp` @ `6526f4803d98e10de50bea3af104a4d26e9df033` |
| NDK | 27.3.13750724 |
| 16 KB compatibility | `LOAD` align `0x4000` on all five `.so` files, `extractNativeLibs=true` |

## Why 2.0-2.2 crashed (new evidence)

A binary comparison of the released APKs found the only **game-code** change
between the working v1.x builds and the crashing v2.x builds:

| APK | VU1 AOT programs |
| --- | --- |
| v1.0.0 (works) | 112 |
| v1.1.0 (works) | 112 |
| v2.0.0-v2.2.0 (crash) | 117 |

v2.0 added five programs when it expanded the table to 117:

```
4176D02B93877C36  54781DABBA793231  758DC1364D0BF01B
E344F110E1EBEEDC  EF764AF45D6D7189
```

v1.x did not compile those five; the interpreter ran them, so the same scenes
were safe. A miscompiled AOT program crashes exactly when the game executes it,
which matches "1.0 works, 2.x closes" and why the v2.1 loader/limit fixes did
not help.

## What changed (vs APK 2.2)

- **Five new VU1 programs are skipped by default.** They fall back to the
  interpreter (slower, always correct), restoring the v1.x program set without
  reverting any engine work.
- **Runtime controls.** `runtime/src/vu_micro.cpp` now honours:
  - `PS2_VU_AOT=0` - disable AOT entirely, run every VU1 program on the
    interpreter;
  - `PS2_VU_SKIP=<hash>,<hash>,...` - skip individual 16-digit programs.
- **Settings → VU1 AOT (diagnostic).** Master checkbox plus one checkbox for
  each of the five 2.0 programs. All five are off after a fresh install.
  Enable them one at a time to identify the program that crashes a specific
  device.
- **Logging.** `[app] game start: ... vu_aot=true, vu_skip=...` and the native
  line `[vu] AOT programs registered: 112/117` make the active set visible in
  `/sdcard/gow2/log.txt` and logcat.
- **Version** 6 / 2.3.

## Install

```powershell
adb install -r gow2-debug.apk
```

Play once with the defaults. If the game boots, the five skipped programs were
the cause; enable them one by one in Settings to find the bad one. If it still
closes, send the log:

```powershell
adb pull /sdcard/gow2/log.txt gow2-log.txt
adb logcat -d -b crash > crash.txt
adb shell dumpsys dropbox --print data_app_crash > dropbox.txt
```

The last `[boot]` line identifies the failing stage; `[vu] AOT programs
registered` confirms the filter is active.

## Verification done here

- Native runner rebuilt with the filter and tested in all three modes:
  `117/117` by default, `112/117` with the five hashes in `PS2_VU_SKIP`, and
  the interpreter message with `PS2_VU_AOT=0`.
- Interpreter fallback regression: 300-frame and 3,500-frame headless runs with
  the five skipped completed (17,206,164,887 cycles, exit 0). A 300-frame run
  with all 117 enabled also completed, so the desktop runtime does not reproduce
  the phone crash in early boot.
- `engine.patch` (113,680 bytes) applies cleanly to a pristine engine checkout
  at `6526f48`.
- APK: versionCode 6 / 2.3, arm64-v8a, `dlopen`/`dlclose` remain undefined
  imports (nothing exported by the app), every `LOAD` segment is 16 KB aligned.
- Java settings strings and the native `PS2_VU_SKIP` strings are present in the
  packaged APK.

## Known limitations

- **Not yet confirmed on a phone.** No Android device is attached to the build
  machine; the 112/117 evidence points at the VU table but the exact offending
  program still needs one device test.
- **No raw Vulkan backend.** wgpu remains the renderer; see
  `docs/RENDERER-MIGRATION.md` for the researched options and effort.
- **Interpreter cost.** The five skipped programs are not hot loops in the
  tested scenes, but their scenes run on the interpreter until the codegen is
  fixed.

## Legal

*God of War* and *God of War II* are trademarks of Sony Interactive
Entertainment. This APK contains recompiled game code from the user's own disc
image, is not affiliated with Sony, and must not be redistributed. Keep this
repository private.
