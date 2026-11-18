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
  $([ "$(uname)" = Darwin ] && echo -Wl,-U,_dc_mods_dir) \
  -o "$TMP/test-native-pack"

DCME_DATA_PAK="$TMP/dcme_data.pak" "$TMP/test-native-pack"

# 18.11.2026 regression (Steam Deck: "the own kernel needs exe in game.toml"): the installed layout - the pack next to
# the executable, the game working in data/ - must find the pack without DCME_DATA_PAK.
mkdir -p "$TMP/game/data" "$TMP/stage"
printf '[game]\nname = "t"\nexe = "SLUS_009.22"\n' > "$TMP/stage/game.toml"
python3 - "$TMP/stage" "$TMP/game/dcme_data.pak" <<'PY'
import os, struct, sys
stage, out = sys.argv[1], sys.argv[2]
names = sorted(os.listdir(stage)); blobs = [open(os.path.join(stage, n), 'rb').read() for n in names]
head = 12 + sum(2 + len(n.encode()) + 1 + 8 for n in names)
idx, body, off = b"", b"", head
for n, b in zip(names, blobs):
    idx += struct.pack("<H", len(n.encode())) + n.encode() + b"\0" + struct.pack("<II", off, len(b))
    body += b + b"\0"; off += len(b) + 1
open(out, "wb").write(b"DCMEPAK1" + struct.pack("<I", len(names)) + idx + body)
PY
cp "$TMP/test-native-pack" "$TMP/game/"
(cd "$TMP/game/data" && env -u DCME_DATA_PAK "$TMP/game/test-native-pack" game)
mkdir -p "$TMP/nopak/data"
cp "$TMP/test-native-pack" "$TMP/nopak/"
(cd "$TMP/nopak/data" && env -u DCME_DATA_PAK "$TMP/nopak/test-native-pack" none 2> "$TMP/nopak.err")
grep -q "dcme_data.pak not found" "$TMP/nopak.err"

python3 -I "$ROOT/linux/tests/test_linux_installer.py"
python3 -I "$ROOT/linux/tests/test_cli.py"

cc -std=c11 -Wall -Wextra -Werror -DDC_HUD_NO_SDL \
  -I"$ROOT/mod/src/include" \
  "$ROOT/mod/src/runtime/dc_hud.c" \
  "$ROOT/linux/tests/test_hud.c" \
  -o "$TMP/test-hud"
"$TMP/test-hud"

# 18.11.2026 regression (Steam Deck with an 8BitDo Ultimate): every gamepad is watched, the one in use is named.
# SDL virtual gamepads, no hardware; skipped where SDL2 is not installed.
if SDL_FLAGS=$(pkg-config --cflags --libs sdl2 2>/dev/null || sdl2-config --cflags --libs 2>/dev/null); then
  # shellcheck disable=SC2086
  cc -std=c11 -Wall -Wextra -Werror -I"$ROOT/mod/src/include" \
    "$ROOT/mod/src/runtime/dc_pads.c" "$ROOT/linux/tests/test_pads.c" $SDL_FLAGS -o "$TMP/test-pads"
  SDL_VIDEODRIVER=dummy "$TMP/test-pads"
else
  echo "gamepad watch test: skipped (no SDL2)"
fi

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
