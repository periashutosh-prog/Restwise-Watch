// Batch test driver: reads one command per line on stdin (tab separated), writes the result on stdout.
//   R  label  start  end  cat  showTime  forceSleep  yOff  -> 64 text rows of '0'/'1', then a line "wrapped=0|1"
//   W  text  maxChars  maxLines  hard                      -> the wrapped lines separated by '|', or NONE
//   G  label                                                -> guessKind index
//   F  today  cur  n  then n lines "days start end cat"     -> findCurrent index
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>
#include "mock_display.h"
#include "../block_screen.h"

static std::vector<std::string> split(const std::string& s) {
  std::vector<std::string> out;
  size_t a = 0;
  while (true) {
    size_t b = s.find('\t', a);
    if (b == std::string::npos) { out.push_back(s.substr(a)); break; }
    out.push_back(s.substr(a, b - a));
    a = b + 1;
  }
  return out;
}

int main() {
  char buf[4096];
  while (fgets(buf, sizeof buf, stdin)) {
    std::string line(buf);
    while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();
    if (line.empty()) continue;
    std::vector<std::string> f = split(line);
    if (f[0] == "R") {
      MockDisplay d;
      int cat = atoi(f[4].c_str());
      const char* catName = (cat >= 0 && cat < rwblk::C_COUNT) ? rwblk::CAT_DEFAULT_NAMES[cat] : "";
      rwblk::drawBlock(d, atoi(f[7].c_str()), f[1].c_str(), atoi(f[2].c_str()), atoi(f[3].c_str()), cat < 0 ? rwblk::C_NONE : (uint8_t)cat, catName,
                       f[5] == "1", f[6] == "1");
      for (int y = 0; y < 64; y++) {
        for (int x = 0; x < 128; x++) putchar(d.buf[y][x] ? '1' : '0');
        putchar('\n');
      }
      printf("wrapped=%d\n", d.wrapped ? 1 : 0);
    } else if (f[0] == "W") {
      char lines[4][16];
      int n = rwblk::wrapLines(f[1].c_str(), atoi(f[2].c_str()), atoi(f[3].c_str()), f[4] == "1", lines);
      if (n == 0) printf("NONE\n");
      else { for (int i = 0; i < n; i++) printf(i ? "|%s" : "%s", lines[i]); printf("\n"); }
    } else if (f[0] == "G") {
      printf("%d\n", (int)rwblk::guessKind(f.size() > 1 ? f[1].c_str() : ""));
    } else if (f[0] == "F") {
      int today = atoi(f[1].c_str()), cur = atoi(f[2].c_str()), n = atoi(f[3].c_str());
      std::vector<rwblk::Block> bl(n);
      for (int i = 0; i < n; i++) {
        if (!fgets(buf, sizeof buf, stdin)) return 1;
        int dd, s, e, c;
        sscanf(buf, "%d %d %d %d", &dd, &s, &e, &c);
        bl[i].days = (uint8_t)dd; bl[i].startMin = (uint16_t)s; bl[i].endMin = (uint16_t)e; bl[i].cat = (uint8_t)c; bl[i].label[0] = 0;
      }
      printf("%d\n", rwblk::findCurrent(bl.data(), n, today, cur));
    }
    fflush(stdout);
  }
  return 0;
}
