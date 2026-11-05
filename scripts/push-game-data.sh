#!/usr/bin/env bash
# Linux port of push-game-data.ps1: pushes the extracted ELF and the ISO to
# the app files directory on the connected device via adb.
#   ./scripts/push-game-data.sh [--skip-iso] [--boot-elf-name <name>]
. "$(dirname -- "${BASH_SOURCE[0]}")/_common.sh"

SKIP_ISO=0
BOOT_ELF_NAME=''
while [[ $# -gt 0 ]]; do
    case "$1" in
        --skip-iso) SKIP_ISO=1; shift ;;
        --boot-elf-name) BOOT_ELF_NAME="$2"; shift 2 ;;
        --boot-elf-name=*) BOOT_ELF_NAME="${1#*=}"; shift ;;
        *) die "Unknown argument: $1" ;;
    esac
done

adb="$(get_adb)" || die 'adb not found. Run ./scripts/setup.sh or set ANDROID_HOME.'
devices="$("$adb" devices | awk 'NR > 1 && $2 == "device" {print}')"
if [[ -z "$devices" ]]; then
    die 'No Android device connected (adb devices shows none).'
fi

elf_path="$(get_game_elf_path)"
if [[ -z "$BOOT_ELF_NAME" ]]; then
    BOOT_ELF_NAME="$(trim "$(head -n 1 "$ELF_NAME_FILE")")"
fi
remote_dir='/sdcard/Android/data/com.gow2.android/files'

write_step 'Creating the app files directory'
invoke_native "$adb" shell mkdir -p "$remote_dir"

write_step "Pushing $BOOT_ELF_NAME"
invoke_native "$adb" push "$elf_path" "$remote_dir/$BOOT_ELF_NAME"

if [[ $SKIP_ISO -eq 0 ]]; then
    if [[ ! -f "$ISO_PATH_FILE" ]]; then
        die 'iso-path.txt is missing. Run ./scripts/extract-disc.sh --iso "<path>.iso" first.'
    fi
    iso="$(trim "$(head -n 1 "$ISO_PATH_FILE")")"
    if [[ ! -f "$iso" ]]; then
        die "The ISO recorded in iso-path.txt no longer exists: $iso"
    fi
    size="$(stat -c %s -- "$iso")"
    size_gb="$(awk -v b="$size" 'BEGIN { printf "%.2f", b / 1073741824 }')"
    write_step "Pushing the ISO ($size_gb GB) - this takes a while"
    invoke_native "$adb" push "$iso" "$remote_dir/gow2.iso"
fi

write_step 'Done'
echo "Data: $remote_dir"
echo 'Next: ./scripts/run-headless.sh --android'
