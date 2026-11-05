#!/usr/bin/env bash
# Linux port of recompile.ps1.
#
# Translates the game ELF to C++ and syncs generated code + overrides into
# the PS2Recomp runner.
#
#   ./scripts/recompile.sh [--sync-only] [--aggressive-logs]
#
# --sync-only       Do not recompile: only re-copy generated/ and overrides/
#                   into the runner (after editing an override).
# --aggressive-logs Reconfigure CMake with AGGRESSIVE_LOGS (off by default;
#                   costs ~11% of the game's time).
#
# Optional native speed-ups from the reference project (captured on a PC with
# a GL 4.5 GPU):
#   GOW_VU1_MICROCODIGO = folder with the VU1 microcode capture
#   GOW_IOP_MODULOS     = folder with modules.txt from PS2X_IOP_DUMP_MODULES
# Both are game-derived and stay local. VU1 native is disabled for Android for
# now (SSE generator).
. "$(dirname -- "${BASH_SOURCE[0]}")/_common.sh"

SYNC_ONLY=0
AGGRESSIVE_LOGS=0
while [[ $# -gt 0 ]]; do
    case "$1" in
        --sync-only) SYNC_ONLY=1; shift ;;
        --aggressive-logs) AGGRESSIVE_LOGS=1; shift ;;
        *) die "Unknown argument: $1" ;;
    esac
done

elf_path="$(get_game_elf_path)"

if [[ $SYNC_ONLY -eq 0 ]]; then
    recomp="$(find_built_exe ps2xRecomp ps2_recomp)" \
        || die 'ps2_recomp is missing. Run ./scripts/build-tools.sh first.'
    if printf '%s' "$REPO_ROOT" | LC_ALL=C grep -q '[^ -~]'; then
        die "The repository path contains non-ASCII characters: $REPO_ROOT"
    fi

    write_step 'Recompiling the ELF'
    toml="$(cat -- "$TEMPLATE_TOML")"
    toml="${toml//@ELF@/$elf_path}"
    toml="${toml//@MAP@/$FUNCMAP_CSV}"
    toml="${toml//@OUT@/$GENERATED_DIR/}"
    mkdir -p "$(dirname -- "$EFFECTIVE_TOML")"
    write_utf8_no_bom "$EFFECTIVE_TOML" "$toml"

    if [[ -e "$GENERATED_DIR" ]]; then rm -rf -- "$GENERATED_DIR"; fi
    mkdir -p "$GENERATED_DIR"

    ( cd "$REPO_ROOT" && invoke_native "$recomp" "$EFFECTIVE_TOML" )

    unit_count=0
    for f in "$GENERATED_DIR"/*.cpp; do
        [[ -e "$f" ]] || continue
        unit_count=$((unit_count + 1))
    done
    echo "Generated $unit_count C++ units in $GENERATED_DIR"
fi

# ── Native IOP modules (optional) ─────────────────────────────────────────
iop_native_dir="$RECOMP_DIR/ps2xIOP/src/native"
if [[ -n "${GOW_IOP_MODULOS:-}" ]]; then
    if [[ ! -f "$GOW_IOP_MODULOS/modules.txt" ]]; then
        die "GOW_IOP_MODULOS has no modules.txt: $GOW_IOP_MODULOS"
    fi
    write_step 'Native IOP modules'
    iop_temp="$BUILD_DIR/iopn-gen"
    if [[ -e "$iop_temp" ]]; then rm -rf -- "$iop_temp"; fi
    mkdir -p "$iop_temp"
    py_exe="$(get_python)"
    invoke_native "$py_exe" -I "$REPO_ROOT/tools/iop/iopn_gen.py" "$GOW_IOP_MODULOS" "$iop_temp"
    fresh=()
    for f in "$iop_temp"/iopn_*.cpp; do
        [[ -e "$f" ]] || continue
        fresh+=("$f")
    done
    if [[ -d "$iop_native_dir" ]]; then
        for f in "$iop_native_dir"/iopn_*.cpp; do
            [[ -e "$f" ]] || continue
            base="$(basename -- "$f")"
            keep=0
            for n in "${fresh[@]}"; do
                if [[ "$(basename -- "$n")" == "$base" ]]; then keep=1; break; fi
            done
            if [[ $keep -eq 0 ]]; then rm -f -- "$f"; fi
        done
    fi
    iop_copied=0
    for f in "${fresh[@]}"; do
        dest="$iop_native_dir/$(basename -- "$f")"
        if [[ -f "$dest" ]] && cmp -s -- "$dest" "$f"; then continue; fi
        cp -f -- "$f" "$dest"
        iop_copied=$((iop_copied + 1))
    done
    echo "Native IOP: ${#fresh[@]} files, $iop_copied updated."
else
    if [[ -d "$iop_native_dir" ]]; then
        for f in "$iop_native_dir"/iopn_*.cpp; do
            [[ -e "$f" ]] || continue
            rm -f -- "$f"
        done
    fi
    echo 'Native IOP modules: none (set GOW_IOP_MODULOS to enable).'
fi

# ── Sync into the runner ──────────────────────────────────────────────────
if [[ ! -d "$GENERATED_DIR" ]]; then
    die 'generated/ is empty. Run recompile.sh without --sync-only first.'
fi

sync_runner_sources

# ── Reconfigure CMake so new files are part of the project ────────────────
cmake="$(get_cmake)"
set_generator_args
cmake_args=(-S "$RECOMP_DIR" -B "$BUILD_DIR" "${CMAKE_GENERATOR_ARGS[@]}"
    -DCMAKE_BUILD_TYPE=Release
    -DPS2X_BUILD_TEST=OFF
    -DPS2X_BUILD_STUDIO=OFF
)
if [[ $AGGRESSIVE_LOGS -eq 1 ]]; then
    cmake_args+=(-DPS2X_ENABLE_AGRESSIVE_LOGS=ON)
else
    cmake_args+=(-DPS2X_ENABLE_AGRESSIVE_LOGS=OFF)
fi
vu1_micro="${GOW_VU1_MICROCODIGO:-}"
if [[ -n "$vu1_micro" ]]; then
    echo "Native VU1 from $vu1_micro"
fi
vu1_gen_source=''
vu1n_override=''
vu1n_heads=''
if [[ -n "$vu1_micro" ]]; then
    vu1_gen_source="$REPO_ROOT/tools/vu1/generar_vu1.cpp"
    vu1n_override="$REPO_ROOT/tools/vu1/vu1n_gen.py"
    vu1n_heads="$REPO_ROOT/tools/vu1/vu1n_heads.txt"
fi
cmake_args+=("-DPS2X_VU1_MICROCODE_DIR=$vu1_micro")
cmake_args+=("-DPS2X_VU1_GENERATOR_SOURCE=$vu1_gen_source")
cmake_args+=("-DPS2X_VU1N_GEN_OVERRIDE=$vu1n_override")
cmake_args+=("-DPS2X_VU1N_HEADS_FILE=$vu1n_heads")
if [[ -f "$BUILD_DIR/CMakeCache.txt" ]]; then
    invoke_native "$cmake" "${cmake_args[@]}" >/dev/null
fi

write_step 'Done'
echo 'Desktop runner: ./scripts/run-headless.sh'
echo 'Android APK:    ./scripts/build-android.sh'
