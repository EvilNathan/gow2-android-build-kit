# Third-party dependencies

This folder is intentionally empty in the distributed kit. `scripts/setup.ps1`
creates `engine/third_party/` and downloads the following sources/libraries into
it. Nothing here is a game asset, and nothing here is committed or shipped in
the build-kit ZIP.

## SDL3 3.4.18

- Archive: `SDL3-3.4.18.zip`
- URL: https://github.com/libsdl-org/SDL/releases/download/release-3.4.18/SDL3-3.4.18.zip
- Destination: `engine/third_party/SDL3-3.4.18/`
- License: zlib; see `LICENSE.txt` in the SDL source.
- Used for: Android activity/window, audio output, gamepad input, lifecycle,
  and the optional desktop window.

## wgpu-native v29.0.1.1 for Android arm64

- Archive: `wgpu-android-aarch64-release.zip`
- URL: https://github.com/gfx-rs/wgpu-native/releases/download/v29.0.1.1/wgpu-android-aarch64-release.zip
- Destination: `engine/third_party/wgpu-native-android/`
- Expected files: `include/webgpu/webgpu.h`, `include/webgpu/wgpu.h`,
  `lib/libwgpu_native.a`, and `lib/libwgpu_native.so`
- License: MIT OR Apache-2.0; the archive includes the wgpu-native metadata and
  license notices for the Rust crates it vendors.
- Used for: the native Vulkan/WebGPU renderer. The Android build links
  `libwgpu_native.a` into `libmain.so`; `libwgpu_native.so` is not packaged.

## SDL3 Java classes

`android/app/src/main/java/org/libsdl/app/` contains SDL3's Java activity
classes. They are zlib-licensed; the license text is reproduced in
`third_party/LICENSE-SDL3.txt`.

## Build tools

CMake, Ninja, LLVM/Clang, Python, Git, JDK 17, and the Android SDK/NDK are
installed by the user. They are not redistributed with this kit.
