"""Pictures for the bench without Pillow: read the program's PPM frames,
write PNG, and measure what a person would see as a display defect."""
from __future__ import annotations

import struct
import zlib
from pathlib import Path


def read_ppm(path) -> tuple[int, int, bytes]:
    data = Path(path).read_bytes()
    # P6\n<w> <h>\n255\n<rgb>
    parts, pos = [], 0
    while len(parts) < 4:
        while data[pos:pos + 1].isspace():
            pos += 1
        if data[pos:pos + 1] == b"#":
            pos = data.index(b"\n", pos) + 1
            continue
        end = pos
        while not data[end:end + 1].isspace():
            end += 1
        parts.append(data[pos:end])
        pos = end
    w, h = int(parts[1]), int(parts[2])
    return w, h, data[pos + 1:pos + 1 + w * h * 3]


def write_png(path, w: int, h: int, rgb: bytes) -> Path:
    def chunk(kind: bytes, body: bytes) -> bytes:
        return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body) & 0xFFFFFFFF)
    rows = b"".join(b"\x00" + rgb[y * w * 3:(y + 1) * w * 3] for y in range(h))
    png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)) \
        + chunk(b"IDAT", zlib.compress(rows, 6)) + chunk(b"IEND", b"")
    Path(path).write_bytes(png)
    return Path(path)


def ppm_to_png(ppm, png) -> Path:
    w, h, rgb = read_ppm(ppm)
    return write_png(png, w, h, rgb)


def crop(img, rect):
    w, h, rgb = img
    x0, y0, x1, y1 = (max(0, int(rect[0])), max(0, int(rect[1])), min(w, int(rect[2])), min(h, int(rect[3])))
    out = b"".join(rgb[(y * w + x0) * 3:(y * w + x1) * 3] for y in range(y0, y1))
    return x1 - x0, y1 - y0, out


def diff_fraction(a, b, threshold: int = 12) -> float:
    """Share of pixels that differ visibly between two frames of one size."""
    if a[0] != b[0] or a[1] != b[1]:
        return 1.0
    ra, rb = a[2], b[2]
    n = a[0] * a[1]
    changed = 0
    for i in range(0, n * 3, 3):
        if abs(ra[i] - rb[i]) > threshold or abs(ra[i + 1] - rb[i + 1]) > threshold or abs(ra[i + 2] - rb[i + 2]) > threshold:
            changed += 1
    return changed / max(n, 1)


def colour_share(img, predicate) -> float:
    w, h, rgb = img
    n = w * h
    hit = sum(1 for i in range(0, n * 3, 3) if predicate(rgb[i], rgb[i + 1], rgb[i + 2]))
    return hit / max(n, 1)


def distinct_colours(img, step: int = 7) -> int:
    w, h, rgb = img
    seen = set()
    for i in range(0, w * h * 3, 3 * step):
        seen.add((rgb[i] >> 3, rgb[i + 1] >> 3, rgb[i + 2] >> 3))
    return len(seen)
