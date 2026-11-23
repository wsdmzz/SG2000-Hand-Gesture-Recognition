#!/usr/bin/env python3
# gen_ui_font.py — 从系统 Noto Sans Mono CJK SC 生成采集器 UI 点阵字库 ui_font.h
#
# 半角/全角模型（与 HZK16 一致）：
#   ASCII 0x20..0x7E → 8x16 点阵（16 字节/字，行优先，MSB first，1bpp）
#   CJK 用字         → 16x16 点阵（32 字节/字）
# CJK 字符集自动从 ui_model.c 扫描（保证标签全覆盖），不手写遗漏。
#
# 字体源：NotoSansCJK-Regular.ttc index 7 = "Noto Sans Mono CJK SC Regular"
#   等宽 CJK 字体，拉丁半角(8px)/汉字全角(16px) @16px，正好对齐 8x16/16x16 格。
#
# 用法：python3 gen_ui_font.py [ui_model.c] [out.h]
import sys, os
from PIL import Image, ImageDraw, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = sys.argv[1] if len(sys.argv) > 1 else os.path.join(HERE, "ui_model.c")
OUT = sys.argv[2] if len(sys.argv) > 2 else os.path.join(HERE, "include", "ui_font.h")

FONT_PATH = "/usr/share/fonts/google-noto-sans-cjk-fonts/NotoSansCJK-Regular.ttc"
FONT_INDEX = 7  # Noto Sans Mono CJK SC Regular
SIZE = 16

def collect_cjk(path):
    with open(path, encoding="utf-8") as f:
        text = f.read()
    cps = sorted({ord(c) for c in text if ord(c) > 0x7F})
    return cps

def render_glyph(font, ch, w, h):
    img = Image.new("1", (w, h), 0)
    d = ImageDraw.Draw(img)
    bbox = font.getbbox(ch)  # (x0,y0,x1,y1)
    gw, gh = bbox[2] - bbox[0], bbox[3] - bbox[1]
    x = (w - gw) // 2 - bbox[0]
    y = (h - gh) // 2 - bbox[1]
    d.text((x, y), ch, font=font, fill=1)
    px = img.load()
    nbytes = (w + 7) // 8
    out = bytearray()
    for row in range(h):
        bits = 0
        for col in range(w):
            if px[col, row]:
                bits |= 1 << (7 - (col % 8))
            if col % 8 == 7 or col == w - 1:
                out.append(bits)
                bits = 0
    assert len(out) == nbytes * h, (ch, len(out))
    return bytes(out)

def c_array(data, per_line=16):
    lines = []
    for i in range(0, len(data), per_line):
        lines.append("\t\t" + ",".join("0x%02x" % b for b in data[i:i+per_line]) + ",")
    return "\n".join(lines)

def main():
    font = ImageFont.truetype(FONT_PATH, SIZE, index=FONT_INDEX)
    name = font.getname()
    cjk = collect_cjk(SRC)
    ascii_codes = list(range(0x20, 0x7F))

    with open(OUT, "w", encoding="utf-8") as f:
        f.write("/* ui_font.h — 自动生成，勿手改。由 gen_ui_font.py 从 %s 生成。\n" % name[0])
        f.write(" * ASCII 8x16（半角），CJK 16x16（全角），1bpp 行优先 MSB first。\n")
        f.write(" * CJK 字符集来自 ui_model.c 标签用字，共 %d 字。 */\n" % len(cjk))
        f.write("#ifndef UI_FONT_H\n#define UI_FONT_H\n\n")
        f.write("#define FONT_ASCII_W 8\n#define FONT_ASCII_H 16\n")
        f.write("#define FONT_CJK_W 16\n#define FONT_CJK_H 16\n")
        f.write("#define FONT_ASCII_FIRST 0x20\n#define FONT_ASCII_COUNT %d\n\n" % len(ascii_codes))
        f.write("/* 半角点阵：[ASCII_COUNT][16] */\n")
        f.write("static const unsigned char font_ascii[FONT_ASCII_COUNT][FONT_ASCII_H] = {\n")
        for cp in ascii_codes:
            g = render_glyph(font, chr(cp), 8, 16)
            f.write("\t{ /* 0x%02x '%s' */\n%s\n\t},\n" % (cp, chr(cp) if cp != 0x20 else ' ', c_array(g)))
        f.write("};\n\n")
        f.write("/* 全角点阵：按码点升序，渲染层二分查找 */\n")
        f.write("typedef struct { unsigned int cp; unsigned char bits[FONT_CJK_W * FONT_CJK_H / 8]; } font_cjk_glyph_t;\n")
        f.write("#define FONT_CJK_COUNT %d\n\n" % len(cjk))
        f.write("static const font_cjk_glyph_t font_cjk[FONT_CJK_COUNT] = {\n")
        for cp in cjk:
            g = render_glyph(font, chr(cp), 16, 16)
            f.write("\t{ 0x%04x, { /* %s */\n%s\n\t} },\n" % (cp, chr(cp), c_array(g)))
        f.write("};\n\n#endif /* UI_FONT_H */\n")
    print("生成 %s：ASCII %d + CJK %d 字（%s）" % (OUT, len(ascii_codes), len(cjk),
          "".join(chr(c) for c in cjk)))

if __name__ == "__main__":
    main()