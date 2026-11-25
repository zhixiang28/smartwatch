"""Characters for the 16/20 px fonts: everything main/*.c uses, plus GB2312 level 1 (3755 most
common hanzi, ~99.7% of everyday text) and the GB2312 punctuation / fullwidth rows, so text from
outside (notifications, AI replies, WiFi names) renders instead of showing boxes."""
import glob
s = set()
for f in glob.glob('main/*.c'):
    for ch in open(f, encoding='utf-8').read():
        if ord(ch) > 0x7e:
            s.add(ch)
s.update('·：，。！？“”、%‘’《》【】…—～')
def gb(r0, r1, c0=0xA1, c1=0xFE):
    for r in range(r0, r1 + 1):
        for c in range(c0, c1 + 1):
            try:
                s.add(bytes([r, c]).decode('gb2312'))
            except UnicodeDecodeError:
                pass
gb(0xA1, 0xA1)          # punctuation
gb(0xA3, 0xA3)          # fullwidth ASCII
gb(0xB0, 0xD7)          # level 1 hanzi
s.discard('　')
print(''.join(sorted(s)))
