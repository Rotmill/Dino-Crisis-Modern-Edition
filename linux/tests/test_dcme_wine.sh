#!/usr/bin/env bash
set -euo pipefail

HERE=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
SCRIPT=$(cd -- "$HERE/.." && pwd)/dcme-wine
TMP=$(mktemp -d)
TMP=$(cd -- "$TMP" && pwd -P)
trap 'rm -rf "$TMP"' EXIT

mkdir -p "$TMP/bin" "$TMP/package/installer" \
  "$TMP/package/mod/tools/python" \
  "$TMP/package/mod/tools/llvm/bin" \
  "$TMP/package/mod/vulkan/Include/vulkan" \
  "$TMP/package/mod/build" "$TMP/install"
touch "$TMP/package/installer/dcme_install.py" \
  "$TMP/package/mod/tools/python/python.exe" \
  "$TMP/package/mod/tools/llvm/bin/x86_64-w64-mingw32-clang.exe" \
  "$TMP/package/mod/vulkan/Include/vulkan/vulkan.h" \
  "$TMP/package/mod/build/game.plan.json" "$TMP/disc.bin"

cat >"$TMP/bin/winepath" <<'EOF'
#!/usr/bin/env bash
[[ $1 == -w ]]
printf 'Z:%s\n' "$2"
EOF
cat >"$TMP/bin/wine" <<'EOF'
#!/usr/bin/env bash
printf '%s\n' "$@" >"$DCME_TEST_LOG"
EOF
chmod +x "$TMP/bin/wine" "$TMP/bin/winepath"

export PATH="$TMP/bin:$PATH" DCME_TEST_LOG="$TMP/wine.log"
"$SCRIPT" --package "$TMP/package" --prefix "$TMP/prefix" install "$TMP/disc.bin" "$TMP/install"

grep -Fx -- '-DiscPath' "$TMP/wine.log" >/dev/null
grep -Fx -- "Z:$TMP/disc.bin" "$TMP/wine.log" >/dev/null
grep -Fx -- '-InstallPath' "$TMP/wine.log" >/dev/null
grep -Fx -- "Z:$TMP/install" "$TMP/wine.log" >/dev/null
grep -Fx -- '-NoShortcut' "$TMP/wine.log" >/dev/null

touch "$TMP/install/DinoCrisisME.exe"
"$SCRIPT" --prefix "$TMP/prefix" run "$TMP/install" --test-argument
grep -Fx -- "$TMP/install/DinoCrisisME.exe" "$TMP/wine.log" >/dev/null
grep -Fx -- '--test-argument' "$TMP/wine.log" >/dev/null

if "$SCRIPT" --package "$TMP/empty" install "$TMP/disc.bin" "$TMP/install" >/dev/null 2>&1; then
  echo "expected an incomplete package to fail" >&2
  exit 1
fi

echo "dcme-wine tests: ok"
