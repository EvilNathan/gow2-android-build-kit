# TASKS

Statuses: `[ ]` todo, `[~]` in progress, `[x]` done, `[!]` blocked.

_Last update: 2026-11-11 (M3 device phase: the game boots, plays the opening
movie and renders the first area on real hardware — Redfinger cloud phones,
Mali-G77 then Adreno in a Galaxy S25+. Native VU1/IOP are generated and
linked, GS composition is pixel-correct on screen, and the guest mode
machine is mapped: post-movie the game waits in mode 11 and the sequence
SELECT → X → START reaches mode 13. Remaining gate: the character does not
yet move under stick input in mode 13. The working tree carries TEMP-DIAG
instrumentation pending cleanup — see "Known issues".)_

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
- [x] Install APK, import ISO, boot on device (2026-11-09, Redfinger cloud
      phone via Tailscale + ADB; ISO SHA-256 verified against the phone's own
      hash — see `VERIFICATION.md`).
- [x] Capture long device runs with no fatal signal (soaks of 19+ min);
      evidence in `VERIFICATION.md`.

## M2 — raw Vulkan GS backend

- [x] Kit-side integration contract: `patches/0003-gs-vulkan-select.patch`
      (`GSThreadedBackend(std::make_unique<GSVkBackend>())` under
      `PS2X_ANDROID_VULKAN_GS`).
- [x] Build plumbing: `android-runner/native/CMakeLists.txt` (Vulkan sources,
      NDK `glslc` shader build, `cmake/embed_spirv.cmake` embedding,
      `PS2X_ANDROID_VULKAN_GS`), contract in
      `android-runner/native/gs_vk/README.md`.
- [x] **`GSVkBackend : GSRasterBackend` written (2026-10-31).**
      `gs_vk_backend.{h,cpp}`: dlopen Vulkan loader (`gs_vk_loader.*`, no link
      dependency), device + swapchain from `ps2x::host::NativeWindow()`,
      render targets keyed by FRAME.FBP with depth, VP-broadcast single
      pipeline family, format tables + inverse tables in a device texture,
      GSTextureStore-backed decodeDirect textures + index/palette textures,
      CLUT-from-render-target rebuilt on the CPU through a target readback,
      reinterpreting fetch (address math on the GPU in `shaders/draw.frag`,
      ported from the desktop renderer), channel shuffles, destination alpha
      test, Z aliasing/inversion, AFAIL two-pass alpha fail, fixed-function +
      software blend split, sprite snapping and seam clamps, clear via a
      colour-only load-op pass, present via circuit composition + blit to the
      swapchain (readback always produced for `latchHostPresentationFrame`).
- [x] Shaders: `gs_vk/shaders/{draw,shuffle,present}.{vert,frag}` compile with
      the NDK `glslc` (`compile_shaders.cmake`) and embed via
      `cmake/embed_spirv.cmake` (`gs_vk_spv::Get`). UBOs are std140 blocks
      matched by static_asserts in the backend.
- [x] `gs_probe` offscreen tests + harness (`tests/gs_probe/gs_probe.cpp`,
      `scripts/build-gs-probe.ps1`): self-provisioning (NDK Vulkan headers,
      glslc, SPIR-V embed) and runs the backend against the SwiftShader ICD.
      Case clear/sprite/transfer drive init, render-target creation, pipelines
      and draw dispatch. Pixel-exact expectations do **not** run on this CI VM:
      both available SwiftShader builds trap (`ud2`) inside their clear/present
      under `REACTOR_JIT=0`; reported in `VERIFICATION.md`, validation moves to
      a real device.
- [x] APK with the Vulkan GS links end to end:
      `gradlew assembleDebug -Pps2xNullGs=OFF` → **BUILD SUCCESSFUL** (arm64
      compile of gs_vk + engine, 24 min full), `verify-android.ps1` all pass,
      79 gs_vk symbols present in `libps2EntryRunner.so`, no `libvulkan`
      DT_NEEDED (dlopen).
- [x] Host lifecycle with `ANativeWindow` exercised on device (two phones:
      Mali-G77, Adreno). GPU wedge root-caused (mid-render-pass flush in
      `drawCircuit`) and fixed structurally; swapchain prefers MAILBOX with
      a deeper image count (acquire time ~2,600 ms → ~10 ms per 5 s window).
- [x] First pixels from the game on device, correct colours and composition
      (2026-11-11): the corruption was in the display copy path — fixed by
      the channel-shuffle gate relaxations and the CT32/CT24 direct texture
      path in `draw()`; rotation on the portrait-native panel handled by
      shader pre-rotation (present mode 1).
- [ ] `tests/gs_units` (pure-CPU store/swizzle tests, no Vulkan) — directories
      are empty; the probe covers device paths instead.

## M3 — playability

- [x] Device bring-up end to end: build → sign (stable local keystore) →
      install → boot → log/screenshot loop fully automated from the Linux VM
      over Tailscale + ADB to the cloud phone.
- [x] VU1/IOP capture → native generators: pure-Python generators
      (`tools/vu1/vu1n_gen.py`, `tools/iop/iopn_gen.py`); live-capture loop on
      device grew VU1 coverage to **2,082 native blocks / 67 images** (the
      post-movie microprogram overlays its own code region between frames, so
      static capture alone was not enough). VU1 ≈254 → ~24 ns/cycle; IOP 8/8
      modules native.
- [x] Opening movie plays to completion on device (deterministic signature:
      3,759 frames displayed, audio packets end at 0x699), then the first
      playable area renders with correct composition.
- [x] Scripted pad input (`padScript` in settings) verified at every layer:
      Java → JNI → host pad state → guest pad struct, buttons and analog
      floats alike.
- [x] **Mode machine mapped** (guest global `[0x2D8EC0]`, evaluator
      `FUN_0025AD58` from the per-frame checker, jump-table dispatch over
      modes 0–20): boot 0 → title 3 → 4 → **11** (post-movie wait) →
      **SELECT** (press latches `[0x2D8F70]`, release fires) → 12 →
      **X** → 11 → **START** → **13**. In mode 13 the character branch
      `FUN_001E9310` (gated on mode == 13 inside `FUN_001E6000`) runs
      continuously (~7,900 calls in the verification run).
- [!] **Character locomotion in mode 13**: with the stick held for minutes in
      mode 13 the rendered scene is a fixed point and the player object's
      first 256 bytes show no position change (only state fields +0x08
      0→2 and +0x14 188→167 across the 11→13 transition). Pad delivery is
      proven, so the gate is inside/beyond the mode-13 update — next step is
      the action-block census (`+0x1F00..+0x1FFF` of the player object, where
      `FUN_001E9310` and `padConsA` work) across stick directions.
- [ ] Performance pass: `Present()` still serializes on a single fence
      (~2 s per 5 s window in the heavy phases; guest ~8 flips/s there vs
      ~88/s in the light modes). Fix candidate: double-buffered frame
      resources. Device matrix + 16 KB / Android 15 validation after that.
- [~] Touch overlay input reaches the guest pad state in tests only via the
      script path so far; a manual on-screen play session is still pending
      (blocked on locomotion).

## Open TODOs (ordered, for the next session)

1. **Mode-13 locomotion gate** (the playable blocker): census the player
   object's action block `+0x1F00..+0x1FFF` plus the pad analog floats across
   stick directions in mode 13 (diag119 build already prints them in the
   watch thread); read the mode-13 update body (`FUN_0025D370`) and
   `FUN_001E9310` for the field it waits on; then a causal force-test in the
   style of the earlier `force394` facility — checking the *first* gate in
   the chain this time.
2. **Present-path performance**: double-buffer frame resources (2 command
   buffers / fences / rings / descriptor pools; wait on the previous frame's
   fence) so the heavy phases are not serialized by `Present()`.
3. **TEMP-DIAG cleanup** (once playable): remove the probe/watch/dump
   instrumentation from `overrides/gow2_overrides.cpp`,
   `android-runner/native/gs_vk/gs_vk_backend.cpp` and
   `android-runner/native/host/jni_bridge.cpp`; restore the 10 MB log cap in
   `host/log.cpp`; hard-code rotation mode 1 (condition: surface transform
   == ROTATE_90) and delete the `present_mode.txt` experiment; keep the
   permanent fixes (circuit flush guard, MAILBOX swapchain, shuffle gate
   relaxations, CT32 present path, watch null-RDRAM guard + loader-poll
   spawn-once fix if those facilities survive in any form).
4. **Crash-handler robustness**: clamp the LogRingDump copy in
   `host/crash.cpp` — the handler itself aborted under fortify while writing
   two earlier crash reports, hiding the original faults. The `dladdr`
   frame annotation added there is worth keeping permanently.
5. **Playable verification + M3 release**: full run — boot → title → movie →
   SELECT/X/START sequence → character moves → 10 min stable — then publish
   `v0.5.0-m3` to `EvilNathan/gow2-android-apk` and make the sequence part of
   the shipped default `padScript` (title taps + post-movie SELECT ≈t400,
   X ≈t500, START ≈t740 in wall-clock script seconds at current speeds).

## Known issues (kendala)

- [!] **Locomotion gate in mode 13** — see M3 above. Everything upstream is
      proven: pad bits/floats reach the guest, the mode machine advances,
      the mode-13 branch runs. The scene stays a fixed point under stick
      input.
- The kit's crash handler can itself abort while dumping the log ring
  (fortify `__memcpy_chk`); two historical crashes (2026-11-11) lost their
  original fault this way. Fix scheduled in TODO 4.
- The on-device session log (`/sdcard/gow2/logs/gow2.log`) is append-only and
  the TEMP-DIAG build caps it at 80 MB; delete it before every launch or
  later samples silently read a capped, stale log.
- AGP's debug certificate is not stable across build environments, so test
  APKs are re-signed with a local stable keystore before `pm install -r`
  (otherwise `INSTALL_FAILED_UPDATE_INCOMPATIBLE`); after a reinstall the
  all-files-access appop must be re-granted.
- Shaders are compiled and embedded at CMake *configure* time;
  `android-runner/native/CMakeLists.txt` now declares
  `CMAKE_CONFIGURE_DEPENDS` on the shader sources — without it, shader edits
  silently never reach the APK.
- VM restarts interrupt long builds; recovery is simply re-running
  `scripts/build-android.sh` after checking the `dist/` APK timestamp.

## Known constraints

- VU1 native generation is disabled *by the engine CMake* on Android
  (`PS2X_IS_ANDROID`); the kit path is host-side codegen plus
  `PS2X_ANDROID_NATIVE_VU1_DIR` (registration is by static initialisers, so no
  engine patch is needed). VU1 parts must be built as a separate OBJECT
  library (each part defines the same registration symbol, so they cannot
  join the unity build).
- Device automation *does* exist on the Linux VM used for the 2026-11
  sessions: the Redfinger cloud phone joins the tailnet and is driven over
  Tailscale + wireless ADB (install via `adb push` + `pm install -r`;
  direct `adb install` is unreliable over the tunnel). The Windows
  workstation still has no attached device.
- The Linux VM's `/tmp` is small (512 MB): Gradle/Android builds must run
  with `TMPDIR` and `-Djava.io.tmpdir` pointed at a home-disk directory or
  they fail with disk-full / shared-memory errors.
- Incremental rebuilds on the Linux VM take ~2–3.5 min (host + overrides);
  the first full build took ~47 min.
- Heavy outputs (`build/`, `generated/`, `games/`, Gradle caches) live on `D:`
  through junctions because `C:` has ~14 GB free.
