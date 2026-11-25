#!/bin/sh
# Generates LVGL fonts containing exactly the characters used in main/*.c
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
for pair in 16:18 20:22 26:30; do
  name=${pair%%:*}; sz=${pair##*:}
  $CONV --font tools/fonts/NotoSansSC-Medium.ttf --size $sz --bpp 4 --no-compress --format lvgl --range 0x20-0x7E --symbols "$CJK" \
        --lv-font-name font_cn$name -o main/fonts/font_cn$name.c
done
$CONV --font tools/fonts/Inter-SemiBold.ttf --size 104 --bpp 4 --no-compress --format lvgl --symbols "0123456789:-°" --lv-font-name font_time -o main/fonts/font_time.c
$CONV --font tools/fonts/Inter-SemiBold.ttf --size 58 --bpp 4 --no-compress --format lvgl --symbols "0123456789:.-+% kg" --lv-font-name font_num -o main/fonts/font_num.c
ls -la main/fonts
