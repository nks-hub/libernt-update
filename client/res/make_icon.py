"""Draws client/res/app.ico (16, 32 and 48 px): a blue rounded square with a white circular arrow.
Run it again after changing the drawing; the .ico is committed so the build needs no Python."""
import math
import struct
from pathlib import Path


def pixel(size: int, x: int, y: int):
    """RGBA of one pixel, supersampled 4x4 for smooth edges."""
    acc = [0.0, 0.0, 0.0, 0.0]
    for sy in range(4):
        for sx in range(4):
            px, py = (x + (sx + 0.5) / 4) / size, (y + (sy + 0.5) / 4) / size
            r = 0.18
            dx, dy = max(abs(px - 0.5) - (0.5 - r), 0), max(abs(py - 0.5) - (0.5 - r), 0)
            if dx * dx + dy * dy > r * r:
                continue
            color = (0, 103, 192)  # background
            ang = math.degrees(math.atan2(0.5 - py, px - 0.5)) % 360
            dist = math.hypot(px - 0.5, py - 0.5)
            if 0.20 <= dist <= 0.30 and not 20 <= ang <= 80:  # ring with a gap at the top right
                color = (255, 255, 255)
            # arrow head at the start of the ring (angle 80 degrees), pointing clockwise
            hx, hy = 0.5 + 0.25 * math.cos(math.radians(80)), 0.5 - 0.25 * math.sin(math.radians(80))
            tx, ty = px - hx, py - hy
            if -0.13 <= tx <= 0.02 and abs(ty) <= 0.13 + tx * 0.95 and tx >= -0.13:
                color = (255, 255, 255)
            for i in range(3):
                acc[i] += color[i]
            acc[3] += 255
    n = 16
    return tuple(int(v / n) for v in acc[:3]) + (int(acc[3] / n),)


def bmp_entry(size: int) -> bytes:
    rows = []
    for y in reversed(range(size)):  # bottom-up
        row = b"".join(struct.pack("BBBB", b, g, r, a) for r, g, b, a in (pixel(size, x, y) for x in range(size)))
        rows.append(row)
    xor = b"".join(rows)
    and_mask = b"\x00" * (((size + 31) // 32) * 4 * size)
    header = struct.pack("<IiiHHIIiiII", 40, size, size * 2, 1, 32, 0, len(xor) + len(and_mask), 0, 0, 0, 0)
    return header + xor + and_mask


def main() -> None:
    sizes = (16, 32, 48)
    images = [bmp_entry(s) for s in sizes]
    out = struct.pack("<HHH", 0, 1, len(sizes))
    offset = 6 + 16 * len(sizes)
    for s, img in zip(sizes, images):
        out += struct.pack("<BBBBHHII", s, s, 0, 0, 1, 32, len(img), offset)
        offset += len(img)
    out += b"".join(images)
    Path(__file__).with_name("app.ico").write_bytes(out)


if __name__ == "__main__":
    main()
