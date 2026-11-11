# VERIFICATION

Evidence for the milestones in `TASKS.md`. Commands are run from the kit root.

## M0 — toolchain and translation

| Check | Command | Result |
| --- | --- | --- |
| Engine pin | `git -C PS2Recomp rev-parse HEAD` | `379fa48b1a292eb7f688ccdc202d97f5ffc09c6d` |
| Kit patches apply | `.\scripts\setup.ps1` | `0001`–`0004` applied in order (5 files, +183/−8) |
| Tools build | `.\scripts\build-tools.ps1` | `ps2_analyzer.exe`, `ps2_recomp.exe` produced |
| Boot ELF SHA1 | `Get-FileHash games\gow2\disc\SCUS_974.81 -Algorithm SHA1` | `E6A62A0F3E781C29513F563F4625B9B3A9103C56` (matches reference) |
| Translation | `.\scripts\recompile.ps1` | 6,609 C++ units + `register_functions.cpp` |
| Runner sync | file count in `PS2Recomp/ps2xRuntime/src/runner` | 6,610 files |

The ISO is read directly (`IoPaths.cdImage`); the full 8 GB disc is not copied.

## M1 — Android boot

| Gate | Check | Result |
| --- | --- | --- |
| G1 | `.\scripts\build-android.ps1 -Configuration Debug` completes | **pass** — `BUILD SUCCESSFUL` (2026-10-30 build: 30 s incremental after full native compile; 328 objects) |
| G2 | APK contains `lib/arm64-v8a/libps2EntryRunner.so` | **pass** — `dist/gow2-android-debug.apk`, 55.9 MB, only arm64 lib packaged |
| G3 | `adb logcat -s ps2x` shows EE/IOP boot and ≥300 frames without a fatal signal | pending — no adb/device attached |

G2 details (`llvm-readelf` / `llvm-nm` from NDK 27.3):

```
Machine: AArch64, Type: DYN
LOAD ... R E 0x4000
LOAD ... RW  0x4000
LOAD ... RW  0x4000
T ANativeActivity_onCreate
T android_main
T _Z21FUN_00228f90_0x228f90PhP12R5900ContextP10PS2Runtime
```

The packaged `.so` has no `.debug_*` sections and no `.symtab` (AGP stripped it);
dynamic symbols include the recompiled game functions, so the generated code is
linked. `PS2X_FORCE_NULL_GS=1` and the ISO path are compile-time inputs, not
device-side configuration.

Expected M1 characteristics: no-op GS (no pixels), interpreter VU1/IOP, arm64
only. G2/G3 cannot pass before G1.

## Session 2026-10-28 — host layer, engine patches, ISO reader

| Check | Command | Result |
| --- | --- | --- |
| Translation re-run | `.\scripts\recompile.ps1` | **pass** — 6,609 units + `register_functions.cpp`, 6,612 files synced |
| Tools build | `.\scripts\build-tools.ps1` | **pass** — `ps2_recomp.exe`, `ps2_analyzer.exe` (exit 0) |
| Engine patches | `git -C PS2Recomp diff --stat` | 5 files, +183/−8 — `0003-gs-vulkan-select`, `0004-android-host` |
| ISO reader, synthetic | `iso_extract_test.exe --self-test <dir>` | **pass** — 26/26 checks (multi-extent, layer 1, fd, SHA1 mismatch, `IOP_MOD/`) |
| ISO reader, real ISO | `iso_extract_test.exe "<iso>" <dir>` | **pass** — 24/24 checks; `SCUS_974.81` SHA1 `E6A62A0F3E781C29513F563F4625B9B3A9103C56`; layer-1 `PART2.PAK` LSN 2084139, 4,265,340,928 B; 10 root `*.IRX` |
| Host/Java typecheck | teammate runs (`javac` vs `android-35`, NDK `clang++ -fsyntax-only`) | reported clean; **not re-run by the Lead** |

Not verified yet (tracked in `TASKS.md`, "Open TODOs"): on-device boot and the
Vulkan backend itself (not implemented).

## Session 2026-10-30 — patch stack, host layer, APK end to end

| Check | Command | Result |
| --- | --- | --- |
| Patch stack from pristine | `git -C PS2Recomp reset --hard 379fa48` + apply `0001`–`0004` | **pass** — all four apply in order; `git diff --stat` = 5 files, +183/−8 |
| Re-applying via script | `.\scripts\setup.ps1` | **pass** — resets tracked engine edits and re-applies the stack |
| Tools build | `.\scripts\build-tools.ps1` | **pass** — `ps2_analyzer.exe`, `ps2_recomp.exe` (exit 0) |
| ISO extract | `.\scripts\extract-disc.ps1 -Iso "…God of War II (USA).iso"` | **pass** — ELF SHA1 `E6A62A0F3E781C29513F563F4625B9B3A9103C56`, 10 root `*.IRX` |
| Translation | `.\scripts\recompile.ps1` | **pass** — 6,609 units + `register_functions.cpp`, 6,610 runner files |
| Desktop compile of the patches | `cmake --build build --config Release --target ps2_runtime` | **pass** — `ps2_runtime.vcxproj -> ps2_runtime.lib` (MSVC, all patched files) |
| Java typecheck | `javac -source 17 -cp android-35/android.jar …` | **pass** — 8 sources, no errors |
| Android native build | `.\scripts\build-android.ps1` | **pass** — full arm64 compile, then `BUILD SUCCESSFUL in 30s` incremental |
| Static APK checks | `.\scripts\verify-android.ps1` | **pass** — package `com.gow2.android`, `1.0.0`, launcher `.MainActivity`, `minSdk 28`, arm64-v8a only, 3/3 LOAD at 16 KB, AArch64 |

Build fixes found by actually compiling the host layer (the earlier
`-fsyntax-only` pass missed them because the engine builds with a **unity
build**):

- `host/log.cpp` and `host/storage.cpp` both defined `kPathLen` in
  `ps2x::host::{anonymous}` → renamed the storage copy.
- `host/game_entry.cpp` defined a weak `ps2x_android_game_main()` that now
  collides with the strong definition from `patches/0004` → file removed.
- `ps2_runtime` needed `native/host` on its include path for `ps2x_host.h`
  (`patches/0004` includes it from `ps2_runtime.cpp`).
- `ps2x::host::SetSdkInt`/`GetSdkInt` were declared but never defined →
  implemented in `host/lifecycle.cpp`.

## Gates for later milestones

- M2: first frame submitted through `GSVkBackend`, screenshot compared with the
  PC OpenGL renderer; no GL symbols in the Android path.
- M3: 30 FPS target measured on the reference device; native VU1/IOP generated
  from a PC capture and verified against the interpreter.

## Session 2026-10-31 — M2 implementation build (GSVkBackend, offscreen probe)

| Check | Command | Result |
| --- | --- | --- |
| Backend written | `android-runner/native/gs_vk/**` | `gs_vk_backend.{h,cpp}` (~2,900 lines), dlopen loader, 6 GLSL→SPIR-V shaders, `sources.cmake`, `compile_shaders.cmake` |
| Shaders compile | NDK `glslc --target-env=vulkan1.1` | **pass** — 6/6 SPIR-V blobs (draw, shuffle, present) |
| Probe builds | `.\scripts\build-gs-probe.ps1 -NoRun` | **pass** — backend + engine GS sources + embedded SPIR-V link, exit 0 |
| Probe device init | `gs_probe clear` with SwiftShader ICD | **pass to draw dispatch** — instance/device/pools/pipelines created, vertex + submission path reached (`CmdDraw` with 6 verts) on the software rasterizer |
| Probe end-to-end pixels | full `gs_probe` | **blocked on this CI VM** — the available SwiftShader builds (Unity Hub + Chrome 156) hit an internal `ud2` assert in the clear/present path under the interpreter (`REACTOR_JIT=0`); not a validation error the backend can fix |
| Vulkan GS APK | `gradlew assembleDebug -Pps2xNullGs=OFF` | see below (native arm64 compile of gs_vk + engine) |

Notes:

- The probe harness (`scripts/build-gs-probe.ps1`) self-provisions: copies the
  NDK Vulkan headers into `gs_vk/_check`, compiles the shaders and embeds the
  SPIR-V into `gs_vk/_gen`, then compiles the backend against the patched
  engine sources. Both scratch dirs are gitignored.
- Early init-order bugs found by the probe and fixed: command buffer/fence/
  semaphore allocation moved before `createDeviceObjects` (self-submitting
  upload paths need them), GPU ring creation likewise, target init-clear now
  ends and submits its command buffer, `GpuRing::alloc` failure is guarded.
- Real-pixel validation is a device task (TASKS.md M2 bring-up): the software
  rasterizers on this machine are not a reference for the Adreno/Mali path.

## Session 2026-11-03 — device black screen triage (log v0.4.7-m2, Mali-G610)
Device log (`gow2.log` from release v0.4.7-m2 of EvilNathan/gow2-android-apk):
boot OK (ELF + 8 IRX modules + pads + AAudio), swapchain OK (1280x720, 4 images),
`Frame N presented OK (fbp=232 … ok1=1)` while `host_hz` collapsed 28 → 0.4 and
`gs_ms` absorbed 75–100% of the window — the presentation pipeline ran but drew
black.

Root causes fixed in `android-runner/native/gs_vk/gs_vk_backend.cpp`:
1. **Texture/palette uploads read staging offset 0** — `uploadTexture` memcpy'd
   the decoded rows to `staging.alloc(bytes)` but left `VkBufferImageCopy.bufferOffset`
   zeroed, so every `CmdCopyBufferToImage` copied GS page-table bytes from the
   start of the staging buffer instead of the texture data; same bug in
   `uploadPaletteImage` (CLUT) and the format-table upload. On-device the GS
   tables decode as near-zero alpha for every texel → alpha test / blend
   discards everything → black screen. The offscreen probe never caught it
   because its first upload happened to land at offset 0.
2. **Per-upload `vkCreateImage` + `vkAllocateMemory`** — games invalidate and
   re-upload textures continuously; each re-upload allocated and freed a
   device image (and freed it through the trash on Mali) → driver churn that
   matches the gs_ms collapse. Textures now reuse their image when size and
   format still match.
3. **Illegal full barriers inside the render pass** — the soft-blend
   triangle-by-triangle loop emitted a `globalBarrier()` (ALL_COMMANDS,
   memory↔memory, no by-region) between `CmdDraw`s *inside* a render pass,
   and every draw ended with one after `CmdEndRenderPass`. Replaced with the
   spec-legal form: by-region COLOR_ATTACHMENT_WRITE → INPUT_ATTACHMENT_READ
   between triangles, and an external-out dependency (added to rpDraw +
   rpClearColor/rpInitCD already had one) for pass→later-work ordering.
4. **Draw pass split** — draws that never read the destination (no DATE, no
   Z alias, no soft blend — the majority) now run in `rpDrawNoInput`
   (no input attachment, no self dependency), shortening tile-memory
   dependency chains on tiling GPUs (Mali).
5. **`draw_nodual.frag`** — devices without `dualSrcBlend` (SwiftShader and
   older mobile drivers) previously built pipelines that declared a second
   blending output; they now use a single-output build of `draw.frag`
   (`-DKS_VK_NO_DUAL_SRC`, selected when `dualSrc == false`) and `blendState`
   routes every fixed-function blend to the software path.
6. Probe repair: desktop `ps2x::host::Log` stub now prints to stderr (pipeline
   build failures were being swallowed), the clear-case expectation was fixed
   to the little-endian RGBA byte order the backend actually uses
   (`0x4080C0FF` = R255 G192 B128), `PS2X_VK_VALIDATION=1` opts into the
   Khronos validation layer on desktop, and `PS2X_VK_NO_DEPTH=1` routes draws
   through the depthless pass for bisecting.
| Check | Command | Result |
| --- | --- | --- |
| Probe builds | `.\scripts\build-gs-probe.ps1 -NoRun` | **pass** (clang, backend + engine GS + 7 SPIR-V blobs) |
| Probe `clear` | `gs_probe.exe clear` (SwiftShader ×4 builds) | **pass** — corner pixel (255,192,128,255), alpha forced |
| Probe `sprite`/`transfer` | same | still black on every SwiftShader build on this VM — pre-existing (see TASKS.md gate), needs a device or a healthy ICD to judge; the depth/input-attachment bisect flags above came out of it |

## M3 — device bring-up and playability (2026-11-09 – 2026-11-11)

Devices: Redfinger cloud phones over Tailscale + wireless ADB — Galaxy
Note20 (SM-N9810, Mali-G77) until 2026-11-10, then Galaxy S25+ (SM-S936U1,
Adreno, Android 15). Session log on device: `/sdcard/gow2/logs/gow2.log`.

| Check | Evidence | Result |
| --- | --- | --- |
| ISO integrity | SHA-256 of `games/God of War II (USA).iso` = `266b08b7823c569be205dc9377b0e2212fbcb9080c4d65029f15040ae62d9f98` | **pass** — matches the hash computed on the phone |
| APK static checks | `scripts/verify-android.sh` | **pass** 10/10 (package, launcher, arm64-v8a only, minSdk 28, 16 KB LOAD alignment, AArch64) |
| Native IOP | device log `native code for` ×8 modules | **pass** |
| Native VU1 | `[vu1-stats]`: 2,082 blocks, 0 interpreted fallbacks of note, ~20–24 ns/cycle (was ~254 interpreted) | **pass** — VU1 is no longer the bottleneck |
| GPU wedge | 5,280+ frames / ~4.5 min with zero fence timeouts after the drawCircuit mid-pass flush fix | **pass** (Mali-G77) |
| Swapchain | MAILBOX chosen on device; `[gs-ent]` acquire ≈10 ms per 5 s window (was ~2,600 ms FIFO) | **pass** |
| Movie | deterministic end signature every run: displayed frames stop at 3,759, audio packets end at 0x699, `[0x2D8190]` 10 → 0 | **pass** |
| Composition | render-target dumps: scene target and display target both hold the correct first-area scene (Kratos on the green hexagon platform); phone screenshot matches, upright, full-frame (present mode 1) | **pass** |
| Mode machine | watch-thread log, full run: mode 0 → 3 → 4 → 11, then scripted SELECT → 12, X → 11, START → 13; `[gow2-modeedge]` lines show the SELECT press latch `[0x2D8F70]` 0→1 and release firing the 11→12 transition | **pass** |
| Mode-13 branch | probe count `FUN_001E9310` ≈ 7,900 calls in mode 13 (2 lifetime calls in all pre-mode-13 runs) | **pass** |
| Stability soak | mode-12 light regime: guest t=673 s (~19 min wall), flip 8.6 Hz sustained, no crash; mode 13 runs at 74–119 guest flips/s | **pass** |
| Locomotion | stick held (up + square) for ~6–11 min in mode 13: screenshots and render-target dumps pixel-identical (0 changed px), player object shows no position-word change | **FAIL — open** (see TASKS.md M3 / Known issues) |

Corrections recorded during this phase: the two SIGABRT run endings seen on
2026-11-11 were artifacts of the TEMP-DIAG layer itself (a watch-thread
null-RDRAM read and an unbounded loader-poll printer thread), not game
crashes — with both fixed, the "light state" ran 21+ min with a constant
thread count. The kit crash handler additionally aborted inside its own
log-ring dump on two occasions, hiding the original faults (fix scheduled).
