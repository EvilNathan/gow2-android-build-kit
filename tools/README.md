# Helper tools

These scripts are optional diagnostics; the build does not depend on them.

## `toc.py`

Inspect or extract records from `GODOFWAR.TOC` after `extract-disc.ps1` has
copied the disc's boot files.

```powershell
python -I tools\toc.py games\gow2\disc\GODOFWAR.TOC list
python -I tools\toc.py games\gow2\disc\GODOFWAR.TOC find RHOD
python -I tools\toc.py games\gow2\disc\GODOFWAR.TOC extract RHOD10.WAD RHOD10.WAD
```

`extract` expects `PART1.PAK` and `PART2.PAK` beside the TOC. The retail USA
disc places those PAK files outside the ISO9660 root, so `extract-disc.ps1`
does not copy them; pass `disc.py`'s PAK extraction output if you need this
command. It is included mainly to document the `(partition, sector)` layout
used by the runtime's `host1:` service.
