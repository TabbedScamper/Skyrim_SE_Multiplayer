"""Builds GameFiles/Skyrim/Interface/SkyrimSEMultiplayer/lobby_vignette.dds: the live lobby's vignette, loaded into the
Main Menu movie under its text and stretched to the screen. A rounded rectangle, not an oval (owner 2026-10-01: "i
hate that its almost a perfect oval"): clear until close to the edges, then a quick smooth fall to solid black at
them. A little noise keeps the 8-bit alpha from banding. Usage: make-vignette.py"""
import os
import random
from PIL import Image

W, H = 512, 288
POWER = 6.0     # superellipse exponent: 2 is an oval, higher hugs the rectangle
INNER = 0.84    # fully clear inside this distance; 1.0 is the screen edge
here = os.path.dirname(os.path.abspath(__file__))
target = os.path.join(here, "..", "..", "GameFiles", "Skyrim", "Interface", "SkyrimSEMultiplayer", "lobby_vignette.dds")
random.seed(1)
image = Image.new("RGBA", (W, H))
pixels = image.load()
for y in range(H):
    for x in range(W):
        dx = abs((x + 0.5) / W - 0.5) * 2
        dy = abs((y + 0.5) / H - 0.5) * 2
        d = (dx ** POWER + dy ** POWER) ** (1 / POWER)
        t = min(1.0, max(0.0, (d - INNER) / (1.0 - INNER)))
        a = t * t * (3 - 2 * t)  # smoothstep
        alpha = min(255, max(0, round(a * 255 + random.uniform(-0.75, 0.75))))
        pixels[x, y] = (0, 0, 0, alpha)
image.save(os.path.normpath(target), format="DDS")
print("wrote", os.path.normpath(target), os.path.getsize(target), "bytes")
