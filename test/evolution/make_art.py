# SPDX-License-Identifier: GPL-2.0-or-later
"""Draw the original Evolution Arena artwork: mutation icons, status icons,
ground marks and the egg and husk battle sprites. Pure standard library.

Usage: python test/evolution/make_art.py   (run from the repository root)
"""
import json
import math
import os
import struct
import zlib

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..")
CONTENT = os.path.join(ROOT, "Mods", "arenabattles", "Content")
SUB = [(i + 0.5) / 4 for i in range(4)]


class Image:
    def __init__(self, w, h):
        self.w, self.h = w, h
        self.px = [[0.0, 0.0, 0.0, 0.0] for _ in range(w * h)]

    def paint(self, box, inside, color):
        x0, y0, x1, y1 = box
        for y in range(max(0, int(y0)), min(self.h, int(math.ceil(y1)))):
            for x in range(max(0, int(x0)), min(self.w, int(math.ceil(x1)))):
                hits = 0
                for oy in SUB:
                    for ox in SUB:
                        if inside(x + ox, y + oy):
                            hits += 1
                if not hits:
                    continue
                r, g, b, a = color(x + 0.5, y + 0.5) if callable(color) else color
                a *= hits / 16.0
                dst = self.px[y * self.w + x]
                out = a + dst[3] * (1 - a)
                if out <= 0:
                    continue
                for i, c in enumerate((r, g, b)):
                    dst[i] = (c * a + dst[i] * dst[3] * (1 - a)) / out
                dst[3] = out

    def fade(self, factor):
        copy = Image(self.w, self.h)
        copy.px = [[p[0], p[1], p[2], p[3] * factor] for p in self.px]
        return copy

    def tint(self, add):
        copy = Image(self.w, self.h)
        copy.px = [[min(255.0, p[0] + add), min(255.0, p[1] + add), min(255.0, p[2] + add), p[3]] for p in self.px]
        return copy

    def save(self, path):
        os.makedirs(os.path.dirname(path), exist_ok=True)
        raw = bytearray()
        for y in range(self.h):
            raw.append(0)
            for x in range(self.w):
                r, g, b, a = self.px[y * self.w + x]
                raw += bytes((int(round(r)), int(round(g)), int(round(b)), int(round(max(0.0, min(1.0, a)) * 255))))

        def chunk(tag, data):
            return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)

        with open(path, "wb") as out:
            out.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", self.w, self.h, 8, 6, 0, 0, 0))
                      + chunk(b"IDAT", zlib.compress(bytes(raw), 9)) + chunk(b"IEND", b""))


def ellipse(cx, cy, rx, ry):
    return lambda x, y: ((x - cx) / rx) ** 2 + ((y - cy) / ry) ** 2 <= 1


def stroke(x1, y1, x2, y2, width):
    dx, dy = x2 - x1, y2 - y1
    length = dx * dx + dy * dy or 1e-9
    half = width / 2.0

    def inside(x, y):
        t = max(0.0, min(1.0, ((x - x1) * dx + (y - y1) * dy) / length))
        return (x - x1 - t * dx) ** 2 + (y - y1 - t * dy) ** 2 <= half * half
    return inside


def polygon(points):
    def inside(x, y):
        odd = False
        j = len(points) - 1
        for i in range(len(points)):
            xi, yi = points[i]
            xj, yj = points[j]
            if (yi > y) != (yj > y) and x < (xj - xi) * (y - yi) / (yj - yi) + xi:
                odd = not odd
            j = i
        return odd
    return inside


def both(a, b):
    return lambda x, y: a(x, y) and b(x, y)


def without(a, b):
    return lambda x, y: a(x, y) and not b(x, y)


def egg_shape(cx, cy, rx, ry):
    # Narrower at the top than at the bottom.
    return lambda x, y: ((x - cx) / (rx * (0.78 + 0.22 * max(0.0, min(1.0, (y - (cy - ry)) / (2 * ry)))))) ** 2 + ((y - cy) / ry) ** 2 <= 1


def shade(base, cx, cy, radius, light=70, dark=60, alpha=1.0):
    def color(x, y):
        d = math.hypot(x - (cx - radius * 0.35), y - (cy - radius * 0.45)) / radius
        k = light * (1 - min(1.0, d)) - dark * max(0.0, d - 0.6)
        return tuple(max(0.0, min(255.0, c + k)) for c in base) + (alpha,)
    return color


RARITY = [(176, 176, 176), (74, 144, 226), (166, 84, 214), (240, 138, 28)]
FAMILY = {
    "egg": [1, 18, 32, 39, 54, 60, 62, 68, 75, 87, 89, 95, 97, 29, 45, 84, 90],
    "drop": [2, 8, 9, 19, 23, 66, 71, 30, 43, 63, 69],
    "hex": [4, 6, 13, 15, 17, 20, 24, 51, 11, 37, 46],
    "swap": [10, 25, 27, 41, 50, 59, 67, 88, 44, 55, 65, 76, 80, 96],
    "shield": [3, 7, 14, 22, 47, 53, 73, 33, 40, 42, 98],
    "spore": [21, 26, 48, 77, 82, 16, 31, 38, 81],
    "claw": [35, 34, 94],
    "bone": [5, 12, 28, 49, 52, 58, 64, 72],
}
GLYPH_COLOR = {"egg": (226, 214, 170), "drop": (206, 40, 44), "hex": (150, 190, 96), "swap": (120, 200, 220),
               "shield": (200, 204, 214), "spore": (128, 214, 96), "claw": (236, 170, 60), "bone": (230, 226, 208),
               "helix": (196, 150, 236)}


def glyph(img, kind, cx, cy, s, color=None):
    """Draw one symbol centred on (cx, cy); s is roughly half of its size."""
    base = color or GLYPH_COLOR[kind]
    box = (cx - s - 3, cy - s - 3, cx + s + 3, cy + s + 3)
    lit = shade(base, cx, cy, s * 1.2)
    edge = (20, 16, 20, 0.9)
    if kind == "egg":
        img.paint(box, egg_shape(cx, cy + 1, s * 0.74 + 1.2, s + 1.2), edge)
        img.paint(box, egg_shape(cx, cy + 1, s * 0.74, s), lit)
        for dx, dy, r in ((-0.25, 0.25, 0.14), (0.22, -0.1, 0.11), (0.05, 0.5, 0.1)):
            img.paint(box, ellipse(cx + dx * s, cy + dy * s, r * s, r * s), (150, 110, 70, 0.75))
    elif kind == "drop":
        tip = polygon([(cx, cy - s), (cx + s * 0.62, cy + s * 0.2), (cx - s * 0.62, cy + s * 0.2)])
        body = ellipse(cx, cy + s * 0.32, s * 0.68, s * 0.68)

        def shape(x, y):
            return tip(x, y) or body(x, y)
        img.paint(box, lambda x, y: shape(x, y + 1.2) or shape(x, y - 1.2) or shape(x + 1.2, y) or shape(x - 1.2, y), edge)
        img.paint(box, shape, lit)
        img.paint(box, ellipse(cx - s * 0.22, cy + s * 0.2, s * 0.16, s * 0.24), (255, 235, 235, 0.55))
    elif kind == "hex":
        outer = [(cx + s * math.sin(math.radians(a)), cy - s * math.cos(math.radians(a))) for a in range(0, 360, 60)]
        inner = [(cx + s * 0.62 * math.sin(math.radians(a)), cy - s * 0.62 * math.cos(math.radians(a))) for a in range(0, 360, 60)]
        img.paint(box, polygon([(cx + (x - cx) * 1.1, cy + (y - cy) * 1.1) for x, y in outer]), edge)
        img.paint(box, without(polygon(outer), polygon(inner)), lit)
        img.paint(box, ellipse(cx, cy, s * 0.26, s * 0.26), lit)
    elif kind == "swap":
        for sign, oy in ((1, -0.42), (-1, 0.42)):
            y = cy + oy * s
            x1, x2 = cx - sign * s * 0.85, cx + sign * s * 0.85
            head = polygon([(x2, y), (x2 - sign * s * 0.55, y - s * 0.42), (x2 - sign * s * 0.55, y + s * 0.42)])
            shaft = stroke(x1, y, x2 - sign * s * 0.5, y, s * 0.3)
            wide = stroke(x1, y, x2 - sign * s * 0.5, y, s * 0.3 + 2.4)
            img.paint(box, lambda x, yy, h=head, sh=wide: sh(x, yy) or h(x, yy - 1.2) or h(x, yy + 1.2) or h(x - 1.2, yy) or h(x + 1.2, yy), edge)
            img.paint(box, lambda x, yy, h=head, sh=shaft: sh(x, yy) or h(x, yy), lit)
    elif kind == "shield":
        top = polygon([(cx - s * 0.8, cy - s * 0.85), (cx + s * 0.8, cy - s * 0.85), (cx + s * 0.8, cy + s * 0.05), (cx - s * 0.8, cy + s * 0.05)])
        low = polygon([(cx - s * 0.8, cy), (cx + s * 0.8, cy), (cx, cy + s)])

        def shape(x, y):
            return top(x, y) or low(x, y)
        img.paint(box, lambda x, y: shape(cx + (x - cx) / 1.12, cy + (y - cy) / 1.12), edge)
        img.paint(box, shape, lit)
        img.paint(box, both(shape, stroke(cx, cy - s * 0.7, cx, cy + s * 0.75, s * 0.18)), (90, 96, 112, 0.8))
        img.paint(box, both(shape, stroke(cx - s * 0.6, cy - s * 0.25, cx + s * 0.6, cy - s * 0.25, s * 0.18)), (90, 96, 112, 0.8))
    elif kind == "spore":
        for dx, dy, r in ((0, 0, 0.42), (-0.6, -0.45, 0.26), (0.62, -0.4, 0.22), (-0.5, 0.58, 0.22), (0.55, 0.55, 0.28), (0.05, -0.78, 0.16)):
            img.paint(box, ellipse(cx + dx * s, cy + dy * s, r * s + 1.2, r * s + 1.2), edge)
            img.paint(box, ellipse(cx + dx * s, cy + dy * s, r * s, r * s), shade(base, cx + dx * s, cy + dy * s, r * s * 1.3))
    elif kind == "claw":
        for off in (-0.55, 0.0, 0.55):
            a = (cx + (off + 0.35) * s, cy - s * 0.9)
            b = (cx + (off - 0.35) * s, cy + s * 0.9)
            img.paint(box, stroke(a[0], a[1], b[0], b[1], s * 0.3 + 2.4), edge)
            img.paint(box, stroke(a[0], a[1], b[0], b[1], s * 0.3), lit)
    elif kind == "bone":
        a = (cx - s * 0.6, cy + s * 0.6)
        b = (cx + s * 0.6, cy - s * 0.6)
        knobs = [(a[0] - s * 0.18, a[1] - s * 0.1), (a[0] + s * 0.1, a[1] + s * 0.18), (b[0] + s * 0.18, b[1] + s * 0.1), (b[0] - s * 0.1, b[1] - s * 0.18)]
        img.paint(box, stroke(a[0], a[1], b[0], b[1], s * 0.3 + 2.4), edge)
        for kx, ky in knobs:
            img.paint(box, ellipse(kx, ky, s * 0.24 + 1.2, s * 0.24 + 1.2), edge)
        img.paint(box, stroke(a[0], a[1], b[0], b[1], s * 0.3), lit)
        for kx, ky in knobs:
            img.paint(box, ellipse(kx, ky, s * 0.24, s * 0.24), lit)
    else:  # helix
        steps = 40
        for width, paint in ((s * 0.24 + 2.0, edge), (s * 0.24, lit)):
            for strand in (0, 1):
                previous = None
                for i in range(steps + 1):
                    t = i / steps
                    x = cx + math.sin(t * math.pi * 2 + strand * math.pi) * s * 0.5
                    y = cy - s + t * 2 * s
                    if previous:
                        img.paint(box, stroke(previous[0], previous[1], x, y, width), paint)
                    previous = (x, y)


def rounded(x0, y0, x1, y1, r):
    def inside(x, y):
        qx = max(x0 + r - x, 0, x - (x1 - r))
        qy = max(y0 + r - y, 0, y - (y1 - r))
        return x0 <= x <= x1 and y0 <= y <= y1 and qx * qx + qy * qy <= r * r
    return inside


def icon(kind, frame, glyph_color=None):
    img = Image(51, 51)
    full = (0, 0, 51, 51)
    img.paint(full, rounded(0.5, 0.5, 50.5, 50.5, 7), tuple(frame) + (1.0,))
    img.paint(full, rounded(3, 3, 48, 48, 5), lambda x, y: (34 + (y / 51) * 26, 28 + (y / 51) * 18, 40 + (y / 51) * 26, 1.0))
    glyph(img, kind, 25.5, 25.5, 15, glyph_color)
    return img


def hexagon(cx, cy, rx, ry):
    return polygon([(cx, cy - ry), (cx + rx, cy - ry / 2), (cx + rx, cy + ry / 2), (cx, cy + ry), (cx - rx, cy + ry / 2), (cx - rx, cy - ry / 2)])


def ground(kind):
    """A 45x52 picture that covers one battlefield cell."""
    img = Image(45, 52)
    full = (0, 0, 45, 52)
    cell = hexagon(22.5, 26, 20, 23)
    if kind == "blood":
        for dx, dy, rx, ry in ((0, 2, 11, 8), (-8, -5, 5, 4), (9, -6, 4, 3), (7, 9, 5, 3), (-10, 8, 3, 3), (2, -11, 3, 2)):
            img.paint(full, both(cell, ellipse(22.5 + dx, 26 + dy, rx, ry)), shade((150, 14, 20), 22.5 + dx, 26 + dy, max(rx, ry) * 1.4, 60, 40, 0.82))
    elif kind == "trap":
        img.paint(full, without(both(cell, ellipse(22.5, 27, 17, 13)), ellipse(22.5, 27, 12, 8.5)), (64, 60, 66, 0.9))
        for i in range(9):
            a = math.radians(i * 40)
            bx, by = 22.5 + math.cos(a) * 14.5, 27 + math.sin(a) * 10.5
            tx, ty = 22.5 + math.cos(a) * 5, 27 + math.sin(a) * 3.5
            nx, ny = -math.sin(a) * 3.0, math.cos(a) * 2.2
            img.paint(full, polygon([(bx + nx, by + ny), (bx - nx, by - ny), (tx, ty)]), (206, 206, 214, 0.95))
    elif kind == "spores":
        dots = ((0, 0, 7), (-10, -7, 5), (9, -8, 6), (-9, 8, 6), (10, 8, 5), (0, -14, 4), (1, 14, 4), (-15, 0, 3), (15, 1, 3))
        for dx, dy, r in dots:
            img.paint(full, both(cell, ellipse(22.5 + dx, 26 + dy, r + 2.5, r + 2.5)), (120, 200, 70, 0.22))
        for dx, dy, r in dots:
            img.paint(full, both(cell, ellipse(22.5 + dx, 26 + dy, r * 0.55, r * 0.55)), shade((150, 220, 90), 22.5 + dx, 26 + dy, r, 60, 30, 0.85))
    elif kind == "shadow":
        img.paint(full, both(cell, ellipse(22.5, 34, 15, 7)), (10, 8, 20, 0.6))
        img.paint(full, both(cell, ellipse(22.5, 24, 9, 14)), lambda x, y: (26, 18, 44, 0.75 - abs(y - 24) / 40))
        for dx in (-3.5, 3.5):
            img.paint(full, ellipse(22.5 + dx, 18, 1.8, 1.3), (230, 60, 60, 0.95))
    elif kind == "portal":
        for radius, color in ((17, (60, 30, 120, 0.85)), (13, (110, 70, 210, 0.9)), (9, (170, 140, 250, 0.9)), (5, (20, 10, 40, 0.95))):
            img.paint(full, both(cell, ellipse(22.5, 27, radius, radius * 0.72)), color)
        for i in range(6):
            a = math.radians(i * 60 + 15)
            img.paint(full, ellipse(22.5 + math.cos(a) * 15, 27 + math.sin(a) * 10.5, 1.5, 1.5), (240, 230, 255, 0.95))
    elif kind == "curse":
        img.paint(full, without(both(cell, ellipse(22.5, 26, 15, 15)), ellipse(22.5, 26, 12.5, 12.5)), (170, 40, 190, 0.85))
        star = [(22.5 + math.sin(math.radians(a)) * 12, 26 - math.cos(math.radians(a)) * 12) for a in (0, 144, 288, 72, 216)]
        for i in range(5):
            x1, y1 = star[i]
            x2, y2 = star[(i + 1) % 5]
            img.paint(full, stroke(x1, y1, x2, y2, 1.8), (220, 90, 235, 0.95))
    elif kind == "ash":
        for dx, dy, rx, ry in ((0, 6, 15, 7), (-4, 1, 10, 6), (3, -3, 7, 5), (0, -7, 4, 3)):
            img.paint(full, both(cell, ellipse(22.5 + dx, 28 + dy, rx, ry)), shade((120, 116, 112), 22.5 + dx, 28 + dy, max(rx, ry) * 1.4, 70, 50, 0.92))
        for dx, dy in ((-9, 8), (7, 5), (-2, 0), (4, 11), (-6, 3), (10, 9)):
            img.paint(full, ellipse(22.5 + dx, 28 + dy, 1.4, 1.2), (236, 150, 60, 0.95))
    elif kind == "mycelium":
        for i in range(7):
            a = math.radians(i * 360 / 7 + 12)
            mx, my = 22.5 + math.cos(a + 0.5) * 9, 26 + math.sin(a + 0.5) * 10
            ex, ey = 22.5 + math.cos(a) * 18, 26 + math.sin(a) * 21
            img.paint(full, both(cell, stroke(22.5, 26, mx, my, 1.8)), (232, 226, 200, 0.8))
            img.paint(full, both(cell, stroke(mx, my, ex, ey, 1.4)), (232, 226, 200, 0.7))
        for dx, dy in ((-7, 4), (8, -3), (1, 11)):
            img.paint(full, stroke(22.5 + dx, 26 + dy, 22.5 + dx, 26 + dy - 6, 2.4), (236, 228, 204, 0.95))
            cap = both(ellipse(22.5 + dx, 26 + dy - 6, 5.5, 4), lambda x, y, lim=26 + dy - 5.5: y <= lim)
            img.paint(full, cap, shade((176, 104, 60), 22.5 + dx, 26 + dy - 7, 6, 60, 30, 0.97))
    else:  # territory
        img.paint(full, without(cell, hexagon(22.5, 26, 17, 19.5)), (230, 150, 40, 0.55))
        for off in (-8, 0, 8):
            img.paint(full, both(cell, stroke(22.5 + off + 5, 14, 22.5 + off - 5, 38, 3.2)), (40, 22, 12, 0.85))
            img.paint(full, both(cell, stroke(22.5 + off + 5, 14, 22.5 + off - 5, 38, 1.6)), (236, 120, 40, 0.9))
    return img


def creature_frame(draw):
    """Battle sprites share the 450x400 canvas of the original creature animations.
    A standing unit is centred near x=196 with its feet near y=272."""
    img = Image(450, 400)
    draw(img, 196.0, 272.0)
    return img


def egg(img, cx, foot, squash=1.0, cracked=0):
    ry = 25 * squash
    rx = 19 / math.sqrt(squash)
    cy = foot - ry
    box = (cx - 30, foot - 62, cx + 30, foot + 6)
    img.paint(box, ellipse(cx, foot - 1, rx * 1.05, 4.5), (0, 0, 0, 0.35))
    img.paint(box, egg_shape(cx, cy, rx + 1.3, ry + 1.3), (40, 34, 26, 0.95))
    img.paint(box, egg_shape(cx, cy, rx, ry), shade((206, 196, 150), cx, cy, ry * 1.25, 70, 80))
    body = egg_shape(cx, cy, rx, ry)
    for dx, dy, r in ((-7, 6, 4.2), (6, -4, 3.4), (2, 13, 3), (-4, -12, 2.4), (10, 8, 2.2)):
        img.paint(box, both(body, ellipse(cx + dx, cy + dy * squash, r, r * squash)), (128, 92, 56, 0.7))
    for x1, y1, x2, y2 in ((-12, -2, -4, -9), (-4, -9, 3, -3), (3, -3, 12, -8)):
        img.paint(box, both(body, stroke(cx + x1, cy + y1 * squash, cx + x2, cy + y2 * squash, 1.2)), (150, 60, 70, 0.55))
    if cracked:
        for x1, y1, x2, y2 in ((-2, -22, 3, -12), (3, -12, -3, -4), (-3, -4, 4, 5), (3, -12, 10, -8))[:cracked + 1]:
            img.paint(box, both(body, stroke(cx + x1, cy + y1 * squash, cx + x2, cy + y2 * squash, 1.8)), (24, 18, 14, 0.95))


def egg_remains(img, cx, foot):
    box = (cx - 30, foot - 30, cx + 30, foot + 6)
    img.paint(box, ellipse(cx, foot - 1, 21, 5), (0, 0, 0, 0.35))
    img.paint(box, ellipse(cx, foot - 3, 17, 4.5), (150, 170, 80, 0.75))
    for pts in (((-17, 0), (-13, -14), (-7, -6), (-2, -16), (2, 0)), ((3, 0), (7, -11), (12, -5), (17, -13), (18, 0))):
        shape = polygon([(cx + x, foot - 2 + y) for x, y in pts])
        img.paint(box, shape, shade((206, 196, 150), cx, foot - 8, 20, 50, 60))


def husk(img, cx, foot, lean=0.0, alpha=1.0):
    box = (cx - 34, foot - 80, cx + 34, foot + 6)
    img.paint(box, ellipse(cx, foot - 1, 19, 4.5), (0, 0, 0, 0.3 * alpha))
    top = foot - 66
    outline = [(cx - 15 + lean * 0.2, foot - 2), (cx - 18 + lean * 0.5, foot - 30), (cx - 12 + lean, top + 8), (cx - 2 + lean, top),
               (cx + 9 + lean, top + 6), (cx + 17 + lean * 0.5, foot - 32), (cx + 14 + lean * 0.2, foot - 2)]
    body = polygon(outline)
    grown = polygon([(cx + (x - cx) * 1.07, foot + (y - foot) * 1.03) for x, y in outline])
    img.paint(box, grown, (44, 36, 30, 0.9 * alpha))
    img.paint(box, body, shade((186, 170, 136), cx, foot - 36, 40, 50, 70, 0.92 * alpha))
    # The split along which the creature left its old skin.
    split = polygon([(cx - 2 + lean, top + 2), (cx + 5 + lean * 0.7, foot - 34), (cx - 1 + lean * 0.4, foot - 12), (cx - 6 + lean * 0.7, foot - 36)])
    img.paint(box, both(body, split), (30, 22, 20, 0.92 * alpha))
    for i in range(5):
        y = foot - 10 - i * 11
        img.paint(box, both(without(body, split), stroke(cx - 16 + lean * 0.4, y, cx + 16 + lean * 0.4, y - 3, 1.3)), (120, 100, 76, 0.6 * alpha))


def resize(src, w, h):
    out = Image(w, h)
    for y in range(h):
        for x in range(w):
            acc = [0.0, 0.0, 0.0, 0.0]
            n = 0
            for sy in range(int(y * src.h / h), max(int(y * src.h / h) + 1, int((y + 1) * src.h / h))):
                for sx in range(int(x * src.w / w), max(int(x * src.w / w) + 1, int((x + 1) * src.w / w))):
                    p = src.px[sy * src.w + sx]
                    for i in range(3):
                        acc[i] += p[i] * p[3]
                    acc[3] += p[3]
                    n += 1
            if acc[3] > 0:
                out.px[y * w + x] = [acc[0] / acc[3], acc[1] / acc[3], acc[2] / acc[3], acc[3] / n]
    return out


def portrait(draw):
    img = Image(58, 64)
    img.paint((0, 0, 58, 64), lambda x, y: True, lambda x, y: (52 + y * 0.5, 44 + y * 0.35, 58 + y * 0.45, 1.0))
    draw(img, 29.0, 60.0)
    return img


def animation(name, groups, overlay=False):
    path = os.path.join(CONTENT, "Sprites", name + ".json")
    data = {"basepath": "arena/evolution/", "sequences": [{"group": g, "frames": frames} for g, frames in sorted(groups.items())]}
    if overlay:
        for sequence in data["sequences"]:
            sequence["generateOverlay"] = 1
    with open(path, "w", encoding="utf-8", newline="\n") as out:
        out.write(json.dumps(data, indent="\t") + "\n")


def main():
    data = os.path.join(CONTENT, "Data", "evolution")
    sprites = os.path.join(CONTENT, "Sprites", "arena", "evolution")
    design = json.load(open(os.path.join(CONTENT, "config", "evolution-design.json"), encoding="utf-8"))
    family = {trait: kind for kind, traits in FAMILY.items() for trait in traits}
    for row in design:
        icon(family.get(row["id"], "helix"), RARITY[row["rarity"]]).save(os.path.join(data, "trait%d.png" % row["id"]))
    status = {"parasite": ("egg", (150, 200, 90)), "wound": ("drop", None), "germ": ("spore", (226, 214, 120)),
              "viral": ("spore", (190, 120, 220)), "poison": ("spore", None), "slow": ("hex", (120, 170, 236)),
              "memory": ("helix", (120, 200, 220))}
    for name, (kind, color) in status.items():
        icon(kind, (150, 44, 44), color).save(os.path.join(data, "status_%s.png" % name))

    for kind in ("blood", "trap", "shadow", "ash", "spores", "mycelium", "portal", "curse", "territory"):
        picture = ground(kind)
        picture.save(os.path.join(sprites, "mark_%s.png" % kind))
        for step, share in enumerate((0.3, 0.6, 0.85)):
            picture.fade(share).save(os.path.join(sprites, "mark_%s_in%d.png" % (kind, step)))
        animation("evoMark_" + kind, {0: ["mark_%s.png" % kind]})
        animation("evoMarkIn_" + kind, {0: ["mark_%s_in%d.png" % (kind, step) for step in range(3)] + ["mark_%s.png" % kind]})

    def write(name, frames, groups):
        for key, image in frames.items():
            image.save(os.path.join(sprites, "%s_%s.png" % (name, key)))
        animation(name, {g: ["%s_%s.png" % (name, key) for key in keys] for g, keys in groups.items()}, True)

    still = creature_frame(lambda i, x, y: egg(i, x, y))
    frames = {"idle0": still, "idle1": creature_frame(lambda i, x, y: egg(i, x, y, 1.04)),
              "idle2": creature_frame(lambda i, x, y: egg(i, x, y, 0.96)), "hit": still.tint(70),
              "crack0": creature_frame(lambda i, x, y: egg(i, x, y, 1.0, 1)), "crack1": creature_frame(lambda i, x, y: egg(i, x, y, 0.9, 3)),
              "dead": creature_frame(egg_remains)}
    one = ["idle0"]
    pulse = ["idle0", "idle1", "idle0", "idle2"]
    dying = ["crack0", "crack1", "dead"]
    groups = {0: pulse, 1: pulse, 2: one, 3: ["hit", "idle0"], 4: ["hit", "idle0"], 5: dying, 6: dying, 7: one, 8: one, 9: one, 10: one,
              11: pulse, 12: pulse, 13: pulse, 20: one, 21: one}
    write("evoEgg", frames, groups)

    still = creature_frame(lambda i, x, y: husk(i, x, y))
    frames = {"idle0": still, "hit": still.tint(60), "fall0": creature_frame(lambda i, x, y: husk(i, x, y, 6.0, 0.8)),
              "fall1": creature_frame(lambda i, x, y: husk(i, x, y, 12.0, 0.5)), "dead": creature_frame(lambda i, x, y: husk(i, x, y, 16.0, 0.22))}
    dying = ["fall0", "fall1", "dead"]
    groups = {0: one, 1: one, 2: one, 3: ["hit", "idle0"], 4: ["hit", "idle0"], 5: dying, 6: dying, 7: one, 8: one, 9: one, 10: one,
              11: one, 12: one, 13: one, 20: one, 21: one}
    write("evoHusk", frames, groups)

    for name, draw in (("egg", lambda i, x, y: egg(i, x, y)), ("husk", lambda i, x, y: husk(i, x, y, 0.0, 1.0))):
        large = portrait(draw)
        large.save(os.path.join(data, "portrait_%s.png" % name))
        resize(large, 32, 32).save(os.path.join(data, "portrait_%s_small.png" % name))


if __name__ == "__main__":
    main()
