# Notices and licenses

This build kit contains only scripts, documentation, small compatibility
headers, source patches, and an Android host project. It does **not** contain
the game, the game ELF, an ISO, translated game code, a memory card, or an APK.

## Build kit

The kit (scripts, Android runner, patches, compatibility headers, docs) is
released under the **GNU General Public License v3.0**. See `LICENSE`.

It is built on [DaniSBR1/PS2Recomp](https://github.com/DaniSBR1/PS2Recomp),
branch `godofwar2`, pinned at `379fa48b1a292eb7f688ccdc202d97f5ffc09c6d`. That
engine is GPL-3.0 and is fetched as a git submodule (`PS2Recomp/`); it remains
under its own copyright and license. The engine includes:

- code derived from PCSX2's EE/VU reference implementation (GPL-3.0+),
- an OpenGL GS backend derived from the Shadow of the Colossus fork
  (GPL-3.0), as noted in its source headers,
- GoW-specific runtime work by the PS2Recomp contributors.

## Build-time dependencies (not vendored)

| Dependency | License | Use |
| --- | --- | --- |
| sse2neon v1.9.1 | MIT | x86 intrinsics on arm64 (fetched by CMake) |
| raylib 5.5 | zlib | window, input, audio (fetched by CMake) |
| fmt | MIT | recompiler formatting (fetched by CMake) |
| rabbitizer | MIT | MIPS decoding (fetched by CMake) |
| libdwarf | LGPL-2.1 (upstream terms) | recompiler DWARF symbols |
| libmpeg2 | GPL-2.0+ | MPEG video decoding (runtime) |
| FFmpeg (prebuilt, Windows, opt-in) | LGPL | alternative video backend, off by default |
| Dear ImGui / rlImGui | MIT | desktop debug UI only (off on Android) |
| Gradle wrapper 8.9 | Apache-2.0 | Android build |
| Android NDK libc++ | Apache-2.0 with LLVM exception | runtime |

Each dependency is downloaded by CMake/Gradle at build time and keeps its own
license files. The Android SDK, NDK, CMake, Clang, Ninja, Git, Python, and JDK
are installed separately and are not redistributed by this kit.

## Legacy parts from the previous wgpu kit (MIT)

These files remain for reference and are **not** part of the Android path:

- `android/` — the old SDL3/SAF Java shell, originally MIT
  (Copyright (c) 2026 Samir Nuri and kit contributors), including
  `org/libsdl/app/*.java` from SDL3 (zlib; see `third_party/LICENSE-SDL3.txt`),
- `LICENSE-ENGINE.txt` — MIT license of `snuri00/ps2-web-recomp`,
- `docs/legacy/*` — old wgpu documentation.

They stay under their original licenses.

## Game content and trademarks

*God of War* and *God of War II* are trademarks of Sony Interactive
Entertainment. The game, its characters, music, voices, and other assets are
copyright Sony Interactive Entertainment and Santa Monica Studio. This project
is not affiliated with or endorsed by them.

You must supply your own legally obtained disc image. The generated C++,
extracted ELF, ISO, memory-card image, screenshots, and APK are derivative works
of the game and must not be redistributed.
