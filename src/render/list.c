#include "clay/list.h"

#include "clay/color.h"
#include "clay/str.h"
#include "clay/term.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* Keeps ordinary list items visually separate from assistant messages. */
#define CLAY_INDENT "  "

static ClayStr g_thinking;
static int g_thinking_ready = 0;
static int g_thinking_streaming = 0;
static int g_thinking_expanded = 0;
static double g_thinking_seconds = 0;
static int g_response_line_start = 1;

static void print_prefix(void);

static void ensure_thinking(void) {
    if (!g_thinking_ready) {
        clay_str_init(&g_thinking);
        g_thinking_ready = 1;
    }
}

static void print_thinking_summary(double seconds) {
    printf("  %sReasoning finished in %.1fs%s\n",
           clay_color(CLAY_GRAY), seconds, clay_color(CLAY_RESET));
}

static void print_prefix(void) {
    printf("%s%s%s  ", clay_color(CLAY_ORANGE), CLAY_ICON_DIAMOND,
           clay_color(CLAY_RESET));
}

void clay_segments_println(const ClaySegment *segments, int count) {
    for (int i = 0; i < count; i++) {
        if (segments[i].color) fputs(clay_color(segments[i].color), stdout);
        fputs(segments[i].text, stdout);
        if (segments[i].color) fputs(clay_color(CLAY_RESET), stdout);
    }
    fputc('\n', stdout);
}

void clay_say(const char *fmt, ...) {
    print_prefix();
    fputs(clay_color(CLAY_GRAY), stdout);
    va_list args;
    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);
    fputs(clay_color(CLAY_RESET), stdout);
    fputc('\n', stdout);
}

void clay_sayc(const char *color, const char *fmt, ...) {
    print_prefix();
    fputs(clay_color(color), stdout);
    va_list args;
    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);
    fputs(clay_color(CLAY_RESET), stdout);
    fputc('\n', stdout);
}

void clay_response_begin(void) {
    fputc('\n', stdout);
    fputs(clay_color(CLAY_WHITE), stdout);
    g_response_line_start = 1;
    fflush(stdout);
}

void clay_response_write(const char *text) {
    if (!text || !*text) return;
    for (const unsigned char *p = (const unsigned char *)text; *p; p++) {
        if (g_response_line_start && *p != '\n') {
            fputs("  ", stdout);
            g_response_line_start = 0;
        }
        fputc(*p, stdout);
        if (*p == '\n')
            g_response_line_start = 1;
        else if (*p != '\r')
            g_response_line_start = 0;
    }
    fflush(stdout);
}

void clay_response_end(void) {
    fputs(clay_color(CLAY_RESET), stdout);
    fflush(stdout);
}

int clay_response_prefix_width(void) {
    return 2;
}

void clay_turn_header(const char *model) {
    printf("%s%s%s %sAgent%s %s(%s)%s\n",
           clay_color(CLAY_ORANGE), CLAY_ICON_DIAMOND,
           clay_color(CLAY_RESET), clay_color(CLAY_WHITE),
           clay_color(CLAY_RESET), clay_color(CLAY_CORAL),
           model && *model ? model : "model", clay_color(CLAY_RESET));
}

/* Return zero for an incomplete escape, which may continue in another chunk. */
static size_t ansi_bytes(const char *text) {
    const unsigned char *p = (const unsigned char *)text + 1;
    if (*p == '[') {
        p++;
        while (*p && (*p < 0x40 || *p > 0x7e)) p++;
        if (!*p) return 0;
        p++;
    } else if (*p == ']') {
        p++;
        while (*p && *p != 0x07 && !(*p == 0x1b && p[1] == 0x5c)) p++;
        if (!*p) return 0;
        p += *p == 0x07 ? 1 : 2;
    } else {
        if (!*p) return 0;
        p++;
    }
    return (size_t)(p - (const unsigned char *)text);
}

/* Bytes fitting on UTF-8 and ANSI sequence boundaries. */
static size_t prefix_within_width(const char *text, int columns, int *used) {
    size_t offset = 0;
    *used = 0;
    while (text[offset]) {
        size_t bytes;
        if ((unsigned char)text[offset] == 0x1b) {
            bytes = ansi_bytes(text + offset);
            if (!bytes) return offset + strlen(text + offset);
        } else {
            size_t width = clay_utf8_char_width(text + offset, &bytes);
            if (width > (size_t)(columns - *used)) break;
            *used += (int)width;
        }
        offset += bytes;
    }
    return offset;
}

/* A delimiter inside a streamed escape is data, not a word boundary. */
static int inside_ansi(const char *text) {
    for (size_t i = 0; text[i]; i++) {
        if ((unsigned char)text[i] == 0x1b) {
            size_t bytes = ansi_bytes(text + i);
            if (!bytes) return 1;
            i += bytes - 1;
        }
    }
    return 0;
}

void clay_wrap_init(ClayWrap *wrap, int indent,
                    void (*write)(const char *text, void *user_data),
                    void (*break_row)(void *user_data), void *user_data) {
    wrap->col = indent;
    wrap->indent = indent;
    wrap->write = write;
    wrap->break_row = break_row;
    wrap->user_data = user_data;
    clay_str_init(&wrap->word);
}

void clay_wrap_free(ClayWrap *wrap) {
    clay_str_free(&wrap->word);
}

static void wrap_break(ClayWrap *wrap) {
    wrap->break_row(wrap->user_data);
    wrap->col = wrap->indent;
}

void clay_wrap_flush(ClayWrap *wrap) {
    if (!wrap->word.len) return;
    /* The terminal's last column stays free: writing it wraps on its own in
       most terminals, which would double the row. */
    int limit = clay_term_width() - 1;
    int word_width = (int)clay_utf8_width(wrap->word.data);
    if (wrap->col + word_width > limit && wrap->indent + word_width <= limit)
        wrap_break(wrap);
    /* A word longer than the row itself has to be cut somewhere. */
    while (wrap->word.len && wrap->col + (int)clay_utf8_width(wrap->word.data) > limit) {
        int room = limit - wrap->col;
        int used;
        size_t bytes = prefix_within_width(wrap->word.data, room > 0 ? room : 0, &used);
        if (bytes && !used && wrap->col == wrap->indent && wrap->word.data[bytes]) {
            size_t char_bytes;
            clay_utf8_char_width(wrap->word.data + bytes, &char_bytes);
            bytes += char_bytes;
        }
        if (!bytes) {
            if (wrap->col > wrap->indent) {
                wrap_break(wrap);
                continue;
            }
            /* A character wider than the entire row must overflow to progress. */
            clay_utf8_char_width(wrap->word.data, &bytes);
        }
        ClayStr piece;
        clay_str_init(&piece);
        clay_str_push_n(&piece, wrap->word.data, bytes);
        wrap->write(piece.data, wrap->user_data);
        wrap->col += (int)clay_utf8_width(piece.data);
        clay_str_free(&piece);
        clay_str_remove_n(&wrap->word, 0, bytes);
        if (wrap->word.len) wrap_break(wrap);
    }
    if (!wrap->word.len) return;
    wrap->write(wrap->word.data, wrap->user_data);
    wrap->col += (int)clay_utf8_width(wrap->word.data);
    clay_str_clear(&wrap->word);
}

void clay_wrap_write(ClayWrap *wrap, const char *text) {
    if (!text || !*text) return;
    for (const unsigned char *p = (const unsigned char *)text; *p;) {
        int escaped = (*p == '\n' || *p == '\r' || *p == ' ' || *p == '\t') &&
                      wrap->word.len && inside_ansi(wrap->word.data);
        if (*p == '\n' && !escaped) {
            clay_wrap_flush(wrap);
            wrap_break(wrap);
            p++;
            continue;
        }
        if (*p == '\r' && !escaped) {
            p++;
            continue;
        }
        clay_str_push_char(&wrap->word, (char)*p);
        if ((*p == ' ' || *p == '\t') && !escaped) clay_wrap_flush(wrap);
        p++;
    }
}

/* Reasoning is streamed for reassurance, not for reading: it goes on one
   line that rewrites itself, so a minute of thinking costs one row of scroll
   instead of a screenful. Ctrl+O still expands the whole thing afterwards. */
static const char *thinking_tail(const char *text, int columns) {
    /* Walk back `columns` display characters from the end, on a UTF-8
       boundary. */
    size_t len = strlen(text);
    size_t offset = len;
    int seen = 0;
    while (offset > 0 && seen < columns) {
        offset--;
        while (offset > 0 && (((unsigned char)text[offset]) & 0xC0) == 0x80) offset--;
        seen++;
    }
    if (offset == 0) return text;
    /* Start at a word, not halfway through one. */
    size_t word = offset;
    while (text[word] && text[word] != ' ' && word < offset + 24) word++;
    return text[word] == ' ' ? text + word + 1 : text + offset;
}

static void thinking_redraw(void) {
    int room = clay_term_width() - 14; /* indent, "Thinking: ", last column */
    if (room < 8) return;
    ClayStr line;
    clay_str_init(&line);
    /* Newlines and runs of spaces would break the single row, so the tail
       reads as one continuous sentence. */
    for (const char *p = thinking_tail(g_thinking.data, room); *p; p++) {
        char c = (*p == '\n' || *p == '\r' || *p == '\t') ? ' ' : *p;
        if (c == ' ' && line.len && line.data[line.len - 1] == ' ') continue;
        clay_str_push_char(&line, c);
    }
    fputc('\r', stdout);
    clay_term_clear_line();
    printf("  %sThinking:%s %s", clay_color(CLAY_GRAY), clay_color(CLAY_RESET),
           clay_color(CLAY_GRAY));
    clay_term_write_clipped(line.data, room);
    fputs(clay_color(CLAY_RESET), stdout);
    fflush(stdout);
    clay_str_free(&line);
}

void clay_thinking_begin(void) {
    ensure_thinking();
    clay_str_clear(&g_thinking);
    g_thinking_streaming = 1;
    g_thinking_expanded = 0;
    printf("  %sThinking:%s", clay_color(CLAY_GRAY), clay_color(CLAY_RESET));
    fflush(stdout);
}

void clay_thinking_write(const char *text) {
    if (!text || !*text) return;
    ensure_thinking();
    clay_str_push(&g_thinking, text); /* kept whole, for Ctrl+O */
    thinking_redraw();
}

void clay_thinking_finish(double seconds) {
    ensure_thinking();
    if (!g_thinking_streaming) return;
    g_thinking_seconds = seconds;
    fputc('\r', stdout);
    clay_term_clear_line();
    print_thinking_summary(seconds);
    g_thinking_streaming = 0;
    g_thinking_expanded = 0;
    fflush(stdout);
}

void clay_thinking_restore(const char *text, double seconds) {
    ensure_thinking();
    clay_str_clear(&g_thinking);
    clay_str_push(&g_thinking, text ? text : "");
    g_thinking_seconds = seconds;
    g_thinking_streaming = 0;
    g_thinking_expanded = 0;
    if (g_thinking.len) print_thinking_summary(seconds);
}

void clay_thinking_forget(void) {
    ensure_thinking();
    clay_str_clear(&g_thinking);
    g_thinking_streaming = 0;
    g_thinking_expanded = 0;
    g_thinking_seconds = 0;
}

int clay_thinking_can_toggle(void) {
    return g_thinking_ready && g_thinking.len > 0 &&
           !g_thinking_streaming && !g_thinking_expanded;
}

void clay_thinking_toggle(void) {
    ensure_thinking();
    if (!clay_thinking_can_toggle()) return;
    if (g_thinking_seconds > 0) {
        clay_sayc(CLAY_GRAY, "Reasoning (%.1fs):\n%s",
                  g_thinking_seconds, g_thinking.data);
    } else {
        clay_sayc(CLAY_GRAY, "Reasoning:\n%s", g_thinking.data);
    }
    g_thinking_expanded = 1;
    fflush(stdout);
}

void clay_list_header(const char *fmt, ...) {
    print_prefix();
    fputs(clay_color(CLAY_GRAY), stdout);
    va_list args;
    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);
    fputs(clay_color(CLAY_RESET), stdout);
    fputc('\n', stdout);
}

void clay_list_step(int index, const char *verb, const char *target, const char *info, int link) {
    printf("%s%s%d.%s %s%s%s ", CLAY_INDENT, clay_color(CLAY_GRAY), index, clay_color(CLAY_RESET),
           clay_color(CLAY_WHITE), verb, clay_color(CLAY_RESET));

    fputs(clay_color(CLAY_CYAN), stdout);
    if (link) {
        clay_term_hyperlink_file(target);
    } else {
        fputs(target, stdout);
    }
    fputs(clay_color(CLAY_RESET), stdout);

    if (info) {
        printf(" %s(%s)%s", clay_color(CLAY_GRAY), info, clay_color(CLAY_RESET));
    }
    fputc('\n', stdout);
}

void clay_plan_step(ClayStepState state, const char *text) {
    const char *icon = state == CLAY_STEP_DONE     ? CLAY_ICON_CHECK
                       : state == CLAY_STEP_ACTIVE ? CLAY_ICON_ARROW
                                                   : CLAY_ICON_DOT;
    const char *icon_color = state == CLAY_STEP_DONE     ? CLAY_GREEN
                             : state == CLAY_STEP_ACTIVE ? CLAY_ORANGE
                                                         : CLAY_GRAY;
    const char *text_color = state == CLAY_STEP_ACTIVE ? CLAY_WHITE : CLAY_GRAY;
    printf("  %s%s%s ", clay_color(icon_color), icon, clay_color(CLAY_RESET));
    fputs(clay_color(text_color), stdout);
    int room = clay_term_width() - 5;
    if (room > 1 && (int)clay_utf8_width(text) > room) {
        clay_term_write_clipped(text, room - 1);
        fputs("\xe2\x80\xa6", stdout);
    } else {
        fputs(text, stdout);
    }
    printf("%s\n", clay_color(CLAY_RESET));
}

void clay_list_bullet(const char *fmt, ...) {
    printf("%s%s%s%s ", CLAY_INDENT, clay_color(CLAY_GRAY), CLAY_ICON_DOT, clay_color(CLAY_RESET));
    va_list args;
    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);
    fputc('\n', stdout);
}

/* One row per line: a wrapped tool line reads as two entries and pushes the
   real output off screen. */
void clay_tool_output_line(const char *fmt, ...) {
    printf("    %s%s%s ", clay_color(CLAY_GRAY), CLAY_ICON_DOT,
           clay_color(CLAY_RESET));
    ClayStr line;
    clay_str_init(&line);
    va_list args;
    va_start(args, fmt);
    clay_str_vprintf(&line, fmt, args);
    va_end(args);
    int room = clay_term_width() - 7; /* indent, bullet, space, last column */
    if (room > 1 && (int)clay_utf8_width(line.data) > room) {
        clay_term_write_clipped(line.data, room - 1);
        printf("%s…%s", clay_color(CLAY_GRAY), clay_color(CLAY_RESET));
    } else {
        fputs(line.data, stdout);
    }
    clay_str_free(&line);
    fputc('\n', stdout);
}
