/*
 * 网络诊断 —— 板子连不上外网时，用它定位问题出在哪一层
 * ================================================================
 * 按「自下而上」的顺序逐层测试，哪一层挂了就报哪一层：
 *
 *   第 0 层  WiFi 关联        -> 拿不到 IP，后面全没意义
 *   第 1 层  DNS 解析         -> 域名变不成 IP
 *   第 2 层  TCP 连 443       -> DNS 没问题，但网络到不了这台服务器
 *   第 3 层  明文 HTTP        -> 最基本的出网能力（不带 TLS）
 *   第 4 层  HTTPS 国内站点   -> TLS 握手本身能不能成
 *   第 5 层  HTTPS 目标站点   -> 前面都过、只有这步挂 = 目标被墙
 *
 * 结果全打在串口，不画屏 —— 诊断工具用串口更直接。
 *     ../../build.sh flash net_diag
 *     ../../build.sh mon
 *
 * 注意：诊断期间串口要一直开着，板子每 15 秒重跑一轮。
 */

#include <WiFi.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <stdarg.h>

// ======================= 改这里 =======================
// WiFi 凭据：复制 secrets.h.example 成 secrets.h 填自己的（不进版本库）
#include "secrets.h"

// 要诊断的目标站点（换项目时改这两行就行）
const char *TARGET_HOST = "api.coingecko.com";
const char *TARGET_URL = "https://api.coingecko.com/api/v3/ping";
const char *CONTROL_URL = "https://www.baidu.com";  // 对照组：国内站点
// =====================================================

static void head(const char *s) { Serial.printf("\n---- %s\n", s); }
static void ok(const char *fmt, ...) {
  char b[256];
  va_list a; va_start(a, fmt); vsnprintf(b, sizeof(b), fmt, a); va_end(a);
  Serial.printf("[OK]   %s\n", b);
}
static void bad(const char *fmt, ...) {
  char b[256];
  va_list a; va_start(a, fmt); vsnprintf(b, sizeof(b), fmt, a); va_end(a);
  Serial.printf("[FAIL] %s\n", b);
}

static const char *wifiStatusText(wl_status_t s) {
  switch (s) {
    case WL_IDLE_STATUS:     return "IDLE（还在连）";
    case WL_NO_SSID_AVAIL:   return "找不到这个 SSID（名字写错？或者是 5G 专用网络？）";
    case WL_SCAN_COMPLETED:  return "扫描完成";
    case WL_CONNECTED:       return "已连接";
    case WL_CONNECT_FAILED:  return "密码错误？";
    case WL_CONNECTION_LOST: return "连接丢失";
    case WL_DISCONNECTED:    return "已断开";
    default:                 return "未知";
  }
}

// 第 3 / 4 / 5 层都用它：GET 一个地址，只报结果
static int tryGet(const char *label, WiFiClient &client, const char *url) {
  HTTPClient http;
  http.setConnectTimeout(8000);
  http.setTimeout(8000);

  if (!http.begin(client, url)) {
    bad("%s  http.begin 失败（URL 写错了？）", label);
    return -999;
  }
  int code = http.GET();
  if (code > 0) {
    String body = http.getString();
    if (body.length() > 120) body = body.substring(0, 120) + "...";
    body.replace("\n", " ");
    ok("%s  HTTP %d   响应: %s", label, code, body.c_str());
  } else {
    bad("%s  失败，code=%d", label, code);
  }
  http.end();
  return code;
}

static void diagnose() {
  // ---------- 第 0 层：WiFi 关联 ----------
  head("第 0 层  WiFi 关联");
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 20000) {
    delay(400);
    Serial.print(".");
  }
  Serial.println();

  if (WiFi.status() != WL_CONNECTED) {
    bad("连不上 WiFi：%s", wifiStatusText(WiFi.status()));
    return;
  }
  ok("WiFi 已连接");
  Serial.printf("       IP        %s\n", WiFi.localIP().toString().c_str());
  Serial.printf("       子网掩码  %s\n", WiFi.subnetMask().toString().c_str());
  Serial.printf("       网关      %s\n", WiFi.gatewayIP().toString().c_str());
  Serial.printf("       DNS       %s\n", WiFi.dnsIP().toString().c_str());
  Serial.printf("       信号强度  %d dBm  (%s)\n", WiFi.RSSI(),
                WiFi.RSSI() > -60 ? "很强" : WiFi.RSSI() > -75 ? "还行" : "偏弱");

  // 网关 ping 不通的话说明连路由器都没通
  if (WiFi.gatewayIP() == IPAddress(0, 0, 0, 0)) {
    bad("网关是 0.0.0.0 —— DHCP 没拿到完整配置（网页认证的网络常见）");
  }

  // ---------- 第 1 层：DNS ----------
  head("第 1 层  DNS 解析");
  IPAddress ctrl;
  bool ctrlOk = WiFi.hostByName("www.baidu.com", ctrl);
  if (ctrlOk) {
    ok("www.baidu.com   -> %s", ctrl.toString().c_str());
  } else {
    bad("连 www.baidu.com 都解析不了 —— 路由器给的 DNS 有问题");
  }

  IPAddress ip;
  uint32_t t = millis();
  bool dnsOk = WiFi.hostByName(TARGET_HOST, ip);
  if (dnsOk) {
    ok("%s -> %s   (耗时 %u ms)", TARGET_HOST, ip.toString().c_str(), millis() - t);
    if (ip == IPAddress(0, 0, 0, 0) || ip == IPAddress(127, 0, 0, 1)) {
      bad("解析结果是 %s —— 这是被 DNS 污染/劫持的典型特征", ip.toString().c_str());
    }
  } else {
    bad("%s 解析不了", TARGET_HOST);
    if (ctrlOk) {
      Serial.println("       国内域名能解析、这个不能 —— 大概率是域名被墙或 DNS 污染");
      Serial.println("       试试把 WiFi.dnsIP 改成 223.5.5.5（阿里）或 1.1.1.1（Cloudflare）");
    }
  }

  // ---------- 第 2 层：TCP 直连 443 ----------
  head("第 2 层  TCP 直连 443");
  if (dnsOk) {
    WiFiClient c;
    t = millis();
    if (c.connect(ip, 443, 8000)) {
      ok("连上 %s:443   (耗时 %u ms)", ip.toString().c_str(), millis() - t);
      c.stop();
    } else {
      bad("连不上 %s:443", ip.toString().c_str());
      Serial.println("       DNS 没问题但 TCP 通不了 —— 路由器/运营商层面拦了");
    }
  } else {
    Serial.println("       (DNS 没解析出来，这步跳过)");
  }

  // ---------- 第 3 层：明文 HTTP ----------
  head("第 3 层  明文 HTTP（不带 TLS，测最基本出网）");
  {
    WiFiClient c;
    tryGet("http://www.baidu.com ", c, "http://www.baidu.com");
  }

  // ---------- 第 4 层：HTTPS 国内站点（测 TLS 本身） ----------
  head("第 4 层  HTTPS 国内站点（测 TLS 握手）");
  {
    WiFiClientSecure c;
    c.setInsecure();
    tryGet("baidu (对照组)      ", c, CONTROL_URL);
  }

  // ---------- 第 5 层：HTTPS 目标站点 ----------
  head("第 5 层  HTTPS 目标站点");
  {
    WiFiClientSecure c;
    c.setInsecure();
    tryGet("coingecko (目标)    ", c, TARGET_URL);
  }

  head("结论怎么读");
  Serial.println("  第 4 层过、第 5 层挂  ->  目标站点被墙，换个 API 或者自己搭个中转");
  Serial.println("  第 3 层就挂           ->  根本没出网（网页认证？路由器限制？）");
  Serial.println("  第 1 层挂             ->  换 DNS 试试");
  Serial.println("  第 0 层挂             ->  WiFi 名字/密码/频段的问题");
}

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("\n\n============================================");
  Serial.println("     ESP32-S3-RLCD-4.2  网络诊断");
  Serial.println("============================================");
  diagnose();
  Serial.println("\n(15 秒后重来一轮)");
}

void loop() {
  delay(15000);
  Serial.println("\n\n########## 重新诊断 ##########");
  WiFi.disconnect(true);
  delay(500);
  diagnose();
  Serial.println("\n(15 秒后重来一轮)");
}
