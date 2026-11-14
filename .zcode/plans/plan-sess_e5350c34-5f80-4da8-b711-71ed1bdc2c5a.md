# M3 Comprehensive Fix — Menu Black, FMV No-Visuals+Lag, Frozen Character

## Context

On-device testing (Galaxy S25+ / Adreno / Android 15) shows: boot trademark screen renders fine, but (1) menu is black though input works, (2) cutscene plays audio with no video and severe lag, (3) gameplay shows one static T-pose frame. Investigation traced each symptom to a concrete root cause:

- **FMV lag**: `gs_vk_backend.cpp` uses **one shared `frameFence`** for everything (present, texture uploads, CLUT readbacks). During FMV, constant uploads + a **full GPU stall + CPU readback per CLUT reload** (`readTargetCpu` L2068–2101, called from `clutFromTarget` L2107–2167) serialize the entire pipeline. TASKS.md TODO 2 already plans double-buffering for this.
- **FMV no visuals**: local→local BITBLT transfers (how IPU-decoded movie frames reach GS VRAM) go only to the **CPU-side store** (`BeginTransfer` L618 + `invalidateBlocks`) — **GPU target images are never updated**, so Present samples stale/black targets.
- **Menu black**: most likely the same class — `submit()` silently **drops draws on fpsm family mismatch** (L2209–2211) and rejected channel-shuffles fall back to paths that may not cover the menu's composite; also `Present`'s "most recently drawn target" fallback (L4018–4029) can show a stale buffer. Needs on-device dump confirmation first.
- **Static T-pose**: the known **mode-13 locomotion gate** (TASKS.md open blocker). Pad analogs reach the guest, but the player never moves. The planned **diag120** (call-profile + `0x32F300` peek + raised log cap) was never run.

Work proceeds in phases; each phase ends with commit+push (AGENTS rule 8). All engine-side changes land in `android-runner/` (no PS2Recomp edits unless a patch is regenerated).

## Phase 0 — Baseline repro & evidence capture (device, read-mostly)

1. Launch game; at the **black menu**, create `/sdcard/gow2/dump_rt.txt` + `dump_present.txt` (existing facilities, gs_vk_backend.cpp L4053/L4166) and pull PPMs; note `stShufRej` counters from the 5-second stats.
2. Repeat during **FMV**. This pins down menu-black and FMV-black root causes before code changes.
3. Record baseline perf numbers from `[gow-perf]` for later comparison.

## Phase 1 — Present pipeline performance (fixes FMV/gameplay lag)

File: `android-runner/native/gs_vk/gs_vk_backend.cpp` (standard Vulkan frame-in-flight architecture per [docs.vulkan.org](https://docs.vulkan.org/tutorial/latest/Synchronization/Frame_in_Flight/01_introduction.html)):

1. **Double-buffer frame resources**: N=2 sets of {command buffer(s), fence, semaphores, vertex ring, descriptor pool}; Present waits on *previous* frame's fence, not the just-recorded one. Rotate by frame index.
2. **Decouple uploads from the frame fence**: texture/palette uploads use their own short-lived fence or batch into the current frame's command buffer with proper barriers, instead of `waitFrameFence("texture-upload", 2s)` (L1253–1257) stalling everything.
3. **GPU palette path**: cache CLUT reloads (palette cache key already exists, L2017–2023); avoid `readTargetCpu` per reload — only read back when the owning target actually changed since last read (track via uploadLog/drawLog generation counters).
4. Verify: `[gow-perf]` host_hz up, `gs_ms` share down; fence-timeout counter stays 0.

## Phase 2 — Local→local transfers update GPU targets (fixes FMV visuals)

In `gs_vk_backend.cpp`, modeled on PCSX2's texture-cache invalidation approach:

1. In `BeginTransfer` (LOCL path), compute the affected VRAM block range; for each **live GPU target** overlapping it, queue a **region re-upload** from the CPU store (decode + `CmdCopyBufferToImage` into the target image with proper layout transitions), replacing the current "invalidate textures only" behavior.
2. When src and dst are both GPU targets and formats/families match, prefer a direct `vkCmdCopyImage` (blit) path.
3. Guard: mark the re-uploaded region in uploadLog so RT-as-texture sampling stays coherent.
4. Verify: FMV frames visible in `dump_present.txt` PPM dumps before device screenshot check.

## Phase 3 — Menu black fixes

Guided by Phase 0 dumps:

1. Replace the **fpsm family-mismatch hard drop** (L2209–2211) with reinterpretation: treat the draw as a reinterpreting fetch/rewrite (the `uReinterp` machinery already exists) instead of dropping.
2. Relax validated shuffle gates only where counters show mass rejection (`stShufRej[0..7]` tells which gate).
3. If Present fallback presents a stale target, prefer the target the game's dispfb actually points at; blank-out rather than show stale content.
4. Verify: menu visible in device screenshot; mode machine still advances via padScript.

## Phase 4 — Locomotion gate (diag120, then fix)

Per TASKS.md TODO 1:

1. Enable `GOW2_CALL_PROFILE=5` (add `SetEnvVar` in `android-runner/native/host/jni_bridge.cpp`), extend `GOW2_PEEK` with `32F338,32F33C,32F358,32F35C,2D8EC0`, raise/remove the 10 MB log cap (`host/log.cpp`), delete `/sdcard/gow2/force394.txt`.
2. Build → install → scripted run (existing padScript) → read `[gow2-prof]` top-25: do `FUN_001E6000`/`FUN_001E5AF0`/`FUN_00278100`/`FUN_002A7728` execute in mode 13, and do the `0x32F300` analogs move with the stick?
3. Fix based on result: either the analog consumer reads a buffer the host doesn't inject into (route host injection into the `0x32F300` struct via an override), or the player update never runs (trace the indirect dispatch).
4. Verify: Kratos walks in mode 13 (screenshot diff shows position change).

## Phase 5 — Cleanup, full verification, release

1. TEMP-DIAG cleanup per TASKS.md TODO 3 (keep permanent fixes), crash-handler `LogRingDump` clamp (TODO 4) in `host/crash.cpp`.
2. Full soak: boot → title → movie → SELECT/X/START → character moves → 10 min stable, zero fence timeouts.
3. `scripts/verify-android.ps1` — all 10 checks pass (incl. 16 KB alignment).
4. Commit + push each phase as it completes (`EvilNathan` credentials, Co-authored-by trailers). Publish `v0.5.0-m3` APK to private `EvilNathan/gow2-android-apk` via `gh release create` (never to this repo).

## Verification loop per phase

`.\scripts\build-android.ps1 -Configuration Debug -Install` → reinstall caveat: re-sign with stable keystore + re-grant all-files-access appop (TASKS.md known issue) → `adb logcat` full dump + grep `ps2x` (tag filter `ps2x:V` returns 0 on this build) → pull `/sdcard/gow2/logs/gow2.log` (delete before each launch; `run-as` blocked by Knox) → screenshot at steady state → compare `[gow-perf]` / `vu1-stats` / shuffle counters against baseline.

## Key files

- `android-runner/native/gs_vk/gs_vk_backend.cpp` (Phases 1–3)
- `android-runner/native/gs_vk/shaders/` (only if reinterpret path needs a variant)
- `android-runner/native/host/jni_bridge.cpp`, `host/log.cpp`, `host/crash.cpp` (Phases 4–5)
- `overrides/gow2_overrides.cpp` (Phase 4 fix, watch/prof hooks)
- `TASKS.md`, `VERIFICATION.md` (record evidence after each phase)