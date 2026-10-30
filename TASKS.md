# TASKS

Statuses: `[ ]` todo, `[~]` in progress, `[x]` done, `[!]` blocked.

_Last update: 2026-10-30 (M1 closed on the build side: the patched engine builds
on Windows, the host layer compiles for arm64 and the APK passes the static
checks. Device bring-up and the Vulkan GS backend remain)._

## M0 — rebase onto PS2Recomp (done 2026-10-26)

- [x] Archive the wgpu engine (tag `v2.3-wgpu`), branch `android-vulkan`.
- [x] Replace the `engine` submodule with `PS2Recomp` (`godofwar2` @ `379fa48`).
- [x] Bring over `config/`, `overrides/`, `tools/`, `.recomp.json`, notes.
- [x] Build `ps2_analyzer` + `ps2_recomp` on Windows.
- [x] Extract boot ELF + IOP modules from the ISO; SHA1 matches
      `E6A62A0F3E781C29513F563F4625B9B3A9103C56`.
- [x] Recompile: 6,609 generated units + `register_functions.cpp`
      (re-run 2026-10-30, 6,610 files synced into the runner).
- [x] `patches/0001-read-iso-image.patch` (disc image via `GOW2_ISO` or sibling
      `gow2.iso`).
- [x] `patches/0002-null-gs-backend.patch` (`PS2X_FORCE_NULL_GS`).

## M1 — Android arm64 boot (build side done 2026-10-30)

- [x] Verify sse2neon/ARM path; add `compat/arm64` shims.
- [x] Gradle/NativeActivity host `android-runner/` (arm64-v8a, 16 KB pages).
- [x] Gradle 8.9 wrapper + AGP 8.6.1 configuration succeeds.
- [x] Engine host patch `patches/0004-android-host.patch`: window, audio,
      present pacing, exit, pad and the `ps2x_android_game_main()` entry point.
- [x] Java app + JNI host layer (`android-runner/native/host/**`,
      `android-runner/app/src/main/java/com/gow2/android/**`):
      `ANativeWindow` hand-off, AAudio output, touch overlay, gamepad mapper,
      settings store, crash/log capture, ISO import UI.
- [x] In-app ISO9660 reader + SHA1 boot-ELF extractor
      (`android-runner/native/iso/**`, validated against the real 7.95 GB ISO).
- [x] Launcher activity: `MainActivity` (ISO import via SAF, boot-ELF
      extraction, settings dialog) replaces `android.app.NativeActivity`;
      `GameActivity` hosts the SurfaceView and the native session.
- [x] `android:hasCode="false"` removed; manifest declares `MainActivity`
      (exported, launcher) and `GameActivity` (not exported).
- [x] `android-runner/app/build.gradle` wired to the native switches
      (`PS2X_GS_VK`, `PS2X_ANDROID_NATIVE_VU1_DIR`, `PS2X_FORCE_NULL_GS`),
      `versionName 1.0.0`.
- [x] `scripts/verify-android.ps1`: static APK checks (package, launcher, ABI,
      minSdk, 16 KB LOAD alignment, AArch64).
- [x] Host layer compiles for arm64 and the APK links
      (unity-build symbol clashes fixed: `kPathLen`, weak
      `ps2x_android_game_main`, `ps2x_host.h` include for `ps2_runtime`,
      `SetSdkInt`/`GetSdkInt` defined).
- [x] Build the APK end to end with the host layer and the engine patches:
      `dist/gow2-android-debug.apk` (56 MB, null GS).
- [ ] Install APK, import ISO, boot on device (no adb/device attached here).
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
2. **Device bring-up** (user-driven): install, import ISO, logcat, screenshot,
   `selftest-*.zip`; tune the ~20 FPS menu / ~10 FPS first-area targets. The
   launcher activity is `com.gow2.android/.MainActivity`; place the ISO via the
   in-app import or `scripts/push-game-data.ps1`.
3. **PC capture pass** for native VU1/IOP (`GOW_VU1_CAPTURA`, IOP module dump),
   then `scripts/gen-native.ps1`, then measure the FPS gain on the desktop
   runner before trusting it on the phone.
4. **Desktop runner run** (`scripts/run-headless.ps1`) is still unverified on
   this machine; only the `ps2_runtime` target has been compiled here (no GPU
   session). The patched engine compiles on MSVC.

Done in the 2026-10-30 session: day-one patch stack rebased so `0001`–`0004`
apply from a pristine `PS2Recomp` checkout (`+183/−8`), LF checkout forced in
`scripts/_common.ps1`, app/manifest/Gradle wiring, `verify-android.ps1`,
`docs/ANDROID.md`, and the first full APK build with the host layer.

## Known constraints

- VU1 native generation is disabled *by the engine CMake* on Android
  (`PS2X_IS_ANDROID`); the kit path is host-side codegen plus
  `PS2X_ANDROID_NATIVE_VU1_DIR` (registration is by static initialisers, so no
  engine patch is needed).
- No adb/device automation on this machine: device verification is manual.
- 4 build cores; a full Android native build is ~20 min and the generated code
  dominates it. An incremental host-only rebuild is ~30 s.
- Heavy outputs (`build/`, `generated/`, `games/`, Gradle caches) live on `D:`
  through junctions because `C:` has ~14 GB free.
