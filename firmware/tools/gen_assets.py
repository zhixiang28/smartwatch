#!/usr/bin/env python3
"""Render fish sprites, tank background and app icons with headless Chrome,
then convert them to LVGL v9 image descriptors (C arrays) in main/assets/."""
import os, io, sys
from PIL import Image
from playwright.sync_api import sync_playwright

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, 'main', 'assets')
os.makedirs(OUT, exist_ok=True)

FISH = [
    dict(name='gold', s=1.25, c1='#ff8a4c', c2='#ffd2a6', spot=None),
    dict(name='koi', s=1.0, c1='#fff4ee', c2='#ffd9cf', spot='#ff4d4d'),
    dict(name='black', s=0.85, c1='#2b2b30', c2='#f2b84b', spot=None),
]
TAIL_ANGLES = [-13, -5, 5, 13]

def fish_svg(f, angle, flip):
    spot = f'<ellipse cx="50" cy="20" rx="7" ry="4" fill="{f["spot"]}" opacity=".85"/>' if f['spot'] else ''
    tr = 'transform="translate(80,0) scale(-1,1)"' if flip else ''
    w, h = 92 * f['s'], 60 * f['s']
    return f'''<svg xmlns="http://www.w3.org/2000/svg" width="{w:.0f}" height="{h:.0f}" viewBox="-6 -6 92 60">
<defs>
<linearGradient id="b" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="{f['c1']}"/><stop offset="1" stop-color="{f['c2']}"/></linearGradient>
<linearGradient id="t" x1="1" y1="0" x2="0" y2="0"><stop offset="0" stop-color="{f['c1']}" stop-opacity=".95"/><stop offset="1" stop-color="{f['c2']}" stop-opacity=".3"/></linearGradient>
</defs>
<g {tr}>
<path transform="rotate({angle} 22 24)" d="M24 24 C12 6 3 2 0 6 C5 15 8 21 6 24 C8 27 5 33 0 42 C3 46 12 42 24 24Z" fill="url(#t)"/>
<path d="M34 14 C38 2 52 2 57 14Z" fill="url(#t)"/>
<path d="M20 24 C24 10 44 8 58 13 C68 17 73 21 73 24 C73 27 68 31 58 35 C44 40 24 38 20 24Z" fill="url(#b)"/>
<path d="M40 18 q4 4 0 8 M47 17 q4 5 0 10 M54 18 q3 4 0 8" stroke="#fff" stroke-opacity=".25" stroke-width="1.2" fill="none"/>
{spot}
<path transform="rotate({-angle * 1.6} 44 30)" d="M44 30 C40 38 46 42 50 36Z" fill="{f['c1']}" opacity=".85"/>
<circle cx="63" cy="21" r="3.4" fill="#0b0b0b"/><circle cx="64.2" cy="19.9" r="1.1" fill="#fff"/>
</g></svg>'''

BG_HTML = '''<html><body style="margin:0;background:#000">
<div id="bg" style="width:410px;height:502px;position:relative;overflow:hidden;background:linear-gradient(180deg,#0b3a48 0%,#06232d 45%,#03131a 100%)">
<div style="position:absolute;inset:0;opacity:.12;background:conic-gradient(from 200deg at 30% -10%,transparent 0deg,#bff 8deg,transparent 16deg,#bff 26deg,transparent 34deg)"></div>
<svg width="410" height="130" viewBox="0 0 410 130" style="position:absolute;left:0;bottom:0">
<path d="M0 90 Q70 76 140 88 T280 84 T410 90 V130 H0Z" fill="#1b2f2a"/>
<path d="M0 104 Q90 94 200 104 T410 102 V130 H0Z" fill="#26392f"/>
<g fill="#33463b"><circle cx="90" cy="112" r="2"/><circle cx="160" cy="118" r="1.6"/><circle cx="240" cy="110" r="2.2"/><circle cx="372" cy="116" r="1.8"/></g>
<ellipse cx="318" cy="94" rx="30" ry="12" fill="#3a4a40"/><ellipse cx="300" cy="100" rx="14" ry="7" fill="#4b5b4f"/>
<path d="M44 106 C34 76 58 60 46 26" stroke="#2f7d5c" stroke-width="6" fill="none" stroke-linecap="round"/>
<path d="M58 106 C66 84 50 70 62 46" stroke="#3f9a70" stroke-width="5" fill="none" stroke-linecap="round"/>
<path d="M370 104 C360 70 384 56 372 18" stroke="#2f7d5c" stroke-width="6" fill="none" stroke-linecap="round"/>
<path d="M384 104 C392 84 378 72 388 54" stroke="#3f9a70" stroke-width="4" fill="none" stroke-linecap="round"/>
</svg></div></body></html>'''

ICONS = {
    'ai': 'M12 3v3M12 18v3M3 12h3M18 12h3M7.5 12a4.5 4.5 0 1 0 9 0a4.5 4.5 0 1 0 -9 0',
    'tr': 'M4 5h8M8 3v2M6 5c0 4 3 7 6 8M10 5c-1 4-4 7-6 8M13 21l4-9 4 9M14.5 18h5',
    'nt': 'M4 20l4-1 11-11-3-3L5 16zM14 6l3 3',
    'wk': 'M2 9v6M5 7v10M19 7v10M22 9v6M5 12h14',
    'st': 'M8 4c2 0 3 2 3 5s-1 5-3 5-3-2-3-5 1-5 3-5M16 10c2 0 3 2 3 5s-1 5-3 5-3-2-3-5 1-5 3-5',
    'tm': 'M4 13a8 8 0 1 0 16 0a8 8 0 1 0 -16 0M12 9v4l3 2M9 2h6',
    'fo': 'M3 12a9 9 0 1 0 18 0a9 9 0 1 0 -18 0M7 12a5 5 0 1 0 10 0a5 5 0 1 0 -10 0M11 12a1 1 0 1 0 2 0a1 1 0 1 0 -2 0',
    'mu': 'M9 18V5l11-2v13M3 18a3 3 0 1 0 6 0a3 3 0 1 0 -6 0M14 16a3 3 0 1 0 6 0a3 3 0 1 0 -6 0',
    'rc': 'M9 6a3 3 0 0 1 6 0v5a3 3 0 0 1 -6 0zM5 11a7 7 0 0 0 14 0M12 18v3',
    'cm': 'M4 8h3l2-3h6l2 3h3v11H4zM9 13a3 3 0 1 0 6 0a3 3 0 1 0 -6 0',
    'al': 'M5 13a7 7 0 1 0 14 0a7 7 0 1 0 -14 0M12 10v3h2M4 5l3-2M20 5l-3-2',
    'we': 'M7 18h10a4 4 0 0 0 0-8 6 6 0 0 0-11.5 1.5A3.3 3.3 0 0 0 7 18z',
    'fl': 'M8 2h8l-1 6H9zM9 8h6v13H9zM12 12v3',
    'fp': 'M8 5a2 2 0 0 1 2-2h4a2 2 0 0 1 2 2v14a2 2 0 0 1-2 2h-4a2 2 0 0 1-2-2zM19 8c1.5 1 1.5 7 0 8M5 8c-1.5 1-1.5 7 0 8',
    'lt': 'M9 18h6M10 21h4M12 3a6 6 0 0 0-4 10.5c.8.8 1 1.5 1 2.5h6c0-1 .2-1.7 1-2.5A6 6 0 0 0 12 3z',
    'fc': 'M5 7a4 4 0 0 1 4-4h6a4 4 0 0 1 4 4v10a4 4 0 0 1-4 4H9a4 4 0 0 1-4-4zM12 8v4l2 2',
    'ph': 'M7 4a2 2 0 0 1 2-2h6a2 2 0 0 1 2 2v16a2 2 0 0 1-2 2H9a2 2 0 0 1-2-2zM11 18h2',
    'se': 'M9 12a3 3 0 1 0 6 0a3 3 0 1 0 -6 0M12 2v3M12 19v3M2 12h3M19 12h3M5 5l2 2M17 17l2 2M5 19l2-2M17 7l2-2',
    'no': 'M6 16V11a6 6 0 0 1 12 0v5l2 2H4zM10 21h4',
    'bt': 'M7 7l10 10-5 5V2l5 5L7 17',
    'wifi': 'M2 9a15 15 0 0 1 20 0M5 12.5a10 10 0 0 1 14 0M8.5 16a5 5 0 0 1 7 0M12 19.5h.01',
    'moon': 'M20 14A8 8 0 1 1 10 4a6 6 0 0 0 10 10z',
    'mute': 'M11 5L6 9H3v6h3l5 4zM16 9l5 6M21 9l-5 6',
    'batt': 'M3 8h15v8H3zM21 11v2M6 11h5',
    'eye': 'M2 12c3-6 17-6 20 0-3 6-17 6-20 0zM12 12h.01',
    'back': 'M15 5l-7 7 7 7',
    'fish': 'M2 12c3-5 9-6 14-3l5-3v12l-5-3c-5 3-11 2-14-3zM14 11h.01',
}
ICON_PX = 40


def render(page, html, selector, w, h):
    page.set_viewport_size({'width': max(w, 10), 'height': max(h, 10)})
    page.set_content(html)
    return Image.open(io.BytesIO(page.locator(selector).screenshot(omit_background=True))).convert('RGBA')


def c_array(name, cf, w, h, stride, data):
    lines = []
    for i in range(0, len(data), 24):
        lines.append(', '.join(f'0x{b:02x}' for b in data[i:i + 24]))
    body = ',\n'.join(lines)
    return f'''static const uint8_t {name}_map[] = {{
{body}
}};
const lv_image_dsc_t {name} = {{
    .header = {{ .magic = LV_IMAGE_HEADER_MAGIC, .cf = {cf}, .w = {w}, .h = {h}, .stride = {stride} }},
    .data_size = sizeof({name}_map),
    .data = {name}_map,
}};
'''


def argb8888(img):
    out = bytearray()
    for r, g, b, a in img.getdata():
        out += bytes((b, g, r, a))
    return out


def rgb565(img):
    out = bytearray()
    for r, g, b, *_ in img.convert('RGB').getdata():
        v = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
        out += bytes((v & 0xFF, v >> 8))
    return out


def a8(img):
    return bytes(img.getchannel('A').getdata())


def main():
    decls = []
    src = ['#include "assets.h"\n']
    with sync_playwright() as p:
        b = p.chromium.launch(executable_path='/usr/bin/google-chrome')
        page = b.new_page(device_scale_factor=1)
        # background
        img = render(page, BG_HTML, '#bg', 410, 502)
        img.save('/tmp/bg.png')
        src.append(c_array('img_tank_bg', 'LV_COLOR_FORMAT_RGB565', 410, 502, 820, rgb565(img)))
        decls.append('img_tank_bg')
        # fish
        for f in FISH:
            for flip in (0, 1):
                for k, ang in enumerate(TAIL_ANGLES):
                    html = f'<html><body style="margin:0;background:transparent"><div id="f" style="display:inline-block;line-height:0">{fish_svg(f, ang, flip)}</div></body></html>'
                    img = render(page, html, '#f', int(92 * f['s']) + 2, int(60 * f['s']) + 2)
                    name = f'img_fish_{f["name"]}_{"l" if flip else "r"}{k}'
                    if k == 0 and flip == 0:
                        img.save(f'/tmp/{name}.png')
                    src.append(c_array(name, 'LV_COLOR_FORMAT_ARGB8888', img.width, img.height, img.width * 4, argb8888(img)))
                    decls.append(name)
        # icons
        for key, d in ICONS.items():
            svg = f'<svg xmlns="http://www.w3.org/2000/svg" width="{ICON_PX}" height="{ICON_PX}" viewBox="0 0 24 24" fill="none" stroke="#fff" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="{d}"/></svg>'
            html = f'<html><body style="margin:0;background:transparent"><div id="i" style="display:inline-block;line-height:0">{svg}</div></body></html>'
            img = render(page, html, '#i', ICON_PX, ICON_PX)
            name = f'img_icon_{key}'
            src.append(c_array(name, 'LV_COLOR_FORMAT_A8', img.width, img.height, img.width, a8(img)))
            decls.append(name)
        b.close()
    with open(os.path.join(OUT, 'assets.c'), 'w') as fh:
        fh.write('\n'.join(src))
    with open(os.path.join(OUT, 'assets.h'), 'w') as fh:
        fh.write('#pragma once\n#include "lvgl.h"\n\n')
        for n in decls:
            fh.write(f'extern const lv_image_dsc_t {n};\n')
    print('assets:', len(decls))


if __name__ == '__main__':
    main()
