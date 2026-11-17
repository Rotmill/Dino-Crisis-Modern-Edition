#!/usr/bin/env python3
"""Static gate for the Linux-native port.

This intentionally needs no game image or compiler.  It checks the portable
foundation already landed and reports the source modules that still contain
unisolated Windows APIs.  Exit 0 means the foundation is intact; blockers are
reported as roadmap data until they are moved behind platform boundaries.
"""
from __future__ import annotations

import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]


def require(path: str, pattern: str, message: str) -> None:
    text = (ROOT / path).read_text(encoding="utf-8")
    if not re.search(pattern, text, re.MULTILINE):
        raise AssertionError(f"{path}: {message}")


def main() -> int:
    require(
        "mod/project/CMakeLists.txt",
        r"find_package\(Vulkan REQUIRED\)",
        "native Vulkan dependency is not configured",
    )
    require(
        "mod/project/CMakeLists.txt",
        r'DCME_DATA_PAK_PATH="dcme_data\.pak"',
        "native external data pack is not configured",
    )
    require(
        "mod/src/runtime/dc_pak.c",
        r"getenv\(\"DCME_DATA_PAK\"\)",
        "native data-pack override is missing",
    )
    require(
        "mod/src/runtime/dc_pak.c",
        r'strcat\(exe, "mods/"\)',
        "native mod paths must start from mods/ next to the executable",
    )
    require(
        "mod/src/runtime/savestate.c",
        r"mkdir\(path, 0755\)",
        "native quicksave directory creation is missing",
    )
    require(
        "mod/src/include/dc_platform.h",
        r"clock_gettime\(CLOCK_MONOTONIC",
        "POSIX monotonic clock abstraction is missing",
    )

    require(
        "mod/project/CMakeLists.txt",
        r"dc_win32_posix\.c",
        "the Win32-over-POSIX layer is not built on Linux",
    )
    require(
        "mod/project/CMakeLists.txt",
        r"pkg_check_modules\(DCME_FFMPEG",
        "FFmpeg (movies from mods) is not linked on Linux",
    )
    require("mod/src/runtime/dc_pak.c", r"dc_exe_dir\(dir, sizeof dir\)",
            "dcme_data.pak must be found next to the executable (the game works in data/)")
    require("installer/dcme_install_linux.py", r"problems = verify_layout\(game_dir\)",
            "the Linux setup must check the finished game folder")
    require("mod/src/runtime/dc_movie.cpp", r"avformat_open_input", "dc_movie.cpp has no FFmpeg decoder")
    require("mod/src/runtime/dc_mods.c", r"live_snapshot", "the mods watcher has no POSIX twin")
    require("mod/src/runtime/dc_boot.c", r'#ifdef _WIN32\s*\n\s*FILE \*r = freopen\("NUL"',
            "stderr must not be sent to a file named NUL on Linux")

    # A Windows header is a blocker unless it sits inside a block that is only compiled on Windows. The scan follows
    # #if/#ifdef/#elif/#else/#endif nesting (enough for this code base, not a full preprocessor).
    win_header = re.compile(r"^\s*#\s*include\s*[<\"](?:windows|d3d11|dxgi[0-9_]*|wincodec|mfapi|mfidl|mfreadwrite"
                            r"|dbghelp|io|intrin|commdlg)\.h[>\"]", re.IGNORECASE)
    blockers = []
    for path in sorted((ROOT / "mod/src/runtime").glob("*")):
        if path.suffix not in {".c", ".cpp", ".h", ".inc"}:
            continue
        stack = []          # per open #if: "win" (only on Windows), "posix" (never on Windows) or "any"
        for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
            d = re.match(r"^\s*#\s*(ifdef|ifndef|if|elif|else|endif)\b(.*)", line)
            if d:
                kw, cond = d.group(1), d.group(2).replace(" ", "")
                win = "_WIN32" in cond or "_MSC_VER" in cond
                neg = kw == "ifndef" or "!defined(_WIN32)" in cond or "!defined(_MSC_VER)" in cond
                state = ("posix" if neg else "win") if win else "any"
                if kw in ("ifdef", "ifndef", "if"):
                    stack.append(state)
                elif kw == "elif" and stack:
                    stack[-1] = state
                elif kw == "else" and stack:
                    stack[-1] = {"win": "posix", "posix": "win"}.get(stack[-1], "any")
                elif kw == "endif" and stack:
                    stack.pop()
                continue
            if win_header.match(line) and "win" not in stack:
                blockers.append(path.relative_to(ROOT).as_posix())
                break

    print("native foundation: ok")
    print("unconditional Windows headers in mod/src/runtime: %d" % len(blockers))
    for path in blockers:
        print("  - " + path)
    return 1 if blockers else 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except AssertionError as exc:
        print("native foundation: FAILED: " + str(exc), file=sys.stderr)
        raise SystemExit(1)
