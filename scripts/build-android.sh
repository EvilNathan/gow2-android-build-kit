#!/usr/bin/env bash
# Linux port of build-android.ps1: builds the Android APK with the Gradle
# wrapper (./gradlew) and copies it to dist/.
#   ./scripts/build-android.sh [--configuration Debug|Release]
#                              [--boot-elf-name <name>] [--null-gs[=true|false]]
#                              [--clean] [--install]
. "$(dirname -- "${BASH_SOURCE[0]}")/_common.sh"

CONFIGURATION='Debug'
BOOT_ELF_NAME=''
NULL_GS=0
CLEAN=0
INSTALL=0
while [[ $# -gt 0 ]]; do
    case "$1" in
        --configuration) CONFIGURATION="$2"; shift 2 ;;
        --configuration=*) CONFIGURATION="${1#*=}"; shift ;;
        --boot-elf-name) BOOT_ELF_NAME="$2"; shift 2 ;;
        --boot-elf-name=*) BOOT_ELF_NAME="${1#*=}"; shift ;;
        --null-gs) NULL_GS=1; shift ;;
        --null-gs=true) NULL_GS=1; shift ;;
        --null-gs=false) NULL_GS=0; shift ;;
        --clean) CLEAN=1; shift ;;
        --install) INSTALL=1; shift ;;
        *) die "Unknown argument: $1" ;;
    esac
done
case "$CONFIGURATION" in
    Debug|Release) ;;
    *) die "--configuration must be Debug or Release (got '$CONFIGURATION')." ;;
esac

ensure_engine
apply_engine_patches
sync_runner_sources

runner_has_cpp=0
if [[ -d "$RUNNER_SRC_DIR" ]]; then
    for f in "$RUNNER_SRC_DIR"/*.cpp; do
        [[ -e "$f" ]] || continue
        runner_has_cpp=1
        break
    done
fi
if [[ $runner_has_cpp -eq 0 ]]; then
    die 'PS2Recomp/ps2xRuntime/src/runner is empty. Run ./scripts/recompile.sh first.'
fi

reg_funcs="$RUNNER_SRC_DIR/register_functions.cpp"
reg_size=0
if [[ -f "$reg_funcs" ]]; then reg_size="$(stat -c %s -- "$reg_funcs")"; fi
if [[ ! -f "$reg_funcs" || $reg_size -lt 10000 ]]; then
    die 'PS2Recomp/ps2xRuntime/src/runner/register_functions.cpp is missing or is an empty stub. Run ./scripts/recompile.sh.'
fi

if [[ -z "$BOOT_ELF_NAME" ]]; then
    if [[ -f "$ELF_NAME_FILE" ]]; then
        BOOT_ELF_NAME="$(trim "$(head -n 1 "$ELF_NAME_FILE")")"
    else
        BOOT_ELF_NAME='SCUS_974.81'
    fi
fi

write_step 'Android SDK'
ANDROID_HOME="$(default_android_sdk)"
export ANDROID_HOME
if [[ ! -d "$ANDROID_HOME" ]]; then
    die "Android SDK not found. Set ANDROID_HOME (currently '$ANDROID_HOME')."
fi
export ANDROID_SDK_ROOT="$ANDROID_HOME"
local_props="$REPO_ROOT/android-runner/local.properties"
if [[ ! -f "$local_props" ]]; then
    write_utf8_no_bom "$local_props" "sdk.dir=$ANDROID_HOME
"
fi
echo "SDK: $ANDROID_HOME"

gradlew="$REPO_ROOT/android-runner/gradlew"
if [[ ! -f "$gradlew" ]]; then
    die 'android-runner/gradlew is missing. Restore it from the kit repository.'
fi
if [[ ! -x "$gradlew" ]]; then chmod +x "$gradlew"; fi
if ! command -v java >/dev/null 2>&1; then
    die 'Java 17 is required (AGP 8.6.1). Install a JDK 17 and put java on PATH or set JAVA_HOME.'
fi

if [[ $CLEAN -eq 1 ]]; then
    write_step 'Cleaning Android build outputs'
    for dir in 'android-runner/app/build' 'android-runner/app/.cxx' 'android-runner/build'; do
        if [[ -e "$REPO_ROOT/$dir" ]]; then rm -rf -- "$REPO_ROOT/$dir"; fi
    done
fi

if [[ $NULL_GS -eq 1 ]]; then null_gs_value='ON'; else null_gs_value='OFF'; fi
if [[ "$CONFIGURATION" == 'Release' ]]; then task='assembleRelease'; else task='assembleDebug'; fi
mkdir -p "$LOG_DIR"

write_step "Building the $CONFIGURATION APK (arm64-v8a, null GS: $null_gs_value)"
gradle_args=(
    -p "$REPO_ROOT/android-runner"
    "$task"
    --console plain
    "-Pps2xBootElfName=$BOOT_ELF_NAME"
    "-Pps2xNullGs=$null_gs_value"
)
invoke_native "$gradlew" "${gradle_args[@]}"

if [[ "$CONFIGURATION" == 'Release' ]]; then
    apk_name='app-release.apk'
    apk_dir='release'
else
    apk_name='app-debug.apk'
    apk_dir='debug'
fi
apk="$REPO_ROOT/android-runner/app/build/outputs/apk/$apk_dir/$apk_name"
if [[ ! -f "$apk" ]]; then
    die "Gradle finished but $apk was not produced."
fi

dist="$REPO_ROOT/dist"
mkdir -p "$dist"
config_lower="$(printf '%s' "$CONFIGURATION" | tr '[:upper:]' '[:lower:]')"
dist_apk="$dist/gow2-android-$config_lower.apk"
cp -f -- "$apk" "$dist_apk"
echo "APK: $dist_apk"

if [[ $INSTALL -eq 1 ]]; then
    adb="$(get_adb)" || die 'adb not found (ANDROID_HOME/platform-tools/adb).'
    write_step 'Installing on the device'
    invoke_native "$adb" install -r "$dist_apk"
    echo 'Installed. Push data with ./scripts/push-game-data.sh, then ./scripts/run-headless.sh --android'
fi

write_step 'Done'
echo 'Device data: ./scripts/push-game-data.sh'
echo 'Boot on device: ./scripts/run-headless.sh --android'
