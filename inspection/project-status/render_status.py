"""Render the Xbox prototype status snapshot from the local shader census."""

import json
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont


HERE = Path(__file__).resolve().parent
REPORT = HERE / 'shader-census.json'
WIDTH, HEIGHT = 1560, 900
PAPER, INK = '#f8faf8', '#151c18'
MUTED, RULE = '#52615a', '#d8e0db'
DARK, PANEL_TEXT, SOFT = '#101915', '#f1f8f3', '#b7cbbf'
GREEN, LIME, AMBER = '#a6e662', '#c9ef7a', '#f2cf7b'

stage_counts = json.loads(REPORT.read_text(encoding='utf-8'))
counts = stage_counts['program_kinds']
pixel, mesh, compute, vertex, geometry = (counts[kind] for kind in ('0', '13', '5', '1', '2'))
total = pixel + mesh + compute + vertex + geometry
assert stage_counts['parsed'] == total
processed = stage_counts['processed']
fixture_dlls_verified, fixture_dlls_required = 3, 3
verified_translations = ('PS_clearRipple', 'PS_CopyPoint')

image = Image.new('RGB', (WIDTH, HEIGHT), PAPER)
draw = ImageDraw.Draw(image)
font_dir = Path('C:/Windows/Fonts')


def font(size, weight='regular'):
    name = {'regular': 'segoeui.ttf', 'semibold': 'seguisb.ttf', 'bold': 'segoeuib.ttf'}[weight]
    return ImageFont.truetype(str(font_dir / name), size)


def label(x, y, value, size, fill=INK, weight='regular', anchor='lt'):
    draw.text((x, y), value, font=font(size, weight), fill=fill, anchor=anchor)


def badge(x, y, name, value, detail, tone):
    name_width = round(draw.textlength(name, font=font(21, 'semibold'))) + 40
    value_width = max(64, round(draw.textlength(value, font=font(24, 'bold'))) + 36)
    draw.rectangle((x, y, x + name_width, y + 53), fill='#303d35')
    draw.rectangle((x + name_width, y, x + name_width + value_width, y + 53), fill=tone)
    label(x + 20, y + 27, name, 21, PANEL_TEXT, 'semibold', 'lm')
    label(x + name_width + value_width / 2, y + 26, value, 24, '#1d2a15', 'bold', 'mm')
    label(x, y + 62, detail, 16, MUTED)
    return x + name_width + value_width + 12


def tiles(x, y, width, height, palette, pitch=13):
    draw.rectangle((x, y, x + width - 1, y + height - 1), fill=palette[0])
    for row, cell_y in enumerate(range(y + 2, y + height - 1, pitch)):
        for col, cell_x in enumerate(range(x + 2, x + width - 1, pitch)):
            draw.rectangle((cell_x, cell_y, min(cell_x + pitch - 3, x + width - 2),
                            min(cell_y + pitch - 3, y + height - 2)),
                           fill=palette[1 + ((row * 11 + col * 7 + row * col) % (len(palette) - 1))])


def runtime_tile(x, y, width, height, title, detail, pending=False):
    fill = '#2c2a1d' if pending else '#172b20'
    outline = '#4c442d' if pending else '#2a4936'
    draw.rectangle((x, y, x + width, y + height), fill=fill, outline=outline, width=1)
    accent = AMBER if pending else LIME
    label(x + 16, y + 14, 'PENDING' if pending else 'VERIFIED', 13, accent, 'bold')
    label(x + 16, y + 36, title, 20, PANEL_TEXT, 'semibold')
    label(x + 16, y + 62, detail, 16, SOFT)


def stage_tile(x, y, width, height, name, count, palette):
    tiles(x, y, width, height, palette)
    if name:
        share = f'{count / total:.2%}'
        title_width = round(draw.textlength(name, font=font(15, 'semibold')))
        share_width = round(draw.textlength(share, font=font(26, 'bold')))
        plate_width = max(title_width, share_width) + 24
        draw.rectangle((x + 10, y + 10, x + 10 + plate_width, y + 68), fill=DARK)
        label(x + 22, y + 18, name, 15, SOFT, 'semibold')
        label(x + 22, y + 37, share, 26, PANEL_TEXT, 'bold')


# Editorial header and the split badges from the supplied reference.
draw.rectangle((74, 55, 102, 59), fill='#80bc42')
label(114, 49, 'XBOX SERIES X|S', 19, '#356c39', 'bold')
label(72, 84, 'Compatibility status', 74, INK, 'bold')
label(76, 175, 'Windows and WSL2 prototypes', 22, MUTED)
label(1485, 55, 'v0.1.1 · 25 NOV 2026', 18, MUTED, 'semibold', 'rt')
draw.line((74, 222, 1485, 222), fill=RULE, width=2)

x = badge(74, 247, 'Fixture DLLs verified', f'{fixture_dlls_verified / fixture_dlls_required:.0%}',
          f'{fixture_dlls_verified} of {fixture_dlls_required} local shim DLLs', GREEN)
x = badge(x, 247, 'Shader containers decoded', f'{total / processed:.0%}',
          f'{total:,} of {processed:,} archive containers', LIME)
badge(x, 247, 'Shader translations verified', f'{len(verified_translations) / total:.4%}',
      f'{len(verified_translations)} of {total:,} decoded containers', GREEN)

# Runtime milestones and a proportionate shader-stage treemap.
draw.rounded_rectangle((74, 337, 1485, 791), radius=12, fill=DARK)
draw.line((644, 362, 644, 766), fill='#304139', width=1)
label(100, 363, 'Runtime checks', 28, PANEL_TEXT, 'bold')
label(100, 403, 'Local fixtures and public samples', 18, SOFT)
label(674, 363, 'Decoded shader containers', 28, PANEL_TEXT, 'bold')
label(674, 403, f'Share of {total:,} decoded containers by shader stage', 18, SOFT)

runtime_tile(100, 442, 256, 89, 'Import preflight', 'DLL exports checked')
runtime_tile(366, 442, 256, 89, 'Startup shim', 'Initialization and shutdown')
runtime_tile(100, 542, 256, 89, 'Frame shim', 'Frame tokens and GPU fences')
runtime_tile(366, 542, 256, 89, 'Windowed demo', 'SimpleTriangle on Intel')
runtime_tile(100, 642, 522, 89, 'Async compute on WSL2', 'Intel UHD · 99.82% pixel match')

map_x, map_y, map_w, map_h = 674, 442, 785, 244
pixel_w = round(map_w * pixel / total)
others_w = map_w - pixel_w
mesh_h = round(map_h * mesh / (total - pixel))
bottom_h = map_h - mesh_h
compute_w = round(others_w * compute / (compute + vertex + geometry))
geometry_w = round(others_w * geometry / (compute + vertex + geometry))
vertex_w = others_w - compute_w - geometry_w
right_x = map_x + pixel_w
bottom_y = map_y + mesh_h

stage_tile(map_x, map_y, pixel_w, map_h, 'PIXEL', pixel,
           ('#113421', '#256943', '#286e45', '#2c774e', '#307b4e'))
stage_tile(right_x, map_y, others_w, mesh_h, 'MESH', mesh,
           ('#183823', '#3a7046', '#43794a', '#4b8250', '#4f8853'))
stage_tile(right_x, bottom_y, compute_w, bottom_h, 'COMPUTE', compute,
           ('#1b4038', '#34786b', '#3a8574', '#367c6e'))
stage_tile(right_x + compute_w, bottom_y, vertex_w, bottom_h, 'VERTEX', vertex,
           ('#35411e', '#64763e', '#758447', '#7a8b4a'))
stage_tile(map_x + map_w - geometry_w, bottom_y, geometry_w, bottom_h, '', geometry,
           ('#24453e', '#80a696', '#75a28f'))
draw.line((right_x, map_y, right_x, map_y + map_h), fill=DARK, width=2)
draw.line((right_x, bottom_y, map_x + map_w, bottom_y), fill=DARK, width=2)
draw.line((right_x + compute_w, bottom_y, right_x + compute_w, map_y + map_h), fill=DARK, width=2)
draw.line((map_x + map_w - geometry_w, bottom_y, map_x + map_w - geometry_w, map_y + map_h), fill=DARK, width=2)

draw.rectangle((674, 705, 686, 717), fill='#80a696')
label(695, 702, f'Geometry · {geometry / total:.2%}', 16, SOFT)
label(674, 743, 'Verified in desktop draws', 17, LIME, 'semibold')
label(884, 743, '  ·  '.join(verified_translations), 17, PANEL_TEXT)

label(74, 822, 'NEXT MILESTONE', 15, '#356c39', 'bold')
label(74, 847, 'Make async compute runs exit cleanly after capture.', 22, INK, 'semibold')
label(1485, 848, 'RELEASE SNAPSHOT', 14, MUTED, 'semibold', 'rt')

image.save(HERE / 'xbox-compatibility-status.png', optimize=True)
