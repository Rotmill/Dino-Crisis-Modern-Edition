"""DC1 disc reader: parse ISO9660 from a PS1 raw .bin (2352-byte sectors,
Mode2/Form1 -> 2048 bytes user data at offset 24). Lists every file with its
LBA and size, and can read a file's bytes. Foundation for the location/geometry
extractor (step 1: disc map)."""
import os, struct, sys

SECTOR = 2352
DATA_OFF = 24      # Mode2/Form1 user-data offset within the raw sector
DATA_LEN = 2048

class Disc:
    def __init__(self, path):
        self.f = open(path, 'rb')
        self.size = os.path.getsize(path)
        # autodetect 2352 vs 2048
        self.raw = True
        head = self.f.read(16)
        if head[:12] != bytes([0,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0]):
            self.raw = False  # plain 2048 iso
        self.secsize = SECTOR if self.raw else 2048
        self.dataoff = DATA_OFF if self.raw else 0

    def read_sector(self, lba):
        self.f.seek(lba * self.secsize + self.dataoff)
        return self.f.read(DATA_LEN)

    def read_bytes(self, lba, nbytes):
        out = bytearray()
        while len(out) < nbytes:
            out += self.read_sector(lba)
            lba += 1
        return bytes(out[:nbytes])

def le32(b, o): return struct.unpack_from('<I', b, o)[0]

def list_files(disc):
    pvd = disc.read_sector(16)
    assert pvd[1:6] == b'CD001', f"no ISO9660 PVD: {pvd[1:6]}"
    # root dir record at offset 156 of PVD
    root = pvd[156:156+34]
    root_lba = le32(root, 2)
    root_len = le32(root, 10)
    files = []
    def walk(lba, length, prefix):
        data = disc.read_bytes(lba, length)
        off = 0
        while off < length:
            rlen = data[off]
            if rlen == 0:
                # advance to next sector boundary
                off = (off // DATA_LEN + 1) * DATA_LEN
                continue
            rec = data[off:off+rlen]
            ext_lba = le32(rec, 2)
            ext_len = le32(rec, 10)
            flags = rec[25]
            namelen = rec[32]
            name = rec[33:33+namelen]
            off += rlen
            if namelen == 1 and name in (b'\x00', b'\x01'):
                continue  # . and ..
            nm = name.split(b';')[0].decode('ascii', 'replace')
            full = prefix + '/' + nm
            if flags & 0x02:  # directory
                files.append((full+'/', ext_lba, ext_len, True))
                walk(ext_lba, ext_len, full)
            else:
                files.append((full, ext_lba, ext_len, False))
    walk(root_lba, root_len, '')
    return files

if __name__ == '__main__':
    if len(sys.argv) < 2:
        sys.exit("usage: iso.py <disc image .bin>")
    path = sys.argv[1]
    d = Disc(path)
    print(f"# raw={d.raw} secsize={d.secsize} size={d.size}")
    for name, lba, length, isdir in list_files(d):
        if isdir:
            print(f"DIR  {name}")
        else:
            print(f"FILE {name:40s} lba={lba:8d} size={length:10d}")
