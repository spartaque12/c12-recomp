"""Make a PNG contact sheet from BMP screenshots: sheet.py <out.png> <cols> <bmp...> (scale via SHEET_SCALE env)."""
import os
import struct
import sys
import zlib


def read_bmp(p):
    d = open(p, "rb").read()
    off = struct.unpack_from("<I", d, 10)[0]
    w, h = struct.unpack_from("<ii", d, 18)
    row = (w * 3 + 3) & ~3
    rows = []
    for y in range(h):
        r = d[off + (h - 1 - y) * row: off + (h - 1 - y) * row + w * 3]
        rows.append(bytes(b for i in range(0, len(r), 3) for b in (r[i + 2], r[i + 1], r[i])))
    return w, h, rows


def write_png(p, w, h, rows):
    raw = b"".join(b"\0" + r for r in rows)

    def chunk(t, c):
        return struct.pack(">I", len(c)) + t + c + struct.pack(">I", zlib.crc32(t + c))
    open(p, "wb").write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)) +
                        chunk(b"IDAT", zlib.compress(raw, 6)) + chunk(b"IEND", b""))


out, cols = sys.argv[1], int(sys.argv[2])
imgs = [read_bmp(p) for p in sys.argv[3:]]
scale = int(os.environ.get("SHEET_SCALE", "1"))
cw = max(i[0] for i in imgs) * scale
ch = max(i[1] for i in imgs) * scale
nrows = (len(imgs) + cols - 1) // cols
W, H = cw * cols + 2 * (cols - 1), ch * nrows + 2 * (nrows - 1)
canvas = [bytearray(b"\x40" * (W * 3)) for _ in range(H)]
for n, (w, h, rows) in enumerate(imgs):
    ox, oy = (n % cols) * (cw + 2), (n // cols) * (ch + 2)
    for y in range(h):
        r = rows[y]
        if scale > 1:
            r = bytes(b for x in range(w) for b in r[x * 3:x * 3 + 3] * scale)
        for s in range(scale):
            canvas[oy + y * scale + s][ox * 3: ox * 3 + len(r)] = r
write_png(out, W, H, [bytes(r) for r in canvas])
print(out, W, H)
