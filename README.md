# 微雪 ESP32-S3-RLCD-4.2 Demo 合集

**入门手册**，写给第一次接触这块板子的人：这里有什么、怎么从零把环境配起来、
怎么编译烧录，以及想在屏上显示中文该怎么做。

每个 Demo 一个目录，都能**独立编译烧录**，不依赖仓库中其它 Demo。

## 硬件配置

| 部件 | 型号 |
|---|---|
| 开发板 | [微雪 ESP32-S3-RLCD-4.2](https://www.waveshare.net/shop/ESP32-S3-RLCD-4.2.htm) |
| 主控 | ESP32-S3-WROOM-1-**N16R8**（16 MB Flash / 8 MB PSRAM） |
| 屏幕 | **ST7305** 驱动，4.2" 反射式单色 LCD，**300×400**，**1 bit/像素** |
| 其它板载 | ES8311 + ES7210 音频（双麦 + 喇叭）、PCF85063 RTC、SHTC3 温湿度、TF 卡槽、18650 电池座 |

除了板子本身，还需要一根 **USB-C 数据线**（有些线只供电不传数据，插上电脑认不到串口时先换线）。

**动手前先知道屏幕的三个脾气**，不然会以为是故障：

1. **只有黑白，没有灰度** —— 1bpp，整屏显存 15000 字节。所以字体是点阵直出，没有抗锯齿。
2. **没有背光，靠环境光照亮** —— **暗处看不清是物理特性，不是坏了**；太阳底下反而最清楚。
3. **刷新率 27 Hz**（实测，见 [`panel_rate`](arduino/panel_rate/README.md)）——
   比墨水屏快得多，**能做动画**，但谈不上丝滑。两个容易混的点：
   ⚠️ 官方 Demo 串口那个「80 fps」**不是刷新率**，是 MCU 每秒能推几帧；
   ⚠️ MCU 推得比面板快 2.6 倍，**多推的白推，画得再快也不会更流畅**。

引脚是**硬件焊死的，改不了**：

```
屏幕 SPI:   SCK=GPIO11   MOSI=GPIO12   DC=GPIO5   CS=GPIO40   RST=GPIO41   TE=GPIO6
I2C:        SDA=GPIO13   SCL=GPIO14
按键:       GPIO0=BOOT   GPIO18=KEY      电池 ADC: GPIO4
```

> 屏幕只用了 SCK + MOSI，**没有 MISO** —— 它只接收不回报。

## 仓库目录结构

```
waveshare-demo/
├── README.md                    ← 本文件
├── Notes.md                     ← 从零摸索的过程记录（踩坑、概念科普、"为什么这么选"）
├── build.sh                     ← 编译 / 烧录 / 串口 一键脚本（FQBN 在这里）
├── arduino/                     ← 六个 Demo，各有自己的 README（见下表）
│   ├── 10_U8G2_Test/
│   ├── rlcd_stock_demo/
│   ├── net_diag/
│   ├── api_probe/
│   ├── photo_slideshow/
│   └── panel_rate/
├── tools/
│   ├── gen_cn_font.py           ← 扫描源码里的汉字，生成点阵字库（见「四」）
│   ├── img2slides.py            ← 照片转成屏幕能显示的黑白图（见「五」）
│   └── show_slide.py            ← 在终端里查看转好的 .bin，不用烧录（见「五」）
├── LICENSE                      ← MIT（本仓库原创代码）
├── LICENSE-Apache-2.0.txt       ← 屏幕驱动文件的许可
└── NOTICE                       ← 第三方组件声明
```

## 一、这里有什么

| Demo | 干什么 |
|---|---|
| [`10_U8G2_Test`](arduino/10_U8G2_Test/README.md) | 官方原版 Demo，**未作任何修改**。屏幕上跑一个计数器。**屏幕不亮时拿它排查硬件** |
| [`net_diag`](arduino/net_diag/README.md) | 网络诊断工具：WiFi → DNS → TCP → HTTP → HTTPS，哪层挂了报哪层 |
| [`api_probe`](arduino/api_probe/README.md) | 网络诊断工具：批量测一批 API 在板子上通不通，打表输出 |
| [`rlcd_stock_demo`](arduino/rlcd_stock_demo/README.md) | **A 股行情看板**：拉腾讯财经的行情 + 60 日 K 线，画到反射屏上 |
| [`photo_slideshow`](arduino/photo_slideshow/README.md) | **照片幻灯片**：把已有照片转成 1bpp 轮流显示。不用联网，图片存在板子里 |
| [`panel_rate`](arduino/panel_rate/README.md) | 硬件诊断工具：读 TE 引脚量出**面板真实刷新率**，和 MCU 推帧率分开看 |

**建议的阅读顺序**：`10_U8G2_Test` → `rlcd_stock_demo` → `photo_slideshow`。
**给 Demo 换新接口之前先跑 `api_probe`** —— 这块板子没有代理，接口通不通必须在**板子上**实测，
原因见 [`Notes.md`](Notes.md) §5.3。两个联网诊断工具：

```bash
./build.sh flash net_diag     # 分层诊断：WiFi→DNS→TCP→HTTP→HTTPS，哪层挂报哪层
./build.sh flash api_probe    # 批量测 API 可达性（先改代码里的 PROBES 表）
```

每个 Demo 目录里有各自的 README，写它是什么、改哪里、踩过什么坑。
[`tools/`](tools/) 下有三个脚本：要在屏上显示汉字就用**中文字库生成器**（见「四」）；
要把**照片显示到屏上**用 `img2slides.py`，调参数时先用 `show_slide.py` 在终端看一眼再烧（见「五」）。

## 二、从零配置环境

全程命令行，工具是 [arduino-cli](https://arduino.github.io/arduino-cli/)。

### 1. 装 arduino-cli

```bash
brew install arduino-cli                # 本文档写作时是 1.5.1
```

### 2. 加开发板源，装 ESP32 核心

```bash
arduino-cli config init
arduino-cli config add board_manager.additional_urls \
  https://espressif.github.io/arduino-esp32/package_esp32_index.json
arduino-cli core update-index
arduino-cli core install esp32:esp32@3.3.12
```
> ⚠️ **这一步要下 1.7 GB，是全程最慢的一步。** arduino-cli 自带的下载器实测只有
> 4 MB/分钟（同一个 URL `curl` 直连有 20 MB/s，差 300 倍），所以别指望等一等就好了。
> **加速办法见 [`Notes.md`](Notes.md) §5.1** —— 一句话：用 `curl` 从镜像自己下，
> 塞进 `~/Library/Arduino15/staging/packages/`，arduino-cli 认出后会直接解压。

### 3. 装依赖库

```bash
arduino-cli lib install U8g2            # 屏幕图形库（本文档写作时 2.36.19）
arduino-cli lib install "ArduinoJson"   # 解析接口返回的 JSON，必须 v7.x
```

各 Demo 用到的库不完全一样，看它自己 README 里列的那几个。

### 4. 填 WiFi 凭据（只有联网 Demo 需要）

密码**不在仓库里**（`.gitignore` 挡着）。每个联网 Demo 目录下有一份模板，用到哪个复制哪个：

```bash
cd arduino/rlcd_stock_demo
cp secrets.h.example secrets.h
```

```cpp
#define WIFI_SSID "你的WiFi名"
#define WIFI_PASS "你的WiFi密码"
```

> 为什么不放一份共享的？Arduino 编译时会把 sketch 目录整个复制到临时目录，
> `#include "../secrets.h"` 这种跨目录引用会失败，所以每个 sketch 必须自带一份。

目前有 3 个联网 Demo（`rlcd_stock_demo` / `net_diag` / `api_probe`），**换网络时三处都要改**。

> ⚠️ **板子只支持 2.4 GHz —— 这是填 `WIFI_SSID` 之前第一件要确认的事。**
> 5G 专属的 SSID 根本连不上（双频合一、2.4G/5G 同名的通常没问题）。
> 记不住别的，也请记住这一条：连不上时先查它在不在 2.4G，再怀疑代码。

**连不上 WiFi 时，按这个顺序排除**：

1. **拿手机开个热点试**（iPhone 记得开「最大兼容性」，否则它默认只发 5G）。
   热点能连上就说明板子和代码都没问题，是路由器那边的配置 ——
   也顺便验证了上面那条 ⚠️ 和下面两条都不是原因。
2. **网页认证的网络**（校园网 / 酒店）连不上 —— 那要在浏览器里登录，`WiFi.begin()` 绕不过去。
   典型症状是板子显示连上、也拿到了 IP，但所有请求都超时。
3. **WPA2-Enterprise**（要输账号密码的，公司/校园常见）连不上，需要另外的 API。

分层定位挂在哪一步用 [`net_diag`](arduino/net_diag/README.md) —— 它的第 0 层会直接报
「找不到这个 SSID（名字写错？或者是 5G 专用网络？）」。



## 三、编译烧录

### 1. 开发板配置（已经配好了，不用你动）

FQBN 是「给哪块板子、按什么配置编译」的完整描述。**`build.sh` 顶部已经写死了本板正确的值**：

```
esp32:esp32:esp32s3:CDCOnBoot=cdc,FlashMode=qio,FlashSize=16M,PartitionScheme=app3M_fat9M_16MB,PSRAM=opi,UploadSpeed=921600
```

| 选项 | 值 | 作用 |
|---|---|---|
| **`PSRAM=opi`** | OPI PSRAM | ⚠️ **头号翻车点，见下** |
| **`CDCOnBoot=cdc`** | Enabled | 让 `Serial` 走原生 USB。**不设 → 串口监视器一片空白** |
| `FlashMode=qio` | QIO 80MHz | |
| `FlashSize=16M` | 16MB (128Mb) | 板子是 N16 |
| `PartitionScheme=app3M_fat9M_16MB` | 3MB APP / 9.9MB FATFS | 程序最大 3MB |
| `UploadSpeed=921600` | 921600 | |

> ⚠️ **`PSRAM=opi` 是最常见的翻车点。** 板子是 N16R8（八线 PSRAM），PSRAM 在
> **二级 bootloader 阶段**初始化，这个选项决定 bootloader 打不打开外部内存控制器。
> 选成 `QSPI` 或 `Disabled`，申请帧缓冲直接返回 `NULL` → 崩溃 → 重启 → 再崩溃，
> 表现为**一上电就无限重启**，而且**串口报错完全不会提示是这里的问题**。

> ⚠️ **别信 `arduino-cli board list` 自动检测出来的板子。** ESP32-S3 的原生 USB
> 不报告自己是哪块具体板子，工具只能猜成通用的 `esp32_family`（PSRAM 默认关、Flash 默认 4MB），
> 照它编译就是上面那个无限重启。**FQBN 必须手动写全**，展开看 [`Notes.md`](Notes.md) §4.1。

### 2. 一键脚本

```bash
./build.sh flash rlcd_stock_demo   # 编译 + 烧录（传 arduino/ 下的目录名）
./build.sh compile rlcd_stock_demo # 只编译，不烧
./build.sh upload rlcd_stock_demo  # 只烧录（用上次的产物，不重新编译）
./build.sh slides photo_slideshow  # 把 Demo data/ 里的文件传进板子的文件系统
./build.sh mon                     # 开串口监视器（115200，Ctrl+C 退出）
./build.sh ports                   # 列出候选串口
./build.sh help                    # 全部用法
```

`slides` 是给 `photo_slideshow` 用的：把 `arduino/<Demo>/data/` 打包成 LittleFS 镜像，
写进那个 Demo `partitions.csv` 里定义的分区（**偏移量是从分区表里读的，不在脚本里写死**）。

不传 Demo 名时默认编译 `10_U8G2_Test`。**第一次请先烧它**：屏幕上出现跳动的计数器，
说明环境和板子都没问题 —— **先用它把环境验掉，再动自己的代码**，
否则环境问题和代码问题混在一起排查是新手地狱。（背景见 [`Notes.md`](Notes.md) §2）

### 3. 不用脚本的话

```bash
FQBN="esp32:esp32:esp32s3:CDCOnBoot=cdc,FlashMode=qio,FlashSize=16M,PartitionScheme=app3M_fat9M_16MB,PSRAM=opi,UploadSpeed=921600"
SKETCH=arduino/10_U8G2_Test

arduino-cli compile -b "$FQBN" "$SKETCH"
arduino-cli upload  -b "$FQBN" -p /dev/cu.usbmodem101 "$SKETCH"
arduino-cli monitor -p /dev/cu.usbmodem101 -c baudrate=115200
```

> **硬性规则：`.ino` 所在的文件夹名必须和 `.ino` 文件名一致**，
> 改名时两个要一起改，否则编译失败。

## 四、中文点阵字库

U8g2 内置字体**不含中文**，想在屏上显示汉字，得自己造点阵字库 ——
[`tools/gen_cn_font.py`](tools/gen_cn_font.py) 干这件事。

```bash
pip3 install pillow                                     # 依赖，只需装一次
python3 tools/gen_cn_font.py arduino/rlcd_stock_demo    # 扫描该目录，生成 cn_font_data.h
```

它做三件事：**扫描**目标目录下所有 `.ino` / `.h` / `.cpp` 里出现的汉字（注释里的不算）
→ 用系统字体（Hiragino Sans GB）**栅格化**成 1bpp 点阵 → 写出 `cn_font_data.h`。

**所以不用维护字符清单**，改了文案把脚本重跑一遍就行。生成的字库是**自包含**的
（不读文件系统、不用 GBK 转码），而且只收真正用到的字 ——
`rlcd_stock_demo` 一共 52 个汉字，才 **1.8 KB**。

> ⚠️ **改了 `.ino` 里的中文文案，必须重跑这个脚本**，否则新字在屏幕上显示成**空心框**。
> （空心框是故意设计的 —— 一眼能看出是漏生成，而不是盯着空白怀疑屏幕坏了。）

> ⚠️ **脚本目前写死了 macOS 的系统字体**，换平台要改开头的 `FONT_CANDIDATES` 列表。

只有**要在屏上画汉字**的 Demo 需要它：`rlcd_stock_demo` 和 `photo_slideshow`
（后者只在「没找到图片」这类提示上画中文）；`10_U8G2_Test` 不画中文；
`net_diag` / `api_probe` 的中文只打到**串口**（串口不需要字库）。

### 怎么用

生成的字库配合同目录的 `cn_text.h` 使用。它提供两个和 U8g2 原生对应的函数，
**ASCII 走 U8g2 字体、汉字走点阵表，共享同一条基线**，所以混排不会一高一低：

```cpp
g->drawStr(x, y, s)    ->   drawText(g, x, y, s)
g->getStrWidth(s)      ->   textWidth(g, s)
```

`setFont()` 仍然有效，管的是 ASCII 那部分。

> `cn_font_data.h` 是**自动生成的，不要手改** —— 下次重跑脚本就冲掉了。

### 想改字号

改 `tools/gen_cn_font.py` 顶部的 `SIZE`，重跑生成脚本。
**但字变高会串行，要同步调 `.ino` 里的 y 坐标。** 当前 `SIZE=16`，
生成出来的窗口是 16×18、基线偏移 15（汉字有下伸笔画，所以比 16 高）。

### ⚠️ 踩过的坑：字左右镜像

第一版生成出来，屏幕上的汉字是**左右反的**。原因：

> **U8g2 的 XBM 格式和标准 XBM 相反。** 标准 XBM 是 MSB 优先（`0x80 >> i`），
> U8g2 是 **LSB 优先**（`1 << i`，bit 0 = 最左像素）。

证据在 U8g2 源码 `src/clib/u8g2_bitmap.c` 的 `u8g2_DrawHXBM()`：

```c
mask = 1;
while(len > 0) {
  uint8_t current_bit = (*b) & mask;
  ...
  mask <<= 1;          // ← 左移，所以 bit0 是最左像素
}
```

写错的话，**每 8 个像素各自镜像一次** —— 16px 宽的汉字被切成两段 8px，
每段各反一次，看起来就是"字反了"。行优先这点和标准 XBM 一致，不用改。



## 五、图片转黑白点阵（通用工具）

屏幕**只有黑白两色、没有灰度**，普通照片必须先转成 1bpp 才能显示。配两个工具：

| 工具 | 干什么 |
|---|---|
| [`img2slides.py`](tools/img2slides.py) | 照片 → 1bpp 点阵文件。缩放、裁切、调亮度、**抖动**、打包 |
| [`show_slide.py`](tools/show_slide.py) | **不烧录也能看**：把点阵文件在终端渲染出来，或存成 PNG |

它们输出的是**裸 XBM**（行优先、字节内 LSB 优先，无文件头、无压缩、无元数据），
**不绑定这块板子** —— 任何 1bpp 屏（OLED / 墨水屏 / 反射 LCD）都能用，
换个尺寸改 `--size` 就行。

**XBM 是什么** —— X11 时代的黑白位图格式（X **B**it**M**ap）。它最特别的一点是
**格式本身就是一段 C 代码**，长这样：

```c
#define logo_width 400
#define logo_height 300
static const unsigned char logo_bits[] = { 0x3C, 0x42, 0xFF, ... };
```

把最后那行数组的花括号和逗号去掉、按字节顺序写进文件，就是我们生成的 `.bin`。
它比正常的 XBM 还要彻底 —— 连上面那两个 `#define` 都不要，所以叫「**裸**」XBM。
尺寸不写在文件里，得在**用**的时候给：板上是
`u8g2->drawXBM(0, 0, LCD_WIDTH, LCD_HEIGHT, slide_buf)`（宽高当参数传，
见 [`photo_slideshow.ino`](arduino/photo_slideshow/photo_slideshow.ino)）；
在终端看则是 [`show_slide.py`](tools/show_slide.py) 的 `--size`（默认 400×300）。

> ⚠️ **别拿"标准 XBM"的印象去推这个格式。** 标准 XBM 字节内是 **MSB 优先**（`0x80 >> i`），
> 而 U8g2 要的是 **LSB 优先**（`1 << i`，bit0 = 最左像素）—— 同一个字节里左右是反的。
> 本仓库的工具**统一按 U8g2 的方向**打包；自己写解码时要跟这一致，
> 否则整张图会左右镜像（生成中文字库时踩过同源的坑，见「四」）。

```bash
pip3 install pillow      # 依赖，只需装一次
```

### `img2slides.py` —— 照片转 1bpp

```bash
python3 tools/img2slides.py <图片或目录> -o <输出目录> [参数]
```

输入可以是目录（扫里面的图）也可以是若干文件。输出 `slide_000.bin`、`slide_001.bin`…
**按文件名排序编号，这就是播放顺序**。每次运行会先清掉上次生成的 `slide_*.bin`。

| 参数 | 默认 | 作用 |
|---|---|---|
| `--crop x,y,w,h` | — | **先裁到这块再缩放。对画质影响最大的一个参数** —— 见下面「调参心法」 |
| `--contrast N` | 0.7 | 对比度 S 曲线。越大网点感越弱，**太大脸会糊**。**因图而异** |
| `--gamma-floor N` | 0.8 | 自动提亮的下限。暗背景 / 逆光的照片调大能保住主体层次，代价是屏上更暗。`1.0` = 完全不自动提亮 |
| `--gamma-ceil N` | 1.0 | 自动压暗的上限。**默认 1.0 = 只许提亮、不许压暗** —— 理由见下面「调参心法」。`0` = 不限制 |
| `--gamma N` | 自动 | 手动亮度曲线，**越小越亮** |
| `--dither NAME` | `floyd` | `floyd`（照片）/ `atkinson`（线条画，对比度高）/ `bayer`（规则网格纹）/ `threshold`（不抖动，仅纯文字图） |
| `--invert` | 关 | 黑白对调。**默认方向就是本板正确的，正常不用加** |
| `--fit contain` | `crop` | `contain` = 整张完整放下（留白）；`crop` = 填满并裁掉多余 |
| `--no-normalize` | 关 | 关掉亮度归中。不建议 |
| `-o DIR` | **必须传** | 输出目录。没有默认值 —— 这是个通用工具，不该把某个 Demo 的路径写死 |
| `-q` | 关 | 不打印终端预览 |

### `show_slide.py` —— 不烧录先看效果

调 `--gamma` / `--contrast` 这类参数时，如果靠「转图 → 烧录 → 盯屏幕」来试，
烧一次 25 秒，来回试很慢。这个脚本直接读 `img2slides.py` 生成的 `.bin`，
在终端就把画面渲染出来 —— **不碰板子，改完参数立刻能看**。

```bash
python3 tools/show_slide.py data/slide_000.bin                        # 整图 + 局部 1:1
python3 tools/show_slide.py data/slide_000.bin --no-full --zoom 150,140,100,60
python3 tools/show_slide.py 新.bin --diff 旧.bin                       # 改了参数动了多少
python3 tools/show_slide.py data/slide_000.bin --png /tmp/look.png     # 存 PNG 看完整 1:1
```

| 参数 | 默认 | 作用 |
|---|---|---|
| `-w N` | 100 | 整图渲染宽度（字符数）。**终端窄就调小**，不调会折行糊掉 |
| `--zoom x,y,w,h` | `150,140,100,60` | 局部 1:1 区域。`w` 别超过终端列数 |
| `--no-full` / `--no-zoom` | — | 只画网点 / 只画整图 |
| `--png FILE` | — | **存 PNG —— 看完整 1:1 的唯一办法**（终端一行放不下 400 个字符） |
| `--scale N` | 3 | PNG 放大倍数。用 NEAREST，**不会插出灰色过渡像素** |
| `--size WxH` | `400x300` | 要和转换时一致 |
| `--diff FILE` | — | 和另一个 `.bin` 逐像素比对 |

输出里有两个数：**黑点占比**（全图多少像素黑）和**实心区域**（4×4 块里几乎全黑/全白的比例）。
**实心低 = 到处是中间调 = 屏上「麻麻一片灰」**，正是该加大 `--contrast` 的信号。

### 调参心法

**1. 先看构图，别急着调参数。** 屏幕只有 400×300，**主体占多少像素是决定性的**：

| 处理 | 脸在屏上的宽度 | 效果 |
|---|---|---|
| 1440×1440 照片直接缩 | 46 px | 一团白斑，看不出五官 |
| `--crop` 裁到人 | 94 px | 眼睛、鼻梁、嘴都能看出来 |

**`--crop` 比任何参数都管用**，因为细节不是被算法丢的，是压根没剩几个像素。
嫌算坐标麻烦就先用图片编辑裁好再转。

> ⚠️ **裁切是用户的选择，不是助手替用户做的决定。** 裁哪块 = 改取景，画面里留谁、
> 去掉什么，该由拍照的人定。发现「主体太小」可以自动指出来，**顺手挑个窗口直接转掉不行** ——
> 要把问题摆出来，让用户自己决定裁不裁、裁哪块。
> （亮度、抖动算法这类**不改画面内容**的参数则可以自己调。）

**2. 再调亮度**（`--gamma-floor`）。中位数是**全图**统计量，遇到「暗背景 + 一小块亮主体」
（逆光人像、夜景）会被大片暗背景拉偏，自动提亮会把主体提爆。

> **归一化是单向的：只提亮暗图，不压暗亮图**（`--gamma-ceil` 默认 1.0）。
> 反射屏的物理是「白纸 + 墨」，一张本来就亮的图忠实还原就该是亮的一页；
> 强行压到中位数 128 会把它变成一片 50% 黑点的灰糊（实测一张 231 中位数的亮图：
> 黑点 3% → 50%，内容基本看不见）。亮图要更多层次用下面的 `--contrast`。

**3. 最后调对比度**（`--contrast`）。**这个值因图而异，没有通吃的默认值。**

> **拿不准就把几档参数摆成一排传上去，在屏幕上翻着挑** —— 这比在电脑上猜准得多。
> 不想烧录就用 `--png` 存图看。**能看图就别看指标。**

为什么必须抖动、为什么 `autocontrast` 不够、字节序怎么定的 ——
见 [`Notes.md`](Notes.md) §6.5 / §6.6。

> **完整例子**（转图 → 烧录 → 上传 → 屏幕轮播）见
> [`photo_slideshow/README.md`](arduino/photo_slideshow/README.md)。

## 六、常见问题

**串口监视器一片空白** → `USB CDC On Boot` 要选 `Enabled`（FQBN 里是 `CDCOnBoot=cdc`）。

**一上电就重启** → 十有八九是 PSRAM 选错了，必须是 `PSRAM=opi`。

**WiFi 连不上** → 先跑 [`net_diag`](arduino/net_diag/README.md)，看第 0 层怎么报；
再对照「二、4」那条 2.4 GHz 提醒和排查清单。

**屏幕看不清** → 反射屏没有背光，拿亮处去。这是物理特性。

**幻灯片那个 Demo 说「文件系统没挂上」** → 板子上还是旧分区表。分区表是跟着固件一起烧的，
先 `./build.sh flash photo_slideshow`，再 `./build.sh slides photo_slideshow`。顺序不能反。

**幻灯片里的照片太暗或太亮** → 转换脚本的参数问题，不用改代码：`--gamma 0.6` 调亮
（数字越小越亮）。改完重跑转换和 `build.sh slides` 就行。
**黑白反了**（像底片）→ 那是转换脚本的黑白方向不对。本板的正确方向是「亮 = 1」
（这块屏上 U8g2 的 `bit=1` 画出来是**白**，和墨水屏的直觉相反），脚本已按这个设成默认；
万一变了用 `--invert` 对调。详见 [`Notes.md`](Notes.md) §6.5。

**换了个 API 就连不上** → 先跑 [`api_probe`](arduino/api_probe/README.md)：把新 URL 加进它的
`PROBES` 表，在**板子上**实测。**别用开发机的 `curl` 下结论**（开发机常挂代理，
通不通跟板子的处境无关），原因见 [`Notes.md`](Notes.md) §5.3。

**编译报错说找不到库** → 每个 Demo 需要的库不一样。


## 许可

本仓库**原创代码**采用 [MIT 许可](LICENSE)。

屏幕驱动 `ST7305_U8g2.h` / `ST7305_U8g2.cpp` **不是本仓库写的** —— 取自微雪官方仓库
[waveshareteam/ESP32-S3-RLCD-4.2](https://github.com/waveshareteam/ESP32-S3-RLCD-4.2)
的 `02_Example/Arduino/10_U8G2_Test/`，采用 **Apache License 2.0**，
版权归 Waveshare 及原作者。已核对与上游 `main` 分支逐字节一致、未作修改，
详见 [NOTICE](NOTICE)。

