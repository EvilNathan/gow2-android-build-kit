#!/usr/bin/env bash
# Linux port of run-headless.ps1: boots the game runner (desktop or Android)
# and captures logs for diagnostic verification.
#   ./scripts/run-headless.sh [--seconds <n>] [--build]
#   ./scripts/run-headless.sh --android [--seconds <n>]
#
# The Windows -WindowHandle parameter has no Linux equivalent and is dropped.
. "$(dirname -- "${BASH_SOURCE[0]}")/_common.sh"

SECONDS_TO_RUN=60
ANDROID=0
BUILD=0
while [[ $# -gt 0 ]]; do
    case "$1" in
        --seconds) SECONDS_TO_RUN="$2"; shift 2 ;;
        --seconds=*) SECONDS_TO_RUN="${1#*=}"; shift ;;
        --android) ANDROID=1; shift ;;
        --build) BUILD=1; shift ;;
        *) die "Unknown argument: $1" ;;
    esac
done

mkdir -p "$LOG_DIR"

if [[ $ANDROID -eq 1 ]]; then
    adb="$(get_adb)" || die 'adb not found. Run ./scripts/setup.sh or set ANDROID_HOME.'
    log="$LOG_DIR/android-headless.log"

    write_step "Launching on the device, capturing logcat for $SECONDS_TO_RUN s"
    invoke_native "$adb" shell am force-stop com.gow2.android
    invoke_native "$adb" logcat -c
    invoke_native "$adb" shell am start -n 'com.gow2.android/.MainActivity'

    "$adb" logcat -s 'ps2x:V' 'AndroidRuntime:E' 'DEBUG:E' 'libc:F' > "$log" &
    logcat_pid=$!
    sleep "$SECONDS_TO_RUN"
    kill "$logcat_pid" 2>/dev/null || true
    wait "$logcat_pid" 2>/dev/null || true
    tail -n 40 "$log" || true
    echo "Log: $log"
    exit 0
fi

exe=''
if [[ $BUILD -eq 0 ]]; then
    exe="$(find_built_exe ps2xRuntime ps2EntryRunner || true)"
fi
if [[ $BUILD -eq 1 || -z "$exe" ]]; then
    cmake="$(get_cmake)"
    set_generator_args
    write_step 'Configuring and building the desktop runner'
    configure=(-S "$RECOMP_DIR" -B "$BUILD_DIR" "${CMAKE_GENERATOR_ARGS[@]}"
        -DCMAKE_BUILD_TYPE=Release
        -DPS2X_BUILD_TEST=OFF
        -DPS2X_BUILD_STUDIO=OFF
    )
    invoke_native "$cmake" "${configure[@]}"
    invoke_native "$cmake" --build "$BUILD_DIR" --target ps2EntryRunner --parallel
    exe="$(find_built_exe ps2xRuntime ps2EntryRunner)" \
        || die "ps2EntryRunner was not produced under $BUILD_DIR/ps2xRuntime"
fi

GOW2_ISO=''
if [[ -f "$ISO_PATH_FILE" ]]; then
    GOW2_ISO="$(trim "$(head -n 1 "$ISO_PATH_FILE")")"
fi
export GOW2_ISO
if [[ -z "$GOW2_ISO" || ! -f "$GOW2_ISO" ]]; then
    die 'No ISO recorded. Run ./scripts/extract-disc.sh --iso "<path>.iso".'
fi

elf="$(get_game_elf_path)"
if [[ ! -f "$elf" ]]; then
    die "Game ELF not found at $elf. Run ./scripts/extract-disc.sh --iso \"<path>.iso\"."
fi

out_log="$LOG_DIR/desktop-headless.out.log"
err_log="$LOG_DIR/desktop-headless.err.log"

write_step "Running the desktop runner for $SECONDS_TO_RUN s (ELF: $elf, ISO: $GOW2_ISO)"
exe_dir="$(dirname -- "$exe")"
# Like the .ps1 (Start-Process -WorkingDirectory <exe dir>), the runner runs
# from its own folder; setsid gives it its own process group for the timeout
# kill (the equivalent of taskkill /T).
( cd "$exe_dir" && exec setsid "$exe" "$elf" ) > "$out_log" 2> "$err_log" &
proc_pid=$!
# setsid puts the runner in its own session/process group (pgid == pid) when
# it is not already a group leader; fall back to the plain pid otherwise.
proc_pgid="$(ps -o pgid= -p "$proc_pid" 2>/dev/null | tr -d ' ' || true)"
if [[ -z "$proc_pgid" ]]; then proc_pgid="$proc_pid"; fi

elapsed=0
while kill -0 "$proc_pid" 2>/dev/null && [[ $elapsed -lt $SECONDS_TO_RUN ]]; do
    sleep 1
    elapsed=$((elapsed + 1))
done
if kill -0 "$proc_pid" 2>/dev/null; then
    kill -KILL -- "-$proc_pgid" 2>/dev/null || kill -KILL "$proc_pid" 2>/dev/null || true
    wait "$proc_pid" 2>/dev/null || true
    echo 'Stopped (timeout).'
else
    exit_code=0
    wait "$proc_pid" || exit_code=$?
    echo "Exited early with code $exit_code."
fi
tail -n 40 "$out_log" 2>/dev/null || true
tail -n 20 "$err_log" 2>/dev/null || true
echo "Logs: $out_log / $err_log"
