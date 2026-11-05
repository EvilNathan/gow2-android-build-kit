#!/usr/bin/env bash
# Linux port of build-all.ps1: end-to-end chain — setup, tools, extract,
# recompile, desktop test run, Android APK.
#   ./scripts/build-all.sh --iso "<path>.iso" [--configuration Debug|Release]
#                          [--skip-tools] [--skip-desktop-run] [--skip-android]
#                          [--install] [--push-data]
. "$(dirname -- "${BASH_SOURCE[0]}")/_common.sh"

ISO=''
CONFIGURATION='Debug'
SKIP_TOOLS=0
SKIP_DESKTOP_RUN=0
SKIP_ANDROID=0
INSTALL=0
PUSH_DATA=0
while [[ $# -gt 0 ]]; do
    case "$1" in
        --iso) ISO="$2"; shift 2 ;;
        --iso=*) ISO="${1#*=}"; shift ;;
        --configuration) CONFIGURATION="$2"; shift 2 ;;
        --configuration=*) CONFIGURATION="${1#*=}"; shift ;;
        --skip-tools) SKIP_TOOLS=1; shift ;;
        --skip-desktop-run) SKIP_DESKTOP_RUN=1; shift ;;
        --skip-android) SKIP_ANDROID=1; shift ;;
        --install) INSTALL=1; shift ;;
        --push-data) PUSH_DATA=1; shift ;;
        *) die "Unknown argument: $1" ;;
    esac
done
if [[ -z "$ISO" ]]; then
    die 'Usage: ./scripts/build-all.sh --iso "<path>.iso" [options]'
fi
case "$CONFIGURATION" in
    Debug|Release) ;;
    *) die "--configuration must be Debug or Release (got '$CONFIGURATION')." ;;
esac

"$SCRIPT_DIR/setup.sh"
if [[ $SKIP_TOOLS -eq 0 ]]; then "$SCRIPT_DIR/build-tools.sh"; fi
"$SCRIPT_DIR/extract-disc.sh" --iso "$ISO"
"$SCRIPT_DIR/recompile.sh"

if [[ $SKIP_DESKTOP_RUN -eq 0 ]]; then
    "$SCRIPT_DIR/run-headless.sh" --seconds 20 --build
fi

if [[ $SKIP_ANDROID -eq 0 ]]; then
    android_args=(--configuration "$CONFIGURATION")
    if [[ $INSTALL -eq 1 ]]; then android_args+=(--install); fi
    "$SCRIPT_DIR/build-android.sh" "${android_args[@]}"
    if [[ $PUSH_DATA -eq 1 ]]; then "$SCRIPT_DIR/push-game-data.sh"; fi
fi
