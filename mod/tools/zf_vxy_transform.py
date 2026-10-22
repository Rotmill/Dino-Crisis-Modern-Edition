#!/usr/bin/env python3
"""zf_vxy_transform.py — источник вершины для PGXP (12.09.2026, z-fight F8/G).

Генкод пишет вершины в GTE напрямую:
    cpu->gte_data[0] = psx_rw_f(g4 + 16);  /* lwc2 gte[0], 16(g4) */ ...
и трекинг PGXP адреса не видит. Скрипт оборачивает такие строки (регистры
данных 0/2/4 = VXY0/1/2), чтобы адрес слова уходил в pgxp_track_lwc2_sxy():
    { uint32_t va_ = (g4 + 16); cpu->gte_data[0] = psx_rw_f(va_);
      if (g_pgxp_addr_on) pgxp_track_lwc2_sxy(va_, 0, cpu->gte_data[0]); }  /* lwc2 ... */

Идемпотентен (по маркеру «va_ = »). Место в конвейере — ПОСЛЕ
pgxp_transform.py и dc_hooks.patch:
    python tools/zf_vxy_transform.py generated/SLUS_009.22_full.c
Файл читается и пишется в двоичном режиме — переносы строк не трогаются.
"""
import re, sys, os

def main():
    path = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'generated', 'SLUS_009.22_full.c')
    data = open(path, 'rb').read()
    pat = re.compile(rb'cpu->gte_data\[([024])\] = psx_rw_f\(([^;()]+)\);(\s*/\* lwc2 gte\[[024]\][^\n]*)')
    n = 0
    def rep(m):
        nonlocal n
        reg, expr, tail = m.group(1), m.group(2), m.group(3)
        n += 1
        return (b'{ uint32_t va_ = (' + expr + b'); cpu->gte_data[' + reg + b'] = psx_rw_f(va_); '
                b'if (g_pgxp_addr_on) pgxp_track_lwc2_sxy(va_, ' + reg + b', cpu->gte_data[' + reg + b']); }' + tail)
    out = pat.sub(rep, data)
    already = data.count(b'va_ = (')
    if n == 0:
        print('zf_vxy_transform: nothing to do (already applied: %d sites)' % already)
        return
    open(path, 'wb').write(out)
    cr_before = data.count(b'\r'); cr_after = out.count(b'\r')
    print('zf_vxy_transform: %d lwc2 VXY sites wrapped (CR before %d, after %d)' % (n, cr_before, cr_after))

if __name__ == '__main__':
    main()
