#!/usr/bin/env python3
"""
把 .ino 里用到的汉字栅格化成 1bpp 点阵，生成 cn_font_data.h

用法：
    python3 tools/gen_cn_font.py arduino/rlcd_stock_demo

工作流：改完 .ino 里的中文  ->  重跑本脚本  ->  编译烧录

它**自动扫描**目标目录下所有 .ino 文件里出现的汉字，只生成用得上的那些字，
所以不用维护字符清单 —— 改了文案重跑一遍就行。

为什么走这条路（而不是装一套完整中文字库）：
  · 这块屏是 1bpp，汉字点阵天然合身（16×16 一个字才 32 字节）
  · 子集字库自包含，不依赖文件系统，不需要 GBK 转码
  · 代价：字库在编译期定死。要新增汉字，得重跑本脚本。

有个坑：文件必须是 **UTF-8 编码**（Arduino 默认就是），否则扫不到汉字。
"""

import os
import re
import sys
from PIL import Image, ImageDraw, ImageFont

# ---------------- 可调参数 ----------------
SIZE = 16          # 点阵字号（像素）。16/20/24 是常见选择，越大越清楚但越占地方。
                   # 改这里之后要微调 .ino 里的 y 坐标，字变高了会串行
THRESHOLD = 110    # 灰度阈值：> 110 判为黑点。调大 = 笔画更粗
BASE = 32          # 渲染画布里的基线位置（内部用，不用管）
CANVAS_H = 64
FONT_CANDIDATES = [
    "/System/Library/Fonts/Hiragino Sans GB.ttc",   # 冬青黑体，小字号最清楚
    "/System/Library/Fonts/STHeiti Light.ttc",
    "/System/Library/Fonts/Supplemental/Songti.ttc",
]
# -----------------------------------------

# 汉字（含中文标点）的 Unicode 区间
CJK_RANGES = [
    (0x3000, 0x303F),   # 中文标点
    (0x4E00, 0x9FFF),   # 基本汉字
    (0xFF00, 0xFFEF),   # 全角字符
]


def is_cjk(cp):
    return any(lo <= cp <= hi for lo, hi in CJK_RANGES)


def pick_font():
    for path in FONT_CANDIDATES:
        if os.path.exists(path):
            try:
                return ImageFont.truetype(path, SIZE, index=0), path
            except Exception:
                continue
    sys.exit("找不到可用的中文字体，改一下 FONT_CANDIDATES")


def scan_chars(sketch_dir):
    """扫出目录下所有 .ino 里用到的汉字（去重、排序）"""
    chars = set()
    files = [f for f in os.listdir(sketch_dir) if f.endswith((".ino", ".h", ".cpp"))]
    if not files:
        sys.exit(f"{sketch_dir} 里没找到 .ino/.h/.cpp")
    for fn in sorted(files):
        path = os.path.join(sketch_dir, fn)
        text = open(path, encoding="utf-8").read()
        # 先剥掉注释，免得把注释里的汉字也打进字库（白占空间）
        text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
        text = re.sub(r"//[^\n]*", "", text)
        for ch in text:
            if is_cjk(ord(ch)):
                chars.add(ch)
    return sorted(chars)


def render(ch, font):
    img = Image.new("L", (SIZE, CANVAS_H), 0)
    ImageDraw.Draw(img).text((0, BASE), ch, fill=255, font=font, anchor="ls")
    return img


def pack(img, top, bottom):
    """裁成 [0..SIZE) × [top..bottom) 并打包成 1bpp。

    ⚠️ 位序是 **LSB 优先**（bit 0 = 最左边的像素），不是标准 XBM 的 MSB 优先！

    这是被真机教育过的：按标准 XBM 的 `0x80 >> i` 写，屏幕上的字会**左右镜像**，
    因为 U8g2 每个字节里是从低位往高位走的。证据在 U8g2 源码
    src/clib/u8g2_bitmap.c 的 u8g2_DrawHXBM()：

        mask = 1;
        while(len > 0) {
          uint8_t current_bit = (*b) & mask;
          ...
          mask <<= 1;          // ← 左移，所以 bit0 是最左像素
        }

    所以这里必须是 `1 << i`。用 `0x80 >> i` 的话，每 8 个像素会各自镜像一次
    ——16px 宽的汉字被切成两段 8px，每段各反一次，看着就是"字反了"。

    行优先（一行一行往下）这点和标准 XBM 一致，不用改。
    """
    out = bytearray()
    for y in range(top, bottom):
        for bx in range(0, SIZE, 8):
            byte = 0
            for i in range(8):
                if bx + i < SIZE and img.getpixel((bx + i, y)) > THRESHOLD:
                    byte |= 1 << i          # ← LSB 优先，别改成 0x80 >> i
            out.append(byte)
    return bytes(out)


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    sketch_dir = sys.argv[1]
    if not os.path.isdir(sketch_dir):
        sys.exit(f"目录不存在：{sketch_dir}")

    font, font_path = pick_font()
    chars = scan_chars(sketch_dir)
    if not chars:
        sys.exit("没扫到任何汉字。检查 .ino 里有没有中文，以及文件是不是 UTF-8")

    # 渲染全部字形，取并集包围盒。所有字共用同一个窗口，
    # 这样不同字之间基线一致（不会「一」和「上」一高一低）。
    rendered = {ch: render(ch, font) for ch in chars}
    boxes = {ch: img.getbbox() for ch, img in rendered.items()}

    tops = [b[1] for b in boxes.values() if b]
    bots = [b[3] for b in boxes.values() if b]
    lefts = [b[0] for b in boxes.values() if b]
    rights = [b[2] for b in boxes.values() if b]

    top, bottom = min(tops), max(bots)
    if min(lefts) < 0 or max(rights) > SIZE:
        print(f"  ⚠️ 有字形横向超出 {SIZE}px 的窗口，可能被裁")

    cn_h = bottom - top
    cn_ascent = BASE - top          # 从窗口顶部到基线的像素数
    glyph_bytes = ((SIZE + 7) // 8) * cn_h

    out_path = os.path.join(sketch_dir, "cn_font_data.h")
    with open(out_path, "w", encoding="utf-8") as f:
        f.write(f"""/*
 * 汉字点阵库 —— **自动生成，不要手改**
 * ================================================================
 * 生成命令： python3 tools/gen_cn_font.py {sketch_dir}
 * 字体来源： {os.path.basename(font_path)}
 * 字号：     {SIZE}px   共 {len(chars)} 个汉字
 *
 * 改了 .ino 里的中文文案之后，重跑一次生成脚本，否则新字会显示成空框。
 * 怎么用见同目录的 cn_text.h。
 */

#pragma once
#include <stdint.h>

#define CN_W {SIZE}
#define CN_H {cn_h}
#define CN_ASCENT {cn_ascent}
#define CN_GLYPH_BYTES {glyph_bytes}

""")
        f.write(f"static const uint32_t CN_CODEPOINTS[{len(chars)}] = {{\n")
        for i in range(0, len(chars), 8):
            row = ", ".join(f"0x{ord(c):04X}" for c in chars[i:i + 8])
            f.write(f"    {row},\n")
        f.write("};\n\n")

        f.write(f"static const uint8_t CN_GLYPHS[{len(chars)} * CN_GLYPH_BYTES] = {{\n")
        for ch in chars:
            data = pack(rendered[ch], top, bottom)
            assert len(data) == glyph_bytes, (ch, len(data), glyph_bytes)
            f.write(f"    /* {ch} U+{ord(ch):04X} */\n")
            for i in range(0, len(data), 12):
                f.write("    " + ", ".join(f"0x{b:02X}" for b in data[i:i + 12]) + ",\n")
        f.write("};\n\n")
        f.write(f"#define CN_COUNT {len(chars)}\n")

    total = len(chars) * glyph_bytes
    print(f"✓ 生成 {out_path}")
    print(f"  字体   {os.path.basename(font_path)} @ {SIZE}px")
    print(f"  字形   {len(chars)} 个汉字，窗口 {SIZE}×{cn_h}，基线偏移 {cn_ascent}")
    print(f"  体积   {total} 字节（{total / 1024:.1f} KB）")
    print(f"  字符   {''.join(chars)}")


if __name__ == "__main__":
    main()
