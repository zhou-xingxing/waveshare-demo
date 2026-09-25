#!/bin/bash
#
# ESP32-S3-RLCD-4.2 编译/烧录/串口 一键脚本
#
#   ./build.sh              编译（默认 Demo 10_U8G2_Test）
#   ./build.sh flash        编译 + 烧录
#   ./build.sh upload       只烧录（用上次编译的产物，不重新编译）
#   ./build.sh slides       把 Demo data/ 目录里的文件传进板子的文件系统
#   ./build.sh mon          打开串口监视器（Ctrl+C 退出）
#   ./build.sh ports        列出候选串口
#   ./build.sh help         帮助
#
#   ./build.sh flash rlcd_stock_demo     指定 Demo（传 arduino/ 下的目录名）
#   ./build.sh slides photo_slideshow    指定 Demo（默认就是 photo_slideshow）
#   PORT=/dev/cu.usbmodem102 ./build.sh flash    手动指定串口
#
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ARDUINO_DIR="$ROOT/arduino"

# ---- 开发板配置。改这里就行，不用改下面的代码 ----------------------------
FQBN="esp32:esp32:esp32s3:CDCOnBoot=cdc,FlashMode=qio,FlashSize=16M,PartitionScheme=app3M_fat9M_16MB,PSRAM=opi,UploadSpeed=921600"
BAUD=115200
DEFAULT_SKETCH="10_U8G2_Test"
DEFAULT_SLIDES_SKETCH="photo_slideshow"   # ./build.sh slides 不带参数时的 Demo
SLIDES_PARTITION="slides"                 # 往 partitions.csv 里哪个分区写
# -------------------------------------------------------------------------

# ESP32 核心（含 mklittlefs / esptool 这些自带工具）装在哪
ARDUINO15="${ARDUINO15:-$HOME/Library/Arduino15}"

# 颜色（非终端环境自动关闭）
if [ -t 1 ]; then
  R=$'\033[31m'; G=$'\033[32m'; Y=$'\033[33m'; B=$'\033[1m'; N=$'\033[0m'
else
  R=; G=; Y=; B=; N=
fi

die()  { printf '%s\n' "${R}✗ $*${N}" >&2; exit 1; }
info() { printf '%s\n' "${B}▸ $*${N}"; }
ok()   { printf '%s\n' "${G}✓ $*${N}"; }

# 找板子串口。ESP32-S3 原生 USB 会枚举成 /dev/cu.usbmodem*
#
# ⚠️ 调用处必须写成 `port=$(find_port) || exit 1`。
#    这里是 $(...) 子 shell，里面 exit 1 只会结束子 shell，父进程照跑不误 ——
#    于是 port 变成空字符串，后面报的错就跟真正的原因（没插板子）八竿子打不着。
find_port() {
  if [ -n "${PORT:-}" ]; then echo "$PORT"; return; fi
  local p
  p=$(ls /dev/cu.usbmodem* 2>/dev/null | head -1)
  if [ -z "$p" ]; then
    printf '%s\n' "${R}✗ 没找到板子串口${N}" >&2
    printf '%s\n' "  检查：① USB-C 线两端插紧（要数据线，不是充电线）" >&2
    printf '%s\n' "        ② 屏幕亮不亮（亮=有电，是数据断了）" >&2
    printf '%s\n' "        ③ 换个 USB 口试试" >&2
    printf '%s\n' "  看当前有哪些串口：$0 ports" >&2
    exit 1
  fi
  echo "$p"
}

# 找 Demo 目录。文件夹名必须和 .ino 同名（Arduino 硬性规则）
find_sketch() {
  local name="${1:-$DEFAULT_SKETCH}" dir
  dir="$ARDUINO_DIR/$name"
  [ -d "$dir" ] || die "Demo 目录不存在: $dir"
  [ -f "$dir/$name.ino" ] || die "$dir 里没有 $name.ino（文件夹名必须和 .ino 同名）"
  echo "$dir"
}

cmd_compile() {
  local sketch; sketch=$(find_sketch "${1:-}")
  info "编译 $(basename "$sketch")"
  arduino-cli compile -b "$FQBN" "$sketch" || die "编译失败"
  ok "编译成功"
}

cmd_upload() {
  local sketch; sketch=$(find_sketch "${1:-}")
  local port; port=$(find_port) || exit 1
  info "烧录到 $port"
  arduino-cli upload -b "$FQBN" -p "$port" "$sketch" || die "烧录失败"
  ok "烧录完成"
}

cmd_flash() {
  local sketch; sketch=$(find_sketch "${1:-}")
  local port; port=$(find_port) || exit 1
  info "编译 $(basename "$sketch")"
  arduino-cli compile -b "$FQBN" "$sketch" || die "编译失败"
  info "烧录到 $port"
  arduino-cli upload -b "$FQBN" -p "$port" "$sketch" || die "烧录失败"
  ok "完成。看串口输出: $0 mon"
}

cmd_mon() {
  local port; port=$(find_port) || exit 1
  info "串口 $port @ ${BAUD}（Ctrl+C 退出）"
  exec arduino-cli monitor -p "$port" -c baudrate=$BAUD
}

# ---- 文件系统上传（./build.sh slides）-----------------------------------
# 把 arduino/<Demo>/data/ 打包成 LittleFS 镜像，写进板子上的 slides 分区。
# 分区的位置和大小**从 Demo 自己的 partitions.csv 里读**，所以改了分区表这里自动跟着变，
# 不会出现「脚本里的偏移量和分区表对不上」这种最难查的问题。
#
# 用的是 ESP32 核心自带的工具，不用额外装：
#   mklittlefs  打镜像      esptool  写 flash

find_tool() {  # find_tool <tools 下的目录名> <可执行文件名>
  local hit
  hit=$(ls -1 "$ARDUINO15/packages/esp32/tools/$1"/*/"$2" 2>/dev/null | head -1)
  [ -n "$hit" ] || die "找不到 $2（应该在 $ARDUINO15/packages/esp32/tools/$1/ 下）"
  echo "$hit"
}

# 从 partitions.csv 里读某个分区的 offset 和 size
read_partition() {  # read_partition <csv 路径> <分区名>
  awk -F, -v want="$2" '
    /^[[:space:]]*#/ { next }
    NF >= 5 {
      gsub(/[[:space:]]/, "", $1)
      if ($1 == want) {
        gsub(/[[:space:]]/, "", $4); gsub(/[[:space:]]/, "", $5)
        print $4, $5; exit
      }
    }
  ' "$1"
}

cmd_slides() {
  local name="${1:-$DEFAULT_SLIDES_SKETCH}"
  local sketch; sketch=$(find_sketch "$name")
  local csv="$sketch/partitions.csv"
  local data="$sketch/data"

  [ -f "$csv" ] || die "$name 目录下没有 partitions.csv，不知道往哪写"
  [ -d "$data" ] || die "没有 $data 目录 —— 先跑 tools/img2slides.py 生成"

  local n; n=$(ls -1 "$data"/*.bin 2>/dev/null | wc -l | tr -d ' ')
  [ "$n" -gt 0 ] || die "$data 里没有 .bin 文件 —— 先跑 tools/img2slides.py 生成"

  local part; part=$(read_partition "$csv" "$SLIDES_PARTITION")
  [ -n "$part" ] || die "$csv 里没有叫 $SLIDES_PARTITION 的分区"
  local offset="${part% *}" size="${part#* }"

  local mklittlefs esptool port img
  mklittlefs=$(find_tool mklittlefs mklittlefs)
  esptool=$(find_tool esptool_py esptool)
  port=$(find_port) || exit 1
  img=$(mktemp -t rlcd_slides)

  # ⚠️ 变量后面紧跟中文标点时，一定要写成 ${var} 加花括号。
  #    macOS 自带的是 bash 3.2，多字节字符的首字节会被当成变量名的一部分，
  #    写成 $offset， 会报 "offset�: unbound variable" 这种莫名其妙的错。
  info "打包 $n 个文件 -> $SLIDES_PARTITION 分区（偏移 ${offset}，大小 ${size}）"
  # -b/-p 必须和板子上 littlefs 认的格式一致，写错了挂载会失败
  "$mklittlefs" -c "$data" -b 4096 -p 256 -s "$size" "$img" \
    || { rm -f "$img"; die "mklittlefs 打包失败"; }

  info "写入 ${port}（镜像 $(wc -c < "$img" | tr -d ' ') 字节）"
  # esptool 5.x 把 write_flash 改名成 write-flash 了；老版本只有带下划线那个
  local wf=write-flash
  "$esptool" write-flash --help >/dev/null 2>&1 || wf=write_flash
  if ! "$esptool" --chip esp32s3 -p "$port" -b 921600 "$wf" "$offset" "$img"; then
    rm -f "$img"
    die "写入失败"
  fi
  rm -f "$img"
  ok "完成。图片已进板子，看屏幕：$0 mon"
  # 分区表是跟着固件一起烧进去的（写 flash 的 0x8000），所以第一次用必须
  # 「先烧固件，再传图片」。反过来做的话图片写进去了也没人认。
  printf '%s\n' \
    "  ⚠️ 屏幕上若显示「文件系统没挂上」，说明板子上还是旧分区表 ——" \
    "     先跑 $0 flash $name 烧一次固件，再回来跑这条命令。"
}

cmd_ports() {
  info "候选串口（usbmodem* 才是板子）"
  ls -1 /dev/cu.* 2>/dev/null | sed 's/^/  /' || echo "  (无)"
  echo
  info "arduino-cli 检测结果"
  arduino-cli board list 2>/dev/null | sed 's/^/  /'
}

# 打印文件开头那段注释当帮助。用 awk 而不是写死行号，
# 免得以后往头部加一行说明就漏掉一句。
usage() {
  awk 'NR > 2 { if ($0 !~ /^#/) exit; sub(/^# ?/, ""); print }' "$0"
}

case "${1:-compile}" in
  compile|build|"") cmd_compile "${2:-}" ;;
  upload)           cmd_upload  "${2:-}" ;;
  flash)            cmd_flash   "${2:-}" ;;
  slides|fs)        cmd_slides  "${2:-}" ;;
  mon|monitor)      cmd_mon ;;
  ports|list)       cmd_ports ;;
  help|-h|--help)   usage ;;
  *)                die "未知命令: $1（试试 $0 help）" ;;
esac
