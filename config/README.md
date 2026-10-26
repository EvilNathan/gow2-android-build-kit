# config/

Recompiler configuration for GoW II. **Empty at the start: building it is phase 1** (`docs/ROADMAP.md`).

| File | What it is | Where it comes from |
|---|---|---|
| `gow.toml`, `gow_functions.csv` | Ghidra export (not uploaded: they contain local paths and are regenerated) | `scripts/ghidra-analyze.ps1` |
| `funcmap.csv` | Function map (`Name,Start,End,Size`) used by `recompile.ps1` | `py -3 tools/make_funcmap.py --extra out/scan_missing.txt`: the functions from the Ghidra CSV (without its internal labels) plus the ones Ghidra doesn't see (`tools/scan_missing_functions.py`). The order is in the docstring of `make_funcmap.py` |
| `recomp.template.toml` | `ps2_recomp` configuration with `@ELF@`, `@MAP@` and `@OUT@` (filled in by `recompile.ps1`): stubs, entry points and `[mmio]` table | Written by hand, with `referencia/gow1/config/recomp.template.toml` as the model and **GoW II's addresses** |
