"""Regenerate the vector mark and Windows icon sizes. Requires Pillow for exports."""
from pathlib import Path
import math
from PIL import Image, ImageDraw, ImageFont

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "resources"
ANGLE = math.radians(32)

def point(t, back=False):
    x, y = 65 * math.cos(t), 101 * math.sin(t)
    return (120 + x * math.cos(ANGLE) - y * math.sin(ANGLE) + (15 if back else 0),
            122 + x * math.sin(ANGLE) + y * math.cos(ANGLE) + (8 if back else 0))

def hull(points):
    points = sorted(set(points))
    def cross(o, a, b):
        return (a[0]-o[0])*(b[1]-o[1])-(a[1]-o[1])*(b[0]-o[0])
    lower, upper = [], []
    for p in points:
        while len(lower) >= 2 and cross(lower[-2], lower[-1], p) <= 0: lower.pop()
        lower.append(p)
    for p in reversed(points):
        while len(upper) >= 2 and cross(upper[-2], upper[-1], p) <= 0: upper.pop()
        upper.append(p)
    return lower[:-1] + upper[:-1]

front = [point(i*math.tau/256) for i in range(256)]
body = hull(front + [point(i*math.tau/256, True) for i in range(256)])
body_path = "M" + " L".join(f"{x:.3f},{y:.3f}" for x,y in body) + " Z"
svg = f'''<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 256 256">
  <title>HiveDesk — blue perspective disc</title>
  <defs><linearGradient id="face" x1="0" y1="0" x2="0.3" y2="1">
    <stop stop-color="#39bcff"/><stop offset=".53" stop-color="#238ef5"/><stop offset="1" stop-color="#245de0"/>
  </linearGradient></defs>
  <path d="{body_path}" fill="#1649ad"/>
  <ellipse cx="120" cy="122" rx="65" ry="101" transform="rotate(32 120 122)" fill="url(#face)"/>
</svg>
'''
(OUT / "HiveDesk.svg").write_text(svg, encoding="utf-8")

def render(size):
    scale = size * 4 / 256
    n = size * 4
    image = Image.new("RGBA", (n,n))
    ImageDraw.Draw(image).polygon([(x*scale,y*scale) for x,y in body], fill="#1649ad")
    mask = Image.new("L", (n,n))
    ImageDraw.Draw(mask).polygon([(x*scale,y*scale) for x,y in front], fill=255)
    gradient = Image.new("RGBA", (n,n))
    draw = ImageDraw.Draw(gradient)
    stops = [(0,(57,188,255)),(.53,(35,142,245)),(1,(36,93,224))]
    for y in range(n):
        t = max(0,min(1,(y/scale-30)/184))
        lo, hi = (stops[0],stops[1]) if t <= .53 else (stops[1],stops[2])
        f=(t-lo[0])/(hi[0]-lo[0])
        color=tuple(round(a+(b-a)*f) for a,b in zip(lo[1],hi[1]))+(255,)
        draw.line((0,y,n,y), fill=color)
    image.paste(gradient,(0,0),mask)
    return image.resize((size,size),Image.Resampling.LANCZOS)

sizes = [16,20,24,32,40,48,64,96,128,256]
images = [render(n) for n in sizes]
images[-1].save(OUT / "HiveDesk.ico", format="ICO", sizes=[(n,n) for n in sizes], append_images=images[:-1])
render(256).save(OUT / "HiveDesk.png")

# A review sheet is not part of the installed application.
sheet = Image.new("RGB", (900,400), "#f4f6fa")
draw = ImageDraw.Draw(sheet);draw.rectangle((450,0,900,400), fill="#161b24")
font = ImageFont.truetype("C:/Windows/Fonts/segoeui.ttf", 17)
for base, color in [(0,"#354052"),(450,"#c7d2e4")]:
    sheet.paste(render(220),(base+115,20),render(220))
    draw.text((base+24,258),"HiveDesk",font=font,fill=color)
    x=base+24
    for size in [16,20,24,32,48,64]:
        icon=render(size);sheet.paste(icon,(x,307-size//2),icon)
        draw.text((x,351),str(size),font=font,fill=color);x+=size+20
(ROOT / ".local").mkdir(exist_ok=True)
sheet.save(ROOT / ".local" / "alpha13-icon-review.png")
print("Generated SVG, PNG and 10-size ICO (16–256 px).")
