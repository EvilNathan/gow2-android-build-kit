# TASKS

Statuses: `[ ]` todo, `[~]` in progress, `[x]` done, `[!]` blocked.

## M0 — rebase onto PS2Recomp (done 2026-10-26)

- [x] Archive the wgpu engine (tag `v2.3-wgpu`), branch `android-vulkan`.
- [x] Replace the `engine` submodule with `PS2Recomp` (`godofwar2` @ `379fa48`).
- [x] Bring over `config/`, `overrides/`, `tools/`, `.recomp.json`, notes.
- [x] Build `ps2_analyzer` + `ps2_recomp` on Windows.
- [x] Extract boot ELF + IOP modules from the ISO; SHA1 matches
      `E6A62A0F3E781C29513F563F4625B9B3A9103C56`.
- [x] Recompile: 6,609 generated units + `register_functions.cpp`.

## M1 — Android arm64 boot (in progress)

- [x] Verify sse2neon/ARM path; add `compat/arm64` shims.
- [x] `patches/0001-read-iso-image.patch` (disc image via `GOW2_ISO` or sibling
      `gow2.iso`).
- [x] `patches/0002-null-gs-backend.patch` (`PS2X_FORCE_NULL_GS`).
- [x] Gradle/NativeActivity host `android-runner/` (arm64-v8a, 16 KB pages,
      boot ELF path baked from the application id).
- [x] Gradle 8.9 wrapper + AGP 8.6.1 configuration succeeds.
- [x] `assembleDebug` native build (19m45s on 4 cores; 326 objects, warnings only).
- [ ] Install APK, push ELF + ISO, boot on device (no adb device attached yet).
- [ ] Capture ≥300 frames of logcat with no fatal signal; record evidence in
      `VERIFICATION.md`.
- [ ] Measure interpreter speed (expected ~3 FPS without native VU1/IOP).

### Known constraints

- VU1 native code generation is disabled on Android by the engine CMake
  (`PS2X_IS_ANDROID`); M1 uses the interpreter.
- No renderer yet: `PS2X_FORCE_NULL_GS=ON` in `android-runner/gradle.properties`.
- Only 4 build cores on the current machine; generated code dominates build
  time.

## M2 — raw Vulkan GS backend

- [ ] Implement `GSVkBackend : GSRasterBackend` (device/swapchain, VRAM
      textures, draw batches, CLUT, transfers, present).
- [ ] Host lifecycle with `ANativeWindow` (no raylib GL dependency).
- [ ] First pixels from the game; compare against PC OpenGL output.

## M3 — playability

- [ ] Touch/gamepad input; audio device; suspend/resume.
- [ ] VU1/IOP capture → native generators (needs a PC capture pass).
- [ ] Performance pass, device matrix, 16 KB + Android 15 validation.
