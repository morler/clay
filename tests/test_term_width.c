#include "clay/term.h"
#include "clay/color.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#ifdef _WIN32
#include <io.h>
#define dup _dup
#define dup2 _dup2
#define close _close
#define fileno _fileno
#else
#include <unistd.h>
#endif

static void check_clip(const char *input, int columns, const char *expected,
                       size_t width) {
  FILE *capture = tmpfile();
  assert(capture);
  fflush(stdout);
  int saved = dup(fileno(stdout));
  assert(saved >= 0 && dup2(fileno(capture), fileno(stdout)) >= 0);
  assert(clay_term_write_clipped(input, columns) == width);
  fflush(stdout);
  assert(dup2(saved, fileno(stdout)) >= 0);
  close(saved);
  rewind(capture);
  char buf[256] = {0};
  size_t n = fread(buf, 1, sizeof(buf) - 1, capture);
  assert(n == strlen(expected) && memcmp(buf, expected, n) == 0);
  assert(clay_utf8_width(buf) == width);
  fclose(capture);
}

int main(void) {
  size_t n;
  assert(clay_utf8_char_width("中", &n) == 2 && n == 3);
  assert(clay_utf8_char_width("\xcc\x81", &n) == 0 && n == 2);
  assert(clay_utf8_char_width("\xf0\x9f", &n) == 1 && n == 1);
  assert(clay_utf8_char_width("\xed\xa0\x80", &n) == 1 && n == 1);
  assert(clay_utf8_char_width("\xf4\x90\x80\x80", &n) == 1 && n == 1);
  assert(clay_utf8_char_width("", &n) == 0 && n == 0);
  assert(clay_utf8_width("a中e\xcc\x81") == 4);
  assert(clay_utf8_width("\x1b[31m中\x1b[0m") == 2);
  assert(clay_utf8_width("\x1b]8;;url\x1b\\中\x1b]8;;\a") == 2);
  assert(clay_utf8_width(CLAY_ICON_CHECK) == 1);
  assert(clay_utf8_width(CLAY_ICON_SLEEP) == 2);
  clay_term_set_color_enabled(0);
  check_clip("a中b", 2, "a", 1);
  check_clip("a中b", 3, "a中", 3);
  check_clip("e\xcc\x81x", 1, "e\xcc\x81", 1);
  check_clip("\x1b[31m中\x1b[0mx", 2, "\x1b[31m中\x1b[0m", 2);
  check_clip("\xf0\x9f", 1, "\xf0", 1);
  check_clip("中", 0, "", 0);
  clay_term_set_color_enabled(1);
  check_clip("\x1b[31m中x", 2, "\x1b[31m中\x1b[0m", 2);
  puts("term width tests passed");
  return 0;
}