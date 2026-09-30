#!/usr/bin/env python
"""Regenerate PodLens branding assets from a drawn master image.

Usage: python scripts/make-icons.py

Draws the 1024x1024 master (site accent palette, podcast-wave mark) into
assets/branding/icon-source.png, then emits the platform artifacts Rivet
consumes: assets/branding/app.ico (Windows, compiled into the host exe and
referenced by MSI shortcuts) and assets/branding/app.icns (macOS bundle).

To restyle the icon, edit draw_master() and rerun; do not hand-edit the
generated .ico/.icns.
"""

from __future__ import annotations

import math
import struct
from pathlib import Path

from PIL import Image, ImageDraw, ImageOps

BRANDING = Path(__file__).resolve().parent.parent / "assets" / "branding"

ACCENT = (193, 95, 60)        # site accent #c15f3c
ACCENT_DEEP = (168, 78, 47)   # gradient floor
WHITE = (255, 255, 255)

MASTER = 1024
SS = 4  # supersample factor for smooth curves


def draw_master() -> Image.Image:
    size = MASTER * SS
    # Vertical gradient backdrop, rounded like a modern app tile.
    gradient = Image.linear_gradient("L").resize((size, size))
    backdrop = ImageOps.colorize(gradient, black=ACCENT_DEEP, white=ACCENT)

    image = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    mask = Image.new("L", (size, size), 0)
    ImageDraw.Draw(mask).rounded_rectangle(
        (0, 0, size - 1, size - 1), radius=892, fill=255
    )
    image.paste(backdrop, (0, 0), mask)
    draw = ImageDraw.Draw(image)

    center = size / 2
    # Podcast-wave mark: a solid dot flanked by two wave arcs.
    dot_radius = 110 * SS
    draw.ellipse(
        (center - dot_radius, center - dot_radius,
         center + dot_radius, center + dot_radius),
        fill=WHITE,
    )
    stroke = 78 * SS
    radius = 250 * SS
    centerline = radius - stroke / 2  # arc width grows inwards
    for start, end in ((128, 232), (308, 412)):  # degrees, mirrored arcs
        bbox = (center - radius, center - radius, center + radius, center + radius)
        draw.arc(bbox, start=start, end=end, fill=WHITE, width=stroke)
        for angle in (start, end):
            rad = math.radians(angle)
            point = (center + centerline * math.cos(rad),
                     center + centerline * math.sin(rad))
            draw.ellipse(
                (point[0] - stroke / 2, point[1] - stroke / 2,
                 point[0] + stroke / 2, point[1] + stroke / 2),
                fill=WHITE,
            )
    return image.resize((MASTER, MASTER), Image.LANCZOS)


def write_icns(master: Image.Image, path: Path) -> None:
    entries = (
        (b"ic07", 128),
        (b"ic08", 256),
        (b"ic09", 512),
        (b"ic10", 1024),
    )
    payload = b""
    for icon_type, edge in entries:
        buffer = path.with_name(f".icns-tmp-{edge}.png")
        master.resize((edge, edge), Image.LANCZOS).save(buffer, format="PNG")
        data = buffer.read_bytes()
        buffer.unlink()
        payload += icon_type + struct.pack(">I", len(data) + 8) + data
    icns = b"icns" + struct.pack(">I", len(payload) + 8) + payload
    path.write_bytes(icns)


def main() -> None:
    BRANDING.mkdir(parents=True, exist_ok=True)
    master = draw_master()
    master.save(BRANDING / "icon-source.png")

    master.save(
        BRANDING / "app.ico",
        format="ICO",
        sizes=[(16, 16), (24, 24), (32, 32), (48, 48),
               (64, 64), (128, 128), (256, 256)],
    )

    write_icns(master, BRANDING / "app.icns")
    for name in ("icon-source.png", "app.ico", "app.icns"):
        print(f"{(BRANDING / name).name}: {(BRANDING / name).stat().st_size} bytes")


if __name__ == "__main__":
    main()
