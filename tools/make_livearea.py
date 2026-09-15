#!/usr/bin/env python3
"""Generate the LiveArea assets packaged into the VPK.

Colours mirror the palette used by the prototype renderer in src/main.cpp so the
LiveArea entry matches what the game actually looks like on screen.
"""

from __future__ import annotations

import argparse
import pathlib

from PIL import Image, ImageDraw

BACKDROP = (20, 48, 30, 255)
GRID = (35, 69, 45, 255)
PIKMIN_RED = (222, 48, 48, 255)
LEAF_GREEN = (100, 220, 100, 255)
OLIMAR_BLUE = (65, 125, 225, 255)
PART_GOLD = (242, 190, 45, 255)

TEMPLATE_XML = """<?xml version="1.0" encoding="utf-8"?>
<livearea style="a1" format-ver="01.00" content-rev="1">
  <livearea-background>
    <image>bg.png</image>
  </livearea-background>
  <gate>
    <startup-image>startup.png</startup-image>
  </gate>
</livearea>
"""


def draw_backdrop(image: Image.Image, spacing: int) -> ImageDraw.ImageDraw:
    draw = ImageDraw.Draw(image)
    for x in range(0, image.width, spacing):
        draw.line((x, 0, x, image.height), fill=GRID)
    for y in range(0, image.height, spacing):
        draw.line((0, y, image.width, y), fill=GRID)
    return draw


def draw_pikmin(draw: ImageDraw.ImageDraw, x: float, y: float, radius: float) -> None:
    stem_top = y - radius * 2.6
    draw.line((x, y - radius, x, stem_top), fill=LEAF_GREEN, width=max(1, int(radius / 4)))
    leaf = radius * 1.15
    draw.ellipse((x - leaf, stem_top - leaf * 0.7, x, stem_top + leaf * 0.7), fill=LEAF_GREEN)
    draw.ellipse((x, stem_top - leaf * 0.7, x + leaf, stem_top + leaf * 0.7), fill=LEAF_GREEN)
    draw.ellipse((x - radius, y - radius, x + radius, y + radius), fill=PIKMIN_RED)
    eye = radius * 0.22
    for offset in (-radius * 0.34, radius * 0.34):
        draw.ellipse(
            (x + offset - eye, y - radius * 0.2 - eye, x + offset + eye, y - radius * 0.2 + eye),
            fill=(255, 255, 255, 255),
        )


def make_icon(path: pathlib.Path) -> None:
    image = Image.new("RGBA", (128, 128), BACKDROP)
    draw = draw_backdrop(image, 16)
    draw_pikmin(draw, 64, 84, 24)
    draw.ellipse((14, 100, 30, 116), fill=PART_GOLD)
    draw.ellipse((98, 100, 114, 116), fill=OLIMAR_BLUE)
    image.save(path)


def make_startup(path: pathlib.Path) -> None:
    image = Image.new("RGBA", (280, 158), BACKDROP)
    draw = draw_backdrop(image, 20)
    draw_pikmin(draw, 140, 112, 28)
    draw.ellipse((40, 126, 62, 148), fill=PART_GOLD)
    draw.ellipse((218, 126, 240, 148), fill=OLIMAR_BLUE)
    image.save(path)


def make_background(path: pathlib.Path) -> None:
    image = Image.new("RGBA", (840, 500), BACKDROP)
    draw = draw_backdrop(image, 48)
    for index in range(6):
        draw_pikmin(draw, 140 + index * 112, 400 - (index % 3) * 26, 18)
    draw.ellipse((88, 214, 128, 254), fill=OLIMAR_BLUE)
    for x, y in ((300, 150), (560, 190), (700, 120)):
        draw.ellipse((x - 20, y - 20, x + 20, y + 20), fill=PART_GOLD)
    image.save(path)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--sce-sys",
        type=pathlib.Path,
        default=pathlib.Path(__file__).resolve().parents[1] / "sce_sys",
        help="destination sce_sys directory",
    )
    args = parser.parse_args()

    contents = args.sce_sys / "livearea" / "contents"
    contents.mkdir(parents=True, exist_ok=True)

    make_icon(args.sce_sys / "icon0.png")
    make_startup(contents / "startup.png")
    make_background(contents / "bg.png")
    (contents / "template.xml").write_text(TEMPLATE_XML, encoding="utf-8")


if __name__ == "__main__":
    main()
