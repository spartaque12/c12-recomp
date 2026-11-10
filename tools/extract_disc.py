"""Extract an ISO9660 filesystem from a raw MODE2/2352 PSX track.

usage: extract_disc.py <track1.bin> <outdir>

Form1 files are written as 2048-byte user data. Files flagged as XA/Form2
(STR/XA streams) are written raw (2336 bytes/sector, subheader included) with
a .raw2336 suffix-free name; a manifest.json records LBA/size/mode per file.
"""
import json
import os
import struct
import sys

SECTOR = 2352


class Disc:
    def __init__(self, path):
        self.f = open(path, "rb")
        self.nsec = os.path.getsize(path) // SECTOR

    def raw(self, lba):
        self.f.seek(lba * SECTOR)
        return self.f.read(SECTOR)

    def user(self, lba):
        s = self.raw(lba)
        return s[24:24 + 2048]

    def read(self, lba, size):
        out = bytearray()
        n = (size + 2047) // 2048
        for i in range(n):
            out += self.user(lba + i)
        return bytes(out[:size])


def parse_dir(disc, lba, size):
    data = disc.read(lba, size)
    pos = 0
    out = []
    while pos < len(data):
        ln = data[pos]
        if ln == 0:
            pos = (pos // 2048 + 1) * 2048
            continue
        rec = data[pos:pos + ln]
        elba = struct.unpack_from("<I", rec, 2)[0]
        esize = struct.unpack_from("<I", rec, 10)[0]
        flags = rec[25]
        nlen = rec[32]
        name = rec[33:33 + nlen]
        # XA system-use area
        su = 33 + nlen + (1 - nlen % 2)
        xa_attr = 0
        if su + 14 <= ln and rec[su + 6:su + 8] == b"XA":
            xa_attr = struct.unpack_from(">H", rec, su + 4)[0]
        pos += ln
        if name in (b"\x00", b"\x01"):
            continue
        out.append((name.decode("ascii", "replace"), elba, esize, flags, xa_attr))
    return out


def main():
    binpath, outdir = sys.argv[1], sys.argv[2]
    d = Disc(binpath)
    pvd = d.user(16)
    assert pvd[1:6] == b"CD001", "no PVD"
    root = pvd[156:156 + 34]
    rlba = struct.unpack_from("<I", root, 2)[0]
    rsize = struct.unpack_from("<I", root, 10)[0]
    manifest = {"volume": pvd[40:72].decode().strip(), "sectors": d.nsec, "files": []}

    def walk(lba, size, rel):
        for name, elba, esize, flags, xa in parse_dir(d, lba, size):
            clean = name.split(";")[0]
            p = os.path.join(rel, clean)
            if flags & 2:
                os.makedirs(os.path.join(outdir, p), exist_ok=True)
                walk(elba, esize, p)
                continue
            nsec = (esize + 2047) // 2048
            # sniff: is first sector form2 / does the file interleave XA?
            sub = d.raw(elba)[16:20]
            form2 = bool(xa & 0x1000) or bool(xa & 0x4000) or bool(sub[2] & 0x20)
            entry = {"path": p.replace("\\", "/"), "lba": elba, "size": esize,
                     "sectors": nsec, "xa_attr": xa, "raw": form2}
            full = os.path.join(outdir, p)
            os.makedirs(os.path.dirname(full) or outdir, exist_ok=True)
            with open(full, "wb") as o:
                if form2:
                    for i in range(nsec):
                        o.write(d.raw(elba + i)[16:16 + 2336])
                else:
                    o.write(d.read(elba, esize))
            manifest["files"].append(entry)

    os.makedirs(outdir, exist_ok=True)
    walk(rlba, rsize, "")
    with open(os.path.join(outdir, "manifest.json"), "w") as o:
        json.dump(manifest, o, indent=1)
    for e in manifest["files"]:
        print(f"{e['lba']:7d} {e['size']:10d} {'RAW ' if e['raw'] else '    '}{e['path']}")


if __name__ == "__main__":
    main()
