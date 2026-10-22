"""Достаёт исполняемый файл игры (SLUS_009.22) из образа диска пользователя.

Рекомпилятору нужен именно этот файл — PS-X EXE с заголовком, из которого
берутся load_address / entry_pc / text_size. В образе он лежит в корне
как SLUS_009.22;1.

Usage:  python extract_exe.py <disc Track 1 .bin> [out_path] [имя на диске]
        (out_path по умолчанию ./SLUS_009.22; имя на диске - у другой версии игры
        своё, например SLES_022.07 у европейской; сохраняется под out_path)
"""
import os, sys

# Консоль Windows по умолчанию не в UTF-8, а сообщения тут русские: без этого
# скрипт падает на первом же print с UnicodeEncodeError.
for _s in (sys.stdout, sys.stderr):
    try: _s.reconfigure(encoding="utf-8", errors="replace")
    except Exception: pass

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from iso import Disc, list_files

WANT = "SLUS_009.22"


def main():
    if len(sys.argv) < 2:
        sys.exit("extract_exe: python extract_exe.py <Track 1.bin> [out_path]")
    disc_path = sys.argv[1]
    out_path = sys.argv[2] if len(sys.argv) > 2 else os.path.join(os.getcwd(), WANT)
    want = sys.argv[3] if len(sys.argv) > 3 else WANT
    if not os.path.isfile(disc_path):
        sys.exit(f"extract_exe: образ не найден: {disc_path}")

    d = Disc(disc_path)
    hit = None
    for name, lba, ln, isdir in list_files(d):
        if isdir:
            continue
        base = os.path.basename(name).split(';')[0].upper()
        if base == want.upper():
            hit = (name, lba, ln)
            break
    if not hit:
        sys.exit(f"extract_exe: в образе нет {want} - это не та версия Dino Crisis?")

    name, lba, ln = hit
    data = d.read_bytes(lba, ln)
    if data[:8] != b"PS-X EXE":
        sys.exit(f"extract_exe: {name} не похож на PS-X EXE (сигнатура {data[:8]!r})")

    os.makedirs(os.path.dirname(os.path.abspath(out_path)) or ".", exist_ok=True)
    with open(out_path, 'wb') as f:
        f.write(data)
    print(f"{name}: {ln} байт (сектор {lba}) -> {out_path}")


if __name__ == '__main__':
    main()
