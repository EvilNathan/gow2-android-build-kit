# Verification log

This file records what was actually tested while the kit was assembled. It is
not a guarantee for your machine, but it tells you which parts have been run
end to end and which parts have not.

Date: 2026-10-21. Host: Windows 11, LLVM/Clang, Ninja, Python 3, JDK 17,
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
- **APK structure.** The APK contains a single native library
  (`lib/arm64-v8a/libmain.so`), exports `SDL_main` and
  `Java_com_gow2_android_TouchOverlayView_nativeSetPad`, has ELF `LOAD`
  alignment `0x4000` (16 KB compatible), sets `extractNativeLibs=true`, is
  debug-signed, and does not package `libwgpu_native.so`.

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

## Not verified

- **Performance update on hardware.** The v1.1 APK has not run on the
  reporter's phone yet. The expected workflow is `adb logcat -d -s
  GoW2Stats` during play, after which the present-mode/quality knobs can be
  adjusted against real numbers.
- **Dynamic resolution.** Only fixed 1x/2x render scaling exists.
- **Full VU exploration in the clean kit.** The clean-kit workflow was
  validated with `-SkipExplore`; the 78-program VU capture came from the
  assembly workspace. A fresh full run records its own programs on your
  machine.
- **Complete audio.** Some `sound/*.bnk` requests are still unserved, so
  bank-driven music, voice, or effects may be missing. The WAD-backed audio
  path works.
- **Release signing.** The kit builds a debug-signed APK.
