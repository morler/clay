#include "clay/list.h"
#include "clay/below.h"
#include "clay/term.h"
#include <stdlib.h>
#ifdef _WIN32
#include <io.h>
#define dup _dup
#define dup2 _dup2
#define close _close
#define fileno _fileno
#else
#include <unistd.h>
#endif
#include <assert.h>
#include <stdio.h>
#include <string.h>

static int width;
int __wrap_clay_term_width(void) { return width; }

static void output(const char *text, void *data) { clay_str_push(data, text); }
static void newline(void *data) { clay_str_push_char(data, '\n'); }

static void check(int columns, int indent, const char *text, const char *expected) {
    ClayWrap wrap;
    ClayStr out;
    width = columns;
    clay_str_init(&out);
    clay_wrap_init(&wrap, indent, output, newline, &out);
    for (const char *p = text; *p; p++) {
        char chunk[] = {*p, 0};
        clay_wrap_write(&wrap, chunk);
    }
    clay_wrap_flush(&wrap);
    assert(strcmp(out.data, expected) == 0);
    clay_wrap_free(&wrap);
    clay_str_free(&out);
}

static void check_viewport(int columns, const char *input, size_t cursor,
                           const char *visible, const char *position) {
    FILE *capture = tmpfile();
    assert(capture);
    fflush(stdout);
    int saved = dup(fileno(stdout));
    assert(saved >= 0 && dup2(fileno(capture), fileno(stdout)) >= 0);
    width = columns;
    clay_below_render(input, cursor);
    fflush(stdout);
    assert(dup2(saved, fileno(stdout)) >= 0);
    close(saved);
    rewind(capture);
    char result[1024] = {0};
    fread(result, 1, sizeof(result) - 1, capture);
    assert(strstr(result, visible));
    assert(strstr(result, position));
    fclose(capture);
}

int main(void) {
    clay_term_set_color_enabled(0);
    check_viewport(3, "中文", strlen("中文"), "", "\x1b[3G");
    check_viewport(8, "中文中文", strlen("中文中文"), "…文", "\x1b[6G");
    check_viewport(8, "中文abcd", strlen("中文"), "中文", "\x1b[7G");
    check_viewport(5, "a\xf0\x9f", 3, "…", "\x1b[4G");
    check_viewport(8, "\x1b[31m中文中文\x1b[0m", strlen("\x1b[31m中文中文\x1b[0m"), "…文", "\x1b[6G");
    char *long_input = malloc(300001);
    assert(long_input);
    for (size_t i = 0; i < 100000; i++) memcpy(long_input + i * 3, "中", 3);
    long_input[300000] = 0;
    check_viewport(8, long_input, 300000, "…中", "\x1b[6G");
    free(long_input);
    check(6, 0, "ab中文c", "ab中\n文c");
    check(4, 0, "a中b", "a中\nb");
    check(3, 0, "e\xcc\x81中", "e\xcc\x81\n中");
    check(2, 0, "中文a", "中\n文\na");
    check(2, 2, "中a", "中\na");
    check(5, 0, "ab 中", "ab \n中");
    check(8, 0, "x\xf0\x9f", "x\xf0\x9f");
    check(4, 0, "\x1b[31mabcde\x1b[0m", "\x1b[31mabc\nde\x1b[0m");
    check(4, 0, "ab\x1b[31m中文\x1b[0m", "ab\x1b[31m\n中\n文\x1b[0m");
    check(4, 0, "\x1b]8;;a b\x1b\\abcde\x1b]8;;\a", "\x1b]8;;a b\x1b\\abc\nde\x1b]8;;\a");
    check(2, 0, "\x1b[31m中a", "\x1b[31m中\na");
    check(4, 0, "abcd\x1b[31", "abc\nd\x1b[31");
    check(4, 0, "\x1b]8;;a\nb\aabcd", "\x1b]8;;a\nb\aabc\nd");
    check(4, 0, "abc\x1b[0mde", "abc\x1b[0m\nde");
    check(4, 0, "abc\x1b" "7de", "abc\x1b" "7\nde");
    puts("wrap UTF-8 tests passed");
    return 0;
}
