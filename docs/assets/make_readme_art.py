"""Builds the README artwork from the HUD snapshots rendered by the test suite.

1. Render the panel at 4K in both languages:
     $env:VAR_HUD_SNAPSHOT_DIR = "$env:TEMP\\var_hud"; native\\build_tmp\\test_var_core.exe
2. python docs/assets/make_readme_art.py "%TEMP%\\var_hud"

Writes docs/images/<lang>/*.png (one image per screen, rounded, transparent background with a soft shadow,
readable on GitHub light and dark themes) and docs/images/hero_<lang>.png (cover).
"""
import os
import sys

from PIL import Image, ImageDraw, ImageFilter, ImageFont

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
OUT_DIR = os.path.join(ROOT, 'docs', 'images')
FONT_DIR = os.path.join(os.environ.get('WINDIR', r'C:\Windows'), 'Fonts')
SCALE = 0.6        # 4K panel (880 px wide) -> 528 px, shown at ~280 px on GitHub (crisp on HiDPI)
RADIUS = 16        # panel corner radius at 4K (kRadCard 8 * UI scale 2)


def font(name, px):
    return ImageFont.truetype(os.path.join(FONT_DIR, name), px)


def is_panel(c):
    return c[0] < 40 and c[1] < 46 and c[2] < 58


def bbox_dark(im, x0, y0, x1, y1, min_run=30):
    """Bounding box of the HUD (dark navy) inside a region of a snapshot rendered over the green pitch."""
    px = im.load()
    xs, ys = [], []
    for y in range(y0, y1, 2):
        run = 0
        for x in range(x0, x1, 2):
            if is_panel(px[x, y]):
                run += 1
                if run >= min_run // 2:
                    xs.append(x)
                    ys.append(y)
            else:
                run = 0
    if not xs:
        raise RuntimeError('no HUD found')
    left = min(xs) - min_run
    return max(left, x0), max(min(ys) - 2, y0), min(max(xs) + 3, x1), min(max(ys) + 3, y1)


def rounded(im, radius):
    mask = Image.new('L', im.size, 0)
    ImageDraw.Draw(mask).rounded_rectangle([0, 0, im.width - 1, im.height - 1], radius=radius, fill=255)
    out = im.convert('RGBA')
    out.putalpha(mask)
    return out


def with_shadow(card, pad=36, blur=18, alpha=150, offset=10):
    w, h = card.size
    canvas = Image.new('RGBA', (w + pad * 2, h + pad * 2), (0, 0, 0, 0))
    shadow = Image.new('RGBA', canvas.size, (0, 0, 0, 0))
    ImageDraw.Draw(shadow).rounded_rectangle([pad, pad + offset, pad + w, pad + h + offset], radius=RADIUS,
                                             fill=(0, 0, 0, alpha))
    shadow = shadow.filter(ImageFilter.GaussianBlur(blur))
    canvas.alpha_composite(shadow)
    canvas.alpha_composite(card, (pad, pad))
    return canvas


def crop_panel(path):
    im = Image.open(path).convert('RGB')
    w, h = im.size
    box = bbox_dark(im, int(w * 0.70), 0, w, h)
    return im.crop(box)


def crop_region(path, rx0, ry0, rx1, ry1):
    im = Image.open(path).convert('RGB')
    w, h = im.size
    box = bbox_dark(im, int(w * rx0), int(h * ry0), int(w * rx1), int(h * ry1), min_run=16)
    return im.crop(box)


def save_card(img, name, lang):
    card = rounded(img, RADIUS)
    card = card.resize((int(card.width * SCALE), int(card.height * SCALE)), Image.LANCZOS)
    out = with_shadow(card, pad=22, blur=12, alpha=120, offset=6)
    d = os.path.join(OUT_DIR, lang)
    os.makedirs(d, exist_ok=True)
    out.save(os.path.join(d, name + '.png'), optimize=True)
    return card


def hero(snap, lang, revisao, pool, decisao):
    W, H = 2400, 1000
    # Background: the pitch of the snapshot, blurred and darkened, so the cover feels like the match
    pitch = Image.open(os.path.join(snap, f'{lang}_3840x2160_1_revisao.bmp')).convert('RGB')
    pitch = pitch.crop((0, 300, 2600, 300 + int(2600 * H / W))).resize((W, H)).filter(ImageFilter.GaussianBlur(3))
    bg = Image.new('RGBA', (W, H))
    bg.paste(pitch, (0, 0))
    shade = Image.new('RGBA', (W, H), (0, 0, 0, 0))
    sd = ImageDraw.Draw(shade)
    for x in range(W):
        t = x / (W - 1)
        a = int(235 - 120 * t)  # darker on the text side
        sd.line([(x, 0), (x, H)], fill=(8, 12, 18, a))
    bg.alpha_composite(shade)
    vign = Image.new('L', (W, H), 0)
    ImageDraw.Draw(vign).ellipse([-W * 0.2, -H * 0.6, W * 1.2, H * 1.6], fill=255)
    vign = vign.filter(ImageFilter.GaussianBlur(160))
    dark = Image.new('RGBA', (W, H), (6, 9, 14, 255))
    bg = Image.composite(bg, dark, vign)
    bg = bg.convert('RGBA')

    # Panels on the right, staggered like a broadcast graphic stack
    def place(img, x, y, scale):
        c = rounded(img, RADIUS)
        c = c.resize((int(c.width * scale), int(c.height * scale)), Image.LANCZOS)
        c = with_shadow(c, pad=40, blur=26, alpha=170, offset=14)
        bg.alpha_composite(c, (x - 40, y - 40))

    place(decisao, 1290, 170, 0.50)
    place(revisao, 1560, 70, 0.56)
    place(pool, 1890, 200, 0.50)

    d = ImageDraw.Draw(bg)
    logo = Image.open(os.path.join(ROOT, 'docs', 'assets', 'logo.png')).convert('RGBA').resize((200, 200), Image.LANCZOS)
    bg.alpha_composite(logo, (130, 170))
    title = 'VAR para PES 2021' if lang == 'pt' else 'VAR for PES 2021'
    d.text((130, 400), title, font=font('segoeuib.ttf', 104), fill=(245, 248, 252))
    if lang == 'pt':
        lines = ['Pause. Reveja. Decida.', 'O jogo obedece.']
        sub = 'Árbitro de vídeo dentro do eFootball PES 2021, direto na engine.'
    else:
        lines = ['Pause. Review. Decide.', 'The game obeys.']
        sub = 'A video assistant referee inside eFootball PES 2021, straight in the engine.'
    d.text((134, 540), lines[0], font=font('segoeuisl.ttf', 64), fill=(200, 214, 230))
    d.text((134, 616), lines[1], font=font('segoeuib.ttf', 64), fill=(56, 156, 255))
    d.text((136, 728), sub, font=font('segoeui.ttf', 36), fill=(150, 165, 182))

    out = bg.convert('RGB').resize((1600, int(1600 * H / W)), Image.LANCZOS)
    out.save(os.path.join(OUT_DIR, f'hero_{lang}.png'), optimize=True)


# Regions of the review page as fractions of the screen (the layout scales with the UI scale, so they hold at any
# resolution): replay bar, incident card and tactical pitch, inside the panel's content column
REGIONS = {
    'timeline': (0.769, 0.109, 0.981, 0.150),
    'card':     (0.769, 0.155, 0.981, 0.229),
    'pitch':    (0.769, 0.238, 0.981, 0.480),
}
PANEL_TOP = (0.735, 0.0, 0.996, 0.486)  # panel from the header to the bottom of the pitch


def crop_frac(im, box):
    w, h = im.size
    return im.crop((int(w * box[0]), int(h * box[1]), int(w * box[2]), int(h * box[3])))


def feature_cards(snap, lang):
    """Close-ups of the new features, crisp from the 4K render (the replay bar from a frame where the suspect pulses)."""
    full = Image.open(os.path.join(snap, f'{lang}_3840x2160_1_revisao.bmp')).convert('RGB')
    out = {}
    for name in ('card', 'pitch'):
        out[name] = crop_frac(full, REGIONS[name])
    seq = os.path.join(snap, f'seq_{lang}_040.bmp')  # cursor between markers, suspect pulsing
    if os.path.exists(seq):
        tl = crop_frac(Image.open(seq).convert('RGB'), REGIONS['timeline'])
        out['timeline'] = tl.resize((tl.width * 2, tl.height * 2), Image.LANCZOS)
    for name, img in out.items():
        card = rounded(img, 12)
        target_w = 900 if name == 'pitch' else 760
        card = card.resize((target_w, int(card.height * target_w / card.width)), Image.LANCZOS)
        shadowed = with_shadow(card, pad=22, blur=12, alpha=110, offset=6)
        d = os.path.join(OUT_DIR, lang)
        os.makedirs(d, exist_ok=True)
        shadowed.save(os.path.join(d, f'feat_{name}.png'), optimize=True)


def replay_gif(snap, lang):
    """The panel following the replay frame by frame (VAR_HUD_SEQUENCE render): bar, card and pitch."""
    frames = []
    i = 0
    while True:
        f = os.path.join(snap, f'seq_{lang}_{i:03d}.bmp')
        if not os.path.exists(f):
            break
        im = crop_frac(Image.open(f).convert('RGB'), PANEL_TOP)
        im = im.resize((int(im.width * 0.9), int(im.height * 0.9)), Image.LANCZOS)
        frames.append(im)
        i += 1
    if not frames:
        return
    # Hold the last frame (the slide) so the eye can read it before the loop restarts
    durations = [70] * (len(frames) - 1) + [1600]
    # 256-colour octree per frame keeps the role colours (orange offender, cyan victim) and the gold suspect true
    pal = [fr.quantize(colors=256, method=Image.Quantize.FASTOCTREE) for fr in frames]
    pal[0].save(os.path.join(OUT_DIR, lang, 'replay.gif'), save_all=True, append_images=pal[1:],
                duration=durations, loop=0, optimize=True, disposal=1)


def main():
    snap = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.environ.get('TEMP', '.'), 'var_hud')
    for lang in ('pt', 'en'):
        p = lambda n: os.path.join(snap, n)
        revisao = crop_panel(p(f'{lang}_3840x2160_1_revisao.bmp'))
        decisao = crop_panel(p(f'{lang}_3840x2160_1b_decisao.bmp'))
        cartoes = crop_panel(p(f'{lang}_3840x2160_2_cartoes.bmp'))
        opcoes = crop_panel(p(f'{lang}_3840x2160_3_opcoes.bmp'))
        controles = crop_panel(p(f'{lang}_3840x2160_4_controles.bmp'))
        pool = crop_panel(p(f'{lang}_2160_7_aberto_pronta.bmp'))
        pill = crop_region(p(f'{lang}_2160_6_recolhido.bmp'), 0.55, 0.0, 1.0, 0.12)
        for img, name in ((revisao, 'revisao'), (decisao, 'decisao'), (cartoes, 'cartoes'), (opcoes, 'opcoes'),
                          (controles, 'controles'), (pool, 'pool'), (pill, 'recolhido')):
            save_card(img, name, lang)
        hero(snap, lang, revisao, pool, decisao)
        feature_cards(snap, lang)
        replay_gif(snap, lang)
        print('ok', lang)


if __name__ == '__main__':
    main()
