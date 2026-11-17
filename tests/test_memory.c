#include "clay/memory.h"

#include "clay/str.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(void) {
    char template[] = "/tmp/clay-memory-XXXXXX";
    char *home = mkdtemp(template);
    assert(home);
    assert(setenv("HOME", home, 1) == 0);

    assert(clay_memory_valid_slug("auth-decision-1"));
    assert(!clay_memory_valid_slug(""));
    assert(!clay_memory_valid_slug("Has-Upper"));
    assert(!clay_memory_valid_slug("../escape"));
    assert(!clay_memory_valid_slug("has/slash"));

    assert(clay_memory_read("missing") == NULL);
    assert(clay_memory_write("bad slug", "note", "x", "y") != 0);

    assert(clay_memory_write("auth-decision", "decision", "Rewrote auth for compliance", "Long body here.") == 0);
    assert(clay_memory_write("bug-fix-1", "bug-fix", "Fixed off-by-one in parser", "Details.") == 0);

    char *content = clay_memory_read("auth-decision");
    assert(content);
    assert(strcmp(content, "Long body here.") == 0);
    free(content);

    char *index = clay_memory_index();
    assert(strstr(index, "auth-decision [decision]: Rewrote auth for compliance"));
    assert(strstr(index, "bug-fix-1 [bug-fix]: Fixed off-by-one in parser"));
    free(index);

    /* Overwriting a slug replaces the entry, not duplicates it. */
    assert(clay_memory_write("auth-decision", "decision", "Updated summary", "New body.") == 0);
    content = clay_memory_read("auth-decision");
    assert(strcmp(content, "New body.") == 0);
    free(content);
    index = clay_memory_index();
    assert(strstr(index, "Updated summary"));
    assert(!strstr(index, "Rewrote auth for compliance"));
    free(index);

    /* The index is capped: forty entries by newest, then a line that says
       where the rest are. */
    for (int i = 0; i < 40; i++) {
        char slug[16];
        snprintf(slug, sizeof(slug), "filler-%02d", i);
        assert(clay_memory_write(slug, "task", "filler entry", "body") == 0);
    }
    index = clay_memory_index();
    assert(index);
    assert(strlen(index) <= 4 * 1024);
    assert(strstr(index, "entries; memory_list and memory_read show the rest"));
    assert(strstr(index, "filler entry"));
    /* Newest first: the last filler written is listed before the first. */
    char *newest = strstr(index, "filler-39");
    char *oldest = strstr(index, "filler-32");
    assert(newest && oldest && newest < oldest);
    assert(!strstr(index, "filler-00")); /* beyond the ceiling */
    free(index);

    ClayStr path;
    clay_str_init(&path);
    clay_str_printf(&path, "%s/.clay/memory/index.json", home);
    assert(remove(path.data) == 0);
    clay_str_clear(&path);
    clay_str_printf(&path, "%s/.clay/memory/auth-decision.md", home);
    assert(remove(path.data) == 0);
    clay_str_clear(&path);
    clay_str_printf(&path, "%s/.clay/memory/bug-fix-1.md", home);
    assert(remove(path.data) == 0);
    for (int i = 0; i < 40; i++) {
        char slug[16], file[512];
        snprintf(slug, sizeof(slug), "filler-%02d", i);
        snprintf(file, sizeof(file), "%s/.clay/memory/%s.md", home, slug);
        assert(remove(file) == 0);
    }
    clay_str_clear(&path);
    clay_str_printf(&path, "%s/.clay/memory", home);
    assert(rmdir(path.data) == 0);
    clay_str_clear(&path);
    clay_str_printf(&path, "%s/.clay", home);
    assert(rmdir(path.data) == 0);
    assert(rmdir(home) == 0);
    clay_str_free(&path);
    return 0;
}
