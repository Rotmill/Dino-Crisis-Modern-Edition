"""Unpacks a PS-X EXE that a fan translation packed with an LZSS stub, as the stub itself does on the console.

    python unpack_exe.py <packed exe> <out exe>

26.10.2026 (the Arabic translation of the Japanese disc, SLPS_021.80): the exe is a stub at its text address that unpacks
the program into lower RAM, calls FlushCache and jumps to the program's entry. The recompiler needs the program itself,
so setup unpacks it here and checks the result by its sha256 (version.txt: exe_unpacked_sha256).

The stub must be word for word the known one (only the three addresses and the entry may differ), else nothing is
written. The loop: a flag byte, low bit first, with a 0x100 sentinel; bit 1 - one literal byte, bit 0 - a pair b0 b1:
distance b0 | (b1 & 15) << 8 back from the output, (b1 >> 4) + 2 bytes copied one by one. The stub stops when the
output reaches the end address exactly.

The out exe: the header of the packed one with pc0 = the entry, t_addr = the output address, t_size = the output size
rounded up to 2 KB, then the program (zeros after it to that size).
"""
import struct
import sys

# the stub, 49 words from its text address: (word, mask) - the bits of the mask may differ
_ADDR = 0x0000FFFF          # lui/ori of the source, output and end addresses: their immediates
_JUMP = 0x03FFFFFF          # j <entry>: its target
STUB = [
    (0x3C080000, _ADDR), (0x35080000, _ADDR), (0x3C090000, _ADDR), (0x35290000, _ADDR), (0x3C0A0000, _ADDR),
    (0x354A0000, _ADDR), (0x240B0000, 0), (0x112A0024, 0), (0x00000000, 0), (0x2D6C0002, 0), (0x11800005, 0),
    (0x00000000, 0), (0x910B0000, 0), (0x00000000, 0), (0x356B0100, 0), (0x25080001, 0), (0x316C0001, 0),
    (0x000B5842, 0), (0x11800007, 0), (0x00000000, 0), (0x910C0000, 0), (0x25080001, 0), (0xA12C0000, 0),
    (0x25290001, 0), (0x1000FFEE, 0), (0x00000000, 0), (0x910C0000, 0), (0x910D0001, 0), (0x25080002, 0),
    (0x31AE000F, 0), (0x000E7200, 0), (0x01CC7025, 0), (0x012E7023, 0), (0x000D6902, 0), (0x25AD0002, 0),
    (0x91CC0000, 0), (0x25CE0001, 0), (0xA12C0000, 0), (0x25290001, 0), (0x25ADFFFF, 0), (0x15A0FFFA, 0),
    (0x00000000, 0), (0x1000FFDC, 0), (0x00000000, 0), (0x24090044, 0), (0x0C000028, 0), (0x00000000, 0),
    (0x08000000, _JUMP), (0x00000000, 0),
]


def die(msg):
    sys.exit("unpack_exe: " + msg)


def unpack(d):
    """the unpacked exe of a packed one (bytes), or SystemExit"""
    if len(d) < 0x800 + 4 * len(STUB) or d[:8] != b"PS-X EXE":
        die("not a PS-X EXE")
    pc0, t_addr, t_size = struct.unpack_from("<I", d, 0x10)[0], *struct.unpack_from("<II", d, 0x18)
    if pc0 != t_addr or len(d) < 0x800 + t_size:
        die("the entry is not a stub at the text address")

    def word(a):
        o = 0x800 + a - t_addr
        if not 0x800 <= o <= len(d) - 4:
            die("address 0x%08X outside the exe" % a)
        return struct.unpack_from("<I", d, o)[0]

    for k, (w, mask) in enumerate(STUB):
        if word(t_addr + 4 * k) & ~mask & 0xFFFFFFFF != w & ~mask & 0xFFFFFFFF:
            die("not the known LZSS stub (word %d)" % k)

    def lui_ori(a):
        return ((word(a) & 0xFFFF) << 16) | (word(a + 4) & 0xFFFF)
    src, dst, end = lui_ori(t_addr), lui_ori(t_addr + 8), lui_ori(t_addr + 16)
    entry = (t_addr & 0xF0000000) | ((word(t_addr + 4 * 47) & 0x03FFFFFF) << 2)
    if not (0x80010000 <= dst < end <= 0x80200000 and dst <= entry < end):
        die("addresses out of range: output 0x%08X..0x%08X, entry 0x%08X" % (dst, end, entry))
    si = 0x800 + src - t_addr
    stop = 0x800 + t_size
    out = bytearray()
    flags = 0
    n = end - dst
    while len(out) != n:
        if len(out) > n:
            die("the output runs past the end address")
        if flags < 2:
            if si >= stop:
                die("the packed data runs out")
            flags = d[si] | 0x100
            si += 1
        bit, flags = flags & 1, flags >> 1
        if bit:
            if si >= stop:
                die("the packed data runs out")
            out.append(d[si])
            si += 1
        else:
            if si + 1 >= stop:
                die("the packed data runs out")
            b0, b1 = d[si], d[si + 1]
            si += 2
            dist = b0 | ((b1 & 15) << 8)
            if dist == 0 or dist > len(out):
                die("a pair reaches before the output")
            for _ in range((b1 >> 4) + 2):
                out.append(out[len(out) - dist])
    size = (n + 0x7FF) & ~0x7FF
    hdr = bytearray(d[:0x800])
    struct.pack_into("<I", hdr, 0x10, entry)
    struct.pack_into("<II", hdr, 0x18, dst, size)
    return bytes(hdr) + bytes(out) + bytes(size - n), (src, dst, end, entry)


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    d = open(sys.argv[1], "rb").read()
    exe, (src, dst, end, entry) = unpack(d)
    open(sys.argv[2], "wb").write(exe)
    print("unpacked 0x%08X..0x%08X (entry 0x%08X): %d bytes -> %s" % (dst, end, entry, len(exe), sys.argv[2]))


if __name__ == "__main__":
    main()
