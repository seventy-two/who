"""Draws Who's icon: a green speech bubble with a question mark.
Writes src/app.ico (16-256 px). Run: python3 tools/icon.py"""
import sys
from PIL import Image, ImageDraw, ImageFont

S = 1024
img = Image.new("RGBA", (S, S), (0, 0, 0, 0))
d = ImageDraw.Draw(img)
green, dark = (63, 185, 80, 255), (21, 23, 28, 255)
# Bubble: a round body and a tail at the bottom left.
d.ellipse((90, 90, 934, 860), fill=green)
d.polygon([(190, 700), (110, 960), (420, 820)], fill=green)
# The question mark, in the window's dark grey.
font = None
for name in ("DejaVuSans-Bold.ttf", "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf"):
    try:
        font = ImageFont.truetype(name, 640)
        break
    except OSError:
        pass
w, h = font.getsize("?")
ox, oy = font.getoffset("?")
d.text(((S - w - ox) / 2, (950 - h - oy) / 2 - 10), "?", font=font, fill=dark)
out = sys.argv[1] if len(sys.argv) > 1 else "src/app.ico"
img.resize((256, 256), Image.LANCZOS).save(out, sizes=[(16, 16), (20, 20), (24, 24), (32, 32), (40, 40), (48, 48), (64, 64), (128, 128), (256, 256)])
img.resize((256, 256), Image.LANCZOS).save(out.replace(".ico", ".png"))
