#!/usr/bin/env python3
"""
在终端里直接查看 img2slides.py 生成的 .bin —— 不烧录就能看效果

用法：
    python3 tools/show_slide.py arduino/photo_slideshow/data/slide_000.bin
    python3 tools/show_slide.py arduino/photo_slideshow/data/          # 看整个目录
    python3 tools/show_slide.py a.bin --diff b.bin                     # 比对两次转换

为什么要有它：调 --gamma / --contrast 这类参数时，改一次就要
「转图 -> 打包 -> 烧录 -> 盯屏幕」一轮，很慢。这个脚本直接从 .bin 读，
先用眼睛过一遍，觉得行了再烧。

.bin 里没有任何包装，解码就三行：

    stride = (W + 7) // 8                # 每行多少字节。400 宽 = 50
    byte   = data[y * stride + (x >> 3)] # 第 y 行、第 x//8 个字节
    bit    = (byte >> (x & 7)) & 1       # 字节内 LSB 优先

⚠️ 行跨距是 **ceil(W/8) 不是 W//8** —— 宽度不是 8 的倍数时最后那组要补零占满
   一个字节。400 宽时两个算法都是 50，看不出差别；换竖屏 300 宽就是 38 vs 37，
   整个文件会错位。

⚠️ 记住 **bit=1 是白、bit=0 是黑**（这块屏实测的方向，见 Notes.md §6.5）。
   看反了就会觉得整张图是底片。

两种粒度，各有各的用处：

  整图     必须降采样（终端一行放不下 400 像素），只能看构图和明暗分布
  局部 1:1 每个方块 = 一个像素，能直接看到抖动的网点长什么样

选项：
    -w N            整图渲染宽度，默认 100 字符（终端窄就调小）
    --zoom x,y,w,h  局部 1:1 区域，默认 150,140,100,60
    --no-zoom       不画局部
    --no-full       不画整图（只想看网点、不想滚屏时用）
    --png FILE      另存一张 PNG，1:1 全图放大后看（终端放不下整图的解法）
    --scale N       --png 的放大倍数，默认 3（每个像素画成 N×N 的方块）
    --size WxH      图像尺寸，默认 400x300（要和转换时一致）
    --diff FILE     和另一个 .bin 逐像素比对，看改参数到底改动了多少
"""

import os
import sys

# ---------------- 可调参数 ----------------
WIDTH, HEIGHT = 400, 300      # 要和 img2slides.py 的 WIDTH/HEIGHT 一致
FULL_COLS = 100               # 整图渲染宽度（字符数）
ZOOM = (150, 140, 100, 60)    # 局部 1:1 的默认区域 x,y,w,h
RAMP = " .:-=+*#%@"
# -----------------------------------------

EXTS = (".bin",)


def load(path, width, height):
    """读 .bin 并校验大小

    ⚠️ 行跨距是 **ceil(W/8)** 不是 W//8 —— 宽度不是 8 的倍数时，
       最后那组不满 8 个像素也要占满一个字节（img2slides.py 的 pack_xbm
       是这么补零的）。400 宽时两个算法都是 50，看不出差别；
       换成竖屏 300 宽就是 38 vs 37，整个文件会错位。
    """
    data = open(path, "rb").read()
    stride = (width + 7) // 8
    want = stride * height
    if len(data) != want:
        sys.exit(f"{os.path.basename(path)} 是 {len(data)} 字节，"
                 f"{width}×{height} 应该是 {want} 字节。\n"
                 f"  尺寸不对就用 --size 指定转换时用的值；"
                 f"如果差得远，可能是别的 Demo 的文件。")
    return data


def make_reader(data, width):
    """返回 bit(x, y)：1 = 白，0 = 黑"""
    stride = (width + 7) // 8       # ceil，和 load() 保持一致

    def bit(x, y):
        return (data[y * stride + (x >> 3)] >> (x & 7)) & 1

    return bit


def stats(bit, width, height):
    """黑点占比 + 「实心」区域占比

    实心 = 4×4 的块里几乎全黑或全白。这个数低说明到处是中间调，
    屏幕上看着就是「麻麻的一片灰」——正是该加大 --contrast 的信号。
    """
    dark = 0
    solid = tot = 0
    for y in range(height):
        for x in range(width):
            if not bit(x, y):
                dark += 1
    for y in range(0, height - 3, 4):
        for x in range(0, width - 3, 4):
            s = sum(1 - bit(x + dx, y + dy) for dy in range(4) for dx in range(4))
            tot += 1
            if s <= 1 or s >= 15:
                solid += 1
    return dark / (width * height), solid / max(tot, 1)


def render_full(bit, width, height, cols):
    """整图：按 cols 做方块降采样，用字符密度表示明暗"""
    # 目标行数：让字符格大致是「高 ≈ 宽 × 2」（终端字符本来就是这个比例）
    rows = max(1, int(cols * height / width / 2))
    sy = height / rows
    sx = width / cols
    out = []
    for ry in range(rows):
        y0, y1 = int(ry * sy), max(int(ry * sy) + 1, int((ry + 1) * sy))
        line = ""
        for rx in range(cols):
            x0, x1 = int(rx * sx), max(int(rx * sx) + 1, int((rx + 1) * sx))
            s = n = 0
            for y in range(y0, min(y1, height)):
                for x in range(x0, min(x1, width)):
                    s += 1 - bit(x, y)
                    n += 1
            line += RAMP[min(9, s * 10 // max(n, 1))]
        out.append(line)
    return "\n".join(out)


def render_zoom(bit, x0, y0, w, h, width, height):
    """局部 1:1：一个字符 = 1×2 个像素，能看见真实的抖动网点"""
    x0 = max(0, min(x0, width - 1))
    y0 = max(0, min(y0, height - 1))
    x1 = min(width, x0 + w)
    y1 = min(height, y0 + h)
    out = []
    for y in range(y0, y1 - 1, 2):
        line = ""
        for x in range(x0, x1):
            top = bit(x, y)          # 上半像素：1=白 0=黑
            bot = bit(x, y + 1)
            if not top and not bot:
                line += "█"          # 上下都黑
            elif not top:
                line += "▀"          # 只有上黑
            elif not bot:
                line += "▄"          # 只有下黑
            else:
                line += " "          # 都白
        out.append(line)
    return "\n".join(out)


def show(path, data, width, height, opts):
    name = os.path.basename(path)
    bit = make_reader(data, width)
    dark, solid = stats(bit, width, height)
    print(f"\n{'═' * 12} {name} {'═' * 12}")
    print(f"  {width}×{height}  {len(data)} 字节"
          f"   黑点 {dark * 100:.1f}%   实心区域 {solid * 100:.0f}%")
    if opts["full"]:
        print(f"\n【整图】{opts['cols']} 字符宽（降采样，看构图；@ = 黑）")
        print(render_full(bit, width, height, opts["cols"]))
    if opts["zoom"]:
        x, y, w, h = opts["zoom"]
        print(f"\n【局部 1:1】x {x}~{x + w}, y {y}~{y + h}"
              f"   每个方块 = 1 像素   █ 黑 / 空格 白")
        print(render_zoom(bit, x, y, w, h, width, height))


def save_png(path, bit, width, height, scale):
    """把 1bpp 数据存成 PNG，每个像素放大成 scale×scale 的方块

    为什么需要：终端一行放不下 400 个字符，所以 1:1 只能看局部。
    存成图片就能一次看到**完整的 1:1 画面**，还能在图片查看器里随便缩放。

    ⚠️ 用 NEAREST 放大（不是插值）——一插值就会出现灰的过渡像素，
       那就不是这块屏真实的样子了。放大后每块非黑即白。

    PIL 只在用到这个功能时才 import，所以纯看 ASCII 的话不依赖 Pillow。
    """
    from PIL import Image
    img = Image.new("L", (width, height))
    px = img.load()
    assert px is not None       # Pillow 的类型标注里 load() 是 PixelAccess | None
    for y in range(height):
        for x in range(width):
            px[x, y] = 255 if bit(x, y) else 0      # bit=1 是白
    img = img.resize((width * scale, height * scale), Image.Resampling.NEAREST)
    img.save(path)
    return img.size


def diff(a_path, a, b_path, b, width, height):
    """逐像素比对，看改参数到底动了多少"""
    ra, rb = make_reader(a, width), make_reader(b, width)
    same = 0
    first = None
    for y in range(height):
        for x in range(width):
            if ra(x, y) == rb(x, y):
                same += 1
            elif first is None:
                first = (x, y)
    tot = width * height
    print(f"\n{'═' * 12} 比对 {'═' * 12}")
    print(f"  {os.path.basename(a_path)}  vs  {os.path.basename(b_path)}")
    print(f"  相同 {same * 100 / tot:.1f}%   不同 {(tot - same) * 100 / tot:.1f}%"
          f"（{(tot - same)} 个像素）")
    if first:
        print(f"  第一个不同处在 ({first[0]}, {first[1]})"
              f" —— 可以用 --zoom 看那一块")


def parse_args(argv):
    opts = {"cols": FULL_COLS, "zoom": ZOOM, "full": True, "png": None,
            "scale": 3, "size": (WIDTH, HEIGHT), "diff": None}
    inputs = []
    i = 0
    while i < len(argv):
        a = argv[i]

        def val(what):
            if i + 1 >= len(argv):
                sys.exit(f"{a} 后面要跟{what}")
            return argv[i + 1]

        if a in ("-h", "--help"):
            sys.exit(__doc__)
        elif a == "-w":
            opts["cols"] = max(20, int(val("宽度")))
            i += 1
        elif a == "--zoom":
            try:
                opts["zoom"] = tuple(int(v) for v in val("x,y,w,h").split(","))
                assert len(opts["zoom"]) == 4
            except (ValueError, AssertionError):
                sys.exit("--zoom 格式是 x,y,w,h，比如 --zoom 150,140,100,60")
            i += 1
        elif a == "--no-zoom":
            opts["zoom"] = None
        elif a == "--no-full":
            opts["full"] = False
        elif a == "--png":
            opts["png"] = val("输出路径")
            i += 1
        elif a == "--scale":
            try:
                opts["scale"] = max(1, int(val("倍数")))
            except ValueError:
                sys.exit("--scale 要跟一个整数，比如 --scale 3")
            i += 1
        elif a == "--size":
            try:
                opts["size"] = tuple(int(v) for v in val("宽x高").lower().split("x"))
                assert len(opts["size"]) == 2
            except (ValueError, AssertionError):
                sys.exit("--size 格式是 宽x高，比如 400x300")
            i += 1
        elif a == "--diff":
            opts["diff"] = val("另一个 .bin 路径")
            i += 1
        elif a.startswith("-"):
            sys.exit(f"不认识的参数：{a}\n\n{__doc__}")
        else:
            inputs.append(a)
        i += 1

    if not inputs:
        sys.exit(__doc__)

    files = []
    for p in inputs:
        if os.path.isdir(p):
            files += [os.path.join(p, f) for f in sorted(os.listdir(p))
                      if f.lower().endswith(EXTS) and not f.startswith(".")]
        elif os.path.isfile(p):
            files.append(p)
        else:
            sys.exit(f"找不到：{p}")
    if not files:
        sys.exit(f"{'、'.join(inputs)} 里没有 .bin")
    return files, opts


def main():
    files, opts = parse_args(sys.argv[1:])
    W, H = opts["size"]

    # 文件多的时候自动关掉局部图，否则输出会长到没法看
    if len(files) > 1 and opts["zoom"]:
        opts["zoom"] = None
        print(f"（{len(files)} 个文件，已自动关掉局部 1:1。想看得单独指定某个文件）")

    loaded = [(f, load(f, W, H)) for f in files]
    for path, data in loaded:
        show(path, data, W, H, opts)

    if opts["png"]:
        if len(loaded) != 1:
            sys.exit("--png 只能配合单个文件用")
        size = save_png(opts["png"], make_reader(loaded[0][1], W), W, H, opts["scale"])
        print(f"\n✓ 已存 {opts['png']}   {size[0]}×{size[1]}（{W}×{H} 放大 {opts['scale']} 倍）")
        print("  用图片查看器打开，放大看就是每个像素。⚠️ 别让查看器做平滑缩放，"
              "那样会插出灰的过渡像素。")

    if opts["diff"]:
        if len(loaded) != 1:
            sys.exit("--diff 只能配合单个文件用")
        diff(loaded[0][0], loaded[0][1], opts["diff"], load(opts["diff"], W, H), W, H)


if __name__ == "__main__":
    main()
