#!/usr/bin/env python3
"""Generate indexed Doom assets for the Vaporware example.

This script converts the local enemy sheet and death-screen reference image into
4bpp indexed C assets that fit the Doom mini-game's display path.
"""

from __future__ import annotations

from collections import deque
from pathlib import Path

import numpy as np
from PIL import Image, ImageEnhance, ImageFilter


ROOT = Path(__file__).resolve().parents[1]
SRC_DIR = ROOT / "src"
ENEMY_SHEET_PATH = SRC_DIR / "enemys sheet.png"
DEATH_REF_PATH = SRC_DIR / "Deathscreen.jpg"

ENEMY_SPECS = [
    {"name": "zombieman", "box": (81, 312, 154, 409), "target_h": 52},
    {"name": "shotgun_guy", "box": (215, 314, 294, 409), "target_h": 54},
    {"name": "imp", "box": (360, 306, 435, 409), "target_h": 58},
    {"name": "lost_soul", "box": (628, 326, 685, 383), "target_h": 34},
    {"name": "pinky", "box": (490, 328, 569, 409), "target_h": 44},
    {"name": "cacodemon", "box": (656, 554, 769, 657), "target_h": 48},
    {"name": "mancubus", "box": (476, 542, 611, 665), "target_h": 54},
    {"name": "baron", "box": (411, 1018, 574, 1177), "target_h": 74},
]

ENEMY_HEADER_PATH = SRC_DIR / "doom_enemy_sprites.h"
ENEMY_SOURCE_PATH = SRC_DIR / "doom_enemy_sprites.c"
DEATH_HEADER_PATH = SRC_DIR / "doom_deathscreen.h"
DEATH_SOURCE_PATH = SRC_DIR / "doom_deathscreen.c"

SHEET_BG_RGB = np.array([91, 91, 91], dtype=np.int16)

DEATHSCREEN_DRAW_IMPL = """
static void ds_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t c) {
    if (x >= LCD_WIDTH || y >= LCD_HEIGHT || w == 0U || h == 0U) return;
    if (x + w > LCD_WIDTH) w = (uint16_t)(LCD_WIDTH - x);
    if (y + h > LCD_HEIGHT) h = (uint16_t)(LCD_HEIGHT - y);
    display_fill_rect(x, y, w, h, c);
}

static void ds_draw_digit(uint8_t d, uint16_t x, uint16_t y, uint16_t c) {
    static const uint8_t bits[10][5] = {
        {7, 5, 5, 5, 7}, {2, 6, 2, 2, 7}, {7, 1, 7, 4, 7}, {7, 1, 7, 1, 7}, {5, 5, 7, 1, 1},
        {7, 4, 7, 1, 7}, {7, 4, 7, 5, 7}, {7, 1, 1, 2, 2}, {7, 5, 7, 5, 7}, {7, 5, 7, 1, 7}
    };
    for (uint8_t row = 0; row < 5U; row++) {
        for (uint8_t col = 0; col < 3U; col++) {
            if (bits[d][row] & (1U << (2U - col))) {
                ds_rect((uint16_t)(x + col * 2U), (uint16_t)(y + row * 2U), 2, 2, c);
            }
        }
    }
}

static void ds_draw_num(uint16_t n, uint16_t x, uint16_t y, uint16_t c) {
    if (n >= 10U) {
        ds_draw_digit((uint8_t)((n / 10U) % 10U), x, y, c);
        x = (uint16_t)(x + 8U);
    }
    ds_draw_digit((uint8_t)(n % 10U), x, y, c);
}

static void ds_draw_block_letter(uint8_t id, uint16_t x, uint16_t y, uint16_t s, uint16_t c) {
    uint8_t top = 0U, mid = 0U, bot = 0U, ul = 0U, ur = 0U, ll = 0U, lr = 0U;
    switch (id) {
        case 'A': top = mid = ul = ll = ur = lr = 1U; break;
        case 'D': top = bot = ul = ll = ur = lr = 1U; break;
        case 'E': top = mid = bot = ul = ll = 1U; break;
        case 'P': top = mid = ul = ur = ll = 1U; break;
        case 'R': top = mid = ul = ur = ll = lr = 1U; break;
        case 'S': top = mid = bot = ul = lr = 1U; break;
        default: break;
    }
    if (top) ds_rect(x, y, (uint16_t)(4U * s), s, c);
    if (mid) ds_rect(x, (uint16_t)(y + 2U * s), (uint16_t)(4U * s), s, c);
    if (bot) ds_rect(x, (uint16_t)(y + 4U * s), (uint16_t)(4U * s), s, c);
    if (ul) ds_rect(x, y, s, (uint16_t)(3U * s), c);
    if (ur) ds_rect((uint16_t)(x + 3U * s), y, s, (uint16_t)(3U * s), c);
    if (ll) ds_rect(x, (uint16_t)(y + 2U * s), s, (uint16_t)(3U * s), c);
    if (lr) ds_rect((uint16_t)(x + 3U * s), (uint16_t)(y + 2U * s), s, (uint16_t)(3U * s), c);
}

static uint16_t ds_shade(uint16_t color, uint8_t numer, uint8_t denom) {
    uint16_t r = color & 0x1FU;
    uint16_t g = (color >> 5) & 0x3FU;
    uint16_t b = (color >> 11) & 0x1FU;
    r = (uint16_t)((r * numer) / denom);
    g = (uint16_t)((g * numer) / denom);
    b = (uint16_t)((b * numer) / denom);
    return (uint16_t)((b << 11) | (g << 5) | r);
}

static uint16_t ds_sample(uint16_t x, uint16_t y) {
    const uint8_t packed = doom_deathscreen_pixels_4bpp[y * (DOOM_DEATHSCREEN_W / 2U) + (x >> 1)];
    const uint8_t idx = (uint8_t)((x & 1U) ? (packed & 0x0FU) : (packed >> 4));
    return doom_deathscreen_palette[idx];
}

static void ds_draw_panel(uint16_t x, uint16_t y, uint16_t w, uint16_t h) {
    uint16_t row[96];

    for (uint16_t dy = 0; dy < h; dy++) {
        const uint16_t sy = (uint16_t)(8U + (((uint32_t)dy * 100U) / h));
        for (uint16_t dx = 0; dx < w; dx++) {
            const uint16_t sx = (uint16_t)(10U + (((uint32_t)dx * 108U) / w));
            uint16_t color = ds_sample(sx, sy);
            if (dy & 1U) color = ds_shade(color, 2U, 3U);
            if (((dx + dy) & 7U) == 0U) color = ds_shade(color, 4U, 5U);
            row[dx] = color;
        }
        display_draw_image(row, x, (uint16_t)(y + dy), w, 1);
    }
}

void doom_deathscreen_draw(void) {
    static const uint16_t c_bg = COL_RGB(11, 9, 12);
    static const uint16_t c_panel = COL_RGB(56, 32, 30);
    static const uint16_t c_panel2 = COL_RGB(74, 78, 82);
    static const uint16_t c_blood = COL_RGB(180, 0, 0);
    static const uint16_t c_blood2 = COL_RGB(118, 18, 14);
    static const uint16_t c_fire = COL_RGB(255, 210, 50);
    static const uint16_t c_fire2 = COL_RGB(210, 165, 22);
    static const uint16_t c_floor = COL_RGB(35, 30, 26);
    static const uint16_t c_floor2 = COL_RGB(54, 43, 33);
    static const uint16_t c_skin = COL_RGB(210, 160, 110);
    static const uint16_t c_dark = COL_RGB(12, 12, 12);

    display_fill(c_bg);

    for (uint16_t y = 0; y < 56U; y += 8U) {
        ds_rect(0, y, 128, 8, (y & 16U) ? COL_RGB(18, 15, 20) : COL_RGB(14, 12, 18));
    }
    for (uint16_t y = 56U; y < 132U; y += 12U) {
        ds_rect(0, y, 128, 12, (y & 16U) ? c_floor : c_floor2);
        ds_rect(10, (uint16_t)(y + 4U), 108, 1, COL_RGB(25, 22, 20));
    }

    ds_rect(0, 0, 128, 6, c_blood2);
    ds_rect(0, 6, 128, 2, c_fire2);
    ds_rect(0, 0, 8, 132, c_blood2);
    ds_rect(120, 0, 8, 132, c_blood2);
    ds_rect(8, 20, 18, 86, c_panel);
    ds_rect(102, 20, 18, 86, c_panel);
    ds_rect(24, 26, 6, 72, c_panel2);
    ds_rect(98, 26, 6, 72, c_panel2);
    ds_rect(30, 34, 10, 54, c_blood2);
    ds_rect(88, 34, 10, 54, c_blood2);
    ds_rect(40, 18, 48, 8, c_fire2);
    ds_rect(44, 21, 40, 2, c_dark);

    ds_rect(14, 30, 100, 74, c_panel2);
    ds_rect(16, 32, 96, 70, c_dark);
    ds_rect(18, 34, 92, 66, c_blood2);
    ds_rect(20, 36, 88, 62, c_dark);
    ds_draw_panel(22, 38, 84, 58);
    ds_rect(22, 38, 84, 1, c_fire);
    ds_rect(22, 95, 84, 1, c_blood);
    ds_rect(26, 42, 6, 50, COL_RGB(34, 12, 10));
    ds_rect(96, 42, 6, 50, COL_RGB(34, 12, 10));
    ds_rect(54, 58, 20, 16, COL_BLACK);
    ds_rect(57, 60, 14, 12, COL_RGB(4, 2, 2));

    ds_draw_block_letter('D', 18, 10, 2, c_dark);
    ds_draw_block_letter('E', 37, 10, 2, c_dark);
    ds_draw_block_letter('A', 56, 10, 2, c_dark);
    ds_draw_block_letter('D', 75, 10, 2, c_dark);
    ds_draw_block_letter('D', 16, 8, 2, c_blood);
    ds_draw_block_letter('E', 35, 8, 2, c_fire);
    ds_draw_block_letter('A', 54, 8, 2, c_fire);
    ds_draw_block_letter('D', 73, 8, 2, c_blood);

    ds_draw_block_letter('P', 26, 112, 1, c_fire);
    ds_draw_block_letter('R', 35, 112, 1, c_fire);
    ds_draw_block_letter('E', 44, 112, 1, c_fire);
    ds_draw_block_letter('S', 53, 112, 1, c_fire);
    ds_draw_block_letter('S', 62, 112, 1, c_fire);
    ds_rect(72, 114, 22, 2, c_fire2);
    ds_rect(72, 117, 18, 2, c_blood);

    ds_rect(0, 132, 128, 28, COL_RGB(28, 28, 30));
    ds_rect(0, 132, 128, 2, COL_RGB(78, 68, 52));
    ds_rect(0, 157, 128, 3, c_dark);
    for (uint8_t i = 0U; i < 8U; i++) {
        ds_rect((uint16_t)(i * 16U), 132, 8, 1, (i & 1U) ? c_fire2 : c_panel2);
        ds_rect((uint16_t)(i * 16U + 10U), 153, 4, 2, COL_RGB(48, 48, 48));
    }

    ds_rect(4, 138, 34, 16, COL_RGB(18, 22, 18));
    ds_rect(47, 138, 34, 16, COL_RGB(92, 62, 44));
    ds_rect(90, 138, 34, 16, COL_RGB(24, 20, 18));
    ds_draw_num(0, 10, 142, c_blood);
    ds_draw_num(0, 20, 142, c_blood);
    ds_draw_num(0, 98, 142, c_fire);
    ds_draw_num(0, 108, 142, c_fire);

    ds_rect(52, 140, 24, 12, c_skin);
    ds_rect(55, 143, 4, 3, c_dark);
    ds_rect(68, 143, 4, 3, c_dark);
    ds_rect(59, 149, 10, 2, c_blood);
    ds_rect(54, 138, 20, 2, COL_RGB(235, 185, 128));
    ds_rect(58, 145, 11, 2, COL_RGB(76, 22, 20));
}
"""


def rgb_to_bgr565(rgb: tuple[int, int, int]) -> int:
    r, g, b = rgb
    return ((b >> 3) << 11) | ((g >> 2) << 5) | (r >> 3)


def c_array(values: list[int], width: int, hex_width: int) -> str:
    lines: list[str] = []
    for start in range(0, len(values), width):
        chunk = values[start : start + width]
        lines.append("    " + ", ".join(f"0x{value:0{hex_width}X}" for value in chunk))
    return ",\n".join(lines)


def palette_for_pixels(pixels: np.ndarray, max_colors: int) -> tuple[list[int], np.ndarray]:
    opaque = pixels.reshape(-1, 4)
    alpha = opaque[:, 3] > 0
    rgb_opaque = opaque[alpha, :3]
    palette_size = min(max_colors, max(1, len(np.unique(rgb_opaque, axis=0))))

    opaque_img = Image.fromarray(rgb_opaque.reshape(1, len(rgb_opaque), 3), "RGB")
    quantized = opaque_img.quantize(colors=palette_size, method=Image.Quantize.MEDIANCUT, dither=Image.Dither.NONE)

    quant_indices = np.array(quantized, dtype=np.uint8).reshape(-1)
    raw_palette = quantized.getpalette()[: palette_size * 3]

    palette = [0]
    for index in range(palette_size):
        rgb = tuple(raw_palette[index * 3 : index * 3 + 3])
        palette.append(rgb_to_bgr565(rgb))
    while len(palette) < 16:
        palette.append(0)

    indexed = np.zeros(len(opaque), dtype=np.uint8)
    indexed[alpha] = quant_indices + 1
    return palette, indexed.reshape(pixels.shape[0], pixels.shape[1])


def pack_4bpp(indexed: np.ndarray) -> list[int]:
    packed: list[int] = []
    height, width = indexed.shape
    for y in range(height):
        row = indexed[y]
        for x in range(0, width, 2):
            hi = int(row[x] & 0x0F)
            lo = int(row[x + 1] & 0x0F) if x + 1 < width else 0
            packed.append((hi << 4) | lo)
    return packed


def extract_enemy_sprite(sheet: Image.Image, box: tuple[int, int, int, int], target_h: int) -> Image.Image:
    x0, y0, x1, y1 = box
    crop = sheet.crop((max(0, x0 - 4), max(0, y0 - 4), min(sheet.width, x1 + 5), min(sheet.height, y1 + 5))).convert("RGBA")
    pixels = np.array(crop)

    bg_like = (
        np.abs(pixels[:, :, :3].astype(np.int16) - SHEET_BG_RGB).sum(axis=2) < 28
    ) & ((pixels[:, :, :3].max(axis=2) - pixels[:, :, :3].min(axis=2)) < 18)

    outside = np.zeros(bg_like.shape, dtype=bool)
    queue: deque[tuple[int, int]] = deque()
    height, width = bg_like.shape

    for x in range(width):
        for y in (0, height - 1):
            if bg_like[y, x] and not outside[y, x]:
                outside[y, x] = True
                queue.append((y, x))
    for y in range(height):
        for x in (0, width - 1):
            if bg_like[y, x] and not outside[y, x]:
                outside[y, x] = True
                queue.append((y, x))

    while queue:
        cy, cx = queue.popleft()
        for ny, nx in ((cy - 1, cx), (cy + 1, cx), (cy, cx - 1), (cy, cx + 1)):
            if 0 <= ny < height and 0 <= nx < width and bg_like[ny, nx] and not outside[ny, nx]:
                outside[ny, nx] = True
                queue.append((ny, nx))

    pixels[outside, 3] = 0
    rgba = Image.fromarray(pixels, "RGBA")
    alpha_bbox = rgba.getchannel("A").getbbox()
    if alpha_bbox is None:
        raise RuntimeError(f"Sprite crop {box} became fully transparent")
    rgba = rgba.crop(alpha_bbox)

    target_w = max(1, round(rgba.width * target_h / rgba.height))
    rgba = rgba.resize((target_w, target_h), Image.Resampling.BOX)
    return rgba


def make_deathscreen(image: Image.Image) -> Image.Image:
    screen = image.crop((55, 55, 2438, 1382))
    crop_w = 1060
    crop_h = int(round(crop_w * 160 / 128))
    center_x = 1190
    center_y = 640
    box = (
        center_x - crop_w // 2,
        center_y - crop_h // 2,
        center_x + crop_w // 2,
        center_y + crop_h // 2,
    )
    cropped = screen.crop(box).convert("RGB")
    styled = cropped.resize((64, 80), Image.Resampling.BILINEAR).resize((128, 160), Image.Resampling.NEAREST)
    styled = ImageEnhance.Color(styled).enhance(1.10)
    styled = ImageEnhance.Contrast(styled).enhance(1.08)
    styled = styled.filter(ImageFilter.SHARPEN)
    return styled


def write_enemy_assets(sheet: Image.Image) -> None:
    sprites = []
    for spec in ENEMY_SPECS:
        rgba = extract_enemy_sprite(sheet, spec["box"], spec["target_h"])
        pixels = np.array(rgba)
        palette, indexed = palette_for_pixels(pixels, max_colors=15)
        sprites.append(
            {
                "name": spec["name"],
                "width": rgba.width,
                "height": rgba.height,
                "palette": palette,
                "pixels": pack_4bpp(indexed),
            }
        )

    header = """#ifndef DOOM_ENEMY_SPRITES_H
#define DOOM_ENEMY_SPRITES_H

#include <stdint.h>

typedef struct {
    uint8_t width;
    uint8_t height;
    const uint16_t *palette;
    const uint8_t *pixels_4bpp;
} doom_enemy_sprite_t;

#define DOOM_ENEMY_SPRITE_COUNT 8

extern const doom_enemy_sprite_t doom_enemy_sprites[DOOM_ENEMY_SPRITE_COUNT];

#endif /* DOOM_ENEMY_SPRITES_H */
"""

    blocks = [
        '#include "doom_enemy_sprites.h"',
        "",
    ]
    for sprite in sprites:
        upper = sprite["name"].upper()
        blocks.append(
            f"static const uint16_t {sprite['name']}_palette[16] = {{\n{c_array(sprite['palette'], 8, 4)}\n}};"
        )
        blocks.append(
            f"static const uint8_t {sprite['name']}_pixels_4bpp[{len(sprite['pixels'])}] = {{\n"
            f"{c_array(sprite['pixels'], 16, 2)}\n}};"
        )
        blocks.append(f"#define {upper}_W {sprite['width']}U")
        blocks.append(f"#define {upper}_H {sprite['height']}U")
        blocks.append("")

    blocks.append("const doom_enemy_sprite_t doom_enemy_sprites[DOOM_ENEMY_SPRITE_COUNT] = {")
    for sprite in sprites:
        blocks.append(
            f"    {{{sprite['width']}U, {sprite['height']}U, {sprite['name']}_palette, {sprite['name']}_pixels_4bpp}},"
        )
    blocks.append("};")
    blocks.append("")

    ENEMY_HEADER_PATH.write_text(header)
    ENEMY_SOURCE_PATH.write_text("\n".join(blocks))


def write_deathscreen_assets(image: Image.Image) -> None:
    rgba = make_deathscreen(image).convert("RGBA")
    pixels = np.array(rgba)
    palette, indexed = palette_for_pixels(pixels, max_colors=15)
    packed = pack_4bpp(indexed)

    header = """#ifndef DOOM_DEATHSCREEN_ASSET_H
#define DOOM_DEATHSCREEN_ASSET_H

#include <stdint.h>

#define DOOM_DEATHSCREEN_W 128
#define DOOM_DEATHSCREEN_H 160

extern const uint16_t doom_deathscreen_palette[16];
extern const uint8_t doom_deathscreen_pixels_4bpp[10240];

void doom_deathscreen_draw(void);

#endif /* DOOM_DEATHSCREEN_ASSET_H */
"""

    source = f"""#include "doom_deathscreen.h"
#include "display.h"

const uint16_t doom_deathscreen_palette[16] = {{
{c_array(palette, 8, 4)}
}};

const uint8_t doom_deathscreen_pixels_4bpp[10240] = {{
{c_array(packed, 16, 2)}
}};

{DEATHSCREEN_DRAW_IMPL}
"""

    DEATH_HEADER_PATH.write_text(header)
    DEATH_SOURCE_PATH.write_text(source)


def main() -> None:
    enemy_sheet = Image.open(ENEMY_SHEET_PATH).convert("RGB")
    death_ref = Image.open(DEATH_REF_PATH).convert("RGB")
    write_enemy_assets(enemy_sheet)
    write_deathscreen_assets(death_ref)
    print(f"Wrote {ENEMY_HEADER_PATH.name}, {ENEMY_SOURCE_PATH.name}, {DEATH_HEADER_PATH.name}, and {DEATH_SOURCE_PATH.name}")


if __name__ == "__main__":
    main()
