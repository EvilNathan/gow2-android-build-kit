#!/usr/bin/env bash
# Linux port of setup.ps1: initializes the PS2Recomp submodule at the pinned
# revision and applies the kit patch stack.
#   ./scripts/setup.sh [--force-engine]
. "$(dirname -- "${BASH_SOURCE[0]}")/_common.sh"

FORCE_ENGINE=0
while [[ $# -gt 0 ]]; do
    case "$1" in
        --force-engine) FORCE_ENGINE=1; shift ;;
        *) die "Unknown argument: $1" ;;
    esac
done

write_step 'Checking prerequisites'
require_command git 'Install Git and restart the shell.' >/dev/null
require_command cmake 'Install CMake 3.24 or newer.' >/dev/null
get_python >/dev/null

if [[ ! -d "$REPO_ROOT/.git" ]]; then
    die 'setup.sh must run inside the kit git checkout.'
fi

if [[ $FORCE_ENGINE -eq 1 && -e "$RECOMP_DIR" ]]; then
    write_step 'Removing the engine checkout'
    rm -rf -- "$RECOMP_DIR"
fi

write_step 'Engine submodule (PS2Recomp)'
if [[ ! -e "$RECOMP_DIR/.git" ]]; then
    if [[ -e "$RECOMP_DIR" ]]; then rm -rf -- "$RECOMP_DIR"; fi
    invoke_native git -C "$REPO_ROOT" submodule update --init --recursive
fi
if [[ ! -e "$RECOMP_DIR/.git" ]]; then
    die "Could not initialize $RECOMP_DIR. Check network access to github.com."
fi

head="$(git -C "$RECOMP_DIR" rev-parse HEAD)" || die 'Cannot read the PS2Recomp revision.'
head="$(trim "$head")"
if [[ "$head" != "$ENGINE_PIN" ]]; then
    echo "PS2Recomp is at $head; checking out $ENGINE_PIN."
    if ! git -C "$RECOMP_DIR" cat-file -e "$ENGINE_PIN^{commit}" 2>/dev/null; then
        if ! git -C "$RECOMP_DIR" fetch origin "$ENGINE_PIN"; then
            invoke_native git -C "$RECOMP_DIR" fetch origin
        fi
    fi
    invoke_native git -C "$RECOMP_DIR" checkout --quiet "$ENGINE_PIN"
fi

apply_engine_patches

write_step 'Done'
echo "Engine:  $RECOMP_DIR ($ENGINE_PIN)"
echo 'Next:    ./scripts/extract-disc.sh --iso "<your God of War II (USA).iso>"'
