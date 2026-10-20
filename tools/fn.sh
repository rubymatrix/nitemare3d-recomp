#!/bin/sh
# Print annotated decompilation of the named functions.
ROOT=$(cd "$(dirname "$0")/.." && pwd)
for f in "$@"; do awk -v f="$f" '/^\/\/ ==== /{p = ($3 == f || $5 == f)} p' "$ROOT/re/nite3w.c"; done
