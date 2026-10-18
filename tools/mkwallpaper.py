import argparse
import math
import random
import struct
import zlib

STARS = [(0.18, 0.30), (0.24, 0.36), (0.30, 0.41), (0.27, 0.55), (0.36, 0.60), (0.33, 0.47), (0.40, 0.44)]


def chunk(kind, data):
    body = kind + data
    return struct.pack('>I', len(data)) + body + struct.pack('>I', zlib.crc32(body) & 0xFFFFFFFF)


def render(w, h, seed):
    rnd = random.Random(seed)
    blobs = [(rnd.uniform(0.1, 0.9) * w, rnd.uniform(0.1, 0.9) * h, rnd.uniform(0.15, 0.35) * w,
              (rnd.randint(40, 110), rnd.randint(20, 60), rnd.randint(90, 160))) for _ in range(5)]
    img = [[0.0, 0.0, 0.0] for _ in range(w * h)]
    for y in range(h):
        t = y / (h - 1)
        top = (12 + 10 * t, 16 + 8 * t, 34 + 30 * t)
        for x in range(w):
            p = img[y * w + x]
            p[0], p[1], p[2] = top
    for bx, by, br, col in blobs:
        x0, x1 = max(0, int(bx - br)), min(w, int(bx + br))
        y0, y1 = max(0, int(by - br)), min(h, int(by + br))
        for y in range(y0, y1):
            dy = (y - by) / br
            for x in range(x0, x1):
                dx = (x - bx) / br
                d = dx * dx + dy * dy
                if d >= 1:
                    continue
                k = (1 - d) ** 2 * 0.45
                p = img[y * w + x]
                for c in range(3):
                    p[c] += col[c] * k
    for _ in range(w * h // 900):
        x, y = rnd.randrange(w), rnd.randrange(h)
        v = rnd.randint(120, 255)
        p = img[y * w + x]
        p[0], p[1], p[2] = max(p[0], v), max(p[1], v), max(p[2], v * 0.9 + 25)
    pts = [(int(sx * w), int(sy * h)) for sx, sy in STARS]
    for (ax, ay), (bx2, by2) in zip(pts, pts[1:]):
        n = int(math.hypot(bx2 - ax, by2 - ay))
        for i in range(n):
            x = int(ax + (bx2 - ax) * i / n)
            y = int(ay + (by2 - ay) * i / n)
            p = img[y * w + x]
            p[0], p[1], p[2] = p[0] + 30, p[1] + 40, p[2] + 60
    for sx, sy in pts:
        for y in range(sy - 3, sy + 4):
            for x in range(sx - 3, sx + 4):
                if 0 <= x < w and 0 <= y < h:
                    k = max(0.0, 1 - math.hypot(x - sx, y - sy) / 3.5)
                    p = img[y * w + x]
                    p[0], p[1], p[2] = p[0] + 230 * k, p[1] + 230 * k, p[2] + 255 * k
    rows = bytearray()
    for y in range(h):
        rows.append(0)
        for x in range(w):
            p = img[y * w + x]
            rows += bytes(min(255, int(c)) for c in p)
    return rows


def main():
    ap = argparse.ArgumentParser(description='Gera o papel de parede PNG do Orion')
    ap.add_argument('output')
    ap.add_argument('--size', default='1280x800')
    ap.add_argument('--seed', type=int, default=2026)
    a = ap.parse_args()
    w, h = (int(v) for v in a.size.split('x'))
    raw = render(w, h, a.seed)
    png = b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0))
    png += chunk(b'IDAT', zlib.compress(bytes(raw), 9)) + chunk(b'IEND', b'')
    with open(a.output, 'wb') as fh:
        fh.write(png)
    print('mkwallpaper: %s, %dx%d, %d bytes' % (a.output, w, h, len(png)))


if __name__ == '__main__':
    main()
