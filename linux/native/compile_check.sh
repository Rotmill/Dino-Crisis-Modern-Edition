#!/usr/bin/env bash
# Compile every runtime translation unit of the native target without the game's recompiled code.
# Usage (inside the build container or on a Linux host):
#   linux/native/compile_check.sh <psxrecomp framework with the mod patch applied> [file filter]
# The framework is the tree the installer prepares: posdiff-patched psxrecomp + mod/src copied in.
set -uo pipefail
FW=${1:?framework dir}
FILTER=${2:-}
OUT=${OUT:-/tmp/dcme-compile-check}
mkdir -p "$OUT"
SDL_CFLAGS=$(pkg-config --cflags sdl2)
AV_CFLAGS=$(pkg-config --cflags libavformat libavcodec libavutil libswscale libswresample 2>/dev/null || true)
DEFS=(-D_GNU_SOURCE -DPSX_NATIVE_BUILD=1 -DPSX_ENABLE_BLOCK_CYCLES=1 -DPSX_HAS_GAME_DISPATCH=1 -DPSX_HLE_BIOS=1
      -DPSX_NO_DEBUG_TOOLS=1 -DMCM_NO_IMGUI -DMCM_NO_PT -DDCME_RELEASE -DFMT_HEADER_ONLY=1 -DSDL_MAIN_HANDLED "-DFMT_CONSTEVAL="
      -DDEFAULT_DEBUG_PORT=4472 '-DPSX_DEFAULT_BIOS_PATH="bios/SCPH1001.BIN"' '-DPSX_DEFAULT_GAME_CONFIG_PATH="game.toml"'
      '-DPSX_WINDOW_TITLE="Dino Crisis"' '-DDCME_DATA_PAK_PATH="dcme_data.pak"')
INC=(-I"$FW/runtime/include" -I"$FW/recompiler/src" -I"$FW/recompiler/lib/fmt/include" -I"$FW/recompiler/lib/toml11"
     -I"$FW/runtime/deps/stb")
CC=${CC:-cc}; CXX=${CXX:-c++}
fail=0; n=0
SRCS=$(sed -n '/set(PSXRECOMP_RUNTIME_SOURCES/,/^)/p' "$FW/runtime/runtime.cmake" | grep -o '/runtime/src/[^ ]*\|/recompiler/src/[^ ]*' )
SRCS="$SRCS /runtime/src/bios_hle.c /runtime/src/hle_dispatch.c /runtime/src/stub_interpreter.c /runtime/src/dc_png.cpp /runtime/src/dc_win32_posix.c"
for rel in $SRCS; do
  src="$FW$rel"
  [ -n "$FILTER" ] && [[ "$rel" != *$FILTER* ]] && continue
  [ -f "$src" ] || { echo "MISSING $rel"; fail=1; continue; }
  n=$((n+1))
  obj="$OUT/$(basename "$src").o"
  case "$src" in
    *.c)   cmd=("$CC"  -std=gnu11  -O0 -w -c "$src" -o "$obj") ;;
    *.cpp) cmd=("$CXX" -std=gnu++20 -O0 -w -include exception -c "$src" -o "$obj") ;;
  esac
  if ! "${cmd[@]}" "${DEFS[@]}" "${INC[@]}" $SDL_CFLAGS $AV_CFLAGS > "$OUT/$(basename "$src").log" 2>&1; then
    echo "FAIL $rel ($(grep -c 'error' "$OUT/$(basename "$src").log") errors)"; fail=1
  else
    echo "ok   $rel"
  fi
done
echo "compiled $n files, failures: $fail"
exit $fail
