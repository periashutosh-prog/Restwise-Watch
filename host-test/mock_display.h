// A tiny stand-in for Adafruit_SSD1306 so block_screen.h can run on a PC. It implements only the calls block_screen.h uses,
// with the same rules as Adafruit_GFX (the real 5x7 font, text wrap, setTextColor setting both colours).
#pragma once
#include <stdint.h>
#include <string.h>
#ifndef PROGMEM
#define PROGMEM
#endif
#ifndef pgm_read_byte
#define pgm_read_byte(addr) (*(const uint8_t*)(addr))
#endif
#include "glcdfont_copy.c"          // the classic font, copied from Adafruit_GFX_Library by compare_with_python.py

struct MockDisplay {
  static const int W = 128, H = 64;
  uint8_t buf[H][W];
  int cx = 0, cy = 0;
  int size = 1;
  uint16_t color = 1, bg = 1;
  bool wrap = true;

  MockDisplay() { memset(buf, 0, sizeof buf); }
  void px(int x, int y, uint16_t c) { if (x >= 0 && x < W && y >= 0 && y < H) buf[y][x] = (uint8_t)(c ? 1 : 0); }
  void setTextSize(int s) { size = s; }
  void setTextColor(uint16_t c) { color = bg = c; }
  void setCursor(int x, int y) { cx = x; cy = y; }
  void drawFastHLine(int x, int y, int w, uint16_t c) { for (int i = 0; i < w; i++) px(x + i, y, c); }
  void fillRect(int x, int y, int w, int h, uint16_t c) { for (int i = x; i < x + w; i++) for (int j = 0; j < h; j++) px(i, y + j, c); }
  void drawBitmap(int x, int y, const uint8_t* bmp, int w, int h, uint16_t c) {
    int bw = (w + 7) / 8;
    for (int j = 0; j < h; j++)
      for (int i = 0; i < w; i++)
        if (bmp[j * bw + i / 8] & (128 >> (i & 7))) px(x + i, y + j, c);
  }
  void drawChar(int x, int y, unsigned char c, uint16_t col, uint16_t bgc, int s) {
    if ((x >= W) || (y >= H) || ((x + 6 * s - 1) < 0) || ((y + 8 * s - 1) < 0)) return;
    for (int i = 0; i < 5; i++) {
      uint8_t line = font[c * 5 + i];
      for (int j = 0; j < 8; j++, line >>= 1) {
        if (line & 1) {
          if (s == 1) px(x + i, y + j, col); else fillRect(x + i * s, y + j * s, s, s, col);
        } else if (bgc != col) {
          if (s == 1) px(x + i, y + j, bgc); else fillRect(x + i * s, y + j * s, s, s, bgc);
        }
      }
    }
  }
  void print(const char* t) {
    for (; *t; ++t) {
      char c = *t;
      if (c == '\n') { cx = 0; cy += size * 8; continue; }
      if (c == '\r') continue;
      if (wrap && ((cx + size * 6) > W)) { cx = 0; cy += size * 8; wrapped = true; }
      drawChar(cx, cy, c, color, bg, size);
      cx += size * 6;
    }
  }
  bool wrapped = false;       // set if Adafruit's text wrap ever kicked in (it must not on this screen)
};
