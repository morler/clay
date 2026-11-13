#include <assert.h>
#include <string.h>
#include <stdlib.h>

#include "../src/render/prompt.c"

int main(void) {
    ClayStr buf;
    ClayArray blocks;
    clay_str_init(&buf);
    clay_array_init(&blocks, sizeof(ClayPasteBlock));
    clay_str_push(&buf, "A中🙂[paste]尾");
    ClayPasteBlock block = {8, 15, strdup("pasted text")};
    clay_array_push_val(&blocks, &block);
    size_t cursor = buf.len;
    prompt_cursor_left(&buf, &blocks, &cursor);
    assert(cursor == 15);
    prompt_cursor_left(&buf, &blocks, &cursor);
    assert(cursor == 8);
    prompt_cursor_left(&buf, &blocks, &cursor);
    assert(cursor == 4);
    prompt_backspace(&buf, &blocks, &cursor);
    assert(cursor == 1 && strcmp(buf.data, "A🙂[paste]尾") == 0);
    ClayPasteBlock *b = clay_array_get(&blocks, 0);
    assert(b->start == 5 && b->end == 12);
    prompt_cursor_right(&buf, &blocks, &cursor);
    assert(cursor == 5);
    prompt_cursor_right(&buf, &blocks, &cursor);
    assert(cursor == 12);
    prompt_backspace(&buf, &blocks, &cursor);
    assert(cursor == 5 && blocks.count == 0 && strcmp(buf.data, "A🙂尾") == 0);
    prompt_backspace(&buf, &blocks, &cursor);
    assert(cursor == 1 && strcmp(buf.data, "A尾") == 0);
    assert(utf8_prev(buf.data, buf.len) == 1);
    paste_blocks_free(&blocks);
    clay_array_free(&blocks);
    clay_str_free(&buf);
    clay_str_init(&buf);
    completion_add_clipped(&buf, "中文abc", 5);
    assert(strcmp(buf.data, "中...") == 0);
    clay_str_clear(&buf);
    completion_add_clipped(&buf, "中a", 1);
    assert(strcmp(buf.data, "") == 0);
    clay_str_clear(&buf);
    completion_add_clipped(&buf, "e\xcc\x81中文", 2);
    assert(strcmp(buf.data, "e\xcc\x81") == 0);
    clay_str_free(&buf);
    return 0;
}
