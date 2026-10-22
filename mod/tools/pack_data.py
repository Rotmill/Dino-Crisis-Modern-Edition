"""Packs the game's data into one file that the build puts inside the program.

    python pack_data.py <stage folder> <out.pak>

17.09.2026, player: "keep dc_user.cfg, input.ini and SLUS_009.22 in data,
build everything else into the exe". Setup gathers what used to lie in data\
into a stage folder under the build tree - the startup settings and tuning
files, game.toml with the path to the disc image, the licence texts, the
camera collision and the rooms exported from the disc image - and this script
packs it. CMakeLists.txt links the pack into DinoCrisisME.exe as the resource
DCME_DATA; runtime/src/dc_pak.c reads it.

Every file under the stage folder goes in, named by its path relative to the
folder in lower case with '/' - the name the game used to open it by, relative
to the data folder - and sorted by that name (the order FindFirstFile gave).

Format, little-endian:
    "DCMEPAK1"  u32 count
    count times: u16 name_len, name bytes, 0, u32 offset, u32 size
    the files, each followed by a 0 byte (text can be read as a C string)

The pack is read back and compared with the stage folder before the script
reports success.
"""

import os
import struct
import sys

MAGIC = b"DCMEPAK1"

# Without these the game does not start the way it should: they are checked
# here, while setup can still say so, rather than as a black screen later.
REQUIRED = (
    "dc_env.cfg",
    "game.toml",
    "exported_rooms/library/manifest.txt",
    "exported_rooms/library/rooms_lba.txt",
)


def gather(stage):
    files = []
    for root, _dirs, names in os.walk(stage):
        for n in names:
            full = os.path.join(root, n)
            rel = os.path.relpath(full, stage).replace("\\", "/").lower()
            files.append((rel, full))
    files.sort(key=lambda t: t[0])
    return files


def build(files):
    names = [rel.encode("utf-8") for rel, _ in files]
    datas = [open(full, "rb").read() for _, full in files]
    dir_size = sum(2 + len(nm) + 1 + 8 for nm in names)
    off = len(MAGIC) + 4 + dir_size
    head = bytearray(MAGIC + struct.pack("<I", len(files)))
    body = bytearray()
    for nm, data in zip(names, datas):
        if len(nm) > 0xFFFF:
            sys.exit("name too long: %r" % nm)
        head += struct.pack("<H", len(nm)) + nm + b"\0" + struct.pack("<II", off, len(data))
        body += data + b"\0"
        off += len(data) + 1
    if len(head) + len(body) > 0x7FFFFFFF:
        sys.exit("pack too large")
    return bytes(head + body)


def check(pak, files):
    if pak[:8] != MAGIC:
        return "magic"
    (count,) = struct.unpack_from("<I", pak, 8)
    if count != len(files):
        return "count %d != %d" % (count, len(files))
    at = 12
    for rel, full in files:
        (nl,) = struct.unpack_from("<H", pak, at)
        name = pak[at + 2:at + 2 + nl].decode("utf-8")
        off, size = struct.unpack_from("<II", pak, at + 2 + nl + 1)
        if name != rel:
            return "name %s != %s" % (name, rel)
        if pak[off:off + size] != open(full, "rb").read() or pak[off + size] != 0:
            return "data of " + rel
        at += 2 + nl + 1 + 8
    return None


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    stage, out = sys.argv[1], sys.argv[2]
    if not os.path.isdir(stage):
        sys.exit("no stage folder: " + stage)
    files = gather(stage)
    have = {rel for rel, _ in files}
    missing = [r for r in REQUIRED if r not in have]
    if missing:
        sys.exit("missing from the stage folder: " + ", ".join(missing))
    rooms = sum(1 for r in have if r.startswith("exported_rooms/library/room_") and r.endswith(".dcroom"))
    coll = sum(1 for r in have if r.startswith("exported_rooms/collision/") and r.endswith(".obj"))
    pak = build(files)
    with open(out, "wb") as f:
        f.write(pak)
    bad = check(open(out, "rb").read(), files)
    if bad:
        sys.exit("pack does not read back: " + bad)
    print("dcme_data.pak: %d files, %d bytes (rooms %d, collision %d)" % (len(files), len(pak), rooms, coll))


if __name__ == "__main__":
    main()
