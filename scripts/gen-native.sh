#!/usr/bin/env bash
# Linux port of gen-native.ps1.
#
# Generates the native VU1 + IOP code that makes God of War II playable on
# Android.
#
# Android builds cannot run the host-only VU1 microcode compiler (the engine
# defines ps2x_vu1_generator only when NOT PS2X_IS_ANDROID), so this script
# runs the host generator and the Python generators once on the host and
# leaves plain C++ in generated/. The Android build then compiles those files:
#
#   generated/vu1n-parts/programa*.cpp  13 parts, host generator (host-only)
#   generated/vu1n/vu1n_part*.cpp       6 parts, tools/vu1/vu1n_gen.py
#   generated/iop/iopn_*.cpp            IRX modules, tools/iop/iopn_gen.py
#
# The IOP files are also copied into PS2Recomp/ps2xIOP/src/native/ because
# the engine CMake globs them there (they are gitignored inside the submodule).
#
# The inputs are game-derived and stay local (generated/ and games/ are
# gitignored); only code is committed to the kit.
#
# Capture folders (defaults):
#   generated/capture/vu1  16 KB VU1 microcode .bin files (GOW_VU1_CAPTURA)
#   generated/capture/iop  modules.txt + <module>.bin (PS2X_IOP_DUMP_MODULES)
#
# The script is idempotent: the generators only rewrite changed files, copies
# are content-compared and the CMake target builds incrementally.
#
#   ./scripts/gen-native.sh [--vu1-microcode-dir <dir>] [--iop-modules-dir <dir>]
#                           [--skip-generator-build]
#
# --skip-generator-build: reuse the ps2x_vu1_generator already built under
# build/ (no CMake). (The .ps1 documents the same intent; there the flag
# never actually locates the existing binary — here it is looked up under
# build/, and the script fails if it is not there.)
#
# The generated C++ is consumed by the Android build through
#     -DPS2X_ANDROID_NATIVE_VU1_DIR=<repo>/generated/vu1n
#     -DPS2X_ANDROID_NATIVE_VU1_PARTS_DIR=<repo>/generated/vu1n-parts
# and the engine's ps2xIOP glob. The last section of the output prints the
# exact -D arguments.
. "$(dirname -- "${BASH_SOURCE[0]}")/_common.sh"

VU1_MICROCODE_DIR=''
IOP_MODULES_DIR=''
SKIP_GENERATOR_BUILD=0
while [[ $# -gt 0 ]]; do
    case "$1" in
        --vu1-microcode-dir) VU1_MICROCODE_DIR="$2"; shift 2 ;;
        --vu1-microcode-dir=*) VU1_MICROCODE_DIR="${1#*=}"; shift ;;
        --iop-modules-dir) IOP_MODULES_DIR="$2"; shift 2 ;;
        --iop-modules-dir=*) IOP_MODULES_DIR="${1#*=}"; shift ;;
        --skip-generator-build) SKIP_GENERATOR_BUILD=1; shift ;;
        *) die "Unknown argument: $1" ;;
    esac
done

resolve_required_directory() {
    local path="$1" label="$2" hint="$3"
    if [[ ! -d "$path" ]]; then
        die "$label not found: $path
$hint"
    fi
    ( cd -- "$path" && pwd )
}

assert_file_exists() {
    local path="$1" hint="$2"
    if [[ ! -f "$path" ]]; then
        die "Missing file: $path
$hint"
    fi
}

# Only rewrite a file when its bytes change (same approach as recompile.sh).
test_same_content() {
    [[ -f "$2" ]] && cmp -s -- "$1" "$2"
}

find_generator_exe() {
    if [[ -x "$BUILD_DIR/ps2xRuntime/ps2x_vu1_generator" ]]; then
        printf '%s\n' "$BUILD_DIR/ps2xRuntime/ps2x_vu1_generator"
        return 0
    fi
    if [[ -x "$BUILD_DIR/ps2xRuntime/Release/ps2x_vu1_generator" ]]; then
        printf '%s\n' "$BUILD_DIR/ps2xRuntime/Release/ps2x_vu1_generator"
        return 0
    fi
    local found
    found="$(find "$BUILD_DIR" -type f -name 'ps2x_vu1_generator' -printf '%T@ %p\n' 2>/dev/null | sort -rn | head -n 1 | cut -d' ' -f2-)"
    if [[ -n "$found" ]]; then
        printf '%s\n' "$found"
        return 0
    fi
    return 1
}

# ── Inputs ────────────────────────────────────────────────────────────────
if [[ -z "$VU1_MICROCODE_DIR" ]]; then VU1_MICROCODE_DIR="$REPO_ROOT/generated/capture/vu1"; fi
if [[ -z "$IOP_MODULES_DIR" ]]; then IOP_MODULES_DIR="$REPO_ROOT/generated/capture/iop"; fi

write_step 'Checking the prerequisites'
if [[ ! -f "$ELF_NAME_FILE" ]]; then
    die "games/gow2/elf-name.txt is missing. Extract your own disc first:
  ./scripts/extract-disc.sh --iso \"<your God of War II (USA) disc image>\""
fi
elf_name="$(trim "$(head -n 1 "$ELF_NAME_FILE")")"
if [[ -z "$elf_name" ]]; then
    die 'games/gow2/elf-name.txt is empty.'
fi

VU1_MICROCODE_DIR="$(resolve_required_directory "$VU1_MICROCODE_DIR" 'VU1 microcode capture folder' \
    'Capture the VU1 microcode on the host first (GOW_VU1_CAPTURA, see docs/NOTES.md) or pass --vu1-microcode-dir.')"
IOP_MODULES_DIR="$(resolve_required_directory "$IOP_MODULES_DIR" 'IOP modules capture folder' \
    'Capture the IOP modules on the host first (PS2X_IOP_DUMP_MODULES) or pass --iop-modules-dir.')"

vu1_bins=()
while IFS= read -r f; do vu1_bins+=("$f"); done \
    < <(find "$VU1_MICROCODE_DIR" -maxdepth 1 -type f -name '*.bin' | sort)
if [[ ${#vu1_bins[@]} -eq 0 ]]; then
    die "No VU1 microcode captures (*.bin) in $VU1_MICROCODE_DIR. The engine only creates ps2x_vu1_generator when there is at least one .bin capture."
fi
for f in "${vu1_bins[@]}"; do
    fsize="$(stat -c %s -- "$f")"
    if [[ $fsize -ne 16384 ]]; then
        die "A VU1 microcode capture must be exactly 16384 bytes: $f has $fsize bytes."
    fi
done

modules_txt="$IOP_MODULES_DIR/modules.txt"
assert_file_exists "$modules_txt" 'modules.txt is written by PS2X_IOP_DUMP_MODULES.'
iop_bin_count="$(find "$IOP_MODULES_DIR" -maxdepth 1 -type f -name '*.bin' | wc -l)"
iop_bin_count="$(trim "$iop_bin_count")"
if [[ $iop_bin_count -eq 0 ]]; then
    die "No IOP module dumps (*.bin) in $IOP_MODULES_DIR (modules.txt exists but the module binaries are missing)."
fi

VU1_PARTS_DIR="$REPO_ROOT/generated/vu1n-parts"
VU1_GEN_DIR="$REPO_ROOT/generated/vu1n"
IOP_GEN_DIR="$REPO_ROOT/generated/iop"
IOP_NATIVE_DIR="$RECOMP_DIR/ps2xIOP/src/native"
GENERATOR_SOURCE="$REPO_ROOT/tools/vu1/generar_vu1.cpp"
VU1N_GEN="$REPO_ROOT/tools/vu1/vu1n_gen.py"
VU1N_HEADS="$REPO_ROOT/tools/vu1/vu1n_heads.txt"
IOPN_GEN="$REPO_ROOT/tools/iop/iopn_gen.py"

assert_file_exists "$GENERATOR_SOURCE" 'The kit checkout is incomplete: tools/vu1/generar_vu1.cpp is missing.'
assert_file_exists "$VU1N_GEN" 'The kit checkout is incomplete: tools/vu1/vu1n_gen.py is missing.'
assert_file_exists "$VU1N_HEADS" 'The kit checkout is incomplete: tools/vu1/vu1n_heads.txt is missing.'
assert_file_exists "$IOPN_GEN" 'The kit checkout is incomplete: tools/iop/iopn_gen.py is missing.'
if [[ $SKIP_GENERATOR_BUILD -eq 0 ]] && ! test_engine_ready; then
    die "PS2Recomp/ is missing or empty. Run ./scripts/setup.sh first (engine pin $ENGINE_PIN)."
fi

# The host generators receive paths on the command line; tooling and locale
# handling make non-ASCII paths unreliable.
if printf '%s' "$REPO_ROOT" | LC_ALL=C grep -q '[^ -~]'; then
    die "The repository path contains non-ASCII characters: $REPO_ROOT
Move the kit to an ASCII-only path before generating native code."
fi

py_exe="$(get_python)"
cmake=''
if [[ $SKIP_GENERATOR_BUILD -eq 0 ]]; then cmake="$(get_cmake)"; fi

echo "Boot ELF:      $elf_name"
echo "VU1 captures:  ${#vu1_bins[@]} *.bin in $VU1_MICROCODE_DIR"
echo "IOP dumps:     $iop_bin_count *.bin in $IOP_MODULES_DIR"

# ── 1. Host VU1 microcode generator ───────────────────────────────────────
generator_exe=''
if [[ $SKIP_GENERATOR_BUILD -eq 1 ]]; then
    write_step 'Reusing the host generator (skipping CMake)'
    generator_exe="$(find_generator_exe || true)"
else
    write_step 'Configuring PS2Recomp for the host VU1 generator'
    set_generator_args
    configure_args=(-S "$RECOMP_DIR" -B "$BUILD_DIR" "${CMAKE_GENERATOR_ARGS[@]}")
    configure_args+=("-DPS2X_VU1_MICROCODE_DIR=$VU1_MICROCODE_DIR")
    configure_args+=("-DPS2X_VU1_GENERATOR_SOURCE=$GENERATOR_SOURCE")
    configure_args+=("-DPS2X_VU1N_GEN_OVERRIDE=$VU1N_GEN")
    configure_args+=("-DPS2X_VU1N_HEADS_FILE=$VU1N_HEADS")
    invoke_native "$cmake" "${configure_args[@]}"

    cache_path="$BUILD_DIR/CMakeCache.txt"
    if [[ ! -f "$cache_path" ]]; then
        die "CMake did not write $cache_path. Delete build/ and rerun ./scripts/setup.sh."
    fi
    expected_microcode="PS2X_VU1_MICROCODE_DIR:PATH=$VU1_MICROCODE_DIR"
    if ! grep -qF -- "$expected_microcode" "$cache_path"; then
        die "CMake ignored $expected_microcode. Delete build/CMakeCache.txt and rerun, and check that PS2Recomp is at pin $ENGINE_PIN."
    fi

    # The engine only creates the target when the microcode folder has *.bin
    # captures and the configure is a host (non-Android) one. On Windows this
    # was checked via the generated .vcxproj; here check the build system
    # (Ninja rules / Make target dir) instead.
    target_found=0
    if [[ -f "$BUILD_DIR/build.ninja" ]] && grep -qs 'ps2x_vu1_generator' "$BUILD_DIR/build.ninja"; then
        target_found=1
    fi
    if find "$BUILD_DIR" -name 'ps2x_vu1_generator*' -print -quit 2>/dev/null | grep -q .; then
        target_found=1
    fi
    if [[ $target_found -eq 0 ]]; then
        {
            echo 'error: The ps2x_vu1_generator target was not created by the configure.'
            echo 'The engine adds it only when all of these hold:'
            echo "  - PS2X_VU1_MICROCODE_DIR contains at least one *.bin (currently ${#vu1_bins[@]})"
            echo '  - PS2X_VU1_GENERATOR_SOURCE points at tools/vu1/generar_vu1.cpp'
            echo '  - the configure is NOT Android and NOT Vita (this is a host configure)'
            echo 'Delete build/ and rerun ./scripts/setup.sh, then run this script again.'
        } >&2
        exit 1
    fi

    write_step 'Building ps2x_vu1_generator (host only, no runner/engine build)'
    invoke_native "$cmake" --build "$BUILD_DIR" --target ps2x_vu1_generator

    generator_exe="$(find_generator_exe || true)"
    if [[ -z "$generator_exe" ]]; then
        die "ps2x_vu1_generator was not produced under $BUILD_DIR. Delete build/ and rerun ./scripts/setup.sh."
    fi
fi

if [[ -z "$generator_exe" || ! -x "$generator_exe" ]]; then
    die 'ps2x_vu1_generator not found. Run this script without --skip-generator-build first.'
fi

# ── 2. VU1 native parts (host generator, 13 parts) ────────────────────────
write_step 'Generating VU1 parts (host generator, 13 parts)'
mkdir -p "$VU1_PARTS_DIR"
generator_start=$SECONDS
invoke_native "$generator_exe" "$VU1_PARTS_DIR" "$VU1_MICROCODE_DIR"
generator_seconds=$((SECONDS - generator_start))

expected_vu1_parts=()
for part in 0 1 2 3 4 5 6 7 8 9 10 11 12; do
    expected_vu1_parts+=("$VU1_PARTS_DIR/programa$part.cpp")
done
missing_vu1_parts=()
for p in "${expected_vu1_parts[@]}"; do
    if [[ ! -f "$p" ]]; then missing_vu1_parts+=("$p"); fi
done
if [[ ${#missing_vu1_parts[@]} -gt 0 ]]; then
    missing_joined="$(IFS=', '; echo "${missing_vu1_parts[*]}")"
    die "The VU1 generator did not produce all 13 parts. Missing: $missing_joined"
fi
stale_count=0
for f in "$VU1_PARTS_DIR"/programa*.cpp; do
    [[ -e "$f" ]] || continue
    keep=0
    for p in "${expected_vu1_parts[@]}"; do
        if [[ "$p" == "$f" ]]; then keep=1; break; fi
    done
    if [[ $keep -eq 0 ]]; then
        rm -f -- "$f"
        stale_count=$((stale_count + 1))
    fi
done
if [[ $stale_count -gt 0 ]]; then
    echo "Removed $stale_count stale VU1 part(s)."
fi

# ── 3. VU1 lean parts (vu1n_gen.py, 6 parts) ──────────────────────────────
write_step 'Generating VU1 lean parts (tools/vu1/vu1n_gen.py, 6 parts)'
mkdir -p "$VU1_GEN_DIR"
invoke_native "$py_exe" -I "$VU1N_GEN" "$VU1_GEN_DIR" "$VU1_MICROCODE_DIR" --heads "$VU1N_HEADS" --parts 6

expected_vu1n_parts=()
for part in 0 1 2 3 4 5; do
    expected_vu1n_parts+=("$VU1_GEN_DIR/vu1n_part$part.cpp")
done
missing_vu1n_parts=()
for p in "${expected_vu1n_parts[@]}"; do
    if [[ ! -f "$p" ]]; then missing_vu1n_parts+=("$p"); fi
done
if [[ ${#missing_vu1n_parts[@]} -gt 0 ]]; then
    missing_joined="$(IFS=', '; echo "${missing_vu1n_parts[*]}")"
    die "vu1n_gen.py did not produce all 6 parts. Missing: $missing_joined"
fi
stale_count=0
for f in "$VU1_GEN_DIR"/vu1n_part*.cpp; do
    [[ -e "$f" ]] || continue
    keep=0
    for p in "${expected_vu1n_parts[@]}"; do
        if [[ "$p" == "$f" ]]; then keep=1; break; fi
    done
    if [[ $keep -eq 0 ]]; then
        rm -f -- "$f"
        stale_count=$((stale_count + 1))
    fi
done
if [[ $stale_count -gt 0 ]]; then
    echo "Removed $stale_count stale VU1 lean part(s)."
fi

# ── 4. IOP modules (iopn_gen.py) ──────────────────────────────────────────
write_step 'Generating native IOP modules (tools/iop/iopn_gen.py)'
mkdir -p "$IOP_GEN_DIR"
invoke_native "$py_exe" -I "$IOPN_GEN" "$IOP_MODULES_DIR" "$IOP_GEN_DIR"

generated_iop=()
while IFS= read -r f; do generated_iop+=("$f"); done \
    < <(find "$IOP_GEN_DIR" -maxdepth 1 -type f -name 'iopn_*.cpp' | sort)
registry_path="$IOP_GEN_DIR/iopn_registry.cpp"
if [[ ! -f "$registry_path" ]]; then
    die "iopn_gen.py did not write iopn_registry.cpp. Check modules.txt ($modules_txt) and the dump folder."
fi
if [[ ${#generated_iop[@]} -le 1 ]]; then
    die 'iopn_gen.py produced no module files; every module in modules.txt was skipped (size mismatch?).'
fi

# The engine CMake globs ps2xIOP/src/native/iopn_*.cpp (CONFIGURE_DEPENDS), so
# the files must live inside the engine checkout; they are gitignored there.
if [[ ! -d "$IOP_NATIVE_DIR" ]]; then
    die "PS2Recomp is not checked out at $RECOMP_DIR (missing ps2xIOP/src/native). Run ./scripts/setup.sh."
fi
write_step 'Copying IOP modules into PS2Recomp/ps2xIOP/src/native'
generated_names=()
for f in "${generated_iop[@]}"; do generated_names+=("$(basename -- "$f")"); done
iop_updated=0
for f in "${generated_iop[@]}"; do
    destination="$IOP_NATIVE_DIR/$(basename -- "$f")"
    if [[ -f "$destination" ]] && test_same_content "$f" "$destination"; then continue; fi
    cp -f -- "$f" "$destination"
    touch "$destination"
    iop_updated=$((iop_updated + 1))
done
stale_iop_count=0
for f in "$IOP_NATIVE_DIR"/iopn_*.cpp; do
    [[ -e "$f" ]] || continue
    base="$(basename -- "$f")"
    keep=0
    for n in "${generated_names[@]}"; do
        if [[ "$n" == "$base" ]]; then keep=1; break; fi
    done
    if [[ $keep -eq 0 ]]; then
        rm -f -- "$f"
        stale_iop_count=$((stale_iop_count + 1))
    fi
done
if [[ $stale_iop_count -gt 0 ]]; then
    echo "Removed $stale_iop_count stale IOP module file(s)."
fi
iop_module_count=0
for n in "${generated_names[@]}"; do
    if [[ "$n" != 'iopn_registry.cpp' ]]; then iop_module_count=$((iop_module_count + 1)); fi
done

# ── 5. Summary ────────────────────────────────────────────────────────────
write_step 'Summary'
pattern_count() {
    local n
    n="$(find "$1" -maxdepth 1 -type f -name "$2" | wc -l)"
    trim "$n"
}
pattern_bytes() {
    find "$1" -maxdepth 1 -type f -name "$2" -printf '%s\n' | awk '{ s += $1 } END { print s + 0 }'
}
fmt_mb_1() {
    awk -v b="$1" 'BEGIN { printf "%.1f", b / 1048576 }'
}
vu1n_count="$(pattern_count "$VU1_GEN_DIR" 'vu1n_part*.cpp')"
vu1n_bytes="$(pattern_bytes "$VU1_GEN_DIR" 'vu1n_part*.cpp')"
vu1_parts_count="$(pattern_count "$VU1_PARTS_DIR" 'programa*.cpp')"
vu1_parts_bytes="$(pattern_bytes "$VU1_PARTS_DIR" 'programa*.cpp')"
printf 'VU1 parts (host generator): %s files, %s MB in generated/vu1n-parts (%s s)\n' \
    "$vu1_parts_count" "$(fmt_mb_1 "$vu1_parts_bytes")" "$generator_seconds"
printf 'VU1 lean parts (vu1n_gen):  %s files, %s MB in generated/vu1n\n' \
    "$vu1n_count" "$(fmt_mb_1 "$vu1n_bytes")"
printf 'IOP native modules:         %s modules + registry, %s file(s) updated in PS2Recomp/ps2xIOP/src/native\n' \
    "$iop_module_count" "$iop_updated"
echo ''
echo 'Android build must receive these CMake variables:'
echo "  -DPS2X_ANDROID_NATIVE_VU1_DIR=$VU1_GEN_DIR"
echo "  -DPS2X_ANDROID_NATIVE_VU1_PARTS_DIR=$VU1_PARTS_DIR"
echo '  (the IOP modules are compiled by the engine from PS2Recomp/ps2xIOP/src/native)'
echo ''
echo 'Nothing game-derived was written to the repository: generated/ and the'
echo 'iopn_*.cpp files inside PS2Recomp/ are gitignored. Only code is committed.'
