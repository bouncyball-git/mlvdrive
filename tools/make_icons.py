"""Draws MLV Drive's icons: colour bars over a drive with a light, in colour (mounted) and grey
(not mounted), at every size Windows asks for, into assets/mlvdrive.ico and assets/mlvdrive_off.ico.

    python tools/make_icons.py

Only the standard library: each size is drawn 4x larger and averaged down (anti-aliasing), stored
as PNG inside the .ico (Windows Vista and later read that at any size).
"""
import os
import struct
import zlib

SIZES = [16, 20, 24, 32, 40, 48, 64, 256]
SS = 4  # supersampling

BARS = [(192, 192, 192), (192, 192, 0), (0, 192, 192), (0, 192, 0), (192, 0, 192), (192, 0, 0), (0, 0, 192)]
BODY = (32, 33, 36)
SLOT = (58, 61, 65)
LED_ON = (61, 220, 132)
LED_OFF = (90, 92, 96)


def grey(c):
    y = int(0.2126 * c[0] + 0.7152 * c[1] + 0.0722 * c[2])
    return (y, y, y)


def inside_round_rect(x, y, x0, y0, x1, y1, r):
    if x < x0 or x >= x1 or y < y0 or y >= y1:
        return False
    cx = min(max(x, x0 + r), x1 - r)
    cy = min(max(y, y0 + r), y1 - r)
    return (x - cx) ** 2 + (y - cy) ** 2 <= r * r


def pixel(u, v, on):
    """The colour at (u, v) in 0..1 coordinates, or None (transparent)."""
    if not inside_round_rect(u, v, 0.03, 0.03, 0.97, 0.97, 0.16):
        return None
    if 0.12 <= u < 0.88 and 0.14 <= v < 0.62:
        k = min(6, int((u - 0.12) / 0.76 * 7))
        return BARS[k] if on else grey(BARS[k])
    if inside_round_rect(u, v, 0.12, 0.70, 0.88, 0.86, 0.04):
        if (u - 0.78) ** 2 + (v - 0.78) ** 2 <= 0.045 ** 2:
            return LED_ON if on else LED_OFF
        return SLOT
    return BODY


def draw(size, on):
    n = size * SS
    rows = []
    for y in range(size):
        row = bytearray()
        for x in range(size):
            r = g = b = a = 0
            for sy in range(SS):
                for sx in range(SS):
                    c = pixel((x * SS + sx + 0.5) / n, (y * SS + sy + 0.5) / n, on)
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
