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

## Not verified

- **Physical Android device.** No phone or emulator with a Vulkan driver was
  available here, so the APK has never been launched. Mali-G68/G77 frame rate,
  thermal behavior, touch latency, and audio latency are unknown.
- **Dynamic resolution.** Only fixed 1x/2x render scaling exists.
- **Full VU exploration in the clean kit.** The clean-kit workflow was
  validated with `-SkipExplore`; the 78-program VU capture came from the
  assembly workspace. A fresh full run records its own programs on your
  machine.
- **Complete audio.** Some `sound/*.bnk` requests are still unserved, so
  bank-driven music, voice, or effects may be missing. The WAD-backed audio
  path works.
- **Release signing.** The kit builds a debug-signed APK.
