/*
 * 行情 API 可达性探测
 * ================================================================
 * 板子在墙内、没有代理，哪些行情接口能直连？
 * 挨个试一遍打张表出来，省得一个个改代码试。
 *
 * 每个目标测两件事：
 *   1. DNS 能不能解析出 IP（解析不出来 / 结果可疑 = 被污染）
 *   2. HTTPS GET 能不能拿到响应
 *
 *     ../../build.sh flash api_probe
 *     ../../build.sh mon
 *
 * 换项目时改下面的 PROBES 表就行。
 */

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>

#include "secrets.h"  // WiFi 凭据：复制 secrets.h.example 过来填自己的（不进版本库）

// ======================= 要探测的接口 =======================
struct Probe {
  const char *tag;   // 显示名
  const char *host;  // 用来测 DNS
  const char *url;   // 用来测 HTTPS
};

static const Probe PROBES[] = {
    // ---- 加密货币（都在国外，大概率被墙）----
    {"coingecko", "api.coingecko.com",
     "https://api.coingecko.com/api/v3/ping"},
    {"binance", "api.binance.com",
     "https://api.binance.com/api/v3/ping"},
    {"binance-vision", "data-api.binance.vision",
     "https://data-api.binance.vision/api/v3/ticker/price?symbol=BTCUSDT"},
    {"okx", "www.okx.com",
     "https://www.okx.com/api/v5/market/ticker?instId=BTC-USDT"},
    {"huobi", "api.huobi.pro",
     "https://api.huobi.pro/market/detail/merged?symbol=btcusdt"},
    {"coincap", "api.coincap.io",
     "https://api.coincap.io/v2/assets/bitcoin"},
    {"coinbase", "api.coinbase.com",
     "https://api.coinbase.com/v2/prices/BTC-USD/spot"},
    {"kraken", "api.kraken.com",
     "https://api.kraken.com/0/public/Ticker?pair=XBTUSD"},
    {"gate", "api.gateio.ws",
     "https://api.gateio.ws/api/v4/spot/tickers?currency_pair=BTC_USDT"},
    {"bitfinex", "api-pub.bitfinex.com",
     "https://api-pub.bitfinex.com/v2/ticker/tBTCUSD"},

    // ---- 天气类（核实老 Demo 到底能不能用）----
    {"open-meteo", "api.open-meteo.com",
     "https://api.open-meteo.com/v1/forecast?latitude=31.23&longitude=121.47&current=temperature_2m"},
    {"wttr.in", "wttr.in", "https://wttr.in/Shanghai?format=3"},
    {"sina(股票)", "hq.sinajs.cn", "https://hq.sinajs.cn/list=sh000001"},

    // ---- 国内对照（如果这些也不行，那就是板子网络的事，不是墙）----
    {"baidu(对照)", "www.baidu.com", "https://www.baidu.com"},
    {"腾讯财经(股票)", "qt.gtimg.cn", "https://qt.gtimg.cn/q=sh000001"},
};
#define PROBE_COUNT (sizeof(PROBES) / sizeof(PROBES[0]))
// ==========================================================

static bool results[PROBE_COUNT];

static void probeOne(int i) {
  const Probe &p = PROBES[i];
  char dns[40] = "解析失败";

  IPAddress ip;
  if (WiFi.hostByName(p.host, ip)) {
    snprintf(dns, sizeof(dns), "%s", ip.toString().c_str());
  }

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  http.setConnectTimeout(5000);
  http.setTimeout(5000);
  http.setUserAgent("ESP32-S3-RLCD-4.2/1.0");

  int code = -999;
  if (http.begin(client, p.url)) {
    code = http.GET();
    http.end();
  }

  results[i] = (code > 0);

  Serial.printf("%-16s DNS %-16s  HTTP %s\n", p.tag, dns,
                code > 0 ? String(code).c_str()
                         : (code == -999 ? "begin失败" : String(code).c_str()));
}

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("\n\n============================================");
  Serial.println("     行情 API 可达性探测");
  Serial.println("============================================");

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.printf("连 WiFi %s ", WIFI_SSID);
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 20000) {
    Serial.print(".");
    delay(400);
  }
  Serial.println();

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[FAIL] WiFi 连不上，先跑 net_diag");
    return;
  }
  Serial.printf("[OK] WiFi 已连接  IP=%s  网关=%s  DNS=%s  信号=%d dBm\n\n",
                WiFi.localIP().toString().c_str(),
                WiFi.gatewayIP().toString().c_str(),
                WiFi.dnsIP().toString().c_str(), WiFi.RSSI());

  Serial.println("--- 开始逐个探测（每个最多等 5 秒）---");
  for (size_t i = 0; i < PROBE_COUNT; i++) probeOne(i);

  Serial.println("\n--- 汇总 ---");
  for (size_t i = 0; i < PROBE_COUNT; i++) {
    if (results[i]) Serial.printf("  [可用] %s\n", PROBES[i].tag);
  }
  int okCount = 0;
  for (size_t i = 0; i < PROBE_COUNT; i++) okCount += results[i];
  Serial.printf("  %d / %d 可用\n", okCount, (int)PROBE_COUNT);
  Serial.println("\n(30 秒后重测一遍)");
}

void loop() {
  delay(30000);
  Serial.println("\n########## 重测 ##########");
  for (size_t i = 0; i < PROBE_COUNT; i++) probeOne(i);
  int okCount = 0;
  for (size_t i = 0; i < PROBE_COUNT; i++) okCount += results[i];
  Serial.printf("  %d / %d 可用\n", okCount, (int)PROBE_COUNT);
  Serial.println("(30 秒后重测一遍)");
}
