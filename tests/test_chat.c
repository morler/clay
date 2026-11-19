#include "clay/chat.h"

#include "clay/providers/openai.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(void) {
    char template[] = "/tmp/clay-chat-XXXXXX";
    char *home = mkdtemp(template);
    assert(home);
    assert(setenv("HOME", home, 1) == 0);

    ClayChat *chat = clay_chat_create("you are clay");
    assert(chat);
    char *id = strdup(clay_chat_id(chat));
    assert(strcmp(clay_chat_system_prompt(chat), "you are clay") == 0);
    assert(strcmp(clay_chat_notes(chat), "") == 0);
    assert(clay_chat_set_notes(chat, "remember this") == 0);
    assert(strcmp(clay_chat_notes(chat), "remember this") == 0);
    ClayChatUsage saved_usage = {
        .input_tokens = 125,
        .output_tokens = 48,
        .cached_input_tokens = 98,
        .cached_input_tokens_known = 1,
        .total_input_tokens = 250,
        .total_output_tokens = 96,
        .last_prompt_tokens = 120,
    };
    assert(clay_chat_set_usage(chat, &saved_usage) == 0);
    assert(clay_chat_begin_turn(chat, "hello") == 0);
    clay_chat_set_active_thinking(chat, "first thought\nsecond thought", 1.25);

    ClayJson *messages = clay_json_array();
    clay_json_array_push(messages, clay_json_object());
    ClayJson *user = clay_json_array_get(messages, 0);
    clay_json_object_set(user, "role", clay_json_string("user"));
    clay_json_object_set(user, "content", clay_json_string("hello"));
    clay_json_array_push(messages, clay_json_object());
    ClayJson *assistant = clay_json_array_get(messages, 1);
    clay_json_object_set(assistant, "role", clay_json_string("assistant"));
    clay_json_object_set(assistant, "content", clay_json_string("hi"));
    assert(clay_chat_finish_turn(chat, messages, 0, "completed") == 0);
    clay_json_free(messages);
    clay_chat_destroy(chat);

    chat = clay_chat_load(id);
    assert(chat);
    assert(strcmp(clay_chat_system_prompt(chat), "you are clay") == 0);
    assert(strcmp(clay_chat_notes(chat), "remember this") == 0);
    ClayChatUsage loaded_usage;
    clay_chat_get_usage(chat, &loaded_usage);
    assert(loaded_usage.input_tokens == 125);
    assert(loaded_usage.output_tokens == 48);
    assert(loaded_usage.cached_input_tokens == 98);
    assert(loaded_usage.cached_input_tokens_known);
    assert(loaded_usage.total_input_tokens == 250);
    assert(loaded_usage.total_output_tokens == 96);
    assert(loaded_usage.last_prompt_tokens == 120);
    double thinking_seconds = 0;
    char *thinking = clay_chat_last_thinking(chat, &thinking_seconds);
    assert(thinking && strcmp(thinking, "first thought\nsecond thought") == 0);
    assert(thinking_seconds == 1.25);
    free(thinking);
    ClayJson *history = clay_chat_openai_messages(chat);
    assert(clay_json_array_count(history) == 2);
    assert(strcmp(clay_json_string_value(clay_json_object_get(clay_json_array_get(history, 0), "content")), "hello") == 0);
    assert(strcmp(clay_json_string_value(clay_json_object_get(clay_json_array_get(history, 1), "content")), "hi") == 0);
    clay_json_free(history);
    clay_chat_destroy(chat);

    /* A compaction replaces what came before it in the replay, while the
       journal keeps the old turns for anyone reading the file. */
    chat = clay_chat_create("you are clay");
    assert(chat);
    char *compacted_id = strdup(clay_chat_id(chat));
    assert(clay_chat_begin_turn(chat, "first") == 0);
    ClayJson *first_turn = clay_json_array();
    clay_json_array_push(first_turn, clay_openai_message("user", "first"));
    clay_json_array_push(first_turn, clay_openai_message("assistant", "one"));
    assert(clay_chat_finish_turn(chat, first_turn, 0, "completed") == 0);
    clay_json_free(first_turn);

    ClayJson *replacement = clay_json_array();
    clay_json_array_push(replacement, clay_openai_message(
        "user", "What the user asked for, word for word"));
    clay_json_array_push(replacement,
                         clay_openai_message("system", "summary of the rest"));
    assert(clay_chat_record_compaction(chat, replacement) == 0);
    clay_json_free(replacement);

    ClayJson *replayed = clay_chat_openai_messages(chat);
    assert(clay_json_array_count(replayed) == 2);
    assert(strcmp(clay_json_string_value(clay_json_object_get(
                      clay_json_array_get(replayed, 0), "content")),
                  "What the user asked for, word for word") == 0);
    assert(!strstr(clay_json_string_value(clay_json_object_get(
                       clay_json_array_get(replayed, 1), "content")),
                   "one"));
    clay_json_free(replayed);

    /* And it survives a save/load round trip. */
    clay_chat_destroy(chat);
    chat = clay_chat_load(compacted_id);
    assert(chat);
    replayed = clay_chat_openai_messages(chat);
    assert(clay_json_array_count(replayed) == 2);
    clay_json_free(replayed);
    /* The replay has two messages; the first turn's are replaced. */
    assert(clay_chat_message_count(chat) == 2);
    clay_chat_destroy(chat);
    free(compacted_id);
    /* Remove the compacted chat's directory too. */
    {
        ClayStr path;
        clay_str_init(&path);
        clay_str_printf(&path, "%s/.clay/chats/%s", home, compacted_id);
        char *json = malloc(strlen(path.data) + 16);
        assert(json);
        snprintf(json, strlen(path.data) + 16, "%s/chat.json", path.data);
        assert(remove(json) == 0);
        free(json);
        assert(rmdir(path.data) == 0);
        clay_str_free(&path);
    }

    ClayArray summaries;
    assert(clay_chat_list(&summaries) == 0);
    assert(summaries.count == 1);
    assert(strcmp(((ClayChatSummary *)clay_array_get(&summaries, 0))->id, id) == 0);
    clay_chat_list_free(&summaries);

    ClayStr path;
    clay_str_init(&path);
    clay_str_printf(&path, "%s/.clay/chats/%s/chat.json", home, id);
    assert(remove(path.data) == 0);
    clay_str_clear(&path);
    clay_str_printf(&path, "%s/.clay/chats/%s", home, id);
    assert(rmdir(path.data) == 0);
    clay_str_clear(&path);
    clay_str_printf(&path, "%s/.clay/chats/index.json", home);
    remove(path.data);
    clay_str_clear(&path);
    clay_str_printf(&path, "%s/.clay/chats", home);
    assert(rmdir(path.data) == 0);
    clay_str_clear(&path);
    clay_str_printf(&path, "%s/.clay", home);
    assert(rmdir(path.data) == 0);
    assert(rmdir(home) == 0);
    clay_str_free(&path);
    free(id);
    return 0;
}
