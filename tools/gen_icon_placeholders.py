"""
(Re)creates crude 16x16 placeholder art in assets/icons/*.png - one per menu
row concept - for the user to repaint by hand in an image editor. Only ever
touches a file that doesn't already exist, so it's safe to rerun after some
icons have been replaced with real art (add a new row type later and rerun
to fill in just that one).

    python tools/gen_icon_placeholders.py

Run tools/gen_icons.py afterwards (and after any hand edit) to bake
assets/icons/*.png into the firmware - see that script's docstring.
"""
import os

from PIL import Image, ImageDraw

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT_DIR = os.path.join(ROOT, "assets", "icons")
SIZE = 16

TRANSPARENT = (0, 0, 0, 0)
WHITE = (255, 255, 255, 255)


def new_icon():
    return Image.new("RGBA", (SIZE, SIZE), TRANSPARENT)


def palette():
    img = new_icon()
    d = ImageDraw.Draw(img)
    d.ellipse((1, 3, 14, 14), outline=WHITE, width=2)
    for x, y, c in ((5, 5, (230, 60, 60)), (10, 5, (60, 140, 230)),
                    (4, 10, (230, 200, 60)), (10, 10, (80, 200, 90))):
        d.ellipse((x - 1, y - 1, x + 1, y + 1), fill=(*c, 255))
    return img


def dither():
    img = new_icon()
    d = ImageDraw.Draw(img)
    for y in range(2, 14, 4):
        for x in range(2, 14, 4):
            off = 2 if (y // 4) % 2 else 0
            d.point((x + off, y), fill=WHITE)
            d.point((x + off + 1, y), fill=WHITE)
            d.point((x + off, y + 1), fill=WHITE)
            d.point((x + off + 1, y + 1), fill=WHITE)
    return img


def style():
    img = new_icon()
    d = ImageDraw.Draw(img)
    d.rectangle((1, 5, 14, 13), outline=WHITE, width=2)
    d.rectangle((4, 2, 9, 5), outline=WHITE, width=1)
    d.ellipse((5, 6, 10, 11), outline=WHITE, width=2)
    return img


def scale():
    img = new_icon()
    d = ImageDraw.Draw(img)
    d.line((2, 13, 13, 2), fill=WHITE, width=2)
    d.line((2, 13, 2, 9), fill=WHITE, width=2)
    d.line((2, 13, 6, 13), fill=WHITE, width=2)
    d.line((13, 2, 9, 2), fill=WHITE, width=2)
    d.line((13, 2, 13, 6), fill=WHITE, width=2)
    return img


def method():
    img = new_icon()
    d = ImageDraw.Draw(img)
    pts = [(2, 12), (4, 8), (6, 11), (8, 5), (10, 9), (12, 4), (14, 7)]
    for x, y in pts:
        d.point((x, y), fill=WHITE)
        d.point((x, y - 1), fill=WHITE)
    return img


def size():
    img = new_icon()
    d = ImageDraw.Draw(img)
    for x0, y0, dx, dy in ((1, 1, 1, 0), (1, 1, 0, 1),
                           (14, 1, -1, 0), (14, 1, 0, 1),
                           (1, 14, 1, 0), (1, 14, 0, -1),
                           (14, 14, -1, 0), (14, 14, 0, -1)):
        x1, y1 = x0 + dx * 4, y0 + dy * 4
        d.line((x0, y0, x1, y1), fill=WHITE, width=2)
    return img


def amount():
    img = new_icon()
    d = ImageDraw.Draw(img)
    d.rectangle((3, 1, 12, 14), outline=WHITE, width=2)
    d.rectangle((5, 8, 10, 12), fill=WHITE)
    return img


def denoise():
    img = new_icon()
    d = ImageDraw.Draw(img)

    def star(cx, cy, r):
        d.line((cx - r, cy, cx + r, cy), fill=WHITE)
        d.line((cx, cy - r, cx, cy + r), fill=WHITE)

    star(8, 8, 5)
    star(3, 3, 2)
    star(13, 12, 2)
    return img


def gallery():
    img = new_icon()
    d = ImageDraw.Draw(img)
    d.rectangle((1, 2, 14, 13), outline=WHITE, width=2)
    d.ellipse((3, 4, 6, 7), outline=WHITE, width=1)
    d.line((1, 12, 6, 7, 10, 10, 14, 6), fill=WHITE, width=1)
    return img


def exit_icon():
    img = new_icon()
    d = ImageDraw.Draw(img)
    d.arc((2, 2, 13, 13), start=40, end=320, fill=WHITE, width=2)
    d.line((7, 1, 7, 7), fill=WHITE, width=2)
    return img


def frame():
    img = new_icon()
    d = ImageDraw.Draw(img)
    d.rectangle((1, 1, 14, 14), outline=WHITE, width=2)
    d.rectangle((5, 5, 10, 10), outline=WHITE, width=1)
    return img


ICONS = {
    "palette": palette, "dither": dither, "style": style, "scale": scale,
    "method": method, "size": size, "amount": amount, "denoise": denoise,
    "gallery": gallery, "exit": exit_icon, "frame": frame,
}


def main():
    os.makedirs(OUT_DIR, exist_ok=True)
    made, skipped = 0, 0
    for name, fn in ICONS.items():
        path = os.path.join(OUT_DIR, f"{name}.png")
        if os.path.exists(path):
            skipped += 1
            continue
        fn().save(path)
        made += 1
    print(f"{made} placeholder icon(s) written to {OUT_DIR}, {skipped} already present (left alone)")


if __name__ == "__main__":
    main()
