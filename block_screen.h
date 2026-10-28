// Restwise "current block" screen: an icon on the left (chosen by the block's CATEGORY), the start-end time and the
// block's name on the right. Only a night Sleep block says "Good Night!".
//
// This header is shared by the watch firmware (Restwise-watch.ino) and the PC test in host-test/, so the test exercises
// exactly the code the watch runs. D is any display with the Adafruit_GFX calls used below (Adafruit_SSD1306 on the watch,
// a small mock on the PC). The layout is a line-by-line port of UI-Screens/render_engine/block_ui.py, and
// host-test/compare_with_python.py checks the two give identical pixels.
#pragma once
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "block_icons.h"

namespace rwblk {

// The eight categories of the website's dropdown (School, Tuition, Homework, Meal, Break, Sleep, Free time, Travel).
enum Cat : uint8_t { C_SCHOOL, C_TUITION, C_HOMEWORK, C_MEAL, C_BREAK, C_SLEEP, C_FREE, C_TRAVEL, C_COUNT, C_NONE = 255 };
// Six icons: School, Tuition and Homework share the book.
enum Kind : uint8_t { K_STUDY, K_MEAL, K_SLEEP, K_FREE, K_TRAVEL, K_BREAK };

static const char* const CAT_KEYS[C_COUNT] = {"school", "tuition", "homework", "meal", "break", "sleep", "free", "travel"};
static const char* const CAT_DEFAULT_NAMES[C_COUNT] = {"School", "Tuition", "Homework", "Meal", "Break", "Sleep", "Free time", "Travel"};
const int CAT_NAME_LEN = 12;   // 11 letters + the terminator

struct Block {
  uint8_t  days;       // bit0=Mon ... bit6=Sun
  uint16_t startMin;
  uint16_t endMin;
  char     label[20];  // the name typed on the website (may be empty)
  uint8_t  cat;        // a Cat, or C_NONE for a timetable synced before categories existed
};

inline int catFromKey(const char* key) {
  for (int i = 0; i < C_COUNT; i++)
    if (strcmp(key, CAT_KEYS[i]) == 0) return i;
  return C_NONE;
}

// ---- label words -> icon, only for timetables that were synced without a category ---------------------------------
// Whole words only, so "Breakfast" is not a "break". Order matters: "Travel to School" is travel and "Lunch Break" is a break.
inline bool hasWord(const char* label, const char* const* words, int n) {
  char w[24];
  int len = 0;
  for (const char* p = label;; ++p) {
    char c = *p;
    if (c >= 'A' && c <= 'Z') c += 32;
    if (c >= 'a' && c <= 'z') {
      if (len < 23) w[len++] = c;
    } else {
      if (len) {
        w[len] = 0;
        for (int i = 0; i < n; i++)
          if (strcmp(w, words[i]) == 0) return true;
        len = 0;
      }
      if (!c) break;
    }
  }
  return false;
}

inline Kind guessKind(const char* label) {
  static const char* const SLEEPW[]  = {"sleep", "bedtime", "bed"};
  static const char* const BREAKW[]  = {"break", "breaks", "water", "rest", "recess", "interval"};
  static const char* const TRAVELW[] = {"travel", "travelling", "commute", "commuting", "bus", "drive", "driving", "ride", "journey", "trip", "transit", "walk"};
  static const char* const MEALW[]   = {"meal", "meals", "dinner", "lunch", "breakfast", "snack", "snacks", "food", "eat", "eating", "tiffin", "supper"};
  static const char* const STUDYW[]  = {"school", "class", "classes", "tuition", "tuitions", "coaching", "tutor", "homework", "study", "studies", "exam", "exams", "revision", "lesson"};
  if (hasWord(label, SLEEPW, 3)) return K_SLEEP;
  if (hasWord(label, BREAKW, 6)) return K_BREAK;
  if (hasWord(label, TRAVELW, 12)) return K_TRAVEL;
  if (hasWord(label, MEALW, 12)) return K_MEAL;
  if (hasWord(label, STUDYW, 14)) return K_STUDY;
  return K_FREE;
}

inline Kind kindOf(uint8_t cat, const char* label) {
  switch (cat) {
    case C_SCHOOL: case C_TUITION: case C_HOMEWORK: return K_STUDY;
    case C_MEAL: return K_MEAL;
    case C_BREAK: return K_BREAK;
    case C_SLEEP: return K_SLEEP;
    case C_FREE: return K_FREE;
    case C_TRAVEL: return K_TRAVEL;
    default: return guessKind(label);
  }
}

inline const uint8_t* iconOf(Kind k) {
  switch (k) {
    case K_STUDY: return ICON_STUDY;
    case K_MEAL: return ICON_MEAL;
    case K_SLEEP: return ICON_SLEEP;
    case K_FREE: return ICON_FREE;
    case K_TRAVEL: return ICON_TRAVEL;
    default: return ICON_BREAK;
  }
}

// A Sleep block is "the night" when it runs past midnight or starts in the evening / small hours; a daytime nap is not.
inline bool isNightSleep(int start, int end) {
  return end <= start || start >= 20 * 60 || start < 4 * 60;
}

// ---- name layout -------------------------------------------------------------------------------------------------------
// Greedy line breaking at spaces and after hyphens (mirrors block_ui.wrap). With hardSplit, a word longer than a line is cut
// as "Extracurric-" + "ular". Returns the number of lines, or 0 when it does not fit in maxLines.
inline int wrapLines(const char* text, int maxChars, int maxLines, bool hardSplit, char lines[][16]) {
  int n = 0;
  char cur[24];
  int curLen = 0;
  cur[0] = 0;
  const char* p = text;
  while (*p) {
    while (*p == ' ') p++;
    if (!*p) break;
    const char* q = p;                       // one word: [p, q)
    while (*q && *q != ' ') q++;
    const char* s = p;
    bool glued = false;                      // the pieces after a hyphen are glued to the piece before
    while (s < q) {
      const char* e = s;                     // one piece: [s, e), ends after a hyphen unless the hyphen is its first letter
      while (e < q) {
        char ch = *e;
        e++;
        if (ch == '-' && (e - s) > 1) break;
      }
      int L = (int)(e - s);
      const char* pc = s;
      while (L > maxChars) {
        if (!hardSplit) return 0;
        if (curLen) {
          if (n >= 4) return 0;
          memcpy(lines[n], cur, curLen); lines[n][curLen] = 0; n++;
          curLen = 0; cur[0] = 0;
        }
        if (n >= 4) return 0;
        memcpy(lines[n], pc, maxChars - 1); lines[n][maxChars - 1] = '-'; lines[n][maxChars] = 0; n++;
        pc += maxChars - 1;
        L -= maxChars - 1;
      }
      int joiner = (glued || curLen == 0) ? 0 : 1;
      if (curLen + joiner + L <= maxChars) {
        if (joiner) cur[curLen++] = ' ';
        memcpy(cur + curLen, pc, L); curLen += L; cur[curLen] = 0;
      } else {
        if (n >= 4) return 0;
        memcpy(lines[n], cur, curLen); lines[n][curLen] = 0; n++;
        memcpy(cur, pc, L); curLen = L; cur[curLen] = 0;
      }
      glued = true;
      s = e;
    }
    p = q;
  }
  if (curLen) {
    if (n >= 4) return 0;
    memcpy(lines[n], cur, curLen); lines[n][curLen] = 0; n++;
  }
  return n <= maxLines ? n : 0;
}

// Size-2 text that advances 11 px instead of 12, so a 7-letter name ("Tuition") fits beside the icon.
template <class D> void bigText(D& d, int x, int y, const char* s) {
  for (; *s; ++s) {
    d.drawChar(x, y, (unsigned char)*s, 1, 1, 2);   // colour == background: no background is painted
    x += 11;
  }
}

// ---- the screen --------------------------------------------------------------------------------------------------------
// label: the name typed on the website (may be empty); cat: a Cat or C_NONE; catName: the category's name as synced from the
// website (shown when the name is empty). forceSleep: draw Good Night (used at night when the timetable has no Sleep block).
template <class D>
void drawBlock(D& d, int yOff, const char* label, int start, int end, uint8_t cat, const char* catName,
               bool showTime = true, bool forceSleep = false) {
  char name[24];
  const char* a = label;
  while (*a == ' ') a++;
  int len = (int)strlen(a);
  while (len > 0 && a[len - 1] == ' ') len--;
  if (len == 0) {
    snprintf(name, sizeof name, "%s", (catName && *catName) ? catName : "Block");
  } else {
    if (len > 23) len = 23;
    memcpy(name, a, len);
    name[len] = 0;
  }

  Kind kind = kindOf(cat, label);
  bool goodnight = forceSleep || (kind == K_SLEEP && isNightSleep(start, end));
  d.drawBitmap(2, 12 + yOff, iconOf(goodnight ? K_SLEEP : kind), 40, 40, 1);

  const int X = 49, RIGHT = 125;                       // text column x = 49 .. 125
  d.setTextSize(1);
  d.setTextColor(1);
  if (showTime) {
    char t[16];
    snprintf(t, sizeof t, "%02d:%02d-%02d:%02d", (start / 60) % 24, start % 60, (end / 60) % 24, end % 60);
    d.setCursor(X, 10 + yOff);
    d.print(t);
    d.drawFastHLine(X, 21 + yOff, RIGHT - X + 1, 1);
  }
  if (goodnight) {
    int y0 = showTime ? 26 : 14;
    bigText(d, X, y0 + yOff, "Good");
    bigText(d, X, y0 + 17 + yOff, "Night!");
    return;
  }

  char lines[4][16];
  int n;
  bool homework = strlen(name) == 8;
  if (homework) {                                       // 8 letters do not fit on one big line: a hand-made split
    const char* hw = "homework";
    for (int i = 0; i < 8; i++) {
      char c = name[i];
      if (c >= 'A' && c <= 'Z') c += 32;
      if (c != hw[i]) { homework = false; break; }
    }
  }
  if (homework) { strcpy(lines[0], "Home"); strcpy(lines[1], "work"); n = 2; }
  else n = wrapLines(name, 7, 2, false, lines);
  if (n > 0) {                                          // tier 1: big letters, up to 2 lines of 7
    int y = (n == 1) ? 29 : 25;
    for (int i = 0; i < n; i++) bigText(d, X, y + i * 17 + yOff, lines[i]);
    return;
  }

  n = wrapLines(name, 12, 2, true, lines);              // tier 2: small letters on an inverted bar, 2 lines of 12
  if (n == 0) {                                         // cannot happen for names up to 19 letters; a safety net
    char flat[24];
    strncpy(flat, name, 23); flat[23] = 0;
    int fl = (int)strlen(flat);
    int c0 = fl < 12 ? fl : 12;
    memcpy(lines[0], flat, c0); lines[0][c0] = 0;
    int c1 = fl > 12 ? (fl - 12 < 10 ? fl - 12 : 10) : 0;
    memcpy(lines[1], flat + 12, c1); lines[1][c1] = '.'; lines[1][c1 + 1] = 0;
    n = 2;
  }
  for (int i = 0; i < n; i++) {
    int y = (n == 2) ? 30 + i * 11 : 35;
    d.fillRect(X - 2, y - 2 + yOff, RIGHT - X + 3, 11, 1);
    d.setTextColor(0);
    d.setCursor(X, y + yOff);
    d.print(lines[i]);
  }
  d.setTextColor(1);
}

// ---- which block is running now ------------------------------------------------------------------------------------------
// todayIdx: Monday = 0. A block that runs past midnight belongs to the day it started on, so after midnight it is found
// through YESTERDAY's day mask. When blocks overlap, the one that started most recently wins (then the shorter one).
// Returns the index, or -1 when nothing is running.
inline int findCurrent(const Block* blocks, int count, int todayIdx, int curMin) {
  int yesterday = (todayIdx + 6) % 7;
  int best = -1, bestStart = -100000, bestLen = 100000;
  for (int i = 0; i < count; i++) {
    const Block& b = blocks[i];
    int s = b.startMin, e = b.endMin;
    if (s == e) continue;
    bool todayHit = (b.days & (1 << todayIdx)) != 0;
    bool yestHit = (b.days & (1 << yesterday)) != 0;
    bool hit;
    int st, len;
    if (s < e) {
      hit = todayHit && curMin >= s && curMin < e;
      st = s;
      len = e - s;
    } else {                                            // wraps past midnight
      bool evening = todayHit && curMin >= s;
      bool morning = yestHit && curMin < e;
      hit = evening || morning;
      st = evening ? s : s - 1440;
      len = e + 1440 - s;
    }
    if (!hit) continue;
    if (best < 0 || st > bestStart || (st == bestStart && len < bestLen)) {
      best = i; bestStart = st; bestLen = len;
    }
  }
  return best;
}

}  // namespace rwblk
