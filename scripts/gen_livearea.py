#!/usr/bin/env python3
"""Generate Cordless LiveArea art (icon0/bg/startup/pic0) with Pillow.

Writes PNGs into sce_sys/ from the constants below. Run from the repo root:

    python3 scripts/gen_livearea.py
"""
import os
from PIL import Image, ImageDraw, ImageFont, ImageFilter

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FONTS = os.path.join(ROOT, "assets", "fonts")
OUT_SCE = os.path.join(ROOT, "sce_sys")
OUT_LA = os.path.join(OUT_SCE, "livearea", "contents")

# Discord dark palette + blurple.
BLURPLE   = (88, 101, 242)     # #5865F2
BLURPLE_D = (64, 74, 194)
BLURPLE_L = (124, 135, 246)
DARK      = (24, 25, 28)       # #18191c
DARK2     = (43, 45, 49)       # #2b2d31
CARD      = (49, 51, 56)       # #313338
WHITE     = (255, 255, 255)
MUTED     = (168, 173, 182)

def semi(sz): return ImageFont.truetype(os.path.join(FONTS, "Inter-SemiBold.ttf"), sz)
def reg(sz):  return ImageFont.truetype(os.path.join(FONTS, "Inter-Regular.ttf"), sz)

def vgrad(w, h, top, bot):
    img = Image.new("RGB", (w, h))
    d = ImageDraw.Draw(img)
    for y in range(h):
        t = y / max(1, h - 1)
        d.line([(0, y), (w, y)],
               fill=tuple(int(top[i] + (bot[i] - top[i]) * t) for i in range(3)))
    return img

def dgrad(w, h, tl, br, n=96):
    """Diagonal gradient top-left(tl) -> bottom-right(br). Rendered small + upscaled
    (smooth, and far faster than per-pixel at full size)."""
    g = Image.new("RGB", (n, n))
    px = g.load()
    for y in range(n):
        for x in range(n):
            t = (x + y) / (2 * (n - 1))
            px[x, y] = tuple(int(tl[i] + (br[i] - tl[i]) * t) for i in range(3))
    return g.resize((w, h), Image.BILINEAR)

def glow(size, cx, cy, radius, color, alpha):
    lay = Image.new("RGBA", size, (0, 0, 0, 0))
    ImageDraw.Draw(lay).ellipse([cx - radius, cy - radius, cx + radius, cy + radius],
                                fill=color + (alpha,))
    return lay.filter(ImageFilter.GaussianBlur(radius * 0.55))

def bubble(draw, box, fill, radius, tail='left'):
    """Rounded speech bubble; tail at bottom-'left', bottom-'right', or None."""
    x0, y0, x1, y1 = box
    draw.rounded_rectangle(box, radius=radius, fill=fill)
    w = x1 - x0
    h = y1 - y0
    if tail == 'left':
        tx = x0 + w * 0.26
        draw.polygon([(tx, y1 - h * 0.10), (tx - w * 0.02, y1 + h * 0.34),
                      (tx + w * 0.24, y1 - h * 0.10)], fill=fill)
    elif tail == 'right':
        tx = x1 - w * 0.26
        draw.polygon([(tx, y1 - h * 0.10), (tx + w * 0.02, y1 + h * 0.34),
                      (tx - w * 0.24, y1 - h * 0.10)], fill=fill)

def dots(draw, cx, cy, r, gap, color):
    for dx in (-1, 0, 1):
        x = cx + dx * gap
        draw.ellipse([x - r, cy - r, x + r, cy + r], fill=color)

def save_indexed(img, path):
    """The Vita's scePromoterUtil rejects RGB/RGBA sce_sys PNGs (install error
    0x8010113d): they must be 8bpp INDEXED (mode 'P'). Quantize to a 256-color
    adaptive palette with Floyd-Steinberg dithering to keep the gradients smooth."""
    img.convert("RGB").quantize(colors=256).save(path, optimize=True)

def logo_tile(s):
    """RGBA rounded-square blurple app-icon tile (white speech bubble + dots),
    transparent outside the rounded corners so it can be pasted onto any bg."""
    tile = vgrad(s, s, BLURPLE_L, BLURPLE_D).convert("RGBA")
    d = ImageDraw.Draw(tile, "RGBA")
    bw, bh = s * 0.54, s * 0.42
    cx, cy = s * 0.5, s * 0.46
    box = [cx - bw / 2, cy - bh / 2, cx + bw / 2, cy + bh / 2]
    bubble(d, box, WHITE, radius=int(bh * 0.42))
    dots(d, cx, (box[1] + box[3]) / 2, s * 0.052, s * 0.155, BLURPLE)
    mask = Image.new("L", (s, s), 0)
    ImageDraw.Draw(mask).rounded_rectangle([0, 0, s - 1, s - 1], radius=int(s * 0.235), fill=255)
    tile.putalpha(mask)
    return tile

# --- icon0.png : 128x128 blurple tile + white chat bubble ------------------
def make_icon(path, S=128, ss=4):
    W = S * ss
    img = vgrad(W, W, BLURPLE_L, BLURPLE_D)   # blurple tile
    d = ImageDraw.Draw(img, "RGBA")
    # Centered white bubble, ~46% wide.
    cx, cy = W * 0.50, W * 0.46
    bw, bh = W * 0.46, W * 0.34
    box = [cx - bw / 2, cy - bh / 2, cx + bw / 2, cy + bh / 2]
    bubble(d, box, WHITE, radius=int(bh * 0.42))
    dots(d, cx, (box[1] + box[3]) / 2, W * 0.045, W * 0.135, BLURPLE)
    save_indexed(img.resize((S, S), Image.LANCZOS), path)

# --- startup.png : 280x158 launch tile ---
def make_startup(path, W=280, H=158, ss=4):
    # Branded splash: blurple hero tile with a centered white bubble mark.
    w, h = W * ss, H * ss
    # diagonal gradient, blurple to indigo
    img = dgrad(w, h, (126, 138, 252), (48, 54, 140)).convert("RGBA")
    # faint big bubble off the top-right
    tex = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    bubble(ImageDraw.Draw(tex), [w * 0.64, -h * 0.22, w * 1.2, h * 0.5],
           (255, 255, 255, 255), radius=int(h * 0.16), tail='left')
    tex.putalpha(tex.getchannel("A").point(lambda a: int(a * 0.07)))
    img.alpha_composite(tex)
    # soft spotlight behind the mark
    img.alpha_composite(glow((w, h), w * 0.5, h * 0.40, h * 0.58, (206, 214, 255), 62))
    d = ImageDraw.Draw(img, "RGBA")
    # centered hero mark
    bw, bh = w * 0.40, h * 0.46
    cx, cy = w * 0.5, h * 0.42
    box = [cx - bw / 2, cy - bh / 2, cx + bw / 2, cy + bh / 2]
    # drop shadow behind the bubble
    sh = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    bubble(ImageDraw.Draw(sh), [box[0], box[1] + h * 0.04, box[2], box[3] + h * 0.04],
           (18, 20, 54, 170), radius=int(bh * 0.42))
    img.alpha_composite(sh.filter(ImageFilter.GaussianBlur(h * 0.035)))
    bubble(d, box, WHITE, radius=int(bh * 0.42))
    dots(d, cx, (box[1] + box[3]) / 2, h * 0.044, h * 0.135, BLURPLE)
    save_indexed(img.resize((W, H), Image.LANCZOS), path)

# --- bg.png / pic0.png : atmospheric backdrop ---
def make_bg(path, W=960, H=544):
    # Backdrop with the identity lockup in the lower-left.
    img = vgrad(W, H, (32, 34, 40), (15, 16, 19)).convert("RGBA")
    img.alpha_composite(glow((W, H), W * 0.50, H * 0.32, H * 0.72, BLURPLE, 30))  # soft halo

    # faint oversized speech bubbles off the top-right and right edge
    lay = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    ld = ImageDraw.Draw(lay)
    bubble(ld, [W * 0.76, -H * 0.14, W * 1.20, H * 0.30], BLURPLE + (255,), radius=int(H * 0.14), tail='left')
    bubble(ld, [W * 0.88, H * 0.44, W * 1.28, H * 0.82], BLURPLE_L + (255,), radius=int(H * 0.14), tail='left')
    lay.putalpha(lay.getchannel("A").point(lambda a: int(a * 0.08)))
    img.alpha_composite(lay)

    # Lower-left identity lockup: app-icon tile + wordmark + subtitle.
    s = int(H * 0.15)
    ix, iy = int(W * 0.055), int(H * 0.70)
    sh = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    ImageDraw.Draw(sh).rounded_rectangle([ix, iy + 6, ix + s, iy + s + 10],
                                         radius=int(s * 0.235), fill=(0, 0, 0, 150))
    img.alpha_composite(sh.filter(ImageFilter.GaussianBlur(14)))
    img.alpha_composite(logo_tile(s), (ix, iy))
    d = ImageDraw.Draw(img, "RGBA")
    tx = ix + s + int(W * 0.022)
    d.text((tx, iy + s * 0.36), "Cordless", font=semi(int(H * 0.088)), fill=WHITE, anchor="lm")
    d.text((tx, iy + s * 0.76), "Discord for PS Vita", font=reg(int(H * 0.045)),
           fill=MUTED, anchor="lm")
    save_indexed(img, path)

def main():
    os.makedirs(OUT_LA, exist_ok=True)
    make_icon(os.path.join(OUT_SCE, "icon0.png"))
    make_bg(os.path.join(OUT_SCE, "pic0.png"), 960, 544)   # pic0 full screen
    make_bg(os.path.join(OUT_LA, "bg.png"), 840, 500)      # psmobile LiveArea bg size
    make_startup(os.path.join(OUT_LA, "startup.png"))
    print("LiveArea art written to sce_sys/")

if __name__ == "__main__":
    main()
