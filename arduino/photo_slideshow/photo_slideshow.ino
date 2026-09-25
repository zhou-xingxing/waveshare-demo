/*
 * photo_slideshow —— 照片幻灯片，把主机上转好的 1bpp 图轮播到反射屏上
 * ================================================================
 * 三步流程：
 *
 *     python3 tools/img2slides.py 我的照片/ -o arduino/photo_slideshow/data
 *     ./build.sh flash photo_slideshow
 *     ./build.sh slides photo_slideshow
 *
 * 屏幕只有黑白两色，照片必须先转成 1bpp 才能显示 —— 那是整套流程里**最关键的一步**，
 * 为什么这么转、每一步为什么不能省，见 tools/img2slides.py 开头的长篇说明。
 *
 * 本程序要的数据格式：裸 XBM。每张 (400/8)×300 = 15000 字节，行优先，
 * 字节内 **LSB 优先**（bit0 = 最左像素）。U8g2 的 drawXBM() 直接吃这个格式。
 * 横屏是 U8G2_R1 转出来的，所以转换脚本按 400×300 横向构图就行，不用管旋转。
 *
 * 图片存在 LittleFS 里。本目录下的 partitions.csv 会覆盖分区表，
 * 给 LittleFS 划出 9.875 MB，够放约 690 张。
 *
 * 串口（115200）能敲：
 *     n        立刻看下一张
 *     +  -     把停留时间调长 / 调短（1~60 秒）
 *
 * 需要安装的库：U8g2（和别的 Demo 一样）
 */

#include <LittleFS.h>

#include "ST7305_U8g2.h"
#include "cn_text.h"       // 汉字点阵，见 README「五、屏幕上为什么有中文」

#define LCD_WIDTH 400
#define LCD_HEIGHT 300

#define RLCD_SCK_PIN 11
#define RLCD_MOSI_PIN 12
#define RLCD_DC_PIN 5
#define RLCD_CS_PIN 40
#define RLCD_RST_PIN 41

// 一张图多少字节。必须和 tools/img2slides.py 里的 WIDTH/HEIGHT 一致，
// 不一致会在下面读文件时按大小校验挡下来。
#define SLIDE_BYTES (((LCD_WIDTH + 7) / 8) * LCD_HEIGHT)

#define SLIDE_SECONDS 5         // 每张停留几秒，串口 + / - 可以临时改
#define MAX_SLIDES 200          // 名字表的容量。超了会提示，改大就行
#define NAME_LEN 32

static ST7305_U8g2 lcd(RLCD_SCK_PIN, RLCD_MOSI_PIN, RLCD_DC_PIN, RLCD_CS_PIN, RLCD_RST_PIN);
static U8G2 *u8g2 = nullptr;

// 整张图一次性读进来。静态分配，不走堆 —— 15 KB 而已，省得处理分配失败。
static uint8_t slide_buf[SLIDE_BYTES];

static char names[MAX_SLIDES][NAME_LEN];
static int slide_count = 0;
static int cur = -1;
static uint32_t slide_secs = SLIDE_SECONDS;
static uint32_t last_switch_ms = 0;
static bool force_next = false;

// ---------------- 画点提示文字 ----------------

static void showNotice(const char *line1, const char *line2 = nullptr)
{
  u8g2->clearBuffer();
  u8g2->setDrawColor(1);
  u8g2->setFont(u8g2_font_helvB14_tf);

  int w1 = textWidth(u8g2, line1);
  drawText(u8g2, (LCD_WIDTH - w1) / 2, 140, line1);
  if (line2) {
    int w2 = textWidth(u8g2, line2);
    drawText(u8g2, (LCD_WIDTH - w2) / 2, 166, line2);
  }
  u8g2->sendBuffer();
}

// ---------------- 列文件 ----------------

// arduino-esp32 的 File::name() 在不同版本里有时带路径有时只有文件名，
// 统一砍成纯文件名，后面拼回去的时候才不会拼出 "//xxx"。
static void toBasename(char *s)
{
  char *slash = strrchr(s, '/');
  if (slash) {
    memmove(s, slash + 1, strlen(slash + 1) + 1);
  }
}

static void scanSlides()
{
  File root = LittleFS.open("/");
  if (!root) {
    Serial.println("[slide] 打不开 LittleFS 根目录");
    return;
  }

  File f = root.openNextFile();
  while (f && slide_count < MAX_SLIDES) {
    if (!f.isDirectory()) {
      char name[NAME_LEN];
      snprintf(name, sizeof(name), "%s", f.name());
      toBasename(name);
      size_t len = strlen(name);
      // 只收 .bin，忽略 .DS_Store 之类
      if (len > 4 && strcasecmp(name + len - 4, ".bin") == 0) {
        snprintf(names[slide_count], NAME_LEN, "%s", name);
        slide_count++;
      }
    }
    f = root.openNextFile();
  }
  root.close();

  // 文件名是 slide_000 / slide_001 … 补零的，所以字典序就是播放顺序。
  // 用最笨的插入排序 —— 200 个元素的规模，没必要上 qsort。
  for (int i = 1; i < slide_count; i++) {
    char key[NAME_LEN];
    snprintf(key, sizeof(key), "%s", names[i]);
    int j = i - 1;
    while (j >= 0 && strcmp(names[j], key) > 0) {
      snprintf(names[j + 1], NAME_LEN, "%s", names[j]);
      j--;
    }
    snprintf(names[j + 1], NAME_LEN, "%s", key);
  }

  if (slide_count == MAX_SLIDES) {
    Serial.printf("[slide] ⚠️ 只取了前 %d 张，改大 MAX_SLIDES 可以放更多\n", MAX_SLIDES);
  }
}

// ---------------- 显示一张 ----------------

static bool showSlide(int idx)
{
  char path[NAME_LEN + 2];
  snprintf(path, sizeof(path), "/%s", names[idx]);

  File f = LittleFS.open(path, "r");
  if (!f) {
    Serial.printf("[slide] 打不开 %s\n", path);
    return false;
  }

  // 大小对不上说明转换脚本和这里的尺寸不一致（比如改了 --size），
  // 直接跳过而不是画一张花屏出来
  if ((size_t)f.size() != SLIDE_BYTES) {
    Serial.printf("[slide] %s 大小 %u 字节，应该是 %u —— 重跑 img2slides.py\n",
                  path, (unsigned)f.size(), (unsigned)SLIDE_BYTES);
    f.close();
    return false;
  }

  size_t got = f.read(slide_buf, SLIDE_BYTES);
  f.close();
  if (got != SLIDE_BYTES) {
    Serial.printf("[slide] %s 只读到 %u 字节\n", path, (unsigned)got);
    return false;
  }

  // 三段分开计时。「画」是纯 CPU（U8g2 逐位扫 120000 个像素），
  // 「送屏」是 SPI 传输，两者瓶颈完全不同，混在一起看不出该优化谁。
  uint32_t t0 = micros();
  u8g2->clearBuffer();
  u8g2->setDrawColor(1);
  // 透明模式：0 的位置不画。上面刚 clearBuffer 过，所以直接跳过 0 位
  // 省掉一半的活；不透明的话每张图会多写一遍背景。
  u8g2->setBitmapMode(1);
  uint32_t t1 = micros();
  u8g2->drawXBM(0, 0, LCD_WIDTH, LCD_HEIGHT, slide_buf);
  u8g2->setBitmapMode(0);
  uint32_t t2 = micros();
  u8g2->sendBuffer();
  uint32_t t3 = micros();

  Serial.printf("[slide] %d/%d  %-16s  清屏 %lu  画图 %lu  送屏 %lu us\n",
                idx + 1, slide_count, names[idx],
                (unsigned long)(t1 - t0), (unsigned long)(t2 - t1),
                (unsigned long)(t3 - t2));
  return true;
}

static void advance()
{
  if (slide_count == 0) {
    return;
  }
  // 最多绕一圈，避免某些文件坏掉时死循环
  for (int tries = 0; tries < slide_count; tries++) {
    cur = (cur + 1) % slide_count;
    if (showSlide(cur)) {
      last_switch_ms = millis();
      return;
    }
  }
  Serial.println("[slide] 一张都显示不出来，检查一下 data/ 里的文件");
}

// ---------------- 串口控制 ----------------

static void pollSerial()
{
  while (Serial.available()) {
    int c = Serial.read();
    if (c == 'n' || c == 'N') {
      force_next = true;
    } else if (c == '+') {
      if (slide_secs < 60) {
        slide_secs++;
      }
      Serial.printf("[slide] 停留 %lu 秒\n", (unsigned long)slide_secs);
    } else if (c == '-') {
      if (slide_secs > 1) {
        slide_secs--;
      }
      Serial.printf("[slide] 停留 %lu 秒\n", (unsigned long)slide_secs);
    }
  }
}

// ---------------- 主流程 ----------------

void setup()
{
  Serial.begin(115200);
  delay(300);
  Serial.println("\n=== RLCD-4.2 photo slideshow ===");

  lcd.begin(0, U8G2_R1);
  u8g2 = lcd.getU8g2();

  // 不用 formatOnFail：分区是空的八成是因为还没上传过图片，
  // 这时候格式化只会掩盖问题，不如把话说明白。
  // label "slides" 必须和同目录 partitions.csv 里的分区名一致。
  if (!LittleFS.begin(false, "/littlefs", 10, "slides")) {
    Serial.println("[slide] LittleFS 挂载失败");
    // 顺序很重要：分区表是跟着固件一起烧进去的（烧录时会写 0x8000 那张表）。
    // 所以板子上如果还是旧分区表，这里必然找不到 slides 分区 —— 得先烧固件，
    // 再传图片。反过来做的话图片写进去了也没人认。
    Serial.println("[slide] 按顺序检查：");
    Serial.println("[slide]   1) 先烧固件（会一起写入新的分区表）：./build.sh flash photo_slideshow");
    Serial.println("[slide]   2) 再传图片：./build.sh slides photo_slideshow");
    showNotice("文件系统没挂上", "先烧固件再传图");
    return;
  }

  scanSlides();
  if (slide_count == 0) {
    Serial.println("[slide] 没找到任何 .bin");
    Serial.println("[slide] 先转图：python3 tools/img2slides.py <照片目录>");
    Serial.println("[slide] 再上传：./build.sh slides photo_slideshow");
    showNotice("没有找到图片", "跑 img2slides.py");
    return;
  }

  Serial.printf("[slide] 找到 %d 张，每张停留 %lu 秒\n",
                slide_count, (unsigned long)slide_secs);
  Serial.println("[slide] 串口敲 n = 下一张，+ / - 调停留时间");
  advance();
}

void loop()
{
  if (slide_count == 0) {
    delay(1000);
    return;
  }

  pollSerial();

  bool due = force_next || (millis() - last_switch_ms >= slide_secs * 1000UL);
  if (due) {
    force_next = false;
    advance();
  }

  delay(20);
}
