"""Draws MLV Drive's icons: a drive with a dashed copy of itself behind it (a virtual drive), in
blue with a green light (mounted) and in grey with the light off (not mounted), at every size
Windows asks for, into assets/mlvdrive.ico and assets/mlvdrive_off.ico.

    python tools/make_icons.py

Only the standard library: each size is drawn 4x larger and averaged down (anti-aliasing), stored
as PNG inside the .ico (Windows Vista and later read that at any size).
"""
import os
import struct
import zlib

SIZES = [16, 20, 24, 32, 40, 48, 64, 256]
SS = 4  # supersampling

# Mounted / not mounted
COLOURS = {
    True: dict(top=(59, 130, 246), bottom=(29, 78, 216), edge=(23, 37, 84), face=(30, 64, 175),
               slot=(147, 197, 253), ghost=(96, 165, 250), led=(52, 211, 153)),
    False: dict(top=(140, 145, 153), bottom=(100, 106, 115), edge=(45, 50, 58), face=(85, 90, 99),
                slot=(190, 194, 200), ghost=(156, 163, 175), led=(70, 74, 80)),
}

# The drive in front and its virtual copy behind (up and to the right): x0, y0, x1, y1, corner;
# the copy's dashed outline: its width and dash length. Small sizes (the tray at 100%: 16 px)
# get a bigger drive and fewer, thicker dashes, which stay readable at a few pixels.
LAYOUT = {
    False: dict(front=(0.05, 0.38, 0.81, 0.86, 0.11), back=(0.19, 0.14, 0.95, 0.62, 0.11),
                stroke=0.055, dash=0.105, edge=0.035, led=0.062, slot=0.022),
    True: dict(front=(0.0, 0.30, 0.86, 0.97, 0.12), back=(0.20, 0.03, 1.0, 0.62, 0.12),
               stroke=0.105, dash=0.19, edge=0.07, led=0.10, slot=0.045),
}


def round_rect_distance(x, y, x0, y0, x1, y1, r):
    """Signed distance to a rounded rectangle's edge (< 0 inside)."""
    cx, cy = (x0 + x1) / 2, (y0 + y1) / 2
    hx, hy = (x1 - x0) / 2 - r, (y1 - y0) / 2 - r
    dx, dy = abs(x - cx) - hx, abs(y - cy) - hy
    ox, oy = max(dx, 0.0), max(dy, 0.0)
    return (ox * ox + oy * oy) ** 0.5 + min(max(dx, dy), 0.0) - r


def dashed(x, y, x0, y0, x1, y1, r, stroke, dash):
    """On the copy's outline, where a dash is (corners always drawn)."""
    if abs(round_rect_distance(x, y, x0, y0, x1, y1, r)) > stroke / 2:
        return False
    in_corner = (x < x0 + r or x > x1 - r) and (y < y0 + r or y > y1 - r)
    if in_corner:
        return True
    along = x - x0 if (y < y0 + r or y > y1 - r) else y - y0
    return int(along / dash) % 2 == 0


def pixel(u, v, on, small):
    """The colour at (u, v) in 0..1 coordinates, or None (transparent)."""
    c = COLOURS[on]
    L = LAYOUT[small]
    x0, y0, x1, y1, r = L['front']
    d = round_rect_distance(u, v, x0, y0, x1, y1, r)
    if d <= 0:
        if d > -L['edge']:
            return c['edge']  # outline: holds on light and dark taskbars
        t = (v - y0) / (y1 - y0)
        face = y0 + (y1 - y0) * 0.56
        mid = (face + y1) / 2
        if v > face:  # the front face: a slot and the light
            led_x = x0 + (x1 - x0) * 0.80
            if (u - led_x) ** 2 + (v - mid) ** 2 <= L['led'] ** 2:
                return c['led']
            if x0 + (x1 - x0) * 0.13 <= u <= x0 + (x1 - x0) * 0.58 and abs(v - mid) <= L['slot']:
                return c['slot']
            return c['face']
        k = t / 0.56  # the top: a gradient
        return tuple(int(a + (b - a) * k) for a, b in zip(c['top'], c['bottom']))
    if dashed(u, v, *L['back'], L['stroke'], L['dash']):
        return c['ghost']
    return None


def draw(size, on):
    n = size * SS
    rows = []
    for y in range(size):
        row = bytearray()
        for x in range(size):
            r = g = b = a = 0
            for sy in range(SS):
                for sx in range(SS):
                    c = pixel((x * SS + sx + 0.5) / n, (y * SS + sy + 0.5) / n, on, size <= 24)
                    if c is not None:
                        r += c[0]
                        g += c[1]
                        b += c[2]
                        a += 1
            if a:
                row += bytes((r // a, g // a, b // a, a * 255 // (SS * SS)))
            else:
                row += bytes(4)
        rows.append(bytes(row))
    return rows


def png(size, rows):
    def chunk(kind, data):
        return struct.pack('>I', len(data)) + kind + data + struct.pack('>I', zlib.crc32(kind + data) & 0xFFFFFFFF)
    raw = b''.join(b'\x00' + r for r in rows)
    return (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', size, size, 8, 6, 0, 0, 0)) +
            chunk(b'IDAT', zlib.compress(raw, 9)) + chunk(b'IEND', b''))


def ico(images):
    head = struct.pack('<HHH', 0, 1, len(images))
    offset = 6 + 16 * len(images)
    entries, data = b'', b''
    for size, blob in images:
        dim = 0 if size >= 256 else size
        entries += struct.pack('<BBBBHHII', dim, dim, 0, 0, 1, 32, len(blob), offset + len(data))
        data += blob
    return head + entries + data


def main():
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    out = os.path.join(root, 'assets')
    os.makedirs(out, exist_ok=True)
    for name, on in (('mlvdrive.ico', True), ('mlvdrive_off.ico', False)):
        images = [(s, png(s, draw(s, on))) for s in SIZES]
        with open(os.path.join(out, name), 'wb') as f:
            f.write(ico(images))
        print(name, sum(len(b) for _, b in images), 'bytes')


if __name__ == '__main__':
    main()
