# Throwaway: writes the two 256x256 procedural PNGs used by assets/materials/mat_metal.mtlx.
# Usage: python3 -I scratchpad/gen_textures.py assets/textures
import struct, sys, zlib

def write_png(path, width, height, rgb_rows):
    raw = b"".join(b"\x00" + bytes(row) for row in rgb_rows)
    def chunk(tag, data):
        body = tag + data
        return struct.pack(">I", len(data)) + body + struct.pack(">I", zlib.crc32(body) & 0xFFFFFFFF)
    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(raw, 9))
    png += chunk(b"IEND", b"")
    open(path, "wb").write(png)

out = sys.argv[1]
n = 256
cell = 32
# Base color: 8x8 checker, teal / warm orange (sRGB values).
rows = []
for y in range(n):
    row = []
    for x in range(n):
        if ((x // cell) + (y // cell)) % 2 == 0:
            row += [20, 150, 160]
        else:
            row += [235, 140, 40]
    rows.append(row)
write_png(f"{out}/checker_base.png", n, n, rows)
# Roughness: horizontal gradient 0.1 -> 0.9 (stored in all three channels, linear data).
rows = []
for y in range(n):
    row = []
    for x in range(n):
        v = int(round((0.1 + 0.8 * x / (n - 1)) * 255))
        row += [v, v, v]
    rows.append(row)
write_png(f"{out}/gradient_roughness.png", n, n, rows)
