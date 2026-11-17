#include "clay/chat.h"

#include "clay/json.h"
#include "clay/storage.h"

#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define WRITERS 2
#define LINES_PER_WRITER 50

static ClayChat *chat;
static long failures;

static void *write_lines(void *argument) {
    const char *kind = argument;
    for (int i = 0; i < LINES_PER_WRITER; i++) {
        ClayChatRequestUsage usage = {
            .provider = "openai-codex",
            .model = "gpt-5-codex",
            .kind = kind,
            .round = i,
            .input_tokens = 1000 + i,
            .output_tokens = 10 + i,
            .cached_input_tokens = 900,
            .cached_input_tokens_known = 1,
            .context_budget = 128000,
            .duration_ms = 42,
        };
        if (clay_chat_append_request_usage(chat, &usage) != 0)
            __atomic_add_fetch(&failures, 1, __ATOMIC_RELAXED);
    }
    return NULL;
}

int main(void) {
    char template[] = "/tmp/clay-requests-XXXXXX";
    char *home = mkdtemp(template);
    assert(home);
    assert(setenv("HOME", home, 1) == 0);

    chat = clay_chat_create("you are clay");
    assert(chat);
    char *id = strdup(clay_chat_id(chat));

    ClayChatRequestUsage first = {
        .provider = "openai",
        .model = "gpt-5",
        .kind = "main",
        .round = 0,
        .input_tokens = 3200,
        .output_tokens = 128,
        .cached_input_tokens = 3000,
        .cached_input_tokens_known = 1,
        .context_budget = 128000,
        .duration_ms = 850,
    };
    assert(clay_chat_append_request_usage(chat, &first) == 0);

    /* A provider that does not report cache usage must not write the key at
       all: a missing value and an explicit zero mean different things. */
    ClayChatRequestUsage unknown = first;
    unknown.cached_input_tokens_known = 0;
    unknown.kind = "compact";
    assert(clay_chat_append_request_usage(chat, &unknown) == 0);

    pthread_t threads[WRITERS];
    for (int i = 0; i < WRITERS; i++)
        assert(pthread_create(&threads[i], NULL, write_lines,
                              i == 0 ? "subagent" : "subagent") == 0);
    for (int i = 0; i < WRITERS; i++) assert(pthread_join(threads[i], NULL) == 0);
    assert(failures == 0);

    ClayStr relative;
    clay_str_init(&relative);
    clay_str_printf(&relative, "chats/%s/requests.jsonl", id);
    char *path = clay_storage_path(relative.data);
    clay_str_free(&relative);
    assert(path);
    ClayStr text;
    assert(clay_storage_read_limited(path, 4 * 1024 * 1024, &text) == 0);

    size_t lines = 0, parsed = 0;
    const char *cursor = text.data;
    while (cursor && *cursor) {
        const char *end = strchr(cursor, '\n');
        assert(end); /* every record ends its own line */
        size_t length = (size_t)(end - cursor);
        char *line = malloc(length + 1);
        assert(line);
        memcpy(line, cursor, length);
        line[length] = '\0';
        ClayJson *record = clay_json_parse(line, NULL);
        assert(record);
        free(line);
        assert(clay_json_type(record) == CLAY_JSON_OBJECT);
        const char *kind = clay_json_string_value(
            clay_json_object_get(record, "kind"));
        assert(kind && *kind);
        assert(clay_json_object_get(record, "input_tokens"));
        assert(clay_json_object_get(record, "context_budget"));
        if (strcmp(kind, "compact") == 0)
            assert(!clay_json_object_get(record, "cached_input_tokens"));
        clay_json_free(record);
        parsed++;
        lines++;
        cursor = end + 1;
    }
    assert(lines == 2 + WRITERS * LINES_PER_WRITER);
    assert(parsed == lines);

    clay_str_free(&text);
    clay_chat_destroy(chat);

    char cleanup[512];
    snprintf(cleanup, sizeof(cleanup), "%s/.clay/chats/%s/requests.jsonl", home, id);
    assert(remove(cleanup) == 0);
    snprintf(cleanup, sizeof(cleanup), "%s/.clay/chats/%s/chat.json", home, id);
    assert(remove(cleanup) == 0);
    snprintf(cleanup, sizeof(cleanup), "%s/.clay/chats/%s", home, id);
    assert(rmdir(cleanup) == 0);
    snprintf(cleanup, sizeof(cleanup), "%s/.clay/chats", home);
    assert(rmdir(cleanup) == 0);
    snprintf(cleanup, sizeof(cleanup), "%s/.clay", home);
    assert(rmdir(cleanup) == 0);
    assert(rmdir(home) == 0);
    free(id);
    return 0;
}
