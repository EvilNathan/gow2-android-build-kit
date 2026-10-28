# gs_vk — raw Vulkan GS backend (Android)

Status: **not implemented yet** (see `TASKS.md`, M2). This directory is the agreed
home for it; `android-runner/native/CMakeLists.txt` already wires everything up
as soon as `sources.cmake` appears here.

## Contract expected by the kit

- `gs_vk_backend.h` — `class GSVkBackend final : public GSRasterBackend`, default
  constructible. The kit's `patches/0003-gs-vulkan-select.patch` builds
  `GSThreadedBackend(std::make_unique<GSVkBackend>())` on Android, so the
  constructor must not need a thread, a window or a GL context. The GS worker
  thread owns every Vulkan call.
- `sources.cmake` — `target_sources(ps2_runtime PRIVATE ...)` for every backend
  `.cpp` that is valid on Android. Offscreen/probe files stay out of this list.
- `shaders/<name>.<stage>.glsl` — compiled by `compile_shaders.cmake` with the
  NDK's `glslc`; the blobs are embedded by `cmake/embed_spirv.cmake` and reached
  through

  ```cpp
  #include "gs_vk_shaders_generated.h"
  auto blob = gs_vk_spv::Get("solid.vert"); // {data, size}, {nullptr, 0} if missing
  ```

- Window: `#include "ps2x_host.h"` and `ps2x::host::NativeWindow()`; `nullptr`
  means "surface gone — skip present".

## Reference

- Interface: `PS2Recomp/ps2xRuntime/include/runtime/gs/gs_backend.h`
- Desktop implementation to mirror: `PS2Recomp/ps2xRuntime/src/lib/gs/gs_hw_backend.cpp`
- Engine selection point: `patches/0003-gs-vulkan-select.patch`
