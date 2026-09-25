#!/usr/bin/env python3
"""
把照片转成 RLCD 屏能显示的 1bpp 图，生成 photo_slideshow Demo 要的 .bin 文件

用法：
    python3 tools/img2slides.py 我的照片/ -o 输出目录

    -o 是**必须的**，没有默认值。这是个通用工具，不把某个 Demo 的路径写死当默认；
    而且一次会生成几十上百个 15000 字节的文件，悄悄倒进当前目录也不是个事。

    在 photo_slideshow Demo 里：
        python3 tools/img2slides.py 我的照片/ -o arduino/photo_slideshow/data
        ./build.sh slides photo_slideshow

常用参数（完整说明见根 README 的「五、图片转黑白点阵」）：
    --crop x,y,w,h   先裁到这块再缩放。**对画质影响最大的一个参数** ——
                     屏幕只有 400×300，主体占多少像素直接决定它有没有细节
    --contrast N     对比度，默认 0.7。越大网点感越弱，太大脸会糊
    --gamma-floor N  自动提亮的下限，默认 0.8。暗背景的照片调大能保住主体细节
    --gamma-ceil N   自动压暗的上限，默认 1.0（只提亮不压暗）。0 = 不限制
    --gamma N        手动指定亮度曲线（越小越亮），默认自动
    --dither NAME    floyd（默认）/ atkinson / bayer / threshold
    --invert         黑白对调
    --fit contain    整张完整放下（留白），默认 crop 是填满并裁掉多余
    -o DIR           输出目录（**必须传**）    -q  不打印预览

屏幕只有黑白两色，一张普通照片不能直接显示。中间必须做**抖动（dither）**：
用黑白点的疏密去模拟灰阶。这套流程（缩放 → 灰度 → 亮度归中 → 抖动 → 打包）
是业界标准做法，image2cpp / epaper-image-convert / didder 都是这么干的，
只是它们面向墨水屏，我们这块是反射式 LCD。

为什么自己写一个（而不是用现成工具）：
  · 现成工具大多输出 C 数组，我们要的是能塞进 LittleFS 的裸文件
  · 亮度归中这一步（见下面「最关键的一步」）现成工具基本都不做
  · 要能在终端直接预览 —— 抖动结果不预览根本判断不了好坏

最关键的一步是**亮度归中**，不是抖动算法本身：

    实测一张偏暗的照片（中位数 83/255），转 400×300 后抖动：
      什么都不做                      黑点 63.8%   发闷
      ImageOps.autocontrast           黑点 66.5%   更黑了 ← 反直觉
      autocontrast + gamma 归中       黑点 47.4%   层次清楚，五官可辨

  autocontrast 只把最暗/最亮拉到端点，**不移动中位数**，所以对整体偏暗或偏亮的
  照片无效，甚至更糟。1bpp 抖动的理想状态是图像中位数落在 128 附近
  （黑白各占一半，误差扩散才有余量）。做法是解 median^γ = 0.5 求出 gamma 再映射。

调完亮度还要调**对比度**（--contrast，默认 0.7）：
  抖动出来的「灰」其实是黑白点的疏密，中间调越多，画面上「麻麻的网点」就越多，
  看着越平。S 曲线把中间调推向两端，更多区域变成接近纯黑/纯白，网点感就弱了。
  实测同一张照片，「接近纯黑/纯白」的区域占比：对比度 0 → 10%，0.5 → 21%，1.0 → 32%。
  曲线绕中点对称，不会把刚调好的中位数带偏（黑点占比恒定），所以放在亮度归中之后。

  ⚠️ **这个值因图而异，别指望一个默认值通吃。** 1.0 在一张暗背景人像上最好看，
     换一张有面部细节的就把中间调压没了（真机上踩过）。0.7 是个折中，
     嫌太硬往下调、嫌发灰往上加。

字节序的坑（和 tools/gen_cn_font.py 是同一个坑，那边被真机教育过）：
  U8g2 的 XBM 是 **LSB 优先** —— 一个字节里 bit0 是最左边的像素。
  按直觉的 0x80 >> i 写会左右镜像。所以打包时用 `1 << i`，别改。
  证据：U8g2 源码 src/clib/u8g2_bitmap.c 的 u8g2_DrawHXBM()
  里是 `mask = 1; ... mask <<= 1;`。

黑白方向的坑（这个是**真机上翻过车**的）：
  直觉会以为「1 = 黑」（毕竟墨水屏都这样），但**这块屏是反的** ——
  U8g2 的 bit=1 画出来是**白**，clearBuffer() 之后的 0 才是黑。
  第一版按「黑 = 1」打包，屏幕上出来的是**底片**。
  所以默认按「**亮 = 1**」打包；万一以后换到相反的屏，用 --invert 对调。
"""

import math
import os
import sys

from PIL import Image, ImageOps

# ---------------- 可调参数（也可以从命令行覆盖）----------------
# 输出目录**没有默认值**，必须用 -o 指定。
# 这是个通用工具，不该把某个 Demo 的路径写死当默认 —— 而且一次会生成
# 几十上百个 15000 字节的文件，悄悄倒进当前目录也不是个事。
OUT_DIR = None
WIDTH, HEIGHT = 400, 300                   # 屏幕分辨率（横屏）。改这里要同步改 .ino
DITHER = "floyd"                           # floyd / atkinson / bayer / threshold
NORMALIZE = True                           # 亮度归中（强烈建议开着，见文件开头）
GAMMA = None                               # 手动指定 gamma；None = 自动求解
GAMMA_FLOOR = 0.8                          # 自动提亮的下限，防止暗背景的图被提爆（见 normalize_to_mid）。
                                           # 0.8 是在真机上把 0.5/0.65/0.8/1.0 摆一排，用一张
                                           # 逆光人像（中位数 27/255）挑出来的
GAMMA_CEIL = 1.0                           # 自动压暗的上限。**1.0 = 只许提亮、不许压暗**，理由见
                                           # normalize_to_mid。设 0 表示不限制（恢复旧行为）
CONTRAST = 0.7                             # S 曲线强度，0 = 不调。觉得画面「发灰」就加大。
                                           # ⚠️ 这个值**因图而异**：1.0 在暗背景人像上好看，
                                           #    在需要保留面部层次的图上会把中间调压没。
                                           #    拿不准就偏低，宁可发灰也别丢细节。
INVERT = False                             # 黑白对调。默认「亮=1」是本板的正确方向，
                                           # 见文件开头「黑白方向的坑」
FIT = "crop"                               # crop = 填满并裁掉多余 / contain = 留白不裁
CROP = None                                # (x,y,w,h) 先裁到这块再缩放，见 --crop
PREVIEW = True                             # 在终端打印 ASCII 预览
# -------------------------------------------------------------

EXTS = (".jpg", ".jpeg", ".png", ".bmp", ".tif", ".tiff", ".webp", ".gif")

# 分区表里 littlefs 分区的大小，用来提示还能放多少张（partitions.csv）
PARTITION_BYTES = 0x9E0000

# 终端预览的字符密度梯度：左=白，右=黑
RAMP = " .:-=+*#%@"

# 误差扩散的核。每项是 (dx, dy, 权重)，权重的和 = 除数
KERNELS = {
    # Floyd–Steinberg：最经典，噪点细腻，照片首选
    "floyd": ([(1, 0, 7), (-1, 1, 3), (0, 1, 5), (1, 1, 1)], 16),
    # Atkinson：只扩散 6/8 的误差，丢掉 1/4，对比度更高、噪点更干净，
    # 代价是纯色区域容易「断线」。适合线条画、文字截图
    "atkinson": ([(1, 0, 1), (2, 0, 1), (-1, 1, 1), (0, 1, 1), (1, 1, 1), (0, 2, 1)], 8),
}

# 8×8 Bayer 有序抖动矩阵。有序抖动不扩散误差，而是按固定图案比较阈值，
# 结果是规则的网格纹理（廉价但可预测），适合渐变背景
BAYER8 = [
    [0, 32, 8, 40, 2, 34, 10, 42],
    [48, 16, 56, 24, 50, 18, 58, 26],
    [12, 44, 4, 36, 14, 46, 6, 38],
    [60, 28, 52, 20, 62, 30, 54, 22],
    [3, 35, 11, 43, 1, 33, 9, 41],
    [51, 19, 59, 27, 49, 17, 57, 25],
    [15, 47, 7, 39, 13, 45, 5, 37],
    [63, 31, 55, 23, 61, 29, 53, 21],
]


def median_of(img, box=None):
    """图像的中位数灰度值（0-255）。给了 box 就只统计那块区域。"""
    region = img.crop(box) if box else img
    hist = region.histogram()       # 256 个桶
    half = region.width * region.height // 2
    acc = 0
    for v, n in enumerate(hist):
        acc += n
        if acc >= half:
            return v
    return 128


def normalize_to_mid(img, box=None):
    """亮度归中：解 median^γ = 0.5 求 gamma，让中位数落到 128。

    用 gamma 曲线而不是线性拉伸，是为了同时保住高光和暗部的层次
    —— 线性拉伸会把一端顶死，gamma 不会。

    ⚠️ box 是「照片真正占的那块」。--fit contain 会在四周留白，
       留白必须排除在统计之外 —— 否则一张竖构图的照片放进横向画框后，
       大片白边把中位数拉高，照片本身会被压得偏暗。（实测踩到过。）
       白是 255，gamma 曲线下 255 还是映射到 255，所以 LUT 可以整张图照套，
       留白不会被动。

    ⚠️⚠️ 求出的 γ 会被 **GAMMA_FLOOR 卡住下限**（默认 0.8），不让它提得太狠。
       为什么：中位数是**全图**统计量。遇到「暗背景 + 一小块亮主体」的图
       （比如逆光人像），中位数被大片暗背景拉到很低，于是 γ 很小、
       把**整张图**往白里推 —— 主体那点灰阶被挤进亮部，细节就没了。
       实测一张中位数 27/255 的人像：不限制时 γ=0.31，脸部灰阶数从 244 掉到 151、
       标准差从 66 掉到 49（肉眼就是「脸糊了」）；限制到 0.8 则基本恢复。

       **「中位数应该在 128」不是铁律** —— 它是从一张主体占满画面的照片上总结的。
       主体只占一小块时，忠实还原本来就该是暗的。

    ⚠️⚠️ 求出的 γ 还会被 **GAMMA_CEIL 卡住上限**（默认 1.0），
       也就是**只许提亮、不许压暗**。为什么：

       γ 的符号是有方向的 —— γ<1 提亮（暗图需要），γ>1 压暗（亮图会触发）。
       不加限制时，一张中位数 231 的亮图会算出 γ=7，被硬压成 **50% 黑点的灰糊**，
       原本的内容几乎看不见（实测 macOS 壁纸，黑点 3% → 50%）。

       反射屏的物理是「白纸 + 墨」：**一张本来就亮的图，忠实还原就该是亮的一页**。
       把所有图都压到同一个暗度，就丢掉了「这张图是亮的」这个信息。
       而且统计上亮图比暗图多（18 张壁纸样本里，一半以上自动 γ > 1）。

       亮图真需要更多层次时用 **--contrast** —— 它是对称的，不会移动中位数，
       正是给这种情况准备的。两边各管一件事，互不干扰。
    """
    m = median_of(img, box) / 255.0
    if m <= 0.002 or m >= 0.998:     # 纯黑/纯白图，没得救，原样返回
        return img, 1.0, 1.0
    want = math.log(0.5) / math.log(m)
    g = want
    if GAMMA_FLOOR > 0:
        g = max(g, GAMMA_FLOOR)      # 下限：不许提太亮
    if GAMMA_CEIL > 0:
        g = min(g, GAMMA_CEIL)       # 上限：不许压太暗
    lut = [min(255, int(255 * ((i / 255.0) ** g) + 0.5)) for i in range(256)]
    return img.point(lut), g, want


def contrast_lut(amount):
    """S 曲线查找表：把中间调推向两端，0 和 255 保持不动。

    amount = 0 不变，越大越硬（0.5 差不多相当于常见的「对比度 +50%」）。

    为什么需要它：抖动出来的「灰」其实是黑白点的疏密。中间调越多，
    画面上「麻麻的网点」就越多，看着就越平。S 曲线把中间调压向两端，
    更多区域变成接近纯黑/纯白，网点感就弱了 —— 这就是「对比度更明显」。

    曲线绕中点对称，**中位数不会被改变**，所以可以安全地放在亮度归中之后。
    """
    p = 1.0 + amount
    lut = []
    for i in range(256):
        x = i / 255.0
        if x < 0.5:
            y = 0.5 * (2.0 * x) ** p
        else:
            y = 1.0 - 0.5 * (2.0 * (1.0 - x)) ** p
        lut.append(min(255, max(0, int(y * 255.0 + 0.5))))
    return lut


def error_diffuse(img, name):
    """自己实现误差扩散（Pillow 只自带 Floyd–Steinberg）

    数据用 bytearray 而不是 list：mode "L" 的 tobytes() 就是每像素一字节，
    不用 getdata() 那样一个像素一个 Python 对象，快很多。
    """
    kernel, div = KERNELS[name]
    w, h = img.size
    px = bytearray(img.tobytes())
    for y in range(h):
        for x in range(w):
            i = y * w + x
            old = px[i]
            new = 255 if old >= 128 else 0
            px[i] = new
            err = old - new
            if err == 0:
                continue
            for dx, dy, wt in kernel:
                nx, ny = x + dx, y + dy
                if 0 <= nx < w and 0 <= ny < h:
                    j = ny * w + nx
                    # ⚠️ 必须夹到 0..255：bytearray 赋超出范围的值会直接抛
                    #    ValueError，而不是像 C 那样回绕。误差扩散的累加值
                    #    在纯色区域很容易溢出。
                    #    另外用 int(err*wt/div) 截断（C 的语义），
                    #    不用 //（Python 对负数是向下取整，会有 1 级偏差）。
                    v = px[j] + int(err * wt / div)
                    px[j] = 0 if v < 0 else (255 if v > 255 else v)
    return Image.frombytes("L", (w, h), bytes(px))


def pixels(img):
    """求 img 的可写像素视图

    `img.load()` 在 Pillow 的类型标注里是 `PixelAccess | None`（个别模式下
    确实会返回 None），直接索引会被类型检查器报 Optional —— 这里收窄一次，
    省得每个调用点都写 assert。
    """
    px = img.load()
    assert px is not None
    return px


def bayer_dither(img):
    """有序抖动：拿 Bayer 矩阵当阈值图去比"""
    w, h = img.size
    out = Image.new("L", (w, h))
    src = pixels(img)
    dst = pixels(out)
    for y in range(h):
        row = BAYER8[y & 7]
        for x in range(w):
            # 把 0-63 的矩阵值摊到 0-255 当阈值
            t = (row[x & 7] + 0.5) * 255 / 64
            dst[x, y] = 0 if src[x, y] < t else 255
    return out


def to_1bit(gray, dither):
    """灰度图 -> 纯黑白（0 和 255 两种值）"""
    if dither == "threshold":
        return gray.point(lambda v: 0 if v < 128 else 255)
    if dither == "bayer":
        return bayer_dither(gray)
    if dither in KERNELS:
        return error_diffuse(gray, dither)
    sys.exit(f"不认识的抖动算法：{dither}（可选 {'/'.join(['threshold', 'bayer', *KERNELS])}）")


def fit_image(img):
    """等比缩放到屏幕尺寸。crop = 填满并居中裁切，contain = 完整放下并留白

    返回 (画布, 照片在画布上的区域)。那个区域是给亮度归中用的 ——
    留白不能参与中位数统计，理由见 normalize_to_mid 的注释。
    contain 模式下照片比画布小、四周有白边；crop 模式下照片比画布大，
    只有中间一块露出来，所以区域要跟画布求交集。
    """
    tw, th = WIDTH, HEIGHT
    if FIT == "contain":
        r = min(tw / img.width, th / img.height)
    else:
        r = max(tw / img.width, th / img.height)
    im = img.resize((max(1, round(img.width * r)), max(1, round(img.height * r))),
                    Image.Resampling.LANCZOS)
    left, top = (tw - im.width) // 2, (th - im.height) // 2
    canvas = Image.new("L", (tw, th), 255)          # 留白处是白（屏上不显脏）
    canvas.paste(im, (left, top))
    # 求交集：crop 模式下 left/top 是负的，露出来的只是中间那块
    box = (max(0, left), max(0, top),
           min(tw, left + im.width), min(th, top + im.height))
    return canvas, box


def pack_xbm(img_1bit):
    """打包成 U8g2 XBM：行优先，每行 (W+7)/8 字节，**字节内 LSB 优先**

    ⚠️ `1 << i` 不能改成 `0x80 >> i`，否则每 8 个像素镜像一次，画面会是花的。
       理由见文件开头的「字节序的坑」。
    """
    w, h = img_1bit.size
    px = img_1bit.tobytes()
    out = bytearray()
    for y in range(h):
        base = y * w
        for bx in range(0, w, 8):
            byte = 0
            for i in range(min(8, w - bx)):     # 宽度不是 8 的倍数时最后一节补 0
                is_dark = px[base + bx + i] == 0
                # 默认「亮 = 1」（这块屏 bit=1 画出来是白）；--invert 对调。
                # 推一遍就清楚了：
                #   亮 & 不反转 -> 1    暗 & 不反转 -> 0
                #   暗 & 反转   -> 1    亮 & 反转   -> 0
                if (not is_dark) != INVERT:
                    byte |= 1 << i              # ← LSB 优先，别改成 0x80 >> i
            out.append(byte)
    return bytes(out)


def preview(img_1bit, label):
    """在终端打印 ASCII 密度图。

    把 1bpp 结果降采样成灰度 —— 降采样会把抖动噪点平均掉，
    等价于**人眼在一定距离上的观感**。所以这个预览能真实反映屏幕上好不好看，
    比输出 PNG 再打开看还直观，而且不产生任何文件。
    """
    cols, rows = 96, 26
    d = img_1bit.convert("L").resize((cols, rows), Image.Resampling.BOX)
    px = pixels(d)
    print(f"    {label}（远看大概是这样，@ = 黑）")
    for y in range(rows):
        line = "".join(RAMP[min(9, (255 - px[x, y]) * 10 // 256)] for x in range(cols))
        print(f"    |{line}|")


def parse_args(argv):
    """手写参数解析（和 tools/gen_cn_font.py 一样不依赖 argparse）"""
    global OUT_DIR, DITHER, NORMALIZE, GAMMA, GAMMA_FLOOR, GAMMA_CEIL, CONTRAST
    global INVERT, FIT, PREVIEW, CROP, WIDTH, HEIGHT
    inputs, out = [], None

    def val(i, what):
        """取选项后面那个值，缺了就报错退出"""
        if i + 1 >= len(argv):
            sys.exit(f"{argv[i]} 后面要跟{what}")
        return argv[i + 1]

    i = 0
    while i < len(argv):
        a = argv[i]
        if a in ("-h", "--help"):
            sys.exit(__doc__)
        elif a == "-o":
            out = val(i, "输出目录")
            i += 1
        elif a == "--dither":
            DITHER = val(i, "算法名")
            i += 1
        elif a == "--size":
            try:
                WIDTH, HEIGHT = (int(v) for v in val(i, "宽x高").lower().split("x"))
            except (ValueError, AssertionError):
                sys.exit("--size 格式是 宽x高，比如 400x300")
            i += 1
        elif a == "--gamma":
            try:
                GAMMA = float(val(i, "数值"))
            except ValueError:
                sys.exit("--gamma 要跟一个数字，比如 --gamma 0.6")
            i += 1
        elif a == "--gamma-floor":
            try:
                GAMMA_FLOOR = float(val(i, "数值"))
            except ValueError:
                sys.exit("--gamma-floor 要跟一个数字。0.5 = 默认；1.0 = 完全不自动提亮")
            i += 1
        elif a == "--gamma-ceil":
            try:
                GAMMA_CEIL = float(val(i, "数值"))
            except (ValueError, AssertionError):
                sys.exit("--gamma-ceil 要跟一个数字。1.0 = 默认（只提亮不压暗）；0 = 不限制")
            i += 1
        elif a == "--contrast":
            try:
                CONTRAST = float(val(i, "数值"))
            except ValueError:
                sys.exit("--contrast 要跟一个数字，比如 --contrast 0.8")
            i += 1
        elif a == "--fit":
            FIT = val(i, "crop 或 contain")
            i += 1
        elif a == "--crop":
            try:
                CROP = tuple(int(v) for v in val(i, "x,y,w,h").split(","))
                assert len(CROP) == 4 and CROP[2] > 0 and CROP[3] > 0
            except (ValueError, AssertionError):
                sys.exit("--crop 格式是 x,y,w,h（原图像素坐标），比如 --crop 500,400,600,450")
            i += 1
        elif a == "--no-normalize":
            NORMALIZE = False
        elif a == "--invert":
            INVERT = True
        elif a in ("-q", "--quiet"):
            PREVIEW = False
        elif a.startswith("-"):
            sys.exit(f"不认识的参数：{a}\n\n{__doc__}")
        else:
            inputs.append(a)
        i += 1

    if not inputs:
        sys.exit(__doc__)
    if not out:
        sys.exit("必须用 -o 指定输出目录（没有默认值）。比如：\n"
                 "    python3 tools/img2slides.py 我的照片 -o 输出目录\n"
                 "  在 photo_slideshow Demo 里就是：\n"
                 "    python3 tools/img2slides.py 我的照片 -o arduino/photo_slideshow/data")
    OUT_DIR = out
    if GAMMA_FLOOR > 0 and GAMMA_CEIL > 0 and GAMMA_FLOOR > GAMMA_CEIL:
        sys.exit(f"--gamma-floor({GAMMA_FLOOR}) 不能大于 --gamma-ceil({GAMMA_CEIL})")
    if FIT not in ("crop", "contain"):
        sys.exit(f"--fit 只认 crop 或 contain，给的是 {FIT}")

    # 输入可以是目录（扫里面的图），也可以是一个个文件
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
        sys.exit(f"{'、'.join(inputs)} 里没找到图片（认这些后缀：{' '.join(EXTS)}）")
    return files


def main():
    files = parse_args(sys.argv[1:])
    out_dir = OUT_DIR
    assert out_dir is not None       # parse_args 已经保证：没给 -o 就 sys.exit 了
    os.makedirs(out_dir, exist_ok=True)

    # 清掉上次的产物。只删自己生成的 slide_*.bin，不碰目录里别的东西。
    # 不清的话，删掉的照片会以旧 .bin 的形式继续在屏幕上轮播。
    stale = [f for f in os.listdir(out_dir)
             if f.startswith("slide_") and f.endswith(".bin")]
    for f in stale:
        os.remove(os.path.join(out_dir, f))
    if stale:
        print(f"  清掉上次生成的 {len(stale)} 个 .bin")

    frame_bytes = ((WIDTH + 7) // 8) * HEIGHT
    print(f"{WIDTH}×{HEIGHT}  1bpp  抖动={DITHER}"
          f"{'  +亮度归中' if NORMALIZE else '  (未归中)'}"
          f"{'  反转' if INVERT else ''}   每张 {frame_bytes} 字节\n")

    total = 0
    for idx, path in enumerate(files):
        name = os.path.basename(path)
        try:
            src = Image.open(path)
            src.load()
        except Exception as e:
            print(f"  ⚠️ 跳过 {name}：打不开（{e}）")
            continue

        # EXIF 方向修正 —— 手机竖着拍的照片不修就是躺倒的
        gray = ImageOps.exif_transpose(src).convert("L")

        # 先裁到关心的那块。**这一步对画质的影响比任何参数都大**：
        # 屏幕只有 400×300，主体在画面里占多少像素，直接决定它有没有细节。
        # 一张 1440×1440 的全身照，脸只占 165px，缩到屏上就剩 46px —— 什么算法都救不回来。
        if CROP:
            x, y, w, h = CROP
            x = max(0, min(x, gray.width - 1))
            y = max(0, min(y, gray.height - 1))
            w = min(w, gray.width - x)
            h = min(h, gray.height - y)
            if (x, y, w, h) != tuple(CROP):
                print(f"  ⚠️ {name} 的裁切框超出原图（{gray.width}×{gray.height}），已收窄到 "
                      f"{x},{y},{w},{h}")
            gray = gray.crop((x, y, x + w, y + h))

        canvas, box = fit_image(gray)

        g_used = g_want = None
        if NORMALIZE:
            # box = 照片真正占的区域，留白不参与统计（--fit contain 时才不是整张）
            canvas, g_used, g_want = normalize_to_mid(canvas, box)
        if GAMMA is not None:
            lut = [min(255, int(255 * ((i / 255.0) ** GAMMA) + 0.5)) for i in range(256)]
            canvas = canvas.point(lut)

        # 对比度放在亮度归中之后：S 曲线绕中点对称，不会把刚调好的中位数带偏
        if CONTRAST > 0:
            canvas = canvas.point(contrast_lut(CONTRAST))

        bits = to_1bit(canvas, DITHER)
        data = pack_xbm(bits)

        out_path = os.path.join(out_dir, f"slide_{idx:03d}.bin")
        with open(out_path, "wb") as f:
            f.write(data)
        total += len(data)

        ink = bits.histogram()[0] / (WIDTH * HEIGHT)   # 灰度 0 的像素数 = 黑点
        if not g_used:
            note = ""
        elif g_want and g_want < g_used - 0.005:
            # 下限生效：图很暗，本来要提亮很多，被拦住了。
            # 提醒一句，因为「画面比预期暗」这时候是**故意的**，不是 bug。
            note = f"γ={g_used:.2f}（已限幅，本来要 {g_want:.2f}；想更亮就 --gamma-floor 调小）"
        elif g_want and g_want > g_used + 0.005:
            # 上限生效：图偏亮，本来要压暗，被拦住了 → 画面会比「归中」亮。
            # 这也是故意的：亮图忠实还原就该是亮的一页，见 normalize_to_mid。
            note = f"γ={g_used:.2f}（图偏亮，本来要压到 {g_want:.2f}，已按「只提亮不压暗」放过）"
        else:
            note = f"γ={g_used:.2f}"
        print(f"✓ slide_{idx:03d}.bin  ← {name}   黑点 {ink * 100:4.1f}%  {note}")
        if PREVIEW:
            preview(bits, f"{name}")

    if not total:
        sys.exit("\n一张都没转成功")

    print(f"\n✓ 共 {len(os.listdir(out_dir))} 张，{total} 字节（{total / 1024:.0f} KB）")
    print(f"  分区还能放约 {PARTITION_BYTES // frame_bytes} 张，已用 {total * 100 / PARTITION_BYTES:.1f}%")
    print(f"\n下一步： ./build.sh slides photo_slideshow")


if __name__ == "__main__":
    main()
