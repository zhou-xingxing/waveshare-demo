/*
 * ESP32-S3-RLCD-4.2  —  A 股行情看板（国内 API 版）
 * ================================================================
 * 流程：连 WiFi -> HTTPS GET 腾讯财经 -> 解析 -> 画到 1bpp 反射屏
 *
 * 最重要的一课在选 API 上：
 *   一开始用的是 CoinGecko（国外），板子没有代理根本连不上。
 *   实测被 DNS 污染，解析出来的是 0.0.0.0 / 169.254.x.x / Twitter 网段的假 IP。
 *   换成腾讯财经，墙内直连，稳定可用。
 *   ⚠️ 换新接口之前，先用 ../api_probe 在**板子上**实测可达性，
 *      别用开发机的 curl 下结论（开发机有代理）。详见 README 和 Notes.md §5.3。
 *
 * 需要安装的库：
 *   - U8g2        (by oliver)
 *   - ArduinoJson (by Benoit Blanchon)  v7.x
 * 另外需要本目录下的 ST7305_U8g2.h / ST7305_U8g2.cpp
 *
 * 装环境 + 开发板设置见同目录 README.md 的「〇」节 —— PSRAM 那项选错会直接重启！
 */

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>

#include "ST7305_U8g2.h"
#include "cn_text.h"  // 中英混排：drawText() / textWidth()

// ======================= 改这里 =======================
// WiFi 凭据：复制 secrets.h.example 成 secrets.h 填自己的（不进版本库）
#include "secrets.h"

// 要显示哪几个标的。第一个是主标的（大字 + K线折线图），后面几个各占一行。
//   代码前缀：sh = 上交所，sz = 深交所
//   指数：sh000001 上证指数   sz399001 深证成指   sz399006 创业板指
//   个股：sh600519 贵州茅台   sz000001 平安银行   sh601318 中国平安
//   基金/ETF 也可以：sh510300 沪深300ETF
#define SYMBOL_COUNT 3
static const char *SYMBOLS[SYMBOL_COUNT] = {"sh000001", "sz399001", "sz399006"};

// 屏上显示的标签。现在可以直接写中文，汉字点阵由 cn_font_data.h 提供。
//
// ⚠️ 改了这里的中文之后，**必须重跑一次生成脚本**，否则新字会显示成空框：
//       python3 tools/gen_cn_font.py arduino/rlcd_stock_demo
//
// 顺带说明：接口其实也返回中文名，但那是 GBK 编码，直接拿来会乱码，
// 而且换标的就得重新处理编码，所以这里还是自己写死。
static const char *TAGS[SYMBOL_COUNT] = {"上证指数", "深证成指", "创业板指"};

#define KLINE_DAYS 60  // 折线图取最近多少个交易日

const uint32_t REFRESH_MS = 60000;  // 多久拉一次新数据：60 秒
const uint32_t TICK_MS = 1000;      // 多久重画一次倒计时：1 秒
// =====================================================

#define RLCD_SCK_PIN 11
#define RLCD_MOSI_PIN 12
#define RLCD_DC_PIN 5
#define RLCD_CS_PIN 40
#define RLCD_RST_PIN 41

#define LCD_WIDTH 400
#define LCD_HEIGHT 300

#define KLINE_MAX 120  // K 线条数上限

// _tf = 完整 ASCII 字符集（含逗号 $）。别用 _tn，那种画不出千分位逗号。
#define FONT_BIG u8g2_font_logisoso50_tf
#define FONT_MID u8g2_font_helvB14_tf
#define FONT_TXT u8g2_font_helvB12_tf
#define FONT_SMALL u8g2_font_6x13_tf

static ST7305_U8g2 lcd(RLCD_SCK_PIN, RLCD_MOSI_PIN, RLCD_DC_PIN, RLCD_CS_PIN, RLCD_RST_PIN);
static U8G2 *g = nullptr;

// 一个标的的行情
struct Quote {
  const char *tag;
  bool ok;
  float price;      // 现价
  float prevClose;  // 昨收
  float change;     // 涨跌额
  float changePct;  // 涨跌幅 %
  char time[16];    // 行情时间 "0924 16:14"
};

static Quote quotes[SYMBOL_COUNT];

// 主标的的 K 线收盘价（画折线用）
static float kline[KLINE_MAX];
static int klineN = 0;
static float klineLo = 0, klineHi = 0;

static bool haveData = false;
static char lastErr[48] = "";
static uint32_t lastFetch = 0;
static uint32_t lastDraw = 0;
// 时间与市场状态的实现放在后面的「时间与市场状态」一节，
// C++ 要求先声明后用，而 showStock() 在前面就要用到它俩，所以这里先打招呼。
static bool timeSynced = false;
static const char *marketStatus();

// ---------------------------------------------------------------- 小工具

// 数字转带千分位的字符串：3888.37 -> "3,888.37"
static void formatMoney(char *out, size_t outSize, float v, int decimals) {
  char raw[32];
  snprintf(raw, sizeof(raw), "%.*f", decimals, v);

  const char *dot = strchr(raw, '.');
  int intLen = dot ? (int)(dot - raw) : (int)strlen(raw);

  char *o = out;
  char *end = out + outSize - 1;
  for (int i = 0; i < intLen && o < end; i++) {
    *o++ = raw[i];
    int remaining = intLen - 1 - i;
    if (remaining > 0 && remaining % 3 == 0 && o < end) *o++ = ',';
  }
  if (dot) {
    while (*dot && o < end) *o++ = *dot++;
  }
  *o = '\0';
}

static int decimalsFor(float v) { return v >= 1000.0f ? 2 : 3; }

// 以屏幕水平中心为基准画一行（中英混排都支持）
static void drawCentered(int y, const char *s) {
  int w = textWidth(g, s);
  int x = (LCD_WIDTH - w) / 2;
  drawText(g, x < 0 ? 0 : x, y, s);
}

// 实心三角，涨跌指示用。画出来比字符 +- 醒目得多。
static void drawArrow(int x, int baseline, bool up) {
  if (up) {
    g->drawTriangle(x, baseline - 1, x + 10, baseline - 1, x + 5, baseline - 12);
  } else {
    g->drawTriangle(x, baseline - 12, x + 10, baseline - 12, x + 5, baseline - 1);
  }
}

static void showMessage(const char *title, const char *l1, const char *l2, const char *l3) {
  g->clearBuffer();
  g->setDrawColor(1);

  g->setFont(FONT_MID);
  drawCentered(46, title);
  g->drawHLine(16, 60, LCD_WIDTH - 32);

  g->setFont(FONT_SMALL);
  if (l1) drawCentered(100, l1);
  if (l2) drawCentered(126, l2);
  if (l3) drawCentered(152, l3);

  g->sendBuffer();
}

// ---------------------------------------------------------------- 画图

// 把收盘价序列画成折线。和加密货币版是同一个函数，逻辑完全通用。
static void drawSparkline(int x, int y, int w, int h) {
  if (klineN < 2 || w < 2) return;

  float span = klineHi - klineLo;
  if (span <= 0) span = 1;

  // 虚线 = 60 天前的收盘价，折线在它上面就是这段时间涨了
  int yRef = y + h - (int)((kline[0] - klineLo) / span * (float)h);
  for (int px = 0; px < w; px += 5) g->drawHLine(x + px, yRef, 2);

  int prevX = 0, prevY = 0;
  for (int px = 0; px < w; px++) {
    int idx = (int)((long)px * (klineN - 1) / (w - 1));
    int py = y + h - (int)((kline[idx] - klineLo) / span * (float)h);
    if (py < y) py = y;
    if (py > y + h) py = y + h;

    if (px > 0) g->drawLine(prevX, prevY, x + px, py);
    prevX = x + px;
    prevY = py;
  }

  g->drawFrame(x, y, w, h);
}

static void showStock() {
  char buf[64], money[32];

  g->clearBuffer();
  g->setDrawColor(1);

  // ---- 顶部：标题 + 市场状态
  g->setFont(FONT_MID);
  drawText(g, 16, 24, "A股行情");

  g->setFont(FONT_SMALL);
  if (timeSynced) {
    // 已对时：显示市场状态 + **当前日期时间**（板子自己的钟，不是数据时间）。
    // 格式和右下角的「数据 MM-DD HH:MM」刻意保持一致 —— 这样两个时间能直接对比，
    // 一眼看出数据是今天还是隔了好几天（休市时就是这个效果）。
    time_t nowT = time(nullptr);
    struct tm tmv;
    localtime_r(&nowT, &tmv);
    snprintf(buf, sizeof(buf), "%s %02d-%02d %02d:%02d", marketStatus(),
             tmv.tm_mon + 1, tmv.tm_mday, tmv.tm_hour, tmv.tm_min);
  } else if (haveData && quotes[0].ok && strlen(quotes[0].time) >= 12) {
    // 没对上时：老实承认不知道现在几点，只报数据时间（右下角也有同样的标注）
    const char *t = quotes[0].time;
    snprintf(buf, sizeof(buf), "数据 %c%c-%c%c %c%c:%c%c", t[4], t[5], t[6], t[7],
             t[8], t[9], t[10], t[11]);
  } else {
    snprintf(buf, sizeof(buf), "获取中...");
  }
  drawText(g, LCD_WIDTH - 16 - textWidth(g, buf), 24, buf);

  g->drawHLine(16, 34, LCD_WIDTH - 32);

  // ---- 主标的大字
  Quote &q0 = quotes[0];
  if (q0.ok) {
    formatMoney(money, sizeof(money), q0.price, decimalsFor(q0.price));
    g->setFont(FONT_BIG);
    drawText(g, 16, 100, money);

    // 涨跌：涨画上三角，跌画下三角
    drawArrow(18, 128, q0.changePct >= 0);
    g->setFont(FONT_TXT);
    snprintf(buf, sizeof(buf), "%+.2f   %+.2f%%", q0.change, q0.changePct);
    drawText(g, 36, 128, buf);

    // ---- 60 日 K 线
    drawSparkline(16, 140, LCD_WIDTH - 32, 66);

    // ---- 区间说明（中文 + 数字混排）
    char lo[24], hi[24];
    formatMoney(lo, sizeof(lo), klineLo, decimalsFor(klineLo));
    formatMoney(hi, sizeof(hi), klineHi, decimalsFor(klineHi));
    g->setFont(FONT_SMALL);
    snprintf(buf, sizeof(buf), "%d日区间  %s - %s", KLINE_DAYS, lo, hi);
    drawText(g, 16, 224, buf);

    // 右下角：**行情数据的时间戳**，和顶部的「当前时间」是两个不同的概念。
    // 前缀「数据」不能省 —— 休市时这个时间会连着好几天不动，
    // 不标注清楚就会被当成时钟坏了（真被这么误会过）。
    if (strlen(quotes[0].time) >= 12) {
      const char *t = quotes[0].time;
      snprintf(buf, sizeof(buf), "数据 %c%c-%c%c %c%c:%c%c", t[4], t[5], t[6], t[7],
               t[8], t[9], t[10], t[11]);
      drawText(g, LCD_WIDTH - 16 - textWidth(g, buf), 224, buf);
    }
  } else {
    g->setFont(FONT_MID);
    drawText(g, 16, 100, "无数据");
    g->setFont(FONT_SMALL);
    drawText(g, 16, 130, lastErr);
  }

  g->drawHLine(16, 234, LCD_WIDTH - 32);

  // ---- 其余标的各占一行
  for (int i = 1; i < SYMBOL_COUNT; i++) {
    Quote &q = quotes[i];
    if (!q.ok) continue;

    int y = 258 + (i - 1) * 30;

    g->setFont(FONT_MID);
    drawText(g, 16, y, q.tag);

    formatMoney(money, sizeof(money), q.price, decimalsFor(q.price));
    int pw = textWidth(g, money);
    drawText(g, 250 - pw, y, money);

    drawArrow(272, y, q.changePct >= 0);
    snprintf(buf, sizeof(buf), "%+.2f%%", q.changePct);
    drawText(g, 290, y, buf);
  }

  g->sendBuffer();
}

// ---------------------------------------------------------------- 网络

// 腾讯行情接口返回的**不是 JSON**，是这种一行行的文本（而且是 GBK 编码）：
//     v_sh000001="1~上证指数~000001~3888.37~3936.52~3925.32~...";
//     v_sz399001="51~深证成指~399001~13316.97~...";
// 字段用 ~ 分隔。中文名是 GBK，屏上显示不了，所以只用数字字段。
//
// 取第 idx 个 ~ 分隔的字段（下标从 0 开始）
static bool pickField(const String &body, int idx, char *out, size_t outSize) {
  int cur = 0, start = 0;
  int len = body.length();
  for (int i = 0; i <= len; i++) {
    if (i == len || body[i] == '~') {
      if (cur == idx) {
        body.substring(start, i).toCharArray(out, outSize);
        return true;
      }
      cur++;
      start = i + 1;
    }
  }
  return false;
}

// 一次请求拿回全部标的（腾讯支持用逗号分隔）
static String quoteUrl() {
  String u = "https://qt.gtimg.cn/q=";
  for (int i = 0; i < SYMBOL_COUNT; i++) {
    if (i) u += ",";
    u += SYMBOLS[i];
  }
  return u;
}

// 字段下标（0 开始，实测数出来的）：
//   1=名字(GBK) 3=现价 4=昨收 5=今开 30=时间 31=涨跌额 32=涨跌幅% 33=最高 34=最低
//
// ⚠️ 坑：下标 29 是个**空字段**（腾讯返回里有个 "~~"）。肉眼数下标非常容易
// 差一位，症状就是涨跌幅显示成了涨跌额。要核对下标就用：
//   curl -s 'https://qt.gtimg.cn/q=sh000001' | iconv -f GBK -t UTF-8 \
//     | python3 -c "import sys; print(list(enumerate(sys.stdin.read().split(chr(34))[1].split('~'))))"
#define F_PRICE 3
#define F_PREVCLOSE 4
#define F_TIME 30
#define F_CHANGE 31
#define F_CHANGEPCT 32

static bool fetchQuotes() {
  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  http.setConnectTimeout(8000);
  http.setTimeout(8000);
  http.setUserAgent("ESP32-S3-RLCD-4.2/1.0");

  String url = quoteUrl();
  if (!http.begin(client, url)) {
    snprintf(lastErr, sizeof(lastErr), "http.begin failed");
    return false;
  }

  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    snprintf(lastErr, sizeof(lastErr), "HTTP %d", code);
    http.end();
    return false;
  }

  String payload = http.getString();
  http.end();

  for (int i = 0; i < SYMBOL_COUNT; i++) quotes[i].ok = false;

  // 按行拆，每行一个标的
  int pos = 0;
  int total = payload.length();
  while (pos < total) {
    int nl = payload.indexOf('\n', pos);
    if (nl < 0) nl = total;
    String line = payload.substring(pos, nl);
    pos = nl + 1;

    int eq = line.indexOf('=');
    int q1 = line.indexOf('"');
    int q2 = line.lastIndexOf('"');
    if (eq < 1 || q1 < 0 || q2 <= q1) continue;

    String code = line.substring(2, eq);         // 去掉开头的 "v_"
    code.trim();
    String body = line.substring(q1 + 1, q2);     // 去掉两边的引号

    for (int i = 0; i < SYMBOL_COUNT; i++) {
      if (!code.equalsIgnoreCase(SYMBOLS[i])) continue;

      char f[32];
      if (!pickField(body, F_PRICE, f, sizeof(f))) break;
      quotes[i].price = atof(f);

      if (pickField(body, F_PREVCLOSE, f, sizeof(f))) quotes[i].prevClose = atof(f);
      if (pickField(body, F_CHANGE, f, sizeof(f))) quotes[i].change = atof(f);
      if (pickField(body, F_CHANGEPCT, f, sizeof(f))) quotes[i].changePct = atof(f);

      // 时间字段是 14 位数字，原样留着，画屏时再掐
      if (pickField(body, F_TIME, f, sizeof(f))) {
        snprintf(quotes[i].time, sizeof(quotes[i].time), "%s", f);
      }
      quotes[i].ok = true;
      break;
    }
  }

  if (!quotes[0].ok) {
    snprintf(lastErr, sizeof(lastErr), "main symbol not found");
    return false;
  }
  return true;
}

static bool fetchKline() {
  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  http.setConnectTimeout(8000);
  http.setTimeout(8000);
  http.setUserAgent("ESP32-S3-RLCD-4.2/1.0");

  String url = String("https://web.ifzq.gtimg.cn/appstock/app/fqkline/get?param=") +
               SYMBOLS[0] + ",day,,," + KLINE_DAYS + ",qfq";

  if (!http.begin(client, url)) {
    snprintf(lastErr, sizeof(lastErr), "kline begin failed");
    return false;
  }

  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    snprintf(lastErr, sizeof(lastErr), "kline HTTP %d", code);
    http.end();
    return false;
  }

  String payload = http.getString();
  http.end();

  JsonDocument doc;
  if (deserializeJson(doc, payload)) {
    snprintf(lastErr, sizeof(lastErr), "kline json error");
    return false;
  }

  JsonObject d = doc["data"][SYMBOLS[0]];
  if (d.isNull()) {
    snprintf(lastErr, sizeof(lastErr), "kline no data");
    return false;
  }

  // ⚠️ 坑：个股的字段叫 qfqday，指数叫 day。写死一个另一种就取到空。
  JsonArray rows = d["day"];
  if (rows.isNull()) rows = d["qfqday"];
  if (rows.isNull()) {
    snprintf(lastErr, sizeof(lastErr), "kline no day array");
    return false;
  }

  // 每行是 [日期, 开, 收, 高, 低, 量]，收盘价在下标 2
  klineN = 0;
  klineLo = 1e30f;
  klineHi = -1e30f;
  // 注意遍历变量的类型写 JsonVariant（迭代器实际返回的就是它）
  for (JsonVariant row : rows) {
    if (klineN >= KLINE_MAX) break;
    if (row.size() < 3) continue;
    float close = row[2].as<float>();
    if (close <= 0) continue;
    kline[klineN++] = close;
    if (close < klineLo) klineLo = close;
    if (close > klineHi) klineHi = close;
  }

  if (klineN < 2) {
    snprintf(lastErr, sizeof(lastErr), "kline too few rows");
    return false;
  }
  return true;
}

// ---------------------------------------------------------------- 时间与市场状态

// 北京时间。中国不用夏令时，所以时区偏移固定 +8 小时。
#define TZ_OFFSET_SEC (8 * 3600)

// （timeSynced 的声明在文件开头的全局区，因为 showStock() 要用到）

// NTP 对时。**服务器必须选国内的** —— 跟行情接口一个道理，
// pool.ntp.org 之类在墙内大概率同步不上。
static bool syncTime() {
  configTime(TZ_OFFSET_SEC, 0, "ntp.aliyun.com", "ntp.tencent.com", "cn.pool.ntp.org");

  // configTime 不阻塞，是后台慢慢同步的。这里最多等 10 秒。
  // 同步成功前 time(nullptr) 会返回 0 或一个很小的值。
  uint32_t t0 = millis();
  time_t now = 0;
  while (millis() - t0 < 10000) {
    now = time(nullptr);
    if (now > 1600000000) {  // 2020-09-13 之后的才算合理
      timeSynced = true;
      struct tm tmv;
      localtime_r(&now, &tmv);
      char buf[32];
      strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tmv);
      Serial.printf("[ntp] 对时成功: %s\n", buf);
      return true;
    }
    delay(200);
  }
  Serial.println("[ntp] 对时失败（超时）");
  return false;
}

// 市场状态。
//
// **刻意不维护节假日表** —— 一年改一次太容易忘，而且调休还得跟着更新。
// 改用「数据反推」：如果今天交易过，行情数据的日期必然是今天；
// 数据日期不是今天，就说明今天没开盘。
//
//   交易中/已收盘  <-  数据日期 == 今天，再看当前时刻落在哪个时段
//   休市中        <-  数据日期 != 今天，工作日且已过 9:30（= 节假日）
//   周末休市      <-  看星期几
//   未开盘        <-  数据日期 != 今天，且还没到 9:30
//
// 好处是一年到头不用动。代价是**依赖接口的日期准确性**，
// 如果接口自己返回了陈旧数据，这里会误判成休市。
static const char *marketStatus() {
  if (!timeSynced) return "未对时";

  time_t now = time(nullptr);
  struct tm tmv;
  localtime_r(&now, &tmv);

  int hhmm = tmv.tm_hour * 100 + tmv.tm_min;
  bool weekend = (tmv.tm_wday == 0 || tmv.tm_wday == 6);

  // 行情数据里的日期。"20260924161401" 前 8 位是 yyyymmdd
  bool dataToday = false;
  const char *t = quotes[0].time;
  if (quotes[0].ok && strlen(t) >= 8) {
    int dY = (t[0] - '0') * 1000 + (t[1] - '0') * 100 + (t[2] - '0') * 10 + (t[3] - '0');
    int dM = (t[4] - '0') * 10 + (t[5] - '0');
    int dD = (t[6] - '0') * 10 + (t[7] - '0');
    dataToday = (dY == tmv.tm_year + 1900 && dM == tmv.tm_mon + 1 && dD == tmv.tm_mday);
  }

  if (dataToday) {
    if (hhmm < 915)  return "未开盘";    // 9:15 前（集合竞价都没开始）
    if (hhmm < 1130) return "交易中";    // 含 9:15-9:25 集合竞价
    if (hhmm < 1300) return "午间休市";
    if (hhmm < 1500) return "交易中";
    return "已收盘";
  }

  if (weekend) return "周末休市";        // 周末且没数据，正常
  if (hhmm < 930) return "未开盘";       // 工作日但还没开盘
  return "休市中";                       // 工作日、开盘时间却没数据 = 节假日
}

static bool connectWiFi() {
  char line[64];

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  showMessage("连接 WiFi", WIFI_SSID, "", "");

  uint32_t t0 = millis();
  int dots = 0;
  while (WiFi.status() != WL_CONNECTED) {
    if (millis() - t0 > 20000) {
      showMessage("WiFi 连接失败", "检查名称和密码", "以及是不是 2.4G", "");
      return false;
    }
    dots = (dots + 1) % 4;
    snprintf(line, sizeof(line), "%.*s", dots, "...");
    showMessage("连接 WiFi", WIFI_SSID, line, "");
    delay(400);
  }

  snprintf(line, sizeof(line), "IP %s", WiFi.localIP().toString().c_str());
  showMessage("WiFi 已连接", line, "正在获取行情", "");
  Serial.printf("[wifi] connected, IP=%s\n", WiFi.localIP().toString().c_str());
  return true;
}

// ---------------------------------------------------------------- 主流程

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("\n=== RLCD-4.2 A-share demo ===");

  for (int i = 0; i < SYMBOL_COUNT; i++) {
    quotes[i].tag = TAGS[i];
    quotes[i].ok = false;
  }

  lcd.begin(0, U8G2_R1);  // R1 = 横屏 400x300
  g = lcd.getU8g2();
  showMessage("RLCD-4.2", "屏幕初始化完成", "正在连接 WiFi", "");

  if (connectWiFi()) {
    showMessage("正在对时", "ntp.aliyun.com", "", "");
    syncTime();  // 对不上时也不致命，marketStatus() 会降级成「未对时」
  }

  lastFetch = 0;
  lastDraw = 0;
}

void loop() {
  uint32_t now = millis();

  // 开机时没对上时，这里看后台有没有补上。
  //
  // ⚠️ 这里**故意不调用 syncTime()**：它会阻塞最多 10 秒，每分钟卡一次屏幕很难看。
  // configTime() 已经让 SNTP 在后台自己重试了，我们只看结果就行，不主动催。
  if (!timeSynced && WiFi.status() == WL_CONNECTED && time(nullptr) > 1600000000) {
    timeSynced = true;
    Serial.println("[ntp] 后台对时完成");
  }

  bool needFetch = (lastFetch == 0) || (now - lastFetch >= REFRESH_MS);
  bool needRedraw = (lastDraw == 0) || (now - lastDraw >= TICK_MS);

  if (needFetch) {
    if (WiFi.status() != WL_CONNECTED) {
      showMessage("WiFi 断开", "正在重连", "", "");
      if (!connectWiFi()) {
        delay(5000);
        lastFetch = 0;
        return;
      }
    }

    if (fetchQuotes() && fetchKline()) {
      haveData = true;
      for (int i = 0; i < SYMBOL_COUNT; i++) {
        if (quotes[i].ok) {
          Serial.printf("[api] %-8s price %9.2f  chg %+7.2f  pct %+6.2f%%  time %s\n",
                        quotes[i].tag, quotes[i].price, quotes[i].change,
                        quotes[i].changePct, quotes[i].time);
        }
      }
      Serial.printf("[api] kline %d rows, %.2f ~ %.2f\n", klineN, klineLo, klineHi);

      // 把「现在几点 + 判成了什么状态 + 数据是哪天的」打出来。
      // 屏幕上显示的就是这几个值，打出来省得靠猜。
      if (timeSynced) {
        time_t nowT = time(nullptr);
        struct tm tmv;
        localtime_r(&nowT, &tmv);
        Serial.printf("[status] 现在 %04d-%02d-%02d %02d:%02d  数据 %s  市场 %s\n",
                      tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday, tmv.tm_hour,
                      tmv.tm_min, quotes[0].time, marketStatus());
      } else {
        Serial.println("[status] 未对时");
      }
    } else {
      Serial.printf("[api] failed: %s\n", lastErr);
      showMessage("请求失败", lastErr, "10 秒后重试", "");
      delay(10000);
      lastFetch = 0;
      lastDraw = 0;
      return;
    }

    lastFetch = now;
    needRedraw = true;
  }

  if (needRedraw) {
    showStock();
    lastDraw = millis();
  }

  delay(50);
}
