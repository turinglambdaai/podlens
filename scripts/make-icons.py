#!/usr/bin/env python
"""Regenerate PodLens branding assets from a drawn master image.

Usage: python scripts/make-icons.py

Draws the 1024x1024 master (site accent palette, podcast-wave mark) into
assets/branding/icon-source.png, then emits the platform artifacts Rivet
consumes: assets/branding/app.ico (Windows, compiled into the host exe and
referenced by MSI shortcuts), assets/branding/app.icns (macOS bundle), and
assets/branding/app.png (Linux desktop/deb/rpm/AppImage icon).

To restyle the icon, edit draw_master() and rerun; do not hand-edit the
generated .ico/.icns/.png.
"""

from __future__ import annotations

import math
import struct
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont, ImageOps

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


def draw_wave_mark(edge: int) -> Image.Image:
    """The icon's dot-plus-arcs mark alone, RGBA, at `edge` pixels.

    Geometry is authored on the 1024 master grid, so it scales by
    edge / MASTER rather than reusing the master's absolute pixel radii."""
    size = edge * SS
    unit = edge / MASTER * SS
    image = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    draw = ImageDraw.Draw(image)
    center = size / 2
    dot_radius = 110 * unit
    draw.ellipse(
        (center - dot_radius, center - dot_radius,
         center + dot_radius, center + dot_radius),
        fill=WHITE,
    )
    stroke = round(78 * unit)
    radius = 250 * unit
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
    return image.resize((edge, edge), Image.LANCZOS)


def load_wordmark_font(size: int):
    for name in ("arialbd.ttf", "DejaVuSans-Bold.ttf"):
        try:
            return ImageFont.truetype(name, size)
        except OSError:
            continue
    return ImageFont.load_default()


def write_wizard_bitmap(path: Path, width: int, height: int, *,
                        mark_edge: int, mark_xy: tuple[int, int],
                        wordmark_size: int, wordmark_xy: tuple[int, int]) -> None:
    """One WiX wizard bitmap at the stock-UI size: brand gradient backdrop,
    the white wave mark and the wordmark, saved as 24-bit BMP so the WixVariable
    extension consumes it as-is (banner 493x58, dialog 493x312)."""
    image = ImageOps.colorize(
        Image.linear_gradient("L").resize((width, height)),
        black=ACCENT_DEEP, white=ACCENT).convert("RGB")
    mark = draw_wave_mark(mark_edge)
    image.paste(mark, mark_xy, mark)
    ImageDraw.Draw(image).text(
        wordmark_xy, "PodLens", font=load_wordmark_font(wordmark_size),
        fill=WHITE, anchor="mm")
    image.save(path, format="BMP")


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

    # Linux: one 512x512 PNG, consumed via rivet.rktd's linux-icon.
    master.resize((512, 512), Image.LANCZOS).save(BRANDING / "app.png")
    # Windows installer wizard bitmaps (rivet.rktd windows-installer-bitmaps).
    write_wizard_bitmap(BRANDING / "wizard-banner.bmp", 493, 58,
                        mark_edge=44, mark_xy=(16, 7),
                        wordmark_size=26, wordmark_xy=(110, 29))
    write_wizard_bitmap(BRANDING / "wizard-dialog.bmp", 493, 312,
                        mark_edge=190, mark_xy=(152, 30),
                        wordmark_size=58, wordmark_xy=(246, 262))

    for name in ("icon-source.png", "app.ico", "app.icns", "app.png",
                 "wizard-banner.bmp", "wizard-dialog.bmp"):
        print(f"{(BRANDING / name).name}: {(BRANDING / name).stat().st_size} bytes")


if __name__ == "__main__":
    main()
