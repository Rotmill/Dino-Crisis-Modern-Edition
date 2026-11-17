#!/usr/bin/env bash
set -euo pipefail

ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
TMP=$(cd -- "$TMP" && pwd -P)

python3 "$ROOT/linux/native_port_check.py"

python3 - "$TMP/dcme_data.pak" <<'PY'
import pathlib, struct, sys
out = pathlib.Path(sys.argv[1])
name = b"test/hello.txt"
data = b"hello"
header_size = 12 + 2 + len(name) + 1 + 8
out.write_bytes(
    b"DCMEPAK1" + struct.pack("<I", 1) +
    struct.pack("<H", len(name)) + name + b"\0" +
    struct.pack("<II", header_size, len(data)) + data + b"\0"
)
PY

cc -std=c11 -D_DEFAULT_SOURCE -Wall -Wextra -Werror -pthread \
  -I"$ROOT/mod/src/include" \
  "$ROOT/mod/src/runtime/dc_pak.c" \
  "$ROOT/mod/src/runtime/dc_win32_posix.c" \
  "$ROOT/linux/tests/test_native_pack.c" \
  -o "$TMP/test-native-pack"

DCME_DATA_PAK="$TMP/dcme_data.pak" "$TMP/test-native-pack"

cc -std=c11 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Werror -pthread \
  -I"$ROOT/mod/src/include" \
  "$ROOT/linux/tests/test_dc_platform.c" \
  -o "$TMP/test-dc-platform"
"$TMP/test-dc-platform"

cc -std=gnu11 -D_GNU_SOURCE -Wall -Wextra -Werror -pthread \
  -I"$ROOT/mod/src/include" \
  "$ROOT/mod/src/runtime/dc_win32_posix.c" \
  "$ROOT/linux/tests/test_win32_posix.c" \
  -o "$TMP/test-win32-posix"
mkdir "$TMP/w32"
"$TMP/test-win32-posix" "$TMP/w32"
echo "native foundation tests: ok"
