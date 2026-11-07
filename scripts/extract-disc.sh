#!/usr/bin/env bash
# Linux port of extract-disc.ps1.
#
# Reads the boot ELF (and the IOP modules) from your own ISO, or extracts the
# whole disc.
#
# By default only the boot ELF and the root *.IRX modules are copied to
# games/gow2/disc/. That is all the recompiler and the Android build need: the
# runtime reads the game data straight from the ISO (IoPaths.cdImage), so the
# 8 GB disc is never duplicated.
#
# Use --full to extract every file plus the sector layout. The desktop runner
# only needs that if you want to run without GOW2_ISO pointing at the image.
#
#   ./scripts/extract-disc.sh --iso "<path>.iso" [--full]
. "$(dirname -- "${BASH_SOURCE[0]}")/_common.sh"

ISO=''
FULL=0
while [[ $# -gt 0 ]]; do
    case "$1" in
        --iso) ISO="$2"; shift 2 ;;
        --iso=*) ISO="${1#*=}"; shift ;;
        --full) FULL=1; shift ;;
        *) die "Unknown argument: $1" ;;
    esac
done
if [[ -z "$ISO" ]]; then
    die 'Usage: ./scripts/extract-disc.sh --iso "<path>.iso" [--full]'
fi
if [[ ! -f "$ISO" ]]; then
    die "ISO not found: $ISO"
fi
iso_path="$(cd -- "$(dirname -- "$ISO")" && pwd)/$(basename -- "$ISO")"

py_exe="$(get_python)"

write_step 'Reading the disc image'
mkdir -p "$DISC_DIR"

listing="$("$py_exe" -I "$REPO_ROOT/tools/iso_extract.py" "$iso_path" --list)" \
    || die "iso_extract.py failed on $iso_path"

names=()
while IFS= read -r line; do
    if [[ "$line" =~ ^[[:space:]]*([0-9]+)[[:space:]]+([0-9]+)[[:space:]]+(.+)$ ]]; then
        names+=("$(trim "${BASH_REMATCH[3]}")")
    fi
done <<< "$listing"

has_cnf=0
for n in "${names[@]}"; do
    if [[ "$n" == 'SYSTEM.CNF' ]]; then has_cnf=1; break; fi
done
if [[ $has_cnf -eq 0 ]]; then
    die "SYSTEM.CNF was not found in $iso_path. Is this a PS2 disc image?"
fi

if [[ $FULL -eq 1 ]]; then
    write_step 'Extracting the full disc (this copies ~8 GB)'
    invoke_native "$py_exe" -I "$REPO_ROOT/tools/iso_extract.py" "$iso_path" "$DISC_DIR" --layout
else
    invoke_native "$py_exe" -I "$REPO_ROOT/tools/iso_extract.py" "$iso_path" "$DISC_DIR" --only 'SYSTEM.CNF'
fi

cnf_path="$DISC_DIR/SYSTEM.CNF"
if [[ ! -f "$cnf_path" ]]; then
    die "SYSTEM.CNF is missing in $DISC_DIR. Is this a PS2 disc image?"
fi
cnf="$(cat -- "$cnf_path")"
elf_name="$(printf '%s' "$cnf" | sed -n 's/.*BOOT2[[:space:]]*=[[:space:]]*cdrom0:\\\([^;[:space:]]*\).*/\1/p' | head -n 1)"
if [[ -z "$elf_name" ]]; then
    die "Cannot read BOOT2 from SYSTEM.CNF:
$cnf"
fi

if [[ $FULL -eq 0 ]]; then
    want=("$elf_name")
    for n in "${names[@]}"; do
        if [[ "$n" =~ ^[^/]+\.IRX$ ]]; then want+=("$n"); fi
    done
    write_step "Extracting the boot ELF and $((${#want[@]} - 1)) IOP modules"
    only_list="$(IFS=,; echo "${want[*]}")"
    invoke_native "$py_exe" -I "$REPO_ROOT/tools/iso_extract.py" "$iso_path" "$DISC_DIR" --only "$only_list"
fi

# The game opens IOP_MOD/NAME.IRX relative to the ELF; on the disc they live in the root.
iop_mod="$DISC_DIR/IOP_MOD"
mkdir -p "$iop_mod"
for f in "$DISC_DIR"/*.IRX; do
    [[ -e "$f" ]] || continue
    cp -f -- "$f" "$iop_mod/"
done

printf '%s\n' "$elf_name" > "$ELF_NAME_FILE"
write_utf8_no_bom "$ISO_PATH_FILE" "$iso_path"

elf_path="$DISC_DIR/$elf_name"
sha1="$(sha1sum -- "$elf_path" | awk '{print $1}' | tr 'a-f' 'A-F')"

write_step 'Done'
echo "ELF:  $elf_path"
echo "SHA1: $sha1"
echo "ISO:  $iso_path (read directly; no full extraction needed)"
echo 'The reference ELF (SCUS_974.81 v1.01) is E6A62A0F3E781C29513F563F4625B9B3A9103C56.'
