# TASKS

Statuses: `[ ]` todo, `[~]` in progress, `[x]` done, `[!]` blocked.

_Last update: 2026-10-28 (session pause — everything below was committed and
pushed; see the "Open TODOs" section for what is left)._

## M0 — rebase onto PS2Recomp (done 2026-10-26)

- [x] Archive the wgpu engine (tag `v2.3-wgpu`), branch `android-vulkan`.
- [x] Replace the `engine` submodule with `PS2Recomp` (`godofwar2` @ `379fa48`).
- [x] Bring over `config/`, `overrides/`, `tools/`, `.recomp.json`, notes.
- [x] Build `ps2_analyzer` + `ps2_recomp` on Windows.
- [x] Extract boot ELF + IOP modules from the ISO; SHA1 matches
      `E6A62A0F3E781C29513F563F4625B9B3A9103C56`.
- [x] Recompile: 6,609 generated units + `register_functions.cpp`
      (re-run 2026-10-28, 6,612 files synced into the runner).
- [x] `patches/0001-read-iso-image.patch` (disc image via `GOW2_ISO` or sibling
      `gow2.iso`).
- [x] `patches/0002-null-gs-backend.patch` (`PS2X_FORCE_NULL_GS`).

## M1 — Android arm64 boot (in progress)

- [x] Verify sse2neon/ARM path; add `compat/arm64` shims.
- [x] Gradle/NativeActivity host `android-runner/` (arm64-v8a, 16 KB pages).
- [x] Gradle 8.9 wrapper + AGP 8.6.1 configuration succeeds.
- [x] `assembleDebug` native build (19m45s on 4 cores; 326 objects, warnings only).
- [x] APK packaging check: `lib/arm64-v8a/libps2EntryRunner.so` only,
      16 KB-aligned LOAD segments (see `VERIFICATION.md`, G2).
- [x] Engine host patch `patches/0004-android-host.patch`: window, audio,
      present pacing, exit, pad and the `ps2x_android_game_main()` entry point.
- [x] Java app + JNI host layer (`android-runner/native/host/**`,
      `android-runner/app/src/main/java/com/gow2/android/**`):
      `ANativeWindow` hand-off, AAudio output, touch overlay, gamepad mapper,
      settings store, crash/log capture, ISO import UI.
- [x] In-app ISO9660 reader + SHA1 boot-ELF extractor
      (`android-runner/native/iso/**`, validated against the real 7.95 GB ISO).
- [ ] Java entry activity is still the old `NativeActivity` manifest: the app
      must launch `MainActivity`/`GameActivity` (`android:hasCode="false"` and
      `android.app.NativeActivity` are still in
      `android-runner/app/src/main/AndroidManifest.xml`).
- [ ] Wire `android-runner/app/build.gradle` to the new `native/CMakeLists.txt`
      switches (`-DPS2X_GS_VK=`, `-DPS2X_ANDROID_NATIVE_VU1_DIR=`,
      `-Dps2xNullGs=`) and bump `versionName` for the final APK.
- [ ] `scripts/verify-android.ps1` (static APK/symbol checks) — not written yet.
- [ ] Build the APK with the new host layer and the engine patches applied
      (only the M1 null-GS APK has been built so far).
- [ ] Install APK, push ELF + ISO, boot on device (no adb device attached).
- [ ] Capture ≥300 frames of logcat with no fatal signal; record evidence in
      `VERIFICATION.md`.

## M2 — raw Vulkan GS backend

- [x] Kit-side integration contract: `patches/0003-gs-vulkan-select.patch`
      (`GSThreadedBackend(std::make_unique<GSVkBackend>())` under
      `PS2X_ANDROID_VULKAN_GS`).
- [x] Build plumbing: `android-runner/native/CMakeLists.txt` (Vulkan sources,
      NDK `glslc` shader build, `cmake/embed_spirv.cmake` embedding,
      `PS2X_ANDROID_VULKAN_GS`), contract in
      `android-runner/native/gs_vk/README.md`.
- [ ] **`GSVkBackend : GSRasterBackend` itself — not written.**
      `android-runner/native/gs_vk/` contains only the README. This is the
      single largest remaining piece (device/swapchain, VRAM textures, CLUT,
      blend mapping, transfers, present).
- [ ] Host lifecycle with `ANativeWindow` (no raylib GL dependency).
- [ ] First pixels from the game; compare against PC OpenGL output.
- [ ] `tests/gs_units` + `tests/gs_probe` (SwiftShader ICD) — directories are
      empty.

## M3 — playability

- [~] Touch/gamepad input and audio are implemented in the host layer but have
      never run on a device.
- [ ] VU1/IOP capture → native generators (needs a PC capture pass: boot the
      desktop runner to the menu/first area with `GOW_VU1_CAPTURA` and the IOP
      dump env vars).
- [ ] `scripts/gen-native.ps1` exists but has not been run; the generated parts
      must be fed to the Android build through `-DPS2X_ANDROID_NATIVE_VU1_DIR`.
- [ ] Performance pass, device matrix, 16 KB + Android 15 validation.

## Open TODOs (ordered, for the next session)

1. **Vulkan GS backend** (`android-runner/native/gs_vk/**`): implement
   `GSVkBackend`, `compile_shaders.cmake`, `sources.cmake`, plus
   `tests/gs_units` and `tests/gs_probe`; verify the probe offscreen with the
   SwiftShader ICD (`C:\Program Files\Unity Hub\vk_swiftshader_icd.json`).
2. **Prove the patched engine still compiles on desktop** (`scripts/run-headless.ps1
   -Build`): patches 0003/0004 only add `#if defined(...)` branches, but they
   were committed without a desktop compile.
3. **Prove patches 0003/0004 apply from a pristine submodule** and make
   `scripts/_common.ps1` set `git -C PS2Recomp config core.autocrlf false` before
   `git apply` (the working copy is LF; a CRLF checkout can make the patches
   fail).
4. **Android app wiring** (items left in M1): manifest → `MainActivity`,
   `build.gradle` CMake arguments + versionName 1.0.0, `MainActivity.java`
   is still missing (only `GameActivity`, `DiscInstaller`, `NativeBridge`,
   `PadMapper`, `Settings`, `SettingsStore`, `TouchControlsView` exist).
5. **Compile the host layer for Android** (javac typecheck of the Java sources +
   `clang++ -fsyntax-only` of `native/host/*.cpp`) and fix whatever the earlier
   `-fsyntax-only` pass missed; then build the APK end to end.
6. **PC capture pass** for native VU1/IOP (`GOW_VU1_CAPTURA`, IOP module dump),
   then `scripts/gen-native.ps1`, then measure the FPS gain on the desktop
   runner before trusting it on the phone.
7. **Device bring-up** (user-driven): install, import ISO, logcat, screenshot,
   `selftest-*.zip`; tune the ~20 FPS menu / ~10 FPS first-area targets.
8. **Docs**: `README.md` build instructions, `.recomp.json` still says
   Windows/PC target, `docs/` Android install guide, `scripts/verify-android.ps1`.

## Known constraints

- VU1 native generation is disabled *by the engine CMake* on Android
  (`PS2X_IS_ANDROID`); the kit path is host-side codegen plus
  `PS2X_ANDROID_NATIVE_VU1_DIR` (registration is by static initialisers, so no
  engine patch is needed).
- No adb/device automation on this machine: device verification is manual.
- 4 build cores; a full Android native build is ~20 min and the generated code
  dominates it.
- Heavy outputs (`build/`, `generated/`, `games/`, Gradle caches) live on `D:`
  through junctions because `C:` has ~14 GB free.
