#!/usr/bin/env python3
"""
pgxp_transform.py — rewrite the recompiler-generated game C so the PGXP
address-keyed move-chain wrappers (runtime/include/pgxp_addr.h) see every
SXY-carrying data move: sw/lw/sh/lh/lhu/move/mfc2/mtc2/swc2/lwc2.

Idempotent: already-transformed lines (containing "pgxp_") are left alone.
A one-time backup <file>.pgxp_orig is written on first run; re-running
re-transforms from the BACKUP so the result never double-wraps.

Usage: python pgxp_transform.py <path-to-SLUS_full.c>
"""
import re
import shutil
import sys
from pathlib import Path

# (name, compiled regex, replacement) — applied in order, first match wins.
RULES = [
    # ⭐ 09.09.2026: регистры гостя в генкоде теперь локальные переменные gN
    # (см. reg_name в code_generator.cpp), а не cpu->gpr[N]. Обёртки, которым
    # нужно ЗНАЧЕНИЕ регистра, получают его отдельным параметром — из CPUState
    # его больше не достать.
    # swc2 SXY0/1/2 -> memory
    ("swc2_sxy",
     re.compile(r"^(\s*)psx_ww_f\((.+), cpu->gte_data\[(1[234])\]\);"
                r"(\s*/\* swc2 .*)$"),
     r"\1pgxp_swc2_sxy(cpu, \2, \3);\4"),
    # lwc2 memory -> SXY0/1/2
    ("lwc2_sxy",
     re.compile(r"^(\s*)cpu->gte_data\[(1[234])\] = psx_rw_f\((.+)\);"
                r"(\s*/\* lwc2 .*)$"),
     r"\1pgxp_lwc2_sxy(cpu, \3, \2);\4"),
    # mfc2 SXY0/1/2 -> gpr
    ("mfc2_sxy",
     re.compile(r"^(\s*)g(\d+) = cpu->gte_data\[(1[234])\];"
                r"(\s*/\* mfc2 \*/.*)$"),
     r"\1g\2 = pgxp_mfc2_sxy(cpu, \2, \3);\4"),
    # mtc2 gpr -> SXY0/1/2
    ("mtc2_sxy",
     re.compile(r"^(\s*)cpu->gte_data\[(1[234])\] = g(\d+);"
                r"(\s*/\* mtc2 \*/.*)$"),
     r"\1pgxp_mtc2_sxy_v(cpu, \2, \3, g\3);\4"),
    # sw gpr -> memory
    ("sw",
     re.compile(r"^(\s*)psx_ww_f\((.+), g(\d+)\);(.*)$"),
     r"\1pgxp_sw_v(cpu, \2, \3, g\3);\4"),
    # sh gpr -> memory
    ("sh",
     re.compile(r"^(\s*)psx_wh_f\((.+), \(uint16_t\)g(\d+)\);(.*)$"),
     r"\1pgxp_sh_v(cpu, \2, \3, g\3);\4"),
    # lw memory -> gpr
    ("lw",
     re.compile(r"^(\s*)g(\d+) = psx_rw_f\((.+)\);(.*)$"),
     r"\1g\2 = pgxp_lw(cpu, \3, \2);\4"),
    # lh (signed) memory -> gpr
    ("lh",
     re.compile(r"^(\s*)g(\d+) = \(int32_t\)\(int16_t\)"
                r"psx_rh_f\((.+)\);(.*)$"),
     r"\1g\2 = pgxp_lh(cpu, \3, \2);\4"),
    # lhu memory -> gpr
    ("lhu",
     re.compile(r"^(\s*)g(\d+) = psx_rh_f\((.+)\);(.*)$"),
     r"\1g\2 = pgxp_lhu(cpu, \3, \2);\4"),
    # register move (marked by the recompiler)
    ("move",
     re.compile(r"^(\s*)g(\d+) = g(\d+);(\s*/\* move \*/.*)$"),
     r"\1g\2 = pgxp_move_v(cpu, \2, \3, g\3);\4"),
]

# Residual patterns that would mean an SXY path we do NOT track — report them.
AUDIT = [
    ("gte15_write", re.compile(r"cpu->gte_data\[15\] =")),
    ("sxy_via_gte_read", re.compile(r"gte_read_data\(cpu, 1[234]\)")),
    ("untransformed_sw_gpr", re.compile(r"psx_ww_f\(.+, g\d+\);")),
    ("untransformed_sh_gpr", re.compile(r"psx_wh_f\(.+\(uint16_t\)g\d+\);")),
    ("untransformed_lw_gpr", re.compile(r"g\d+ = psx_rw_f\(")),
]


def main() -> int:
    if len(sys.argv) != 2:
        print(__doc__)
        return 2
    target = Path(sys.argv[1])
    backup = target.with_suffix(target.suffix + ".pgxp_orig")
    # ⛔⛔ ГРАБЛЯ 09.09.2026, СТОИВШАЯ ОДНОЙ ЛОЖНОЙ «ПЕРЕГЕНЕРАЦИИ».
    # Скрипт всегда читал из БЭКАПА (чтобы повторный прогон не заворачивал
    # обёртки дважды). Пока генератор не менялся, это было верно. Но после
    # смены генератора бэкап — августовский файл, и прогон МОЛЧА затирал
    # свежесгенерированный код старым: на выходе получался генкод другой
    # эпохи, без новых прологов, зато с адресными комментариями — глазами не
    # отличить. Поэтому решаем по САМОМУ target: если в нём ещё нет наших
    # обёрток, это свежий выход рекомпилятора и бэкап надо обновить.
    fresh = True
    if backup.exists():
        with open(target, "r", encoding="utf-8", errors="surrogateescape") as fh:
            for i, line in enumerate(fh):
                if "pgxp_" in line or "psx_rw_f(" in line:
                    fresh = False
                    break
                if i > 400000:
                    break
    if not backup.exists() or fresh:
        shutil.copy2(target, backup)
        print(f"backup refreshed from generator output: {backup}")
    else:
        print(f"re-transforming from existing backup: {backup}")

    # -------- fast memory access (09.09.2026) ---------------------------
    # Byte and word accesses still went through a FUNCTION POINTER in
    # CPUState (cpu->read_byte etc.) - an indirect branch the CPU cannot
    # predict, 8900 of them in the generated file. The pointers are bound
    # once in main.cpp and never change, so call the inline fast paths in
    # runtime/include/psx_fastmem.h instead: RAM hit = one load/store,
    # everything else falls back to the very same psx_* function.
    FAST = (
        ("cpu->read_byte(",  "psx_rb_f("),
        ("cpu->write_byte(", "psx_wb_f("),
        ("cpu->read_half(",  "psx_rh_f("),
        ("cpu->write_half(", "psx_wh_f("),
        ("cpu->read_word(",  "psx_rw_f("),
        ("cpu->write_word(", "psx_ww_f("),
    )
    fastn = {a: 0 for a, _ in FAST}

    counts = {name: 0 for name, _, _ in RULES}
    audit = {name: [] for name, _ in AUDIT}
    out = []
    with open(backup, "r", encoding="utf-8", errors="surrogateescape") as fh:
        for lineno, line in enumerate(fh, 1):
            if "cpu->" in line:
                for a, b in FAST:
                    if a in line:
                        fastn[a] += line.count(a)
                        line = line.replace(a, b)
            if "pgxp_" not in line:
                for name, rx, repl in RULES:
                    new, n = rx.subn(repl, line.rstrip("\n"), count=1)
                    if n:
                        counts[name] += 1
                        line = new + "\n"
                        break
            for name, rx in AUDIT:
                if "pgxp_" not in line and rx.search(line):
                    audit[name].append(lineno)
            out.append(line)

    with open(target, "w", encoding="utf-8", errors="surrogateescape",
              newline="") as fh:
        fh.writelines(out)

    print("fast memory access:")
    for a, n in fastn.items():
        print(f"  {a:20s} {n}")

    print("transformed:")
    for name, n in counts.items():
        print(f"  {name:12s} {n}")
    bad = False
    for name, lines in audit.items():
        if lines:
            bad = True
            print(f"AUDIT {name}: {len(lines)} lines, first: {lines[:5]}")
    if not bad:
        print("audit clean: no untracked SXY paths remain")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
