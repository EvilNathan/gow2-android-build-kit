#!/usr/bin/env bash
# Linux port of build-tools.ps1: builds the host recompiler tools
# (ps2_analyzer, ps2_recomp) with CMake + Ninja/Makefiles instead of MSVC.
#   ./scripts/build-tools.sh [--aggressive-logs]
. "$(dirname -- "${BASH_SOURCE[0]}")/_common.sh"

AGGRESSIVE_LOGS=0
while [[ $# -gt 0 ]]; do
    case "$1" in
        --aggressive-logs) AGGRESSIVE_LOGS=1; shift ;;
        *) die "Unknown argument: $1" ;;
    esac
done

ensure_engine
apply_engine_patches

cmake="$(get_cmake)"
set_generator_args

write_step 'Configuring PS2Recomp tools'
configure=(-S "$RECOMP_DIR" -B "$BUILD_DIR" "${CMAKE_GENERATOR_ARGS[@]}"
    -DCMAKE_BUILD_TYPE=Release
    -DPS2X_BUILD_TEST=OFF
    -DPS2X_BUILD_STUDIO=OFF
)
if [[ $AGGRESSIVE_LOGS -eq 1 ]]; then
    configure+=(-DPS2X_ENABLE_AGRESSIVE_LOGS=ON)
else
    configure+=(-DPS2X_ENABLE_AGRESSIVE_LOGS=OFF)
fi
invoke_native "$cmake" "${configure[@]}"

write_step 'Building ps2_analyzer and ps2_recomp'
# Single-config generators have no --config Release; the Release build type is
# set at configure time above instead (see -DCMAKE_BUILD_TYPE=Release).
invoke_native "$cmake" --build "$BUILD_DIR" --target ps2_analyzer ps2_recomp --parallel

recomp="$(find_built_exe ps2xRecomp ps2_recomp)" || die "ps2_recomp was not produced under $BUILD_DIR/ps2xRecomp"
analyzer="$(find_built_exe ps2xAnalyzer ps2_analyzer)" || die "ps2_analyzer was not produced under $BUILD_DIR/ps2xAnalyzer"

echo ''
echo 'Tools ready:'
echo "  $analyzer"
echo "  $recomp"
