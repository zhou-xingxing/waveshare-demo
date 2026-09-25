/*
 * 中英混排显示 —— 让 U8g2 画得出汉字
 * ================================================================
 * U8g2 内置字体不含 CJK，所以汉字得自己带点阵。
 * 点阵数据在 cn_font_data.h，由 tools/gen_cn_font.py 自动生成。
 *
 * 用法和 U8g2 原生的几乎一样，函数名换一下就行：
 *
 *     g->drawStr(x, y, s)    ->   drawText(g, x, y, s)
 *     g->getStrWidth(s)      ->   textWidth(g, s)
 *
 * ASCII 部分照旧走当前设置的 U8g2 字体（所以 setFont 仍然有效），
 * 汉字走自己的点阵表，两者**共享同一条基线**，混排不会一高一低。
 *
 * 例：
 *     g->setFont(u8g2_font_helvB12_tf);
 *     drawText(g, 16, 24, "上证指数 3888");     // 中英混着写就行
 */

#pragma once
#include <stdint.h>
#include <string.h>
#include "cn_font_data.h"

// 查一个码点对应的点阵，找不到返回 nullptr。
// 码点表是升序的，所以用二分查找 —— 比线性扫快，字多了也不怕。
static inline const uint8_t *cnLookup(uint32_t cp) {
  int lo = 0, hi = CN_COUNT - 1;
  while (lo <= hi) {
    int mid = (lo + hi) / 2;
    if (CN_CODEPOINTS[mid] == cp) return &CN_GLYPHS[mid * CN_GLYPH_BYTES];
    if (CN_CODEPOINTS[mid] < cp) lo = mid + 1;
    else hi = mid - 1;
  }
  return nullptr;
}

// UTF-8 解码一个字符，p 自动前进到下一个字符的开头。
// 汉字在 UTF-8 里是 3 个字节，所以不能简单地一个字节一个字节走。
static inline uint32_t utf8Next(const char *&p) {
  uint8_t c = (uint8_t)*p++;
  if (c < 0x80) return c;  // 纯 ASCII

  uint32_t cp;
  int extra;
  if ((c & 0xE0) == 0xC0)      { cp = c & 0x1F; extra = 1; }
  else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; extra = 2; }  // 汉字走这里
  else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; extra = 3; }
  else return 0xFFFD;          // 非法起始字节

  for (int i = 0; i < extra; i++) {
    if (((uint8_t)*p & 0xC0) != 0x80) return 0xFFFD;  // 续字节格式不对
    cp = (cp << 6) | ((uint8_t)*p++ & 0x3F);
  }
  return cp;
}

// 一段连续 ASCII 的长度上限（画字前要先攒成一段，见下面注释）
#define CN_ASCII_RUN_MAX 64

// 画一串中英混排的文本，(x, y) 是**基线左端**，和 drawStr 的约定一致。
// 返回实际画了多宽，方便接着往右排。
static inline int drawText(U8G2 *g, int x, int y, const char *s) {
  int startX = x;

  while (*s) {
    if ((uint8_t)*s < 0x80) {
      // 攒一段连续的 ASCII 一次性画。
      // 逐字符画会丢掉字体自带的字距调整（kerning），英文会变得很难看。
      const char *run = s;
      while (*s && (uint8_t)*s < 0x80) s++;
      int n = (int)(s - run);
      if (n > CN_ASCII_RUN_MAX) n = CN_ASCII_RUN_MAX;

      char buf[CN_ASCII_RUN_MAX + 1];
      memcpy(buf, run, n);
      buf[n] = '\0';

      g->drawStr(x, y, buf);
      x += g->getStrWidth(buf);
    } else {
      uint32_t cp = utf8Next(s);
      const uint8_t *bm = cnLookup(cp);

      if (bm) {
        // drawXBMP 的 y 是**顶边**，而我们要的是基线，所以要减去 ascent
        g->drawXBMP(x, y - CN_ASCENT, CN_W, CN_H, bm);
      } else {
        // 字库里没这个字 —— 画个空心框。
        // 故意不做成"静默跳过"：缺字时一眼能看出来是漏生成，
        // 而不是盯着空白怀疑是屏幕坏了。
        g->drawFrame(x, y - CN_ASCENT, CN_W, CN_H);
      }
      x += CN_W;
    }
  }
  return x - startX;
}

// 量一串中英混排文本的宽度（像素），用来自动居中 / 右对齐
static inline int textWidth(U8G2 *g, const char *s) {
  int w = 0;

  while (*s) {
    if ((uint8_t)*s < 0x80) {
      const char *run = s;
      while (*s && (uint8_t)*s < 0x80) s++;
      int n = (int)(s - run);
      if (n > CN_ASCII_RUN_MAX) n = CN_ASCII_RUN_MAX;

      char buf[CN_ASCII_RUN_MAX + 1];
      memcpy(buf, run, n);
      buf[n] = '\0';

      w += g->getStrWidth(buf);
    } else {
      utf8Next(s);
      w += CN_W;
    }
  }
  return w;
}
