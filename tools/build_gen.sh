#!/bin/sh
# Regenerate src/gen/ from original/NITE3W.EXE:
#   Ghidra analysis + export  ->  NE relocation scan  ->  p-code recompiler  ->  data image.
# Requires Ghidra 12.x (GHIDRA_INSTALL_DIR) and a JDK 21 (JAVA_HOME).
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
if [ ! -f "$ROOT/original/NITE3W.EXE" ]; then
  echo "original/NITE3W.EXE not found (run tools/extract_installer.py first)" >&2
  exit 1
fi
"$ROOT/tools/redecomp.sh"
python3 -I "$ROOT/tools/ne_relocs.py" > /dev/null
python3 -I "$ROOT/tools/recomp.py"
python3 -I "$ROOT/tools/gen_data.py"
