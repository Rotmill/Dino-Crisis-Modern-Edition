#!/usr/bin/env bash
# Every positional patch applies the way the installer applies it (framework patch, mod/src, each version patch).
# Needs the pristine framework at the pinned commit: PSXRECOMP_PRISTINE=<dir> (an unpacked GitHub archive).
set -euo pipefail
ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
PRISTINE=${PSXRECOMP_PRISTINE:?set PSXRECOMP_PRISTINE to psxrecomp at the commit pinned in installer/dcme_install.py}
TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT
cp -R "$PRISTINE" "$TMP/base"
python3 -I "$ROOT/mod/tools/posdiff.py" apply "$ROOT/mod/patches/psxrecomp-dcme.dcpatch" "$TMP/base"
cp "$ROOT"/mod/src/runtime/* "$TMP/base/runtime/src/"
cp "$ROOT"/mod/src/include/* "$TMP/base/runtime/include/"
n=0
for v in "$ROOT"/mod/versions/*/; do
  [ -f "$v/runtime.dcpatch" ] || continue
  rm -rf "$TMP/v"; cp -R "$TMP/base" "$TMP/v"
  python3 -I "$ROOT/mod/tools/posdiff.py" apply "$v/runtime.dcpatch" "$TMP/v" > /dev/null
  n=$((n+1))
done
echo "patches: framework + $n versions apply: ok"
