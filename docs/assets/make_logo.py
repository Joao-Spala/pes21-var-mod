"""Generates docs/assets/logo.png (README logo) in the visual language of the in-game VAR panel.

Usage: python docs/assets/make_logo.py   (needs Pillow and the Segoe UI fonts that ship with Windows)
"""
import os

from PIL import Image, ImageDraw, ImageFilter, ImageFont

SIZE = 576          # drawn at 2x and downsampled for smooth edges
OUT = 288
HERE = os.path.dirname(os.path.abspath(__file__))
FONT_DIR = os.path.join(os.environ.get('WINDIR', r'C:\Windows'), 'Fonts')

BG_TOP = (21, 28, 38)
BG_BOTTOM = (11, 15, 21)
STROKE = (48, 60, 76)
PITCH = (28, 74, 46)
PITCH_LINE = (160, 205, 175)
ACCENT = (56, 156, 255)
ACCENT_DARK = (30, 110, 210)
WHITE = (245, 248, 252)


def font(name, px):
    return ImageFont.truetype(os.path.join(FONT_DIR, name), px)


def main():
    img = Image.new('RGBA', (SIZE, SIZE), (0, 0, 0, 0))

    # Tile with a vertical gradient and a thin border, like the panel cards
    grad = Image.new('RGBA', (SIZE, SIZE))
    gd = ImageDraw.Draw(grad)
    for y in range(SIZE):
        t = y / (SIZE - 1)
        c = tuple(int(BG_TOP[i] + (BG_BOTTOM[i] - BG_TOP[i]) * t) for i in range(3))
        gd.line([(0, y), (SIZE, y)], fill=c + (255,))
    mask = Image.new('L', (SIZE, SIZE), 0)
    md = ImageDraw.Draw(mask)
    radius = 120
    md.rounded_rectangle([8, 8, SIZE - 8, SIZE - 8], radius=radius, fill=255)
    img.paste(grad, (0, 0), mask)
    d = ImageDraw.Draw(img)
    d.rounded_rectangle([8, 8, SIZE - 8, SIZE - 8], radius=radius, outline=STROKE, width=4)

    # Mini tactical pitch (bottom half), same palette as the panel radar
    px0, py0, px1, py1 = 96, 300, SIZE - 96, 492
    d.rounded_rectangle([px0, py0, px1, py1], radius=18, fill=PITCH)
    lw = 5
    inset = 16
    fx0, fy0, fx1, fy1 = px0 + inset, py0 + inset, px1 - inset, py1 - inset
    d.rectangle([fx0, fy0, fx1, fy1], outline=PITCH_LINE, width=lw)
    mx = (fx0 + fx1) // 2
    my = (fy0 + fy1) // 2
    d.line([(mx, fy0), (mx, fy1)], fill=PITCH_LINE, width=lw)
    r = 34
    d.ellipse([mx - r, my - r, mx + r, my + r], outline=PITCH_LINE, width=lw)
    bh = 90
    bw = 46
    d.rectangle([fx0, my - bh // 2, fx0 + bw, my + bh // 2], outline=PITCH_LINE, width=lw)
    d.rectangle([fx1 - bw, my - bh // 2, fx1, my + bh // 2], outline=PITCH_LINE, width=lw)
    # Contact: offender (red) to fouled player (blue) with a dashed line, as drawn by the panel
    off = (fx0 + 96, my + 24)
    vic = (mx + 70, my - 40)
    steps = 9
    for i in range(steps):
        if i % 2:
            continue
        a = i / steps
        b = (i + 1) / steps
        d.line([(off[0] + (vic[0] - off[0]) * a, off[1] + (vic[1] - off[1]) * a),
                (off[0] + (vic[0] - off[0]) * b, off[1] + (vic[1] - off[1]) * b)], fill=WHITE, width=4)
    d.ellipse([off[0] - 15, off[1] - 15, off[0] + 15, off[1] + 15], fill=(226, 54, 74), outline=WHITE, width=4)
    d.ellipse([vic[0] - 15, vic[1] - 15, vic[0] + 15, vic[1] + 15], fill=ACCENT, outline=WHITE, width=4)

    # VAR badge (top), the panel's own mark, with a soft glow
    bx0, by0, bx1, by1 = 120, 92, SIZE - 120, 244
    glow = Image.new('RGBA', (SIZE, SIZE), (0, 0, 0, 0))
    ImageDraw.Draw(glow).rounded_rectangle([bx0, by0 + 10, bx1, by1 + 10], radius=34, fill=ACCENT + (110,))
    glow = glow.filter(ImageFilter.GaussianBlur(22))
    img.alpha_composite(glow)
    d = ImageDraw.Draw(img)
    d.rounded_rectangle([bx0, by0, bx1, by1], radius=34, fill=ACCENT)
    d.rounded_rectangle([bx0, by0 + (by1 - by0) // 2, bx1, by1], radius=34, fill=ACCENT_DARK)
    d.rectangle([bx0, by0 + (by1 - by0) // 2, bx1, by0 + (by1 - by0) // 2 + 34], fill=ACCENT_DARK)
    d.rounded_rectangle([bx0, by0, bx1, by0 + (by1 - by0) // 2 + 2], radius=34, fill=ACCENT)
    d.rectangle([bx0, by0 + 40, bx1, by0 + (by1 - by0) // 2 + 2], fill=ACCENT)
    f = font('segoeuib.ttf', 112)
    text = 'VAR'
    tb = d.textbbox((0, 0), text, font=f)
    tw, th = tb[2] - tb[0], tb[3] - tb[1]
    d.text(((bx0 + bx1 - tw) // 2 - tb[0], (by0 + by1 - th) // 2 - tb[1]), text, font=f, fill=WHITE)

    out = img.resize((OUT, OUT), Image.LANCZOS)
    out.save(os.path.join(HERE, 'logo.png'))
    print('wrote', os.path.join(HERE, 'logo.png'))


if __name__ == '__main__':
    main()
