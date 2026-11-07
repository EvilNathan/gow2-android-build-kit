# shellcheck shell=bash
# Shared paths and helpers for every script in the kit (Linux port of
# _common.ps1). Source it with:  . "$(dirname -- "${BASH_SOURCE[0]}")/_common.sh"
#
# Differences from the Windows version:
#   * No Visual Studio / vswhere: cmake, ninja/make and the compilers are
#     looked up on PATH and the scripts fail with a clear message if missing.
#   * Host builds use a single-config CMake generator (Ninja preferred, Unix
#     Makefiles as fallback), so built executables land directly in
#     build/<subdir>/ instead of build/<subdir>/Release/ and have no .exe
#     suffix. find_built_exe() checks both layouts.
#   * The Android SDK defaults to $HOME/Android/Sdk (or $ANDROID_HOME /
#     $ANDROID_SDK_ROOT when set) instead of C:\Android\android-sdk.

set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd -- "$SCRIPT_DIR/.." && pwd)"
RECOMP_DIR="$REPO_ROOT/PS2Recomp"
ENGINE_PIN='379fa48b1a292eb7f688ccdc202d97f5ffc09c6d'
PATCHES_DIR="$REPO_ROOT/patches"
COMPAT_DIR="$REPO_ROOT/compat"
BUILD_DIR="$REPO_ROOT/build"
LOG_DIR="$BUILD_DIR/logs"
GAME_DIR="$REPO_ROOT/games/gow2"
DISC_DIR="$GAME_DIR/disc"
ELF_NAME_FILE="$GAME_DIR/elf-name.txt"
ISO_PATH_FILE="$GAME_DIR/iso-path.txt"
TEMPLATE_TOML="$REPO_ROOT/config/recomp.template.toml"
FUNCMAP_CSV="$REPO_ROOT/config/funcmap.csv"
EFFECTIVE_TOML="$BUILD_DIR/gow.effective.toml"
GENERATED_DIR="$REPO_ROOT/generated/gow2"
OVERRIDES_DIR="$REPO_ROOT/overrides"
RUNNER_SRC_DIR="$RECOMP_DIR/ps2xRuntime/src/runner"
SYNC_MANIFEST="$BUILD_DIR/synced-files.txt"

# Filled in by set_generator_args().
CMAKE_GENERATOR_ARGS=()

die() {
    echo "error: $*" >&2
    exit 1
}

write_step() {
    echo ''
    echo "== $1"
}

trim() {
    local s="$1"
    s="${s#"${s%%[![:space:]]*}"}"
    s="${s%"${s##*[![:space:]]}"}"
    printf '%s' "$s"
}

require_command() {
    local name="$1" hint="${2:-}"
    local cmd
    if cmd="$(command -v "$name" 2>/dev/null)"; then
        printf '%s\n' "$cmd"
        return 0
    fi
    local message="Missing '$name'."
    if [[ -n "$hint" ]]; then message+=" $hint"; fi
    die "$message"
}

get_cmake() {
    local cmd
    if cmd="$(command -v cmake 2>/dev/null)"; then
        printf '%s\n' "$cmd"
        return 0
    fi
    die 'CMake not found. Install CMake 3.24+ (e.g. sudo apt install cmake) and make sure it is on PATH.'
}

# Fills CMAKE_GENERATOR_ARGS for host (desktop) configures. The Windows
# scripts pick the newest Visual Studio via vswhere; on Linux there is no
# Visual Studio, so prefer Ninja and fall back to Unix Makefiles.
set_generator_args() {
    CMAKE_GENERATOR_ARGS=()
    if command -v ninja >/dev/null 2>&1; then
        CMAKE_GENERATOR_ARGS=(-G Ninja)
    elif command -v make >/dev/null 2>&1; then
        CMAKE_GENERATOR_ARGS=(-G 'Unix Makefiles')
    else
        die 'No CMake generator available. Install ninja (preferred) or make.'
    fi
}

# Echoes the path of a built host executable: find_built_exe <subdir> <name>.
# Checks the single-config layout (build/<subdir>/<name>) first, then the
# multi-config layout (build/<subdir>/Release/<name>).
find_built_exe() {
    local rel_dir="$1" name="$2"
    if [[ -x "$BUILD_DIR/$rel_dir/$name" ]]; then
        printf '%s\n' "$BUILD_DIR/$rel_dir/$name"
        return 0
    fi
    if [[ -x "$BUILD_DIR/$rel_dir/Release/$name" ]]; then
        printf '%s\n' "$BUILD_DIR/$rel_dir/Release/$name"
        return 0
    fi
    return 1
}

get_python() {
    local cmd
    if cmd="$(command -v python3 2>/dev/null)"; then
        printf '%s\n' "$cmd"
        return 0
    fi
    if cmd="$(command -v python 2>/dev/null)"; then
        printf '%s\n' "$cmd"
        return 0
    fi
    die "Missing 'python3'. Install Python 3.10 or newer."
}

get_game_elf_path() {
    if [[ ! -f "$ELF_NAME_FILE" ]]; then
        die 'The disc has not been extracted. Run: ./scripts/extract-disc.sh --iso "<path>.iso"'
    fi
    local name
    name="$(trim "$(head -n 1 "$ELF_NAME_FILE")")"
    printf '%s\n' "$DISC_DIR/$name"
}

write_utf8_no_bom() {
    # Bash writes no BOM; $2 is written verbatim (no trailing newline added).
    printf '%s' "$2" > "$1"
}

invoke_native() {
    local exe="$1"
    shift
    local rc=0
    "$exe" "$@" || rc=$?
    if [[ $rc -ne 0 ]]; then
        die "Failed: $exe $* (exit $rc)"
    fi
}

test_engine_ready() {
    [[ -f "$RECOMP_DIR/CMakeLists.txt" ]]
}

ensure_engine() {
    if test_engine_ready; then return 0; fi

    if [[ ! -d "$REPO_ROOT/.git" ]]; then
        die "PS2Recomp/ is missing and this is not a git checkout. Clone the kit or put PS2Recomp/ at $ENGINE_PIN."
    fi

    write_step 'Engine submodule (PS2Recomp)'
    invoke_native git -C "$REPO_ROOT" submodule update --init --recursive
    if ! test_engine_ready; then
        die 'PS2Recomp/ is still empty after submodule update. Check network access to github.com.'
    fi
}

set_engine_lf_checkout() {
    # The kit patches are LF. A clone made with core.autocrlf=true checks the
    # engine out as CRLF, which makes `git apply` fail on hunks with accented
    # comments. Force LF for this checkout; existing files keep their bytes
    # until they are refreshed from the index (see apply_engine_patches).
    if [[ ! -e "$RECOMP_DIR/.git" ]]; then return 0; fi
    git -C "$RECOMP_DIR" config core.autocrlf false >/dev/null 2>&1 || true
}

# Runs git with all output swallowed and returns only the exit code
# (`git apply --check` reports the "does not apply" case on stderr).
invoke_git_quiet() {
    git "$@" >/dev/null 2>&1
}

apply_engine_patches() {
    if [[ ! -d "$PATCHES_DIR" ]]; then return 0; fi
    local patches=()
    local patch
    for patch in "$PATCHES_DIR"/*.patch; do
        [[ -e "$patch" ]] || continue
        patches+=("$patch")
    done
    if [[ ${#patches[@]} -eq 0 ]]; then return 0; fi

    set_engine_lf_checkout

    # The patches are ordered and each one applies on top of the previous, so a
    # partially patched tree cannot be inspected patch by patch. Instead, drop
    # any tracked engine modification (the kit patches are the source of truth;
    # untracked files such as generated/ survive) and rebuild the stack from
    # the pinned revision. checkout-index also rewrites a CRLF clone as LF.
    local tracked
    tracked="$(git -C "$RECOMP_DIR" status --porcelain --untracked-files=no 2>/dev/null || true)"
    if [[ -n "$tracked" ]]; then
        echo '  resetting tracked engine changes'
        git -C "$RECOMP_DIR" checkout -- . 2>/dev/null || true
    fi
    git -C "$RECOMP_DIR" checkout-index -f -a 2>/dev/null || true

    write_step 'Kit patches'
    local rc
    for patch in "${patches[@]}"; do
        rc=0
        invoke_git_quiet -C "$RECOMP_DIR" apply --check --ignore-whitespace "$patch" || rc=$?
        if [[ $rc -ne 0 ]]; then
            die "Patch $(basename -- "$patch") does not apply to PS2Recomp at $ENGINE_PIN. Delete PS2Recomp/ and rerun setup.sh."
        fi
        invoke_native git -C "$RECOMP_DIR" apply --ignore-whitespace "$patch"
        echo "  applied $(basename -- "$patch")"
    done

    if [[ -f "$GENERATED_DIR/register_functions.cpp" ]]; then
        sync_runner_sources
    fi
}

sync_runner_sources() {
    local has_cpp=0
    local f
    if [[ -d "$GENERATED_DIR" ]]; then
        for f in "$GENERATED_DIR"/*.cpp; do
            [[ -e "$f" ]] || continue
            has_cpp=1
            break
        done
    fi
    if [[ $has_cpp -eq 0 ]]; then
        echo "WARNING: generated/ is empty ($GENERATED_DIR). Run ./scripts/recompile.sh first." >&2
        return 0
    fi

    write_step 'Syncing generated code and overrides'
    mkdir -p "$(dirname -- "$SYNC_MANIFEST")"
    mkdir -p "$RUNNER_SRC_DIR"

    local copied=()
    local changed=0
    local dest name
    for f in "$GENERATED_DIR"/*; do
        [[ -f "$f" ]] || continue
        name="$(basename -- "$f")"
        dest="$RUNNER_SRC_DIR/$name"
        if ! cmp -s -- "$f" "$dest" 2>/dev/null; then
            cp -f -- "$f" "$dest"
            touch "$dest"
            changed=$((changed + 1))
        fi
        copied+=("$dest")
    done
    for f in "$OVERRIDES_DIR"/*; do
        [[ -f "$f" ]] || continue
        name="$(basename -- "$f")"
        case "$name" in
            *.cpp|*.h) ;;
            *) continue ;;
        esac
        dest="$RUNNER_SRC_DIR/$name"
        if ! cmp -s -- "$f" "$dest" 2>/dev/null; then
            cp -f -- "$f" "$dest"
            touch "$dest"
            changed=$((changed + 1))
        fi
        copied+=("$dest")
    done

    local removed=0
    if [[ -f "$SYNC_MANIFEST" ]]; then
        local -A current=()
        local p old
        for p in "${copied[@]}"; do
            current["$p"]=1
        done
        while IFS= read -r old; do
            [[ -n "$old" ]] || continue
            if [[ -z "${current["$old"]:-}" && -e "$old" ]]; then
                rm -f -- "$old"
                removed=$((removed + 1))
            fi
        done < "$SYNC_MANIFEST"
    fi
    if [[ ${#copied[@]} -gt 0 ]]; then
        printf '%s\n' "${copied[@]}" > "$SYNC_MANIFEST"
    else
        : > "$SYNC_MANIFEST"
    fi
    echo "Runner: ${#copied[@]} files, $changed updated, $removed removed."
}

# Echoes the Android SDK location: $ANDROID_HOME, then $ANDROID_SDK_ROOT,
# then the usual Linux default $HOME/Android/Sdk.
default_android_sdk() {
    if [[ -n "${ANDROID_HOME:-}" ]]; then
        printf '%s\n' "$ANDROID_HOME"
    elif [[ -n "${ANDROID_SDK_ROOT:-}" ]]; then
        printf '%s\n' "$ANDROID_SDK_ROOT"
    else
        printf '%s\n' "$HOME/Android/Sdk"
    fi
}

get_adb() {
    local cmd
    if cmd="$(command -v adb 2>/dev/null)"; then
        printf '%s\n' "$cmd"
        return 0
    fi
    local sdk
    for sdk in "${ANDROID_HOME:-}" "${ANDROID_SDK_ROOT:-}" "$HOME/Android/Sdk"; do
        [[ -n "$sdk" ]] || continue
        if [[ -x "$sdk/platform-tools/adb" ]]; then
            printf '%s\n' "$sdk/platform-tools/adb"
            return 0
        fi
    done
    return 1
}
