# Renderer migration: WebGPU → native Vulkan

Status: research + plan. No renderer code has been replaced yet.

## Where the renderer stands

The GS renderer is written against the **WebGPU** API and linked against
`wgpu-native`. On Android, wgpu-native's primary backend is **Vulkan**, so every
compute dispatch and present already reaches the vendor Vulkan driver through
Vulkan commands. OpenGL is not used for GS rendering; the SDL presenter is only
a fallback when a WebGPU device cannot be created.

```
game code → ps2::gs (gs.cpp, gif.cpp, queue.cpp)
          → ps2::gs::gpu (gpu.cpp, gs.wgsl, blit.wgsl)
          → wgpu-native → Vulkan → driver
```

Relevant files:

| File | Lines | Role |
| --- | ---: | --- |
| `runtime/src/gs/gpu/gpu.cpp` | 1,858 | instance/adapter/device, buffers, pipelines, compute, present, DRS |
| `runtime/src/gs/gs.cpp` | 1,082 | GS state, vertex assembly, draw decisions |
| `runtime/src/gs/gpu/gs.wgsl` | 1,081 | GS rasterization compute shader |
| `runtime/src/gs/gpu/blit.wgsl` | 52 | VRAM → swapchain blit |

## Options

### Tier 1 — direct Vulkan for the existing compute renderer

Replace wgpu-native with a small Vulkan backend that implements the same
`ps2::gs::gpu` interface. Reuse the GS algorithm and translate the WGSL shaders
to SPIR-V offline with `naga` (or re-express them in GLSL).

Work:

- instance/device/queue, `VK_KHR_android_surface` (ANativeWindow from SDL),
  swapchain + FIFO present, resize;
- one allocator for VRAM/scratch/readback/params (VMA or manual);
- descriptor sets for the current bind groups, compute pipelines, dispatch;
- manual barriers and frame pacing (wgpu inserts these today — this is the main
  source of new bugs);
- timestamps/queries for the existing stats.

Estimate: ~3,000 LOC, 2–4 weeks for one experienced Vulkan developer. Risk:
medium-high (synchronization and mobile driver differences). Expected gain:
10–30% on CPU-bound submission; close to zero on GPU-bound scenes. **Measure
before committing**: if the phone is GPU-bound, Tier 1 will not fix the lag.

### Tier 2 — port Play!'s Vulkan GS renderer

Play! (`jpd002/Play-`, **BSD-2-Clause**) has a complete Vulkan GS backend:

- `Source/gs/GSH_Vulkan/` — 335,745 bytes, ~10k LOC, 18 translation units;
- `GSH_VulkanDrawMobile.cpp` (69 KB) — a mobile-specific draw path;
- `Source/ui_android/GSH_VulkanAndroid.cpp` — Android surface handling;
- shaders are generated through **Nuanceur** (BSD-2), a C++ → SPIR-V DSL.

It is draw-based (graphics pipelines) rather than the compute rasterizer used
here, so it needs an adapter between our GS front-end and Play!'s
`CGSHandler` interface (or adopting Play!'s GS state machine), plus the
Framework/Vulkan helpers. Estimate: 6–10 weeks, high integration risk, but it
is a mature renderer with a proven Android path and a compatible license.

### Tier 3 — PCSX2's Vulkan backend

`PCSX2/pcsx2` has the most mature backend (`pcsx2/GS/Renderers/Vulkan`, 18
files, ~450 KB). It is **GPL-3.0**: including it in this closed, game-derived
APK would force GPL distribution terms on the combined work. Not viable for
this project; use it as a reference only (clean-room reimplementation).

### OpenGL ES

An OpenGL ES 3.1 backend is possible (Play!'s `GSH_OpenGL` is BSD-2), but it is
desktop-GL oriented, has weaker compute/storage support, and Android GLES
drivers are generally less predictable than Vulkan for this workload. Not
recommended as the primary path. A GLES fallback for broken Vulkan drivers is
not worth the maintenance cost today.

### Other repositories checked

| Project | License | Verdict |
| --- | --- | --- |
| [Play!](https://github.com/jpd002/Play-) | BSD-2 | **Best reusable reference** (Vulkan + Android) |
| [PCSX2](https://github.com/PCSX2/pcsx2) | GPL-3.0 | Reference only |
| [Iris](https://github.com/allkern/iris) | MIT | GS renderer is a 7 KB wrapper over `parallel-gs`; emulator still WIP |
| [parallel-gs](https://github.com/Arntzen-software/parallel-gs) | LGPL-3.0 | Compute GS, but LGPL + Rust integration |
| [Granite](https://github.com/Themaister/Granite) | MIT | Vulkan framework; Android surface work still needed |
| [Nuanceur](https://github.com/jpd002/Nuanceur) | BSD-2 | Shader DSL used by Play! |
| [wgpu-native](https://github.com/gfx-rs/wgpu-native) | Apache-2.0 | Current layer |
| [SDL3](https://github.com/libsdl-org/SDL) | Zlib | Window/surface/audio |
| [naga](https://github.com/gfx-rs/naga) | MIT/Apache-2.0 | WGSL → SPIR-V for Tier 1 |
| [VMA](https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator) | MIT | Vulkan allocator |

## Recommended order

1. Fix the v2.0–v2.2 crash first (see `TASKS.md`, Fase 7). A renderer rewrite
   cannot fix a crash caused by game/VU code.
2. Profile the stable build: CPU submission vs GPU compute/raster vs present.
3. If CPU-bound → Tier 1. If GPU-bound → optimize the existing shaders/GS path
   first; only then evaluate Tier 2.
4. Keep wgpu as the desktop/web backend behind the same interface; Vulkan is an
   Android backend, not a replacement for every platform.

## Phase plan (Tier 1, when approved)

| Phase | Deliverable | Exit criteria |
| --- | --- | --- |
| 0 | Renderer interface split (`gs::gpu` → backend vtable) | wgpu backend still builds and passes the 3,500-frame run |
| 1 | Vulkan context + Android surface/swapchain + present | clear colour on device, resize/orientation safe |
| 2 | Buffers + compute pipeline + SPIR-V from `gs.wgsl` | one GS frame matches wgpu output on desktop |
| 3 | Full GS compute path + CLUT/texture upload + readback | boot + first gameplay scene on Adreno and Mali |
| 4 | Timestamps, frame pacing, DRS, fallback | stats parity; no validation errors |
| 5 | Remove/keep wgpu per platform | Android defaults to Vulkan; wgpu remains for web |

## Risks

- **Synchronization**: manual barriers replace wgpu's automatic tracking.
- **Driver diversity**: Adreno, Mali, Xclipse and PowerVR differ in limits,
  render-pass rules and shader compilation time.
- **Surface lifecycle**: Android pause/resume and 16 KB page devices.
- **Effort vs gain**: the current bottleneck must be measured; a native backend
  is not automatically faster.
