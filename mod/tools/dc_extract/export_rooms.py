"""Offline exporter: Dino Crisis 1 (PS1) room geometry ST*.DAT -> OBJ.

Reverse-engineered from the recompiled game code (SLUS_009.22):
  * LZSS decompressor  = func_8002B138 (streamed per CD sector in-game)
  * container dispatch = jump table on entry type (func_8002B26C = type-8 path)

Container format (ST<stage><room>.DAT):
  Header sector (0x800 bytes, tail padded with "dummy header "):
    entries of 16 bytes from offset 0: {u32 type, u32 size, u32 dest, u32 param}
    type 0 = raw copy to RAM        dest=RAM addr
    type 2 = raw VRAM rect (CLUTs)  dest=(x,y) u16 pair, param=(w,h) u16 pair
    type 3 = SPU: VAB header        dest=SPU addr
    type 4 = SPU: VAB body          dest=SPU addr
    type 7 = LZSS -> RAM 0x80100000 (THE ROOM BLOCK: geometry etc.)
    type 8 = LZSS -> VRAM rect      dest=(x,y), param=(w,h) (texture pages)
  Data blocks follow sequentially from file offset 0x800; each block starts on
  the next 2048-byte boundary. `size` is the stored (compressed) byte count.

LZSS codec (control bits LSB-first from a control byte):
  bit=1: copy 1 literal byte
  bit=0: read lo,hi bytes: offset = ((hi&0x0F)<<8)|lo  (distance back from dst)
         length = (hi>>4)+2   -> copy length bytes from dst-offset

Room block (loaded at 0x80100000):
  +0x00: 9 x u32 RAM pointers. ptr[7] -> first tri array (0x...28),
         ptr[8] -> first quad array. ptr[0..6] -> other sections (cameras,
         scripts, collision, object models...).
  +0x24: u16 n_tri, u16 n_quad of mesh group 0; tri records at +0x28,
         quad records right after.
  After each group's quad data the next group header follows:
         {u32 tri_ptr(==hdr+12), u32 quad_ptr(==tri_ptr+n_tri*40),
          u16 n_tri, u16 n_quad} then the records. Chain ends when the
         pattern breaks (an indexed object-model section follows).

  Tri record  (40 bytes): 3 x vertex(8), u32 texinfo, 3 x RGBA color
  Quad record (52 bytes): 4 x vertex(8), u32 texinfo, 4 x RGBA color
  vertex(8) = {s16 x, s16 y, s16 z, u8 u, u8 v}  (UV packed in the 4th s16)
  texinfo   = {u16 tpage_id, u16 clut_id} (standard PS1 encodings)
  Quad vertex order is the GPU Z-order: split into tris (0,1,2),(1,3,2).

Verified against the runtime GTE capture captured_rooms/library/room_00.obj
(= ST103): 1516/1546 vertices exact, 1911/1933 triangles exact with identical
winding. Missing bits: flat floor plane (drawn separately by the engine) and
door models (separate posed objects).

Usage:  python export_rooms.py <disc.bin> [out_dir]
        (или переменная DC_DISC=<путь к Track 1 .bin>; out_dir по умолчанию
         ./exported_rooms в текущем каталоге)
"""
import os, struct, sys

# Консоль Windows по умолчанию не в UTF-8, а сообщения ниже русские.
for _s in (sys.stdout, sys.stderr):
    try: _s.reconfigure(encoding="utf-8", errors="replace")
    except Exception: pass

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from iso import Disc, list_files

# Путь к образу задаётся аргументом или переменной DC_DISC: в релизной сборке
# диск лежит у пользователя, а не в дереве разработки.
_args = [a for a in sys.argv[1:] if not a.startswith("-")]
# --library-only: не писать рядом с библиотекой ещё две копии каждой комнаты. Они удобны
# при разборе (OBJ открывается в редакторе, .dcroom назван по имени ST),
# но игра читает только library/room_*.dcroom — в установке остальное было бы
# 36 МБ мёртвого груза.
LIBRARY_ONLY = "--library-only" in sys.argv[1:]
DISC = os.environ.get("DC_DISC") or (_args.pop(0) if _args else "")
if not DISC or not os.path.isfile(DISC):
    sys.exit("export_rooms: укажи образ игры: python export_rooms.py <Track 1.bin> [out_dir]")
BASE = 0x80100000
SECT = 2048


def lzss(src, start, insize):
    """DC1 room codec, translated from func_8002B138."""
    out = bytearray()
    pos = start
    end = start + insize
    ctrl = 0
    bits = 0
    while pos < end:
        if bits == 0:
            ctrl = src[pos]; pos += 1; bits = 8
            continue
        bit = ctrl & 1
        ctrl >>= 1
        bits -= 1
        if bit:
            if pos >= end:
                break
            out.append(src[pos]); pos += 1
        else:
            if pos + 1 >= end:
                break
            lo = src[pos]; hi = src[pos + 1]; pos += 2
            off = ((hi & 0x0F) << 8) | lo
            ln = (hi >> 4) + 2
            if off == 0:
                break        # a translated disc (RU, VIT / Akella) pads the stream to the old size with zero pairs
            if off > len(out):
                return None  # corrupt / not this codec
            for _ in range(ln):
                out.append(out[-off])
    return bytes(out)


def parse_container(data):
    """Return [(type, size, dest, param, file_offset)] for each block."""
    entries = []
    off = 0
    while off + 16 <= min(0x800, len(data)):
        t, sz, dest, param = struct.unpack_from('<4I', data, off)
        if t > 20 or sz == 0 or sz > len(data):
            break
        entries.append((t, sz, dest, param))
        off += 16
    pos = 0x800
    out = []
    for (t, sz, dest, param) in entries:
        if pos + sz > len(data):
            break
        out.append((t, sz, dest, param, pos))
        pos += sz
        pos = (pos + SECT - 1) & ~(SECT - 1)
    return out


def walk_groups(room):
    """Return [(tri_off, n_tri, quad_off, n_quad)] of the static mesh chain,
    or [] if the block is not a room-geometry block."""
    if len(room) < 0x2c:
        return []
    ptr7, ptr8 = struct.unpack_from('<2I', room, 7 * 4)
    nt, nq = struct.unpack_from('<2H', room, 0x24)
    # strong validation using the room header pointers
    if ptr7 != BASE + 0x28 or ptr8 != BASE + 0x28 + nt * 40:
        return []
    if 0x28 + nt * 40 + nq * 52 > len(room):
        return []
    groups = [(0x28, nt, 0x28 + nt * 40, nq)]
    pos = 0x28 + nt * 40 + nq * 52
    while pos + 12 <= len(room):
        a, b = struct.unpack_from('<2I', room, pos)
        nt, nq = struct.unpack_from('<2H', room, pos + 8)
        if a != BASE + pos + 12 or b != a + nt * 40:
            break
        if nt > 5000 or nq > 5000:
            break
        end = b - BASE + nq * 52
        if end > len(room):
            break
        groups.append((a - BASE, nt, b - BASE, nq))
        pos = end
    return groups


def read_vertex(room, off):
    x, y, z, w = struct.unpack_from('<4h', room, off)
    w &= 0xFFFF
    return (x, y, z, w & 0xFF, w >> 8)  # x,y,z,u,v


def read_colors(room, off, n):
    """The n RGBA words that follow a primitive's vertex block. Low three bytes
    are the baked Gouraud colour of the matching vertex; the top byte of the
    FIRST word is the GPU command code (0x34 gouraud-textured tri, 0x3C quad),
    which is why it must not be mistaken for alpha."""
    cols = struct.unpack_from('<%dI' % n, room, off)
    return [c & 0xFFFFFF for c in cols], (cols[0] >> 24) & 0xFF


def collect_tris(room, groups, extra_tris=None):
    """Flatten mesh groups into per-triangle arrays matching the runtime capture:
    v[9] int16, uv[6] int16 (raw PS1 bytes), tp u16 (raw tpage), clut-x/y in
    VRAM pixels (cx=(clut&0x3F)*16, cy=clut>>6), rgb[9] u8 (baked per-vertex
    Gouraud = the game's ORIGINAL lighting) and the u8 GPU primitive code.
    Quads split (0,1,2),(1,3,2) exactly like the pt_dc_prim hook."""
    V = []; UV = []; TP = []; CX = []; CY = []; RGB = []; CODE = []

    def emit(vs, tpage, clut, order, cols=None, code=0):
        for k in order:
            x, y, z, u, v = vs[k]
            V.extend((x, y, z))
            UV.extend((u, v))
            c = cols[k] if cols else 0x808080
            RGB.extend((c & 255, (c >> 8) & 255, (c >> 16) & 255))
        TP.append(tpage)
        CX.append((clut & 0x3F) * 16)
        CY.append((clut >> 6) & 0x1FF)
        CODE.append(code)

    for (toff, nt, qoff, nq) in groups:
        for i in range(nt):
            o = toff + i * 40
            vs = [read_vertex(room, o + k * 8) for k in range(3)]
            tpage, clut = struct.unpack_from('<2H', room, o + 24)
            cols, code = read_colors(room, o + 28, 3)
            emit(vs, tpage, clut, (0, 1, 2), cols, code)
        for i in range(nq):
            o = qoff + i * 52
            vs = [read_vertex(room, o + k * 8) for k in range(4)]
            tpage, clut = struct.unpack_from('<2H', room, o + 32)
            cols, code = read_colors(room, o + 36, 4)
            emit(vs, tpage, clut, (0, 1, 2), cols, code)
            emit(vs, tpage, clut, (1, 3, 2), cols, code)
    if extra_tris:
        for vs, tpage, clut in extra_tris:
            emit(vs, tpage, clut, (0, 1, 2))
    return V, UV, TP, CX, CY, RGB, CODE


def write_dcroom(out_path, V, UV, TP, CX, CY, RGB=None, CODE=None):
    """DCR2 = DCRM plus the baked vertex colours. Layout:
         "DCR2", u32 n, v[9n]i16, uv[6n]i16, tp[n]u16, cx[n]u16, cy[n]u16,
         rgb[9n]u8, code[n]u8
       Without colours it still writes the old DCRM container so nothing that
       only knows v1 has to change."""
    n = len(TP)
    have_rgb = RGB is not None and CODE is not None
    with open(out_path, 'wb') as f:
        f.write(b"DCR2" if have_rgb else b"DCRM")
        f.write(struct.pack("<I", n))
        f.write(struct.pack("<%dh" % (9 * n), *V))
        f.write(struct.pack("<%dh" % (6 * n), *UV))
        f.write(struct.pack("<%dH" % n, *TP))
        f.write(struct.pack("<%dH" % n, *CX))
        f.write(struct.pack("<%dH" % n, *CY))
        if have_rgb:
            f.write(struct.pack("<%dB" % (9 * n), *RGB))
            f.write(struct.pack("<%dB" % n, *CODE))
    return n


def export_obj(room, groups, out_path, name):
    vlines = []
    vtlines = []
    flines = []          # list of (group_idx, tpage, clut, [face index tuples])
    vmap = {}
    vtmap = {}

    def vid(x, y, z):
        key = (x, y, z)
        i = vmap.get(key)
        if i is None:
            i = len(vlines) + 1
            vmap[key] = i
            vlines.append(f"v {x} {y} {z}")
        return i

    def vtid(u, v):
        key = (u, v)
        i = vtmap.get(key)
        if i is None:
            i = len(vtlines) + 1
            vtmap[key] = i
            vtlines.append(f"vt {(u + 0.5) / 256.0:.6f} {1.0 - (v + 0.5) / 256.0:.6f}")
        return i

    n_tri_total = 0
    body = []
    for gi, (toff, nt, qoff, nq) in enumerate(groups):
        body.append(f"g {name}_part{gi:02d}")
        cur_mtl = None
        for i in range(nt):
            o = toff + i * 40
            vs = [read_vertex(room, o + k * 8) for k in range(3)]
            tpage, clut = struct.unpack_from('<2H', room, o + 24)
            mtl = f"tp{tpage:04x}_cl{clut:04x}"
            if mtl != cur_mtl:
                body.append("usemtl " + mtl)
                cur_mtl = mtl
            ids = [(vid(x, y, z), vtid(u, v)) for x, y, z, u, v in vs]
            body.append("f " + " ".join(f"{a}/{b}" for a, b in ids))
            n_tri_total += 1
        for i in range(nq):
            o = qoff + i * 52
            vs = [read_vertex(room, o + k * 8) for k in range(4)]
            tpage, clut = struct.unpack_from('<2H', room, o + 32)
            mtl = f"tp{tpage:04x}_cl{clut:04x}"
            if mtl != cur_mtl:
                body.append("usemtl " + mtl)
                cur_mtl = mtl
            ids = [(vid(x, y, z), vtid(u, v)) for x, y, z, u, v in vs]
            # GPU Z-order quad -> two tris, winding verified vs capture
            for tri in ((0, 1, 2), (1, 3, 2)):
                body.append("f " + " ".join(f"{ids[k][0]}/{ids[k][1]}" for k in tri))
                n_tri_total += 1

    with open(out_path, 'w') as f:
        f.write(f"# Dino Crisis 1 room mesh, exported from {name}.DAT\n")
        f.write("# native int16 model/world coordinates (PS1)\n")
        f.write(f"# {len(vlines)} vertices, {n_tri_total} triangles, {len(groups)} groups\n")
        f.write("\n".join(vlines) + "\n")
        f.write("\n".join(vtlines) + "\n")
        f.write("\n".join(body) + "\n")
    return len(vlines), n_tri_total


def main():
    out_dir = _args[0] if _args else os.path.join(os.getcwd(), "exported_rooms")
    lib_dir = os.path.join(out_dir, "library")
    os.makedirs(out_dir, exist_ok=True)
    os.makedirs(lib_dir, exist_ok=True)
    d = Disc(DISC)
    files = [(n, lba, ln) for n, lba, ln, isdir in list_files(d)
             if not isdir and n.upper().startswith('/PSX/DATA/ST')
             and n.upper().endswith('.DAT')]
    files.sort()
    # 17.09.2026: НОМЕРА КОМНАТ — КАК В БИБЛИОТЕКЕ ДЕРЕВА РАЗРАБОТКИ. Там ST50B и ST60E стоят последними
    # (room_119, room_120): несжатые комнаты экспортёр научился брать позже, и их дописали в конец. Подгонка
    # игрока завязана на эти номера (dc_hiprooms_user.cfg, столбец комнаты в dc_zfsel_user.cfg, номера в
    # журналах дева), а по порядку файлов у всех комнат с 67-й номер был бы на единицу-две больше.
    late = ('ST50B', 'ST60E')
    def lib_order(t):
        base = os.path.basename(t[0]).rsplit('.', 1)[0].upper()
        return (late.index(base) + 1 if base in late else 0, t)
    files.sort(key=lib_order)
    exported = 0
    skipped = []
    manifest = []          # (library room index, ST name, tris)
    for n, lba, ln in files:
        base = os.path.basename(n).rsplit('.', 1)[0]
        data = d.read_bytes(lba, ln)
        entries = parse_container(data)
        done = False
        idx = 0
        for (t, sz, dest, param, pos) in entries:
            # type 7 = LZSS-packed room block; type 0 with dest == 0x80100000 is
            # the SAME block stored uncompressed (ST50B, ST60E ship this way).
            if t == 7:
                room = lzss(data, pos, sz)
            elif t == 0 and dest == BASE:
                room = data[pos:pos + sz]
            else:
                continue
            if room is None:
                continue
            groups = walk_groups(room)
            if not groups:
                continue
            suffix = "" if idx == 0 else f"_{idx}"
            out_path = os.path.join(out_dir, base + suffix + ".obj")
            nv = nf = 0
            if not LIBRARY_ONLY:
                nv, nf = export_obj(room, groups, out_path, base)
            # textured binary room next to the OBJ + a copy named for the
            # runtime room library (names are arbitrary: the runtime matches
            # rooms by exact-triangle voting, only the room_*.dcroom glob and
            # enumeration order matter).
            V, UV, TP, CX, CY, RGB, CODE = collect_tris(room, groups)
            if LIBRARY_ONLY:
                ntri = len(CODE)
            else:
                ntri = write_dcroom(os.path.join(out_dir, base + suffix + ".dcroom"),
                                    V, UV, TP, CX, CY, RGB, CODE)
            lib_name = "room_%03d.dcroom" % len(manifest)
            write_dcroom(os.path.join(lib_dir, lib_name), V, UV, TP, CX, CY, RGB, CODE)
            manifest.append((lib_name, base + suffix, ntri, lba, ln))
            print(f"{base}{suffix}: {len(groups)} groups, {nv} verts, {nf} tris -> {out_path} (+.dcroom)")
            exported += 1
            idx += 1
            done = True
        if not done:
            skipped.append(base)
    with open(os.path.join(lib_dir, "manifest.txt"), 'w') as f:
        f.write("# room library manifest: library file -> source ST DAT (tris)\n")
        for lib_name, src, ntri, lba, ln in manifest:
            f.write(f"{lib_name} {src} {ntri}\n")

    # Опознание комнаты по прочитанному сектору привода: рантайм
    # (dc_room_note_lba в pt_renderer.c) сопоставляет LBA каждого чтения
    # с диапазоном секторов файла ST*.DAT.
    with open(os.path.join(lib_dir, "rooms_lba.txt"), 'w') as f:
        f.write("# lba_start lba_end room_index st_name tris\n")
        for idx, (lib_name, src, ntri, lba, ln) in enumerate(manifest):
            f.write(f"{lba} {lba + (ln + SECT - 1) // SECT} {idx} {src} {ntri}\n")
    print(f"\nexported {exported} rooms; skipped {len(skipped)}: {' '.join(skipped)}")
    print(f"library: {len(manifest)} rooms in {lib_dir}")


if __name__ == '__main__':
    main()
