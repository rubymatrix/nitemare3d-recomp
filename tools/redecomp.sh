#!/bin/sh
# Re-run Ghidra on NITE3W.EXE: import, apply names/prototypes, export C + asm, annotate.
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
GHIDRA=${GHIDRA_INSTALL_DIR:-/opt/homebrew/opt/ghidra/libexec}
export JAVA_HOME=${JAVA_HOME:-/opt/homebrew/opt/openjdk@21}
mkdir -p "$ROOT/ghidra_proj" "$ROOT/decomp" "$ROOT/recomp/export"
"$GHIDRA/support/analyzeHeadless" "$ROOT/ghidra_proj" nite3w -import "$ROOT/original/NITE3W.EXE" -overwrite \
  -scriptPath "$ROOT/tools/ghidra_scripts" \
  -postScript FindMissingFuncs.java \
  -postScript ApplyNames.java "$ROOT/re/names.txt" \
  -postScript ExportDecomp.java "$ROOT/decomp" \
  -postScript ExportAsm.java "$ROOT/decomp" \
  -postScript ExportRecomp.java "$ROOT/recomp/export" > "$ROOT/ghidra_proj/log.txt" 2>&1 || { tail -30 "$ROOT/ghidra_proj/log.txt"; exit 1; }
grep -E "ApplyNames.java>|FindMissingFuncs.java>|ERROR" "$ROOT/ghidra_proj/log.txt" || true
python3 -I "$ROOT/tools/annotate.py" "$ROOT/decomp/decomp.c" "$ROOT/re/nite3w.c"
python3 -I "$ROOT/tools/annotate.py" "$ROOT/decomp/nite3w.asm" "$ROOT/re/nite3w.asm"
