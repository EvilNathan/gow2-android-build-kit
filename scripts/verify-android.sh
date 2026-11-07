#!/usr/bin/env bash
# Linux port of verify-android.ps1.
#
# Static checks on the built APK: packaging, ABI, 16 KB page alignment and
# the launcher activity. No device is needed; run it after build-android.sh.
#   ./scripts/verify-android.sh [--apk <path>] [--configuration Debug|Release]
. "$(dirname -- "${BASH_SOURCE[0]}")/_common.sh"

APK=''
CONFIGURATION='Debug'
while [[ $# -gt 0 ]]; do
    case "$1" in
        --apk) APK="$2"; shift 2 ;;
        --apk=*) APK="${1#*=}"; shift ;;
        --configuration) CONFIGURATION="$2"; shift 2 ;;
        --configuration=*) CONFIGURATION="${1#*=}"; shift ;;
        *) die "Unknown argument: $1" ;;
    esac
done
case "$CONFIGURATION" in
    Debug|Release) ;;
    *) die "--configuration must be Debug or Release (got '$CONFIGURATION')." ;;
esac

if [[ -z "$APK" ]]; then
    if [[ "$CONFIGURATION" == 'Release' ]]; then
        apk_name='app-release.apk'
        apk_subdir='release'
    else
        apk_name='app-debug.apk'
        apk_subdir='debug'
    fi
    APK="$REPO_ROOT/android-runner/app/build/outputs/apk/$apk_subdir/$apk_name"
fi
if [[ ! -f "$APK" ]]; then
    die "APK not found: $APK. Run ./scripts/build-android.sh first."
fi
APK="$(cd -- "$(dirname -- "$APK")" && pwd)/$(basename -- "$APK")"

ANDROID_HOME="$(default_android_sdk)"
export ANDROID_HOME
if [[ ! -d "$ANDROID_HOME" ]]; then
    die 'Android SDK not found. Set ANDROID_HOME.'
fi

py_exe="$(get_python)"

failures=()
check() {
    local name="$1" ok="$2" detail="$3"
    if [[ "$ok" -eq 1 ]]; then
        printf '  PASS  %-14s %s\n' "$name" "$detail"
    else
        printf '  FAIL  %-14s %s\n' "$name" "$detail"
        failures+=("$name")
    fi
}
fmt_mb() {
    awk -v b="$1" 'BEGIN { printf "%.1f", b / 1048576 }'
}

write_step "APK: $APK"
size="$(stat -c %s -- "$APK")"
ok=0
if [[ $size -gt 20971520 ]]; then ok=1; fi
check 'apk-size' "$ok" "$(fmt_mb "$size") MB"

# ---------------------------------------------------------------- badging
aapt2=''
while IFS= read -r dir; do
    if [[ -x "$dir/aapt2" ]]; then
        aapt2="$dir/aapt2"
        break
    fi
done < <(find "$ANDROID_HOME/build-tools" -mindepth 1 -maxdepth 1 -type d 2>/dev/null | sort -Vr)
if [[ -z "$aapt2" ]]; then
    die 'aapt2 not found in ANDROID_HOME/build-tools.'
fi

badging="$("$aapt2" dump badging "$APK" 2>&1 || true)"
package_line="$(printf '%s\n' "$badging" | grep -m 1 '^package:' || true)"
launch_line="$(printf '%s\n' "$badging" | grep -m 1 'launchable-activity:' || true)"
native_line="$(printf '%s\n' "$badging" | grep -m 1 'native-code:' || true)"
sdk_line="$(printf '%s\n' "$badging" | grep -m 1 '^minSdkVersion:' || true)"

ok=0
if [[ "$package_line" == *"name='com.gow2.android'"* ]]; then ok=1; fi
check 'package' "$ok" "$package_line"

version_name=''
if [[ "$package_line" =~ versionName=\'([^\']+)\' ]]; then
    version_name="${BASH_REMATCH[1]}"
fi
ok=0
if [[ -n "$version_name" ]]; then ok=1; fi
check 'version' "$ok" "versionName=$version_name"

ok=0
if [[ "$launch_line" == *"name='com.gow2.android.MainActivity'"* ]]; then ok=1; fi
check 'launcher' "$ok" "$launch_line"

ok=0
if [[ "$native_line" == *'arm64-v8a'* && "$native_line" != *'x86'* && "$native_line" != *'armeabi'* ]]; then ok=1; fi
check 'abi' "$ok" "$native_line"

ok=0
if [[ "$sdk_line" == *"minSdkVersion:'28'"* ]]; then ok=1; fi
check 'minSdk-28' "$ok" "$sdk_line"

# --------------------------------------------------------------- contents
# The APK is a zip; list entries (name + uncompressed size) with python3
# (System.IO.Compression on Windows).
entries="$("$py_exe" -c '
import sys, zipfile
with zipfile.ZipFile(sys.argv[1]) as z:
    for info in z.infolist():
        print(f"{info.filename}\t{info.file_size}")
' "$APK")"

abis=()
runner_size=''
while IFS=$'\t' read -r entry_name entry_size; do
    case "$entry_name" in
        lib/*/*)
            rest="${entry_name#lib/}"
            abi="${rest%%/*}"
            already=0
            for a in "${abis[@]}"; do
                if [[ "$a" == "$abi" ]]; then already=1; break; fi
            done
            if [[ $already -eq 0 ]]; then abis+=("$abi"); fi
            ;;
    esac
    if [[ "$entry_name" == 'lib/arm64-v8a/libps2EntryRunner.so' ]]; then
        runner_size="$entry_size"
    fi
done <<< "$entries"

ok=0
if [[ ${#abis[@]} -eq 1 && "${abis[0]}" == 'arm64-v8a' ]]; then ok=1; fi
abis_joined="$(IFS=,; echo "${abis[*]}")"
check 'only-arm64' "$ok" "$abis_joined"

ok=0
runner_detail='missing'
if [[ -n "$runner_size" ]]; then
    ok=1
    runner_detail="$(fmt_mb "$runner_size") MB"
fi
check 'runner-so' "$ok" "$runner_detail"

# Drop the library on disk for readelf (it lives inside the zip).
readelf=''
if [[ -n "${ANDROID_NDK_HOME:-}" && -x "$ANDROID_NDK_HOME/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-readelf" ]]; then
    readelf="$ANDROID_NDK_HOME/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-readelf"
else
    while IFS= read -r ndk_dir; do
        candidate="$ndk_dir/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-readelf"
        if [[ -x "$candidate" ]]; then
            readelf="$candidate"
            break
        fi
    done < <(find "$ANDROID_HOME/ndk" -mindepth 1 -maxdepth 1 -type d 2>/dev/null | sort -Vr)
fi

if [[ -n "$runner_size" && -n "$readelf" ]]; then
    temp_so="$(mktemp --suffix=.so)"
    # shellcheck disable=SC2064
    trap "rm -f -- '$temp_so'" EXIT
    "$py_exe" -c '
import sys, zipfile
with zipfile.ZipFile(sys.argv[1]) as z:
    data = z.read("lib/arm64-v8a/libps2EntryRunner.so")
with open(sys.argv[2], "wb") as f:
    f.write(data)
' "$APK" "$temp_so"

    headers="$("$readelf" -lW "$temp_so" 2>&1 || true)"
    loads_count="$(printf '%s\n' "$headers" | grep -cE '^[[:space:]]*LOAD' || true)"
    aligned_count="$(printf '%s\n' "$headers" | grep -E '^[[:space:]]*LOAD' | grep -cE '0x(4000|10000|20000)[[:space:]]*$' || true)"
    ok=0
    if [[ $loads_count -gt 0 && $aligned_count -eq $loads_count ]]; then ok=1; fi
    check 'load-segments' "$ok" "$aligned_count/$loads_count LOAD at >=16 KB"

    machine="$("$readelf" -hW "$temp_so" 2>&1 | grep -m 1 'Machine:' || true)"
    machine="$(trim "$machine")"
    ok=0
    if [[ "$machine" == *'AArch64'* ]]; then ok=1; fi
    check 'machine' "$ok" "$machine"

    rm -f -- "$temp_so"
    trap - EXIT
else
    echo '  SKIP  readelf (native library or NDK toolchain unavailable)'
fi

write_step 'Summary'
if [[ ${#failures[@]} -gt 0 ]]; then
    failures_joined="$(IFS=', '; echo "${failures[*]}")"
    echo "FAILED: $failures_joined"
    exit 1
fi
echo "All APK checks passed: $APK"
