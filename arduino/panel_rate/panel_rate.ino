/*
 * panel_rate —— 量出面板**真实**的刷新率到底是多少
 * ================================================================
 * 起因：关于这块屏的刷新率，手上有两个对不上的数 ——
 *
 *   「约 80 fps」   来自官方计数器 Demo 的串口输出
 *   「0.25~32 Hz」  来自第三方资料（见 Notes.md §1.1）
 *
 * 这俩其实在说**两件不同的事**，本程序就是为了把它们分开量出来：
 *
 *   push fps    = MCU 每秒能画完+推完几整帧（纯 SPI 吞吐，跟面板无关）
 *   TE Hz       = 面板每秒真正刷新几次画面  ← 这个才是「刷新率」
 *
 * 怎么量 TE：板子上的 **TE 引脚（GPIO6）** 是面板的帧同步输出，
 * **一个脉冲 = 一帧**。驱动初始化时就发了 0x35（Tearing Effect Line ON，
 * 参数 0x00 = 只输出垂直消隐，所以每帧恰好一个脉冲），只是从来没人读这个脚。
 * 拿中断数一秒有多少个脉冲，就是面板的真实刷新率 —— 不用查数据手册、不用猜。
 *
 * 屏幕上除了两个数字，还有一条来回扫的竖条：
 * **人眼判断「动起来顺不顺」比看数字直观**，两者可以互相印证。
 *
 * 串口（115200）每秒打印一行。板子没接的话什么都不会发生。
 */

#include "ST7305_U8g2.h"
#include "cn_text.h"       // 屏上的中文走点阵表，U8g2 自带字体不含 CJK

#define LCD_WIDTH 400
#define LCD_HEIGHT 300

#define RLCD_SCK_PIN 11
#define RLCD_MOSI_PIN 12
#define RLCD_DC_PIN 5
#define RLCD_CS_PIN 40
#define RLCD_RST_PIN 41

#define TE_PIN 6            // 面板的帧同步输出

static ST7305_U8g2 lcd(RLCD_SCK_PIN, RLCD_MOSI_PIN, RLCD_DC_PIN, RLCD_CS_PIN, RLCD_RST_PIN);
static U8G2 *u8g2 = nullptr;

// ---------------- TE 计数 ----------------

static volatile uint32_t te_count = 0;

static void IRAM_ATTR onTE()
{
  te_count++;
}

// 中断只加计数，读的时候原子取走
static uint32_t takeTeCount()
{
  noInterrupts();
  uint32_t n = te_count;
  te_count = 0;
  interrupts();
  return n;
}

// TE 的极性和上下拉在数据手册里没写清楚，硬编码一种可能什么都测不到。
// 所以四种组合都试一遍，哪个数得到脉冲就用哪个 —— 诊断工具就该自己找出路。
struct TeProbe {
  int edge;
  int pull;
  const char *name;
};

static const TeProbe probes[] = {
  { RISING,  INPUT,        "上升沿 / 无上下拉" },
  { FALLING, INPUT,        "下降沿 / 无上下拉" },
  { RISING,  INPUT_PULLUP, "上升沿 / 内部上拉" },
  { FALLING, INPUT_PULLUP, "下降沿 / 内部上拉" },
};
static const int PROBE_COUNT = sizeof(probes) / sizeof(probes[0]);
static int chosen = -1;          // 选中的那组，-1 = 还没找到

static uint32_t probeWindow(int i, uint32_t ms)
{
  pinMode(TE_PIN, probes[i].pull);
  attachInterrupt(digitalPinToInterrupt(TE_PIN), onTE, probes[i].edge);
  takeTeCount();                 // 清掉上一轮的残留
  delay(ms);
  uint32_t n = takeTeCount();
  detachInterrupt(digitalPinToInterrupt(TE_PIN));
  return n;
}

// ---------------- 屏幕 ----------------

static void drawStatus(float te_hz, float push_fps, float sweep)
{
  char buf[48];

  u8g2->clearBuffer();
  u8g2->setDrawColor(1);

  u8g2->setFont(u8g2_font_helvB12_tf);
  drawText(u8g2, 10, 20, "TE 是面板真实刷新率");

  u8g2->setFont(u8g2_font_logisoso38_tf);
  if (chosen >= 0) {
    snprintf(buf, sizeof(buf), "%.1f", te_hz);
    u8g2->drawStr(10, 76, buf);
  } else {
    u8g2->drawStr(10, 76, "--");
  }
  u8g2->setFont(u8g2_font_helvB12_tf);
  drawText(u8g2, 140, 76, "Hz 面板刷新率");

  u8g2->setFont(u8g2_font_logisoso38_tf);
  snprintf(buf, sizeof(buf), "%.0f", push_fps);
  u8g2->drawStr(10, 126, buf);
  u8g2->setFont(u8g2_font_helvB12_tf);
  drawText(u8g2, 140, 126, "fps MCU推帧率");

  drawText(u8g2, 10, 152, chosen >= 0 ? probes[chosen].name : "TE 没有信号");

  // 来回扫的竖条：人眼判断流畅度用。位置每帧推进，扫到边就掉头。
  int x = (int)sweep;
  u8g2->drawBox(x, 180, 8, 100);
  u8g2->drawFrame(0, 180, LCD_WIDTH, 100);

  u8g2->sendBuffer();
}

// ---------------- 主流程 ----------------

static uint32_t last_report_us = 0;
static uint32_t frames = 0;
static float sweep_pos = 0;
static float sweep_dir = 6;
static float te_hz = 0, push_fps = 0;

void setup()
{
  Serial.begin(115200);
  delay(300);
  Serial.println("\n=== panel_rate：量面板真实刷新率 ===");

  lcd.begin(0, U8G2_R1);
  u8g2 = lcd.getU8g2();

  // 面板要先跑起来才会有 TE 输出
  drawStatus(0, 0, 0);

  Serial.println("[te] 探测 TE 引脚（每种组合 300ms）...");
  for (int i = 0; i < PROBE_COUNT; i++) {
    uint32_t n = probeWindow(i, 300);
    Serial.printf("[te]   %-18s %5lu 个脉冲%s\n",
                  probes[i].name, (unsigned long)n, n ? "   ← 有信号" : "");
    if (n > 0 && chosen < 0) {
      chosen = i;
    }
  }

  if (chosen < 0) {
    Serial.println("[te] 四种组合都没数到脉冲。可能原因：");
    Serial.println("[te]   · 面板没被点亮（先确认屏幕上有字）");
    Serial.println("[te]   · TE 在这块板上没接到 GPIO6");
    Serial.println("[te]   · 面板处于睡眠/空闲模式，此时 TE 输出行为不同");
    // 不 return，继续跑，屏幕上会显示「TE 没信号」，至少 push fps 还能看
  } else {
    Serial.printf("[te] 用「%s」\n", probes[chosen].name);
    pinMode(TE_PIN, probes[chosen].pull);
    attachInterrupt(digitalPinToInterrupt(TE_PIN), onTE, probes[chosen].edge);
  }

  last_report_us = micros();
}

void loop()
{
  // 扫条推进。用 push 帧数而不是时间，这样两条曲线的对比更直观。
  sweep_pos += sweep_dir;
  if (sweep_pos >= LCD_WIDTH - 8) {
    sweep_pos = LCD_WIDTH - 8;
    sweep_dir = -sweep_dir;
  } else if (sweep_pos <= 0) {
    sweep_pos = 0;
    sweep_dir = -sweep_dir;
  }

  drawStatus(te_hz, push_fps, sweep_pos);
  frames++;

  uint32_t now = micros();
  uint32_t elapsed = now - last_report_us;
  if (elapsed >= 1000000UL) {
    uint32_t te = (chosen >= 0) ? takeTeCount() : 0;

    // 用实际经过的微秒数换算，不假设正好 1 秒
    te_hz    = te * 1e6f / elapsed;
    push_fps = frames * 1e6f / elapsed;

    Serial.printf("[rate] TE = %6.1f Hz    push = %6.1f fps", te_hz, push_fps);
    if (chosen >= 0 && te_hz > 0) {
      Serial.printf("    （每显示 1 帧，MCU 推了 %.1f 帧）", push_fps / te_hz);
    }
    Serial.println();

    frames = 0;
    last_report_us = now;
  }
}
