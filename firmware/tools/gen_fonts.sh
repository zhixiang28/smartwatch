#!/bin/sh
# Generates the LVGL fonts. NotoSansSC-Medium.ttf: instance the variable font from google/fonts
# (ofl/notosanssc) with `fonttools varLib.instancer NotoSansSC[wght].ttf wght=500`.
set -e
cd "$(dirname "$0")/.."
mkdir -p main/fonts
CONV=tools/node_modules/.bin/lv_font_conv
CJK=$(python3 -c "
import glob,re
s=set()
for f in glob.glob('main/*.c'):
    for ch in open(f,encoding='utf-8').read():
        if ord(ch)>0x7e: s.add(ch)
s.update('·：，。！？“”、%')
print(''.join(sorted(s)))")
echo "CJK/extra glyphs: $(printf %s "$CJK" | wc -m)"
# 16/20 px carry GB2312 level 1 too (text from outside: notifications, AI replies, WiFi names)
COMMON=$(python3 tools/cjk_common.py)
for pair in 16:18 20:22; do
  name=${pair%%:*}; sz=${pair##*:}
  $CONV --font tools/fonts/NotoSansSC-Medium.ttf --size $sz --bpp 4 --no-compress --format lvgl --range 0x20-0x7E --symbols "$COMMON" \
        --lv-font-name font_cn$name -o main/fonts/font_cn$name.c
done
# 26 px (big titles) only what the code uses
$CONV --font tools/fonts/NotoSansSC-Medium.ttf --size 30 --bpp 4 --no-compress --format lvgl --range 0x20-0x7E --symbols "$CJK" \
      --lv-font-name font_cn26 -o main/fonts/font_cn26.c
$CONV --font tools/fonts/Inter-SemiBold.ttf --size 104 --bpp 4 --no-compress --format lvgl --symbols "0123456789:-°" --lv-font-name font_time -o main/fonts/font_time.c
$CONV --font tools/fonts/Inter-SemiBold.ttf --size 58 --bpp 4 --no-compress --format lvgl --symbols "0123456789:.-+% kg" --lv-font-name font_num -o main/fonts/font_num.c
ls -la main/fonts
