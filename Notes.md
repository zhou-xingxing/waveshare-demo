# ESP32-S3-RLCD-4.2 上手学习笔记

> 记录时间：2026-09-25
> 目标：从零把微雪 ESP32-S3-RLCD-4.2 开发板跑起来，最终做「联网请求 → 屏幕显示」
> 和「照片 → 屏幕显示」两个 demo
> 笔记范围：从安装 arduino-cli 开始，到跑通行情看板、再到照片幻灯片

> **这份笔记是「为什么」，不是「怎么做」。** 命令和速查都在 [README.md](README.md)，这里不重复。
> 记的是从零摸索的**过程和踩过的坑** —— 包括走错的路（比如为什么最后放弃了 CoinGecko），
> **这些过程比结论本身更值得留**。踩坑记录在 §5，概念科普在 §6。

---


## 1. 硬件：这块板是什么

### 1.1 主控与屏幕

型号表在根 README「硬件配置」，这里只留**笔记里查证过、别处没写的细节**：

- **主控** ESP32-S3-WROOM-1-N16R8：16 MB Flash / 8 MB PSRAM，**双核 LX7 @240MHz**
- **屏幕** ST7305 驱动 IC，4.2" 反射式单色 LCD，300×400，1 bit/像素
  - 整屏显存 = 400×300÷8 = **15000 字节**（1bpp 的直接后果，所以字体只能点阵直出）
  - **刷新率实测 27 Hz** —— 中间推断错过一次，怎么确认的见 §1.1.1
- **其它板载**：ES8311 + ES7210 音频（双麦阵列 + 喇叭，带回声消除）、PCF85063（RTC）、
  SHTC3（温湿度）、TF 卡槽、18650 电池座

### 1.1.1 【实测】面板真实刷新率 = 27 Hz

这块屏的刷新率有三个容易混的数：

| 数字 | 出处 | 实际是什么 |
|---|---|---|
| 约 **80 fps** | 官方计数器 Demo 的串口输出 | **MCU 每秒能推几帧**，纯 SPI 吞吐，不是刷新率 |
| **0.25~32 Hz** | 第三方资料（本条原始记录） | 面板刷新率的范围。**实测 27 Hz 落在区间内 —— 这条原本就是对的** |
| **27 Hz** | 本机实测（TE 引脚） | 面板真实刷新率，见下 |

**中间走过一段弯路，记在这里免得以后再踩**：一开始我根据
[kylehase/ESPHome-ST7305-RLCD](https://github.com/kylehase/ESPHome-ST7305-RLCD)
的源码注释（`0x38` = High Power ≈51 Hz、`0x39` = Low Power ≈1 Hz），
推断微雪驱动发的是 `0x38`，所以应该是 ~51 Hz、原来那条「0.25~32 Hz」是错的。

**实测下来是 27 Hz —— 推断错了，原始记录是对的。** 错在哪：

- 那份 ESPHome 驱动**同时支持好几块不同尺寸的屏**，「≈51 Hz」不是这块 400×300 的数。
- **栅极扫描时间基本固定，帧率 ∝ 1/扫描行数**。反推一下就吻合了：

  | 屏 | 行数 | 帧率 | 行时间 |
  |---|---|---|---|
  | ESPHome 那块小屏 | 200 | ≈51 Hz | 98 µs |
  | 本板 4.2" | 400 | **27 Hz** | 93 µs |

  两者的「行时间」几乎一样（93 vs 98 µs）—— 这说明 27 Hz 是自洽的，
  也反过来印证了 TE **确实是一帧一个脉冲**（要是两个，真值就是 54 Hz，行时间就对不上了）。

> **教训：寄存器含义可以从别的驱动交叉印证，但「这块板的实际数值」只能实测。**
> 我拿一块小屏的参数去推另一块大屏，方向就错了。

**「80 fps」是怎么来的**：`10_U8G2_Test.ino` 里那个 fps 是

```c
uint32_t delta_frames = frames - last_report_frames;
fps_x100 = (uint32_t)((uint64_t)delta_frames * 100000ULL / elapsed_ms);
```

`frames` 每轮 `loop()` 加一，所以它数的是**循环跑得多快**，即「画一帧 + 推一帧」要多久 ——
跟面板实际显示多少帧**完全无关**：你往里推 80 帧，面板可能只显示了其中一部分。

**怎么测的**：TE（Tearing Effect）是面板自己的帧同步输出，接在 **GPIO6**，
**一个脉冲就是一帧**。驱动初始化时其实早就把它打开了（`0x35`，参数 `0x00` = Mode 1，
只输出垂直消隐），只是从来没人读这个脚 —— 所以量到的**就是面板内部的真实扫描节奏**，
不是从 MCU 侧推算的近似值。

`arduino/panel_rate/` 拿中断数一秒有多少脉冲。**实测 TE = 27.0 Hz、MCU 推 69 fps**
（每显示 1 帧，MCU 推了 2.6 帧）。

> 那个程序怎么用、输出长什么样、结果怎么读、对做动画意味着什么 ——
> 都在 [`arduino/panel_rate/README.md`](arduino/panel_rate/README.md)，这里不重复。
>
> 只补一条根 README 里没展开的：27 Hz 差不多是电影（24 fps）的水平，
> **够做动画**，只是谈不上丝滑。真要上动画得**用 TE 同步写入**
> —— TE 这个脚存在的意义就是这个，现在是 MCU 以 69 fps 盲写 27 Hz 的屏，**会撕裂**。

### 1.2 引脚（硬件焊死，不能改）

引脚表同样在根 README「硬件配置」。这里补一条**表里没写的**：

> 屏幕只用了 **SCK + MOSI，没有 MISO** —— 它只接收不回报（write-only）。

---

## 2. 这次从头到尾做了什么

### 阶段 0：确认硬件活着（Bring-up）

**目的：把"硬件坏"和"我的代码错"分开。这一步必须在写代码之前做。**

- [x] 插上 USB-C **数据线**（不是纯充电线）
- [x] 屏幕显示出厂固件的温湿度界面 → 说明上电正常、屏幕能驱动、传感器在工作
- [x] 电脑识别到 `/dev/cu.usbmodem101` → **说明线是数据线，USB 链路通**

> 这一步排除了：板子是坏的、线只能充电、屏幕驱动不了、电源不稳。

### 阶段 1：装工具链 + 跑通官方 Demo

1. 装 `arduino-cli`
2. 配置 Espressif 开发板源 + 更新索引
3. 装 ESP32 核心（**1.7 GB，这次最耗时的一步，见 §5 踩坑**）
4. 装 U8g2 库
5. 拉官方 Demo `10_U8G2_Test`（**原样不改**）到 `arduino/10_U8G2_Test/`
6. 编译 → 烧录 → 屏幕上出现跳动的计数器 ✓

> **为什么"原样不改"很重要**：这一步验证的是**环境**，不是你的代码能力。环境问题（驱动、板子配置、库版本）和代码问题混在一起排查是新手地狱。

---

## 3. 东西都装在哪了

**仓库自己的目录结构见 [README.md](README.md)**，这里只记**装机位置** ——
arduino-cli 把核心、库、编译产物散在下面这些地方，都不在项目目录里：

```
~/Library/Arduino15/                    ← arduino-cli 数据目录（Finder 里默认隐藏）
├── staging/packages/                   ← 【下载缓存】手动塞文件到这里可跳过下载
├── packages/esp32/hardware/esp32/3.3.12/ ← 核心：板子定义 + Arduino API 的 ESP32 实现
└── packages/esp32/tools/               ← 编译器工具链、esptool 等

~/Documents/Arduino/libraries/          ← 第三方库（U8g2 在这里）
~/Library/Caches/arduino/sketches/      ← 编译产物（.bin 在这）
```

> **注意**：核心在 `~/Library/Arduino15/`，库在 `~/Documents/Arduino/libraries/`
> ——**两个不同的地方**，别搞混。（`~/Arduino/` 这个目录**不存在**，别想当然。）
>
> `~/Library` 在 Finder 里默认隐藏。打开方式：`Cmd+Shift+G` 粘贴路径，或终端 `open ~/Library/Arduino15`。

### 3.1 arduino-cli 的两个目录

`arduino-cli` 涉及两个不同的路径概念，各有默认值：

| 概念 | 装什么 | macOS 默认值 | 本机实际 |
|---|---|---|---|
| `directories.data` | 核心、工具链、下载缓存 | `~/Library/Arduino15` | 同左 |
| `directories.user` | 第三方库、你自己的 sketch | `~/Documents/Arduino` | 同左 |

**验证方法**：

```bash
arduino-cli config dump                    # 完整配置
arduino-cli config get directories.user    # 库目录
arduino-cli config get directories.data    # 核心目录
find ~/Documents/Arduino/libraries -maxdepth 1 -iname "U8g2*"   # 库实际在哪
```

### 3.2 顺带搞清楚：核心 和 库 分别是什么

这两个是不同层面的东西，容易混。一句话区别：

> **核心管"这个芯片怎么用"，库管"这个屏幕怎么画"。**

**ESP32 核心**（`esp32:esp32@3.3.12`，Espressif 官方）

让**芯片**能跑 Arduino 代码，提供三样：

1. **板子定义** —— 引脚表、Flash 布局、内存怎么配（`PSRAM=opi` 这类选项就来自这里）
2. **交叉编译器** —— 能在你 Mac 上生成 ESP32 机器码的 gcc（1.7GB 的大头就是它）
3. **Arduino API 的 ESP32 实现** —— `digitalWrite()`、`Serial`、`WiFi`、`SPI` 在 ESP32 上具体怎么工作

> 没它，`.ino` 就是一堆没人认识的文本。

**U8g2 库**（`U8g2@2.36.19`，第三方，作者 Oliver Kraus）

让**屏幕**能画东西，也三样：

1. **画图函数** —— `drawStr` 画字、`drawLine` 画线、`drawBox` 画框
2. **字体** —— 内置几百种点阵字体
3. **缓冲管理** —— `clearBuffer` 清屏、`sendBuffer` 推给屏幕

> 它不管屏幕是 ST7305 还是别的，只提供"画"的能力。

| | ESP32 核心 | U8g2 库 |
|---|---|---|
| 管什么 | **芯片** | **屏幕** |
| 换芯片要换吗 | **要** | 不用 |
| 换屏幕要换吗 | 不用 | 不用（但要换最底层驱动） |
| 谁做的 | Espressif（芯片厂） | 第三方个人 |
| 大小 | 1.7 GB | 13 MB |

**举例**：你换块 Arduino Uno 接个 OLED，**同一套 U8g2 照样用**，但核心必须换成 `arduino:avr`。

> **名字由来**：U8g2 = **U**niversal **8**bit **G**raphics Library, **version 2**。
> "通用"指支持几百种屏幕控制器；"8bit"指它面向 8 位单片机那类低资源设备（所以要极省内存）；
> "2" 是因为前身叫 **U8glib**。它下层还有个 **U8x8**（字符限定 8×8 像素格的纯文本模式，
> 不需要缓冲）——你的代码走的是上层 U8g2，因为要画 50 像素高的大数字和边框。

---

## 4. FQBN 与编译烧录

命令本身都在根 README，这里讲**为什么这么配、输出该怎么看**。

### 4.1 FQBN —— 指定板子和编译配置

> **一句话：FQBN 就是告诉 arduino-cli「给哪块板子编译、用什么配置」。**
> 编译和烧录都必须带，否则工具不知道该按哪块板的引脚、内存、分区来生成代码。

**FQBN** = Fully Qualified Board Name（全限定板名）。"全限定"是编程里的通用概念
（对比网络的 FQDN、Java 的全限定类名），意思是**一个不含歧义、能唯一确定目标的完整标识**。

四段用 `:` 分隔：

```
esp32 : esp32 : esp32s3 : CDCOnBoot=cdc,FlashMode=qio,FlashSize=16M,...
└──┬─┘ └──┬──┘ └───┬───┘ └────────────────────┬─────────────────────┘
   ①      ②       ③                            ④
```

| 段 | 叫法 | 你的值 | 含义 |
|---|---|---|---|
| ① | packager 供应商 | `esp32` | 板子定义由谁提供（Espressif 的包 ID） |
| ② | architecture 架构 | `esp32` | 处理器族。AVR 板子是 `avr` |
| ③ | board 板子 | `esp32s3` | 具体哪块板 |
| ④ | options 选项 | 逗号分隔的 `key=value` | 覆盖板子默认配置 |

> **反直觉点**：`esp32:esp32` 前两段同名，不是重复——是**供应商 ID** 和**架构名**
> 碰巧都叫 esp32。对比 `arduino:avr:uno` 就清楚了。

**为什么必须"全限定"——本板就是活例子：**

`arduino-cli board list` 自动检测出的是：

```
/dev/cu.usbmodem101   ESP32 Family Device   esp32:esp32:esp32_family
                                            ^^^^ 这是"猜"的通用项
```

ESP32-S3 的原生 USB **不报告自己是哪块具体板子**，所以工具只能猜个大概。如果照它编译：

| | 自动猜的 | 我们实际用的 |
|---|---|---|
| 板子定义 | `esp32_family`（通用） | `esp32s3`（精确到 S3） |
| PSRAM | **默认 = Disabled** | **`PSRAM=opi`** |
| Flash | 默认 4MB | `16M` |
| 结果 | **上电无限重启** | ✓ 正常工作 |

所以 **FQBN 必须手动写全，不能信自动检测的结果**。

**查 FQBN 的三个命令：**

```bash
arduino-cli board list                              # 检测已连接的板子（只能猜个大概）
arduino-cli board listall                           # 列出所有已知板子及其 FQBN
arduino-cli board details -b esp32:esp32:esp32s3    # 查某块板的所有选项名和可选值
```

**本板可用的完整 FQBN：**（建议存成 `$FQBN` 变量，别每次手打）

```
esp32:esp32:esp32s3:CDCOnBoot=cdc,FlashMode=qio,FlashSize=16M,PartitionScheme=app3M_fat9M_16MB,PSRAM=opi,UploadSpeed=921600
└──┬──┘ └─┬─┘ └──┬──┘ └───────────────────────────┬────────────────────────────────────┘
 供应商   架构   板子                            选项
```

| FQBN 选项 | 选项名 | 值 | 说明 |
|---|---|---|---|
| `CDCOnBoot=cdc` | USB CDC On Boot | Enabled | 让 `Serial` 走原生 USB。**不设的话串口监视器一片空白** |
| `FlashMode=qio` | Flash Mode | QIO 80MHz | |
| `FlashSize=16M` | Flash Size | 16MB (128Mb) | 板子是 N16 |
| `PartitionScheme=app3M_fat9M_16MB` | Partition Scheme | 16M Flash (3MB APP/9.9MB FATFS) | 程序最大 3MB |
| **`PSRAM=opi`** | PSRAM | **OPI PSRAM** | ⚠️ **最关键的一项，见下** |
| `UploadSpeed=921600` | Upload Speed | 921600 | |

> ⚠️ **`PSRAM=opi` 是这块板的第一号翻车点。**
>
> 板子是 **R8**（八线/Octal PSRAM）。PSRAM 是在**二级 bootloader 阶段**初始化的，
> 这个选项决定 bootloader 要不要打开外部内存控制器。
>
> - 选 `PSRAM=disabled`（默认值！）→ bootloader 不初始化 PSRAM，那 8MB 根本不存在
> - 代码里 `heap_caps_malloc(..., MALLOC_CAP_SPIRAM)` 申请外部内存 → 返回 `NULL` → assert 失败 → 崩溃
> - 重启后又走一遍 → **无限重启循环**
> - 报错信息**完全不会提示是这里的问题**
>
> 三个可选值：`disabled` / `enabled`(QSPI) / `opi`(OPI)。**必须选 `opi`**。

### 4.2 编译

命令在根 README「三、编译烧录」（`build.sh` 或手敲 `arduino-cli compile`），这里只记**输出怎么读**。

成功时的输出：

```
Sketch uses 340320 bytes (10%) of program storage space. Maximum is 3145728 bytes.
Global variables use 24528 bytes (7%) of dynamic memory, leaving 303152 bytes for local variables.
```

**读这两行**：程序占 Flash 的 10%（最大 3MB，因为分区表给了 app 3MB）；
全局变量占内存 7%。都还很宽裕。

> **硬性规则：`.ino` 所在的文件夹名必须和 `.ino` 文件名一致。**
> `10_U8G2_Test.ino` 必须在 `10_U8G2_Test/` 里，改了文件名不改文件夹名会编译失败。

### 4.3 烧录

命令同样在根 README。成功时的输出（关键几行）：

```
Wrote 3072 bytes       at 0x00008000    ← 分区表
Wrote 8192 bytes       at 0x0000e000    ← boot_app0（记录从哪个分区启动）
Wrote 340464 bytes     at 0x00010000    ← 你的程序
Hash of data verified.
Hard resetting via RTS pin...
```

**这几个地址值得记住**：`0x0` 是 bootloader、`0x8000` 是分区表、`0x10000` 是你的程序。

> 上面这次的输出里**没有 bootloader 那一行**——因为它内容没变，工具跳过没重写。
> 首次烧录、或换了核心版本时才会看到它。

> **ESP32-S3 不需要手动按 BOOT 键。** 它内置 USB-Serial/JTAG，esptool 通过 USB
> 发特定控制信号就能让芯片自动进下载模式，软件代劳了"按住 BOOT + 点 RST"。
> 老 ESP32 才需要手动按键，这是 S3 最省事的地方。
>
> **兜底方案**：万一自动进不去（比如程序把 USB 搞崩了），手动「按住 BOOT → 点一下 RST → 松开 BOOT」。

### 4.4 看串口输出

**波特率必须和代码里 `Serial.begin()` 一致**（本项目都是 115200），否则全是乱码。
命令就是根 README 里的 `./build.sh mon`。

### 4.5 常用诊断命令

```bash
# 板子在不在
ls /dev/cu.usbmodem*

# 等板子插上（插上后自动打印并退出）
while true; do p=$(ls /dev/cu.usbmodem* 2>/dev/null|head -1); \
  [ -n "$p" ] && { echo "连上了: $p"; break; }; sleep 1; done

# 板子占用情况（比如串口被别的程序占着）
lsof /dev/cu.usbmodem101
```

---

## 5. 踩坑记录

### 5.1 【大坑】装 ESP32 核心慢到无法接受

`esp32:esp32@3.3.12` 共 **18 个文件、约 1.7 GB**。三个层层叠加的问题：

**问题 1：arduino-cli 的下载器本身慢**

同一个 URL，`curl` 直连 **20 MB/s**，arduino-cli 只有 **4 MB/分钟**——差 300 倍。
所以**不要试图优化网络，绕过它的下载器**。

**问题 2：官方镜像不全**

- `xtensa-esp-elf`、`riscv32-esp-elf` 等**编译器工具链**（来自 `github.com/espressif/crosstool-NG`）
  → 走 **`dl.espressif.cn/github_assets/`** 镜像，**16–22 MB/s**，且必须 `--noproxy '*'` 直连
- `esp32-core-*.zip`、`esp32*-libs-*.zip` 等 **arduino-esp32 自己的 release 资产**（约 680 MB）
  → ❌ 上面那个镜像**没有**（404），**连官方"中国版索引"里给的 URL 都是 404**
  → ✅ 改用 `https://gh-proxy.com/https://github.com/...`，**16 MB/s**
  → ⚠️ **gh-proxy 不支持并发**！并行下载会立刻全部失败，必须**串行**

**问题 3：代理干扰**

`dl.espressif.cn` 走 Clash 代理反而慢。需要临时清掉代理变量直连：

```bash
env -u HTTP_PROXY -u HTTPS_PROXY -u ALL_PROXY \
    -u http_proxy -u https_proxy -u all_proxy \
    arduino-cli core install esp32:esp32@3.3.12
```

**解法：自己下载，塞进缓存，让 arduino-cli 只做解压**

关键机制：**arduino-cli 会复用 `~/Library/Arduino15/staging/packages/` 里的文件**
（输出里会打印 `already downloaded`），只要**文件名 + 大小 + SHA-256 都对**。

```bash
# 1. 从索引里读出每个文件应有的 URL / 文件名 / 大小 / SHA-256
python3 -c "
import json,os
d=json.load(open(os.path.expanduser('~/Library/Arduino15/package_esp32_index.json')))
pkg=[p for p in d['packages'] if p['name']=='esp32'][0]
plat=[v for v in pkg['platforms'] if v['version']=='3.3.12'][0]
# 平台核心：plat['url'] 已是完整地址，不要再拼 archiveFileName
# 工具：按 host 里含 'apple-darwin' 挑，注意别选到 'aarch64-linux-gnu'
"

# 2. 用 curl 下载到 staging/packages/，逐个校验 SHA-256
# 3. 再跑 arduino-cli core install，它会打印 "already downloaded" 直接解压
```

**几个脚本层面的坑**（下次写下载脚本会再遇到）：

| 坑 | 说明 |
|---|---|
| `while read` + curl | curl 会抢 stdin，导致循环乱掉 → curl 后加 `</dev/null` |
| macOS 没有 `timeout` | 是 GNU coreutils 的，需要 `brew install coreutils` 用 `gtimeout` |
| JSON 里 `size` 类型不一 | 有的是 int 有的是 str，比较前要转换 |
| Python 生成 TSV | `'\t'.join()` 要求全是 str，记得 `str(...)` |

### 5.2 【坑】raw.githubusercontent.com 被墙

稳定报 `curl: (35) TLS connect error: unexpected eof while reading`，重试无效。
`github.com` 本身是通的。

**替代方案**（按优先级）：

```bash
# jsDelivr CDN —— 拿单个文件最方便
https://cdn.jsdelivr.net/gh/<owner>/<repo>@<branch>/<path>

# GitHub API —— 列目录树、查仓库元信息
https://api.github.com/repos/<owner>/<repo>
https://api.github.com/repos/<owner>/<repo>/git/trees/<branch>?recursive=1
```

另外 `WebFetch` 工具对 `raw.githubusercontent.com`、`docs.waveshare.com` 等域名会被
安全策略直接拒绝（"Unable to verify if domain is safe to fetch"），此时同样走 `curl`。

### 5.3 【大坑】板子没有代理 —— 联网选型必须先验证可达性

**这是做联网功能时最容易翻车、也最容易误判的一个坑。**

板子 WiFi 连上了、IP 拿到了、TLS 握手也正常，但**访问国外 API 一律失败**。
不是代码问题，是网络环境：**开发机有代理，板子没有。**

#### 症状

最早那版行情 Demo 用的是 CoinGecko（加密货币），烧进去后串口只重复这一行：

```
[api] failed: HTTP -1
```

`-1` = `HTTPC_ERROR_CONNECTION_REFUSED`（连不上）。但**同一块板子**访问
`https://www.baidu.com` 返回 200 —— 说明**板子本身没毛病**。

#### 根因：DNS 污染

`arduino/api_probe` 打出来的实测结果（同一时刻、同一块板子）：

| 域名 | DNS 解析到 | 结果 |
|---|---|---|
| `api.coingecko.com` | `199.59.148.209` | ✗ `HTTP -1` |
| `api.huobi.pro` | `157.240.8.36` | ✗ |
| `api.binance.com` | `104.244.43.228` | ✗ |
| `www.okx.com` | `169.254.0.2` | ✗ |
| `api.coincap.io` | `0.0.0.0` | ✗ |
| `api.coinbase.com` | `108.160.172.232` | ✗ |
| `api.kraken.com` | `199.59.149.207` | ✗ |

**这些 IP 全都不是真实结果**，是投毒：`0.0.0.0`（黑洞）、`169.254.x.x`（链路本地，
本来就不该出现在公网 DNS 结果里）、Twitter / Facebook 网段（`104.244.x`、`199.59.x`、
`157.240.x`）—— 拿这些 IP 顶包。

> **判断依据：正常域名在不同解析器之间结果应该是一致的。**
> 对同一个域名问 4 个 DNS 得到 4 个不同 IP，就是污染。
> 完整特征表见 [`arduino/api_probe/README.md`](arduino/api_probe/README.md)。

#### ⚠️ 最容易犯的错：用开发机的 curl 验证

这次就栽在这上面：用 Mac 的 `curl` 测 CoinGecko **返回 200**，于是判断"接口可用"，
照此写完了整个 Demo —— 板子上一跑就挂。

原因：**开发机上挂着代理。**

```bash
$ env | grep -i proxy
HTTPS_PROXY=http://127.0.0.1:7890      ← Clash 之类
all_proxy=socks5://127.0.0.1:7890
```

`curl` 是**翻墙**出去的，跟板子的处境完全无关。

> **铁律：验证板子的联网能力，必须在板子上验证。**
> 开发机能通 ≠ 板子能通。想在开发机上测直连，得加 `--noproxy '*'` ——
> 但那也只证明了**开发机**直连可达，仍然不能代表板子。

#### 怎么诊断：自下而上分层

联网失败有好几种原因（WiFi 没连上 / DNS 污染 / TCP 被挡 / TLS 失败 / 目标被墙），
混在一起猜是查不出来的。**从底层往上逐层排除，哪层挂了就报哪层**：

```
第 0 层  WiFi 关联       →  拿不到 IP，后面全没意义
第 1 层  DNS 解析        →  域名变不成 IP（污染在这一层暴露）
第 2 层  TCP 连 443      →  DNS 没问题但网络到不了
第 3 层  明文 HTTP       →  最基本的出网能力（不带 TLS）
第 4 层  HTTPS 国内站点   →  TLS 握手本身能不能成（对照组）
第 5 层  HTTPS 目标站点   →  前面都过、只有这步挂 = 目标被墙
```

对应两个现成工具（都在 `arduino/` 下）：

| 工具 | 用途 | 用法 |
|---|---|---|
| `net_diag` | 上面那套分层诊断，跑一次就知道挂在哪层 | `./build.sh flash net_diag` |
| `api_probe` | 批量测一批 API 在板子上通不通，打表输出 | 把 URL 加进 `PROBES` 表再烧 |

**加任何新接口之前，先把 URL 丢进 `api_probe` 跑一遍。**

#### 实测可达性清单（2026-09-25 快照，本网络）

**✓ 板子可直连**

| 接口 | 用途 |
|---|---|
| `api.open-meteo.com` | 天气（实测 200，可用） |
| `wttr.in` | 天气 |
| `qt.gtimg.cn` + `web.ifzq.gtimg.cn` | 腾讯财经：实时行情 + K线 |
| `data-api.binance.vision` | 加密货币 |
| `api.gateio.ws` | 加密货币（Gate.io） |

**✗ 被 DNS 污染**

`api.coingecko.com`、`api.binance.com`、`www.okx.com`、`api.huobi.pro`、
`api.coincap.io`、`api.coinbase.com`、`api.kraken.com`、`api-pub.bitfinex.com`

**⚠️ 能解析但要额外处理**

`hq.sinajs.cn`（新浪财经）解析出真 IP，但返回 `HTTP -11`（读超时）——
它要求请求带 `Referer` 头，不带就直接拒。

> **注意：可达性会变。** 上表只是 2026-09-25 的快照。
> `binance-vision` 和 `gate` 现在是通的，但随时可能被墙。
> **别把清单当承诺，要拿工具实测。**

#### 结论：选 API 的检查清单

1. **优先国内源。** 能用国内的就别用国外的 —— 不是"现在能不能通"，
   而是"明天还通不通"的问题。
2. **把可达性检查做进代码**：请求失败时把错误码画在屏幕上
   （本项目所有 Demo 都这么做了），不接串口也能看见问题出在哪。

---

## 6. 关键概念

### 6.1 U8g2 的分层架构

```
你的代码          g->drawStr("HELLO")
     ↓
U8g2 层           字体渲染 → 画成点阵 → 写进缓冲
     ↓
U8x8 层           缓冲 → 按 tile 切块 → 调字节回调
     ↓
ST7305_U8g2 驱动  位重排 + SPI 传输（把 U8g2 格式翻译成 ST7305 认的格式）
     ↓
ST7305 芯片       接收命令/数据 → 控制液晶
     ↓
玻璃面板          像素变黑或变白
```

**`ST7305_U8g2` 这个类名字可以拆开理解**：`ST7305` 是驱动 IC 型号，`U8g2` 是图形库，
合起来 = "**把 ST7305 接到 U8g2 上的适配层**"。

它是微雪写在**自己仓库**里的桥接层（**不是 U8g2 官方的**），因为 ST7305 太冷门、
不在 U8g2 内置支持列表里。有了它，`getU8g2()` 之后你拿到的就是一个标准 `U8G2*` 对象，
**对上层来说和一块 SSD1306 OLED 没有任何区别**——所有 U8g2 教程、字体、画图函数都能直接用。

### 6.2 U8g2 vs LVGL 怎么选

这块板**两条路官方都提供了**（`08_LVGL_V8_Test` / `09_LVGL_V9_Test` / `10_U8G2_Test`）。

| | U8g2 | LVGL |
|---|---|---|
| 适合 | 几行文字/数字 | 多页面、动画、复杂布局 |
| 要学的概念 | `drawStr` / `setFont` / `sendBuffer` | flush 回调、tick 定时器、驱动注册、`lv_obj` |
| 显示一行字 | 1 行代码 | ~30 行样板 |

**显示天气这类内容，U8g2 足够且简单得多。**

> ⚠️ **U8g2 内置字体不含中文。** 要显示中文有两条路：
> ① 用 U8g2 字体工具把用到的汉字提取成点阵字库（体积可控，推荐）
> ② 换 LVGL + 中文字库（体积大，但排版/label 更方便）

### 6.3 Arduino 程序的模型

**没有 `main()`，只有两个函数**：

```cpp
void setup() { ... }   // 上电后跑一次
void loop()  { ... }   // setup 跑完后无限循环，直到断电
```

Arduino 框架背后生成的 `main()` 是：

```cpp
int main() { setup(); while (1) { loop(); } }
```

**铁律：`loop()` 里不能有长时间阻塞的代码。** 你写 `delay(60000)`，板子这 60 秒
就什么都干不了。所以定时要用 `millis()` 判断：

```cpp
if (millis() - lastFetch >= REFRESH_MS) { ... }   // ✓ 非阻塞
```

> 写成减法形式而不是 `millis() > last + INTERVAL`，是因为 `millis()` 是 32 位无符号数，
> **约 49.7 天会溢出归零**，减法形式利用无符号回绕自动算对。这是嵌入式经典写法。

### 6.4 烧录时板子内部发生了什么

```
上电
 ↓
① ROM Bootloader（固化在硅片里，改不了擦不掉）
   · 采样 GPIO0 决定启动模式：高=正常启动 / 低=下载模式
   · 从 Flash 0x0 读二级 bootloader
 ↓
② 二级 Bootloader（Flash 0x0）
   · 初始化 Flash 高速模式（QIO 80MHz）
   · 初始化 PSRAM  ★ 这里就是 PSRAM=opi 生效的地方
   · 读分区表（0x8000）
   · 从 app 分区加载程序
 ↓
③ 你的程序（Flash 0x10000）
   · setup() → loop()
```

**「下载模式」是靠上电瞬间采样 GPIO0 决定的**（不是持续状态）——这就是为什么老 ESP32
要"按住 BOOT 再按 RST"：按 RST 是让芯片重新采样。**ESP32-S3 由 esptool 通过 USB 自动代劳。**

---

### 6.5 屏幕上的图片：照片怎么变成黑白点

屏幕是 **1bpp**，120000 个像素非黑即白，**没有灰度**。普通照片不能直接往上放。
补上这中间一步的是 [`tools/img2slides.py`](tools/img2slides.py)。

流水线本身是业界标准做法（image2cpp、didder 这些同类工具都是这个套路，链接在 §8）：

```
缩放 → 灰度 → 亮度归中 → 抖动 → 二值化 → 打包字节
```

#### 抖动是什么，为什么必须有

**抖动（dither）= 用黑白点的疏密去模拟灰阶**。50% 的灰度 = 50% 的黑点，
25% 的灰度 = 25% 的黑点，靠人眼在一定距离上自己平均回去。

不抖动、直接拿阈值切，照片会变成大块死黑和死白。同一张照片实测：

| 处理 | 抖动后黑点占比 | 观感 |
|---|---|---|
| 直接阈值二值化 | 73.9% | 一坨死黑，只剩下轮廓 |
| Floyd–Steinberg 抖动 | 66.5% | 层次出来了，但整体发闷 |
| 抖动 + 亮度归中 | **47.4%** | 五官可辨，接近灰度图的观感 |

#### 【坑】`autocontrast` 不够，甚至会帮倒忙

这是这次真正花时间的地方。一张**本身就偏暗**的照片（中位数 83/255，`~/Pictures/gilfoyle.jpg`）：

| 处理 | 抖动后黑点占比 |
|---|---|
| 什么都不做 | 63.8% |
| `ImageOps.autocontrast` | **66.5%** ← 比什么都不做还黑 |
| `autocontrast` + gamma 归中 | **47.4%** |

直觉上「自动对比度」应该能救暗照片，实际反而更黑。原因是
**`autocontrast` 只把最暗和最亮拉到两个端点，不移动中位数** ——
一张中位数本来就低的照片，拉伸完中位数还是低。

> **铁律：1bpp 抖动的理想状态是图像中位数落在 128 附近**
> （黑白各占一半，误差扩散才有向两边分配的余量）。
> 做法是解 `median^γ = 0.5` 求出 gamma 再映射 —— 本例 γ≈0.51。
> 用 gamma 曲线而不是线性拉伸，是因为线性拉伸会把某一端顶死，gamma 不会。

顺带一提，这个「黑点占比」指标很有用：它一眼就能看出照片转出来会不会发闷，
**理想值就是 50% 附近**。转换脚本每张都会打出来。

#### 对比度：抖动图的「灰」是网点密度，所以能调

抖动出来的「灰」不是真灰，是**黑白点的疏密**。中间调越多，画面上「麻麻的网点」越多，
看着就越平。这不是分辨率不够，是 1bpp 的固有代价 —— 但能靠**对比度**缓解：
S 曲线把中间调推向两端，更多区域变成接近纯黑/纯白。

实测同一张照片，**「接近纯黑/纯白」的区域占比**：

| 对比度 | 实心区域占比 |
|---|---|
| 0（不调） | 10% |
| 0.5 | 21% |
| 1.0 | **32%** |

默认值 1.0 是在**真机上把 0 / 0.35 / 0.7 / 1.0 四档摆成一排轮播**挑出来的。

> **拿幻灯片本身当对照工具**：一次上传四张同一张图的不同参数版本，翻着看，
> 比在电脑上盯着 ASCII 预览猜准得多。这个办法值得记住 ——
> 屏幕上的效果只有屏幕能回答。

S 曲线绕中点对称，所以**黑点占比是恒定的**（都 46.4%），不会把前面调好的亮度带偏。
这也是它必须放在亮度归中**之后**的原因。

#### 【坑】中位数归中会把「暗背景 + 小主体」的图提爆

真机上拿一张**逆光人像**试出来的（中位数只有 27/255，大片暗背景里一小块亮的脸）：
屏幕上的脸**糊成一团，没有层次**。

不是对比度的问题（把对比度调到 0 也没救）。根因是**亮度归中用的是全图中位数**：

- 中位数被大片暗背景拉到很低（27）→ 解出 γ=0.31，是个很猛的提亮
- 提亮是**整张图**一起提的 → 脸那原本 100~200 的灰阶被压进 191~236 这 45 级里
- 表现就是「脸是亮的，但没有细节」

量化一下脸部区域的层次（标准差 / 不同灰阶数）：

| 处理 | 脸部 σ | 灰阶数 |
|---|---|---|
| 原样（不归中） | 66.3 | 244 |
| **中位数归中 γ=0.31** | **49.1** | **151** |
| 归中但 γ 限制到 0.7 | 65.0 | 214 |
| 归中但 γ 限制到 0.8 | 64.5 | 220 |

**修法**：给自动提亮卡一个下限 `GAMMA_FLOOR`（默认 0.8）——
宁可欠一点，也别把主体提爆。`--gamma-floor` 可以按图调，`1.0` 等于完全不自动提亮。

**「已限幅」的图屏幕上会偏黑（70~85% 黑点），这是故意的**，不是 bug。
输出里会明确标出来，免得下次又当成故障排查。

> **更值得记的是这个思维错误。** 我之前把「1bpp 抖动的理想状态是中位数落在 128」
> 当成了铁律 —— 但那是从**一张主体占满画面**的照片上总结的。
> 主体只占一小块时，忠实还原本来就该是暗的，硬拉到 128 就是过曝。
>
> 同一类错误这轮犯了三次：
>
> | 我当时的推断 | 依据 | 实际情况 |
> |---|---|---|
> | 刷新率应该是 51 Hz | 另一个驱动的注释 | 27 Hz（那是块小屏的数） |
> | 黑白方向「1 = 黑」 | 墨水屏的直觉 | 这块屏反过来 |
> | 中位数应该归到 128 | 一张照片的实测 | 只对主体占满画面的图成立 |
>
> **共同点：拿单一样本 / 别的硬件 / 通常惯例去推一个具体设备的具体行为。**
> 教训是——**猜出来的结论要标出来是猜的，并且尽快拿真机验掉。**

#### 【坑】同一个中位数规则，对亮图是反效果 —— 归一化必须是单向的

上面那条修的是「暗图被提爆」。隔了一轮才发现，**另一半同样有问题**：
`normalize_to_mid` 只管下限、不管上限，于是**亮图会被无限制地压暗**。

γ 是有方向的：`γ<1` 提亮（暗图需要），`γ>1` 压暗（亮图会触发）。
修了下限之后上线就没人管了，一张中位数 231 的亮图会算出 **γ=7**，
被硬压成 **50% 黑点的灰糊** —— 原本是一张亮色渐变，压完内容几乎看不见：

| 图 | 原图中位数 | 自动 γ | 旧行为黑点 | 加上限后黑点 |
|---|---|---|---|---|
| Soft Pink | 231 | 7.01 | **50%** | **2%** |
| Silver | 228 | 6.19 | 50% | 3% |
| Gold | 227 | 5.96 | 50% | 3% |

**修法：`GAMMA_CEIL = 1.0`，只许提亮、不许压暗。**

> **理由不是调参调出来的，是反射屏的物理**：这块屏就是「白纸 + 墨」。
> 一张本来就亮的图，忠实还原就该渲染成**亮的一页**；把所有图都压到同一个暗度，
> 等于丢掉了「这张图是亮的」这个信息。亮图真需要更多层次，用 `--contrast` ——
> 它是对称的、不移中位数，两件事各管各的。

#### 顺带一提：样本偏差是怎么让我误判的

- 定 `GAMMA_FLOOR=0.8` 时，我的样本是 **2 张照片，都是暗背景人像**（γ = 0.31 / 0.56）
- 后来拿 18 张 macOS 壁纸做统计，自动 γ 的**中位数是 1.59**、p25 是 0.84
- **我那两张比样本里最小的还低** —— 等于拿极端值去推通用默认值

**这轮已经是第四次栽在同一件事上**（前三次见上面那张表），教训也是同一条：
**猜出来的结论要标出来是猜的，并尽快拿真机或大样本验掉** ——
这次壁纸集（19 张，题材各异）就比「我觉得 0.8 挺好看」有用得多。

#### 字节序：U8g2 的 XBM 是 LSB 优先

打包成字节时，一个字节里 **bit0 是最左边的像素**，和直觉的 `0x80 >> i` 相反。

这不是猜的，有两条独立证据：

**一、U8g2 源码** `src/clib/u8g2_bitmap.c` 的 `u8g2_DrawHXBM()`：

```c
mask = 1;
while(len > 0) {
  uint8_t current_bit = (*b) & mask;
  ...
  mask <<= 1;          // ← 左移，所以 bit0 是最左像素
}
```

**二、这个仓库里已经被真机教育过一次** —— `tools/gen_cn_font.py` 里那段注释：

> 按标准 XBM 的 `0x80 >> i` 写，屏幕上的字会**左右镜像**，
> 因为 U8g2 每个字节里是从低位往高位走的。

写错的表现是**每 8 个像素各自镜像一次**：16px 宽的汉字被切成两段 8px，每段各反一次，
看着就是「字反了」。

同时确认了行跨距是 `(w+7)/8` 字节（`u8g2_bitmap.c:167`），
所以 400×300 一张图 = `50 × 300` = **正好 15000 字节**，就是个标准 XBM。
`u8g2->drawXBM()` 直接吃，旋转由 `U8G2_R1` 负责，转换脚本不用管。

#### 【坑】黑白方向：这块屏是反的（真机翻的车）

直觉以为「1 = 黑」（墨水屏都这样），**这块屏是反的**：
U8g2 的 `bit=1` 画出来是**白**，`clearBuffer()` 之后的 0 才是黑。

第一版 `img2slides.py` 按「黑 = 1」打包，烧上去屏幕上是一张**底片**。
把默认改成「**亮 = 1**」就对了 —— 这个方向现在是写死的默认值，
`--invert` 只是留着万一以后换屏。

> **这个坑没法在烧录前发现。** 串口打出来的字节是对的，
> 连「黑点占比 46%」这个指标也看不出方向 —— 反过来就是 54%，
> 两者都是「一张照片」该有的样子。**判断依据只能是看一眼屏幕。**

顺带把 §1.1 那条笔记对齐一下：早先记的「black = 0, white = 0xff」说的是
**面板显存**的取值，和 U8g2 这一层的 bit 值不是一回事。
按「bit=1 = 白」反推，官方计数器 Demo（`setDrawColor(1)` 画在 `clearBuffer()` 之上）
显示的其实是**白字黑底**。

#### 怎么在烧录前就知道画面能不能看

把转换结果**降采样**成灰度密度图，用字符 `" .:-=+*#%@"` 打在终端上。
降采样会把抖动噪点平均掉 —— **等价于人眼在一定距离上的观感**，
所以这个 ASCII 预览是真的能判断好坏的，而且不产生任何文件：

```
|=======++++++*****#####%%%%@@@@@%#*+==-::...       |
|#####%%%%%%%%@@@@@@@@@@@@%%##**++==--::.....      |
```

这比输出 PNG 再打开看还方便，做这类转换工具时值得抄。

#### 实测：画一张图要 55 ms，瓶颈不是 SPI

真机上跑 `photo_slideshow`，把耗时拆开看：

| 阶段 | 耗时 | 性质 |
|---|---|---|
| `clearBuffer()` | 25 µs | 可忽略 |
| **`drawXBM()`** | **45~48 ms** | **纯 CPU，占 82%** |
| `sendBuffer()` | 10.06 ms | SPI 传输 |

送屏那 10 ms 和早先记的 `flush 10ms` 分毫不差，**但画图那步差了一个数量级** ——
我原先按「U8g2 有 run-length 优化，应该很快」估的，实际是 45 ms。

**为什么这么慢**：`u8g2_DrawHXBM()` 是**逐位**扫的，一张 400×300 = **12 万次位循环**，
而且每遇到一段同色位就调一次 `u8g2_DrawHVLine()`。抖动后的照片黑点很碎（平均游程约 2 个像素），
等于**要调几万次画线函数**，每次都还要过一遍 `U8G2_R1` 的坐标旋转。

> 顺带纠正一个常见误判：这块屏「慢」的地方不在 SPI。SPI 送整屏 10 ms（≈100 fps），
> 真正拖后腿的是 U8g2 的软件画图。

**这个数意味着什么**：

- **幻灯片完全无所谓** —— 5 秒才换一张，55 ms 看不出来。
- **动画就卡在这儿了**：全屏 blit 上限 ≈ **1/0.055 ≈ 18 fps**，
  比面板的 27 Hz 还低 —— **瓶颈是 CPU，不是屏幕**。所以想上动画，
  第一件要做的不是省 SPI，是**绕开 U8g2 的逐位画图**。

**怎么绕**（真要做动画时再动手，现在没必要）：目标是 U8g2 的缓冲格式
（`u8g2_ll_hvline_vertical_top_lsb`：一个字节 = 一列上 8 个像素，bit0 在最上）。
源是 XBM（行优先、LSB 优先、每行 50 字节），所以可以写个**位转置**循环，
直接按字节往缓冲里填 —— **15000 次字节操作，而不是 12 万次位操作**，理论上快一个数量级。
代价是旋转要自己算（现在由 `U8G2_R1` 代劳），这部分是固定的坐标映射，不难。

### 6.6 分区表可以被单个 sketch 覆盖

给幻灯片划存储空间时，本以为要改 `build.sh` 里那串 FQBN（把 `app3M_fat9M_16MB`
换成别的方案）。不用 —— `arduino-esp32` 的 `platform.txt` 里有三条 prebuild 钩子：

```
# Lowest priority is copied first and higher priority overwrites it:
#   build.partitions < variant < source
recipe.hooks.prebuild.1  平台方案目录的 {build.partitions}.csv
recipe.hooks.prebuild.2  variant 目录
recipe.hooks.prebuild.3  sketch 源码目录的 partitions.csv   ← 最高优先级
```

三条都是 `cp -f` 无条件覆盖，所以**在 Demo 目录里放一份 `partitions.csv` 就能改分区表**，
而且只影响这个 Demo。`photo_slideshow` 就是靠这个拿到 9.875 MB 空间的。

实测确认：编译产物里的二进制分区表确实是 sketch 自己那份（`slides,data,spiffs,0x610000,10112K`），
FQBN 一个字符没动。

#### 两个小坑

**一、分区名不能叫 `littlefs`。** `gen_esp32part.py` 里 `"littlefs"` 是个**子类型关键字**
（对应 subtype 0x83）。名字恰好等于某个 subtype 关键字、而 subtype 列又不是它，编译会报：

```
WARNING: Partition has name 'littlefs' which is a partition subtype, but this partition
has non-matching type 0x1 and subtype 0x82. Mistake in partition table?
```

功能不受影响（type/subtype 由 CSV 那两列决定，跟名字无关），但会污染构建输出。
改叫 `slides` 就没事了。

**二、`spiffs` 这个 subtype 不是 LittleFS 要求的。** 一开始以为必须用 `spiffs`，
后来反汇编 `libjoltwallet__littlefs.a` 里的 `esp_littlefs_init` 才看清：
**传了 label 时用的是 `ESP_PARTITION_SUBTYPE_ANY`**，所以 `spiffs`(0x82) 和
`littlefs`(0x83) 都能挂上。选 `spiffs` 只是因为它是 Arduino 官方各方案的惯例写法。

#### 为什么不用 FQBN 里现成的那 9.875 MB `ffat` 分区

`app3M_fat9M_16MB` 本来就带一块 9.875 MB 的 FAT 分区，看着正好能用。放弃的原因是**工具链**：
Arduino 侧没有现成的 FAT 镜像生成器，要走 `FFat` 库 + ESP-IDF 的 `fatfsgen.py`，
而本机没装 ESP-IDF。相比之下 LittleFS 是核心自带的（`mklittlefs` 就在
`~/Library/Arduino15/packages/esp32/tools/` 下），一条命令打包、一条命令写 flash。

代价是分区 subtype 从 `fat` 改成 `spiffs`（大小完全一样），
以及 `build.sh` 里多了个 `slides` 子命令。

> 顺带记一个 **bash 的坑**：变量后面紧跟中文标点时，必须写成 `${var}` 加花括号。
> macOS 自带的是 **bash 3.2**，多字节字符的首字节会被当成变量名的一部分 ——
> 写 `$offset，` 会报 `offset�: unbound variable` 这种完全看不出所以然的错。

## 7. 下一步

### 已完成
- [x] 阶段 0：硬件 bring-up
- [x] 阶段 1：工具链 + 官方 Demo 跑通（屏幕出现计数器，串口 79fps）

### 待做
- [ ] **阶段 2 小步改**：改一行代码 → 编译 → 烧录 → 看变化，熟悉这个循环
      （例：在 `loop()` 末尾加 `delay(1000)`，屏幕刷新会明显变慢）
- [x] **阶段 3 长功能**：接 WiFi + HTTPS 请求 + JSON 解析 + 显示
      - ✅ `arduino/rlcd_stock_demo/` —— A股行情（腾讯财经）**已烧录验证运行** ← 主 Demo
      - **选 API 前必读 §5.3：板子没有代理，联网选型必须先验证可达性**
      - 过程中写过的天气版、加密货币版已经删掉（2026-09-25）——被墙那一课的结论
        完整留在 §5.3，没有跟着代码一起丢
- [ ] **阶段 4 稳定性**：断网重连、长时间运行、边界情况
- [x] **阶段 5 把照片放进屏幕**：`arduino/photo_slideshow/` + `tools/img2slides.py`
      —— **已上真机验证**：挂载、列文件、排序、轮播都正常，分区表覆盖生效
      - 转换流水线、亮度归中、字节序、黑白方向、耗时实测都在 §6.5；
        分区表覆盖在 §6.6
      - 真机上翻过一个车：黑白是第一版打包方向反了（见 §6.5「黑白方向」那节）

### 想深入了解的方向
- 参考：[从40fps到700fps的U8g2驱动优化实践](https://yunpan.plus/t/25139-1-1)
  （局部刷新 128×32、SPI 10MHz→24MHz、查表、合并 CS）
- [x] 真机实测刷新率：**面板 27 Hz / MCU 69 fps**（`arduino/panel_rate/`），见 §1.1.1
- [x] 真机实测 `photo_slideshow`：**每张 55 ms**（`drawXBM` 45~48 ms、送屏 10.06 ms）
      —— 原先严重低估了画图那步，见 §6.5 末尾
- **想做动画**（而不只是幻灯片）的话，门槛比原先想的高，但路是清楚的：
  1. **必须用 TE 同步写入**（GPIO6，每帧一个脉冲），否则 69 fps 盲写 27 Hz 的屏会撕裂。
     `arduino/panel_rate/` 已经把读 TE 的代码写好了，直接搬。
  2. 帧率上限是 **27 Hz**，按这个目标推帧就行，推多了纯浪费。
  3. 每帧 15000 字节，27 fps ≈ 400 KB/s，存 flash 的话得压缩 ——
     但**抖动后的照片是高频噪声，RLE 基本压不动**，得用帧间差分或者换有序抖动
     （`bayer` 的规则图案帧间差分效果好得多）。

---

## 8. 参考资源

- 官方仓库 <https://github.com/waveshareteam/ESP32-S3-RLCD-4.2>
- 中文文档 <https://docs.waveshare.net/ESP32-S3-RLCD-4.2/>
- 英文文档 <https://docs.waveshare.com/ESP32-S3-RLCD-4.2>
- 产品页 <https://www.waveshare.net/shop/ESP32-S3-RLCD-4.2-EN.htm>
- ESP-IDF 开发说明 <https://docs.waveshare.com/ESP32-S3-RLCD-4.2/ESP-IDF>
- ESPHome 配置示例 <https://docs.waveshare.com/ESP32-ESPHome-Tutorials/Example-RLCD-Voice>
- 第三方 ESPHome 驱动 <https://github.com/kylehase/ESPHome-ST7305-RLCD>
- U8g2 仓库（字体工具在 `tools/font/` 目录下，做中文字库用）
  <https://github.com/olikraus/u8g2>

同类转换工具（§6.5 调研时看的，都面向墨水屏，抖动那部分思路一样）：
- image2cpp <https://github.com/javl/image2cpp> —— 网页版，直接出 C 数组
- epaper-image-convert <https://github.com/aitjcize/epaper-image-convert> —— Node CLI，带 tone mapping
- didder <https://github.com/makew0rld/didder> —— 通用抖动 CLI，算法最全
- eink-image <https://docs.rs/crate/eink-image/0.1.0> —— Rust，默认 Floyd–Steinberg，带 gamma 处理

ESP32 上做动画的参考（都是 1bpp OLED 上的 Bad Apple，压缩方案值得借鉴）：
- <https://github.com/kenbio216/ESP32_BadApple> —— 帧序列 + RLE + heatshrink 存 flash
- <https://seizu.dev/2026/07/03/post4759.html> —— TMG1，专为 1bpp 视频写的流式 codec
