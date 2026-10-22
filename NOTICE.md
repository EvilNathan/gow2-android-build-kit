# Notices and licenses

This build kit contains only original scripts, Java glue code, documentation,
and a source patch. It does **not** contain the game, the game ELF, an ISO,
translated game code, a memory card, or an APK.

## Build kit

The scripts, Android glue, and documentation in this kit are released under the
MIT License. See `LICENSE`.

The changes in `engine.patch` are derived from
[`snuri00/ps2-web-recomp`](https://github.com/snuri00/ps2-web-recomp), which is
distributed under the MIT License, Copyright (c) 2026 Samir Nuri. The full
upstream license is included as `LICENSE-ENGINE.txt`. The patch remains under
the upstream MIT terms.

## Downloaded dependencies

`scripts/setup.ps1` downloads these dependencies into `engine/third_party/` on
your machine. They are not part of this kit.

| Dependency | Version | License | Source |
| --- | --- | --- | --- |
| SDL3 | 3.4.18 | zlib | https://github.com/libsdl-org/SDL/releases/tag/release-3.4.18 |
| wgpu-native | 29.0.1.1 | MIT OR Apache-2.0 | https://github.com/gfx-rs/wgpu-native/releases/tag/v29.0.1.1 |
| wgpu / naga / Rust crates inside wgpu-native | bundled by wgpu-native | MIT OR Apache-2.0 (per crate) | https://github.com/gfx-rs/wgpu |
| libadrenotools + linkernsbypass | 8fae8ce254dfc1344527e05301e43f37dea2df80 | BSD-2-Clause, Copyright (c) 2021 Billy Laws | https://github.com/bylaws/libadrenotools |

The Android app bundles SDL's Java activity classes (`org/libsdl/app/*.java`)
so that the static SDL3 build can be driven without a separate `libSDL3.so`.
Those files are part of SDL3 and are covered by the zlib license reproduced in
`third_party/LICENSE-SDL3.txt`.

Each download includes its own license files. SDL3's license is at
`engine/third_party/SDL3-3.4.18/LICENSE.txt`; wgpu-native's metadata and license
information is in the downloaded archive and its repository.

libadrenotools and its linkernsbypass submodule are BSD-2-Clause. The Android
build may include them in `libmain.so` and the hook libraries, so the required
notice is reproduced here:

> BSD 2-Clause License
>
> Copyright (c) 2021, Billy Laws
>
> Redistribution and use in source and binary forms, with or without
> modification, are permitted provided that the following conditions are met:
>
> 1. Redistributions of source code must retain the above copyright notice, this
>    list of conditions and the following disclaimer.
>
> 2. Redistributions in binary form must reproduce the above copyright notice,
>    this list of conditions and the following disclaimer in the documentation
>    and/or other materials provided with the distribution.
>
> THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
> AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
> IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
> DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
> FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
> DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
> SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
> CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
> OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
> OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

The Gradle wrapper is Apache License 2.0. The Android SDK, NDK, CMake, Clang,
Ninja, Git, Python, and JDK are installed separately and remain under their own
licenses; this kit does not redistribute them.

## Game content and trademarks

*God of War* and *God of War II* are trademarks of Sony Interactive
Entertainment. The game, its characters, music, voices, and other assets are
copyright Sony Interactive Entertainment and Santa Monica Studio. This project
is not affiliated with or endorsed by them.

You must supply your own legally obtained disc image. The generated C++,
extracted ELF, ISO, memory-card image, screenshots, and APK are derivative works
of the game and must not be redistributed.
