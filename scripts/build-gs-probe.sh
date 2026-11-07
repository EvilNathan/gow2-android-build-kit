#!/usr/bin/env bash
# Linux port of build-gs-probe.ps1.
#
# Builds tests/gs_probe (offscreen Vulkan GS backend tests) with clang and
# runs them against the SwiftShader ICD. No Visual Studio, no Gradle:
#
#   ./scripts/build-gs-probe.sh            build + run
#   ./scripts/build-gs-probe.sh --no-run   build only
#
# The engine sources are compiled from the patched submodule (the same files
# the Android build uses), so a probe failure is a backend/engine regression,
# not a test artifact.
#
# Windows-only paths in the .ps1 (LLVM under C:\Program Files, the Unity Hub /
# Chrome / Edge vk_swiftshader.dll, the NDK windows-x86_64 prebuilt) become:
# clang++ from PATH, a libvk_swiftshader.so (set PS2X_SWIFTSHADER to point at
# one, or install Chrome/Chromium), and the NDK linux-x86_64 prebuilt.
. "$(dirname -- "${BASH_SOURCE[0]}")/_common.sh"

NO_RUN=0
while [[ $# -gt 0 ]]; do
    case "$1" in
        --no-run) NO_RUN=1; shift ;;
        *) die "Unknown argument: $1" ;;
    esac
done

ensure_engine
apply_engine_patches

clang="$(require_command clang++ 'Install clang (e.g. sudo apt install clang).')"

swiftshader=''
if [[ -n "${PS2X_SWIFTSHADER:-}" && -f "$PS2X_SWIFTSHADER" ]]; then
    swiftshader="$PS2X_SWIFTSHADER"
else
    for candidate in \
        '/opt/google/chrome/libvk_swiftshader.so' \
        '/usr/lib/chromium/libvk_swiftshader.so' \
        '/usr/lib/chromium-browser/libvk_swiftshader.so' \
        '/snap/chromium/current/usr/lib/chromium-browser/libvk_swiftshader.so' \
        "${VULKAN_SDK:-}/x86_64/lib/libvk_swiftshader.so"; do
        if [[ -f "$candidate" ]]; then
            swiftshader="$candidate"
            break
        fi
    done
fi
if [[ -z "$swiftshader" ]]; then
    die 'no SwiftShader ICD found (looked for libvk_swiftshader.so in the Chrome/Chromium folders). Install Chrome/Chromium or set PS2X_SWIFTSHADER to a libvk_swiftshader.so.'
fi

# NDK: ANDROID_NDK_HOME wins, then the pinned NDK inside the SDK, then the
# newest NDK in the SDK.
sdk="$(default_android_sdk)"
ndk=''
if [[ -n "${ANDROID_NDK_HOME:-}" && -d "$ANDROID_NDK_HOME" ]]; then
    ndk="$ANDROID_NDK_HOME"
elif [[ -d "$sdk/ndk/27.3.13750724" ]]; then
    ndk="$sdk/ndk/27.3.13750724"
else
    while IFS= read -r d; do
        ndk="$d"
        break
    done < <(find "$sdk/ndk" -mindepth 1 -maxdepth 1 -type d 2>/dev/null | sort -Vr)
fi
if [[ -z "$ndk" ]]; then
    die "Android NDK not found under $sdk/ndk. Install NDK 27.3.13750724 or set ANDROID_NDK_HOME."
fi
ndk_host='linux-x86_64'
ndk_inc="$ndk/toolchains/llvm/prebuilt/$ndk_host/sysroot/usr/include"
if [[ ! -d "$ndk_inc/vulkan" ]]; then
    die "Vulkan headers not found in the NDK sysroot: $ndk_inc"
fi
glslc="$ndk/shader-tools/$ndk_host/glslc"
if [[ ! -x "$glslc" ]]; then
    die "glslc not found at $glslc (install the NDK shader-tools component)."
fi

# Scratch dirs: embedded-SPIR-V C++ (from the same shaders the APK embeds) and
# a copy of the NDK's Vulkan headers (the desktop clang has none).
vk_root="$REPO_ROOT/android-runner/native/gs_vk/_check"
gen_root="$REPO_ROOT/android-runner/native/gs_vk/_gen"
mkdir -p "$vk_root/vulkan" "$vk_root/vk_video" "$gen_root"
cp -f -- "$ndk_inc"/vulkan/*.h "$vk_root/vulkan/"
cp -f -- "$ndk_inc"/vk_video/*.h "$vk_root/vk_video/"
spv_dir="$BUILD_DIR/gs-probe-spv"
mkdir -p "$spv_dir"
while IFS= read -r shader; do
    shader_name="$(basename -- "$shader")"
    "$glslc" --target-env=vulkan1.1 -o "$spv_dir/$shader_name.spv" "$shader" \
        || die "shader failed: $shader_name"
    if [[ "$shader_name" == 'draw.frag' ]]; then
        "$glslc" --target-env=vulkan1.1 -DKS_VK_NO_DUAL_SRC -o "$spv_dir/draw_nodual.frag.spv" "$shader" \
            || die 'shader failed: draw_nodual.frag'
    fi
done < <(find "$REPO_ROOT/android-runner/native/gs_vk/shaders" -type f \( -name '*.vert' -o -name '*.frag' \) | sort)

cmake="$(get_cmake)"
"$cmake" "-DSPV_DIR=$spv_dir" "-DOUT_CPP=$gen_root/gs_vk_shaders_generated.cpp" "-DOUT_H=$gen_root/gs_vk_shaders_generated.h" -P "$REPO_ROOT/cmake/embed_spirv.cmake" \
    || die 'SPIR-V embedding failed'

inc=(
    -I "$REPO_ROOT/android-runner/native"
    -I "$REPO_ROOT/android-runner/native/gs_vk"
    -I "$REPO_ROOT/android-runner/native/gs_vk/_gen"
    -I "$REPO_ROOT/android-runner/native/gs_vk/_check"
    -I "$RECOMP_DIR/ps2xRuntime/include"
    -I "$RECOMP_DIR/ps2xRuntime/src/lib"
    -I "$RECOMP_DIR/ps2xRuntime/src/lib/gs"
)
# -D_CRT_SECURE_NO_WARNINGS from the .ps1 is MSVC-only and is dropped here.
defines=('-DVK_USE_PLATFORM_ANDROID_KHR=0' '-DPS2X_GS_PROBE=1')
# The engine's GL backend and host stuff are not part of the probe; the GS
# sources the backend really needs plus their dependencies are listed here.
engine_sources=(
    "$RECOMP_DIR/ps2xRuntime/src/lib/gs/gs_swizzle.cpp"
    "$RECOMP_DIR/ps2xRuntime/src/lib/gs/ps2_gs_memory.cpp"
    "$RECOMP_DIR/ps2xRuntime/src/lib/gs/gpu/gs_texture_store.cpp"
    "$REPO_ROOT/android-runner/native/gs_vk/gs_vk_loader.cpp"
    "$REPO_ROOT/android-runner/native/gs_vk/gs_vk_backend.cpp"
    "$REPO_ROOT/android-runner/native/gs_vk/_gen/gs_vk_shaders_generated.cpp"
)

out_dir="$BUILD_DIR/gs-probe"
mkdir -p "$out_dir"

write_step 'Compiling the probe'
objects=()
all_sources=("$REPO_ROOT/tests/gs_probe/gs_probe.cpp" "${engine_sources[@]}")
for src in "${all_sources[@]}"; do
    base="$(basename -- "$src")"
    obj="$out_dir/${base%.*}.o"
    rc=0
    log="$("$clang" -std=c++20 -O1 -g0 -pthread -c "$src" -o "$obj" "${inc[@]}" "${defines[@]}" 2>&1)" || rc=$?
    printf '%s\n' "$log" | grep -i 'error' || true
    if [[ $rc -ne 0 ]]; then
        die "compile failed: $src"
    fi
    objects+=("$obj")
done

exe="$out_dir/gs_probe"
write_step 'Linking'
rc=0
log="$("$clang" "${objects[@]}" -o "$exe" -pthread -ldl 2>&1)" || rc=$?
printf '%s\n' "$log" | grep -iE 'error|undefined' || true
if [[ $rc -ne 0 ]]; then
    die 'link failed'
fi

if [[ $NO_RUN -eq 1 ]]; then
    echo "probe: $exe"
    exit 0
fi

write_step 'Running against SwiftShader'
# Software rasterizer: Subzero's JIT emits AVX that crashes on some CI VMs,
# so force the interpreter and accept slower runs.
export REACTOR_JIT='0'
export PS2X_VK_ICD="$swiftshader"
rc=0
"$exe" || rc=$?
exit $rc
