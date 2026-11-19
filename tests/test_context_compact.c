#include "../src/commands/context.h"

#include "clay/json.h"
#include "clay/storage.h"
#include "clay/str.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void test_context_window_config(const char *path) {
    assert(clay_config_context_window() == 0);
    assert(clay_config_context_token_budget() == -1);

    const char *invalid_configs[] = {
        "{}",
        "{\"context_window\":0}",
        "{\"context_window\":-16000}",
        "{\"context_window\":\"16000\"}",
        "{\"context_window\":16000.5}",
        "{\"context_window\":1e20}",
    };
    for (size_t i = 0; i < sizeof(invalid_configs) / sizeof(invalid_configs[0]); i++) {
        const char *config = invalid_configs[i];
        assert(clay_storage_write_atomic_private(path, config, strlen(config)) == 0);
        assert(clay_config_context_window() == 0);
    }

    const char config[] = "{\"context_window\":16000}";
    assert(clay_storage_write_atomic_private(path, config, sizeof(config) - 1) == 0);
    assert(clay_config_context_window() == 16000);
    assert(clay_config_selection_save("openai", "custom-model") == 0);
    assert(clay_config_context_window() == 16000);
    assert(clay_config_context_token_budget() == -1);
}

static void push_message(ClayJson *conversation, const char *role, const char *content) {
    ClayJson *message = clay_json_object();
    clay_json_object_set(message, "role", clay_json_string(role));
    clay_json_object_set(message, "content", clay_json_string(content));
    clay_json_array_push(conversation, message);
}

static void test_utf8_preview(const char *character, int prefix, int included) {
    ClayStr output;
    clay_str_init(&output);
    for (int i = 0; i < prefix; i++) clay_str_push_char(&output, 'x');
    clay_str_push(&output, character);
    for (int i = 0; i < 210; i++) clay_str_push_char(&output, 'y');

    ClayCommands commands = {0};
    commands.last_prompt_tokens = 200000;
    commands.conversation = clay_json_array();
    push_message(commands.conversation, "system", "system");
    push_message(commands.conversation, "user", "old");
    push_message(commands.conversation, "tool", output.data);
    for (int i = 0; i < 6; i++) push_message(commands.conversation, "user", "new");
    assert(clay_commands_maybe_compact(&commands) == 1);
    const char *summary = clay_json_string_value(clay_json_object_get(
        clay_json_array_get(commands.conversation, 2), "content"));
    const char *preview = strchr(summary, '\n');
    assert(preview);
    preview++;
    assert(strncmp(preview, output.data, prefix) == 0);
    preview += prefix;
    if (included) {
        size_t bytes = strlen(character);
        assert(strncmp(preview, character, bytes) == 0);
        preview += bytes;
    }
    assert(strcmp(preview, "...") == 0);
    clay_json_free(commands.conversation);
    clay_str_free(&output);
}

/* Two providers report usage per request: with the same history and three
   rounds, the sum of the rounds is three times one round's prompt. The
   budget is about what the next request costs, so three rounds at 40k must
   not read as 120k and force a collapse of a conversation that still fits. */
static void test_multi_round_accumulation(void) {
    ClayCommands commands;
    memset(&commands, 0, sizeof(commands));
    commands.conversation = clay_json_array();
    push_message(commands.conversation, "system", "you are clay");
    ClayStr long_output;
    clay_str_init(&long_output);
    for (int i = 0; i < 900; i++) clay_str_push_char(&long_output, 'x');
    for (int turn = 0; turn < 8; turn++) {
        push_message(commands.conversation, "user", "do something");
        push_message(commands.conversation, "tool", long_output.data);
        push_message(commands.conversation, "assistant", "done");
    }

    /* Three rounds at 40k leave the context at 40k. */
    commands.accounted_messages = clay_json_array_count(commands.conversation);
    commands.last_prompt_tokens = 40000;
    assert(clay_commands_maybe_compact(&commands) == 0);

    commands.last_prompt_tokens = 127999;
    assert(clay_commands_maybe_compact(&commands) == 0);
    commands.last_prompt_tokens = 128000;
    assert(clay_commands_maybe_compact(&commands) == 2);

    clay_json_free(commands.conversation);
    clay_str_free(&long_output);
}

static void test_configured_compaction_budget(void) {
    ClayCommands commands = {0};
    commands.conversation = clay_json_array();
    char output[512];
    memset(output, 'x', sizeof(output) - 1);
    output[sizeof(output) - 1] = '\0';
    push_message(commands.conversation, "system", "system");
    push_message(commands.conversation, "user", "old");
    push_message(commands.conversation, "tool", output);
    for (int i = 0; i < 6; i++) push_message(commands.conversation, "user", "new");
    commands.accounted_messages = clay_json_array_count(commands.conversation);

    assert(clay_config_set_context_token_budget(0) == 0);
    assert(clay_config_context_token_budget() == 0);
    commands.last_prompt_tokens = 200000;
    assert(clay_commands_maybe_compact(&commands) == 0);

    assert(clay_config_set_context_token_budget(512) == 0);
    assert(clay_config_context_token_budget() == 512);
    commands.last_prompt_tokens = 511;
    assert(clay_commands_maybe_compact(&commands) == 0);
    commands.last_prompt_tokens = 512;
    assert(clay_commands_maybe_compact(&commands) == 1);

    assert(clay_config_set_context_token_budget(-1) == 0);
    assert(clay_config_context_token_budget() == -1);
    clay_json_free(commands.conversation);
}

int main(void) {
    char home[] = "/tmp/clay_test_context_compact_XXXXXX";
    assert(mkdtemp(home));
    assert(setenv("HOME", home, 1) == 0);
    assert(clay_storage_ensure_dir("") == 0);
    char *path = clay_storage_path("config.json");
    assert(path);
    test_context_window_config(path);

    test_utf8_preview("é", 199, 0);
    test_utf8_preview("中", 198, 0);
    test_utf8_preview("😀", 197, 0);
    test_utf8_preview("é", 198, 1);
    test_utf8_preview("中", 197, 1);
    test_utf8_preview("😀", 196, 1);

    ClayStr long_output;
    clay_str_init(&long_output);
    for (int i = 0; i < 500; i++) clay_str_push_char(&long_output, 'x');

    ClayCommands commands;
    memset(&commands, 0, sizeof(commands));
    commands.conversation = clay_json_array();
    push_message(commands.conversation, "system", "you are clay");

    /* 8 turns of [user, tool(long), assistant]; keep window is 6 turns. */
    for (int turn = 0; turn < 8; turn++) {
        push_message(commands.conversation, "user", "do something");
        push_message(commands.conversation, "tool", long_output.data);
        push_message(commands.conversation, "assistant", "done");
    }

    /* Below budget: no-op. */
    commands.last_prompt_tokens = 0;
    assert(clay_commands_maybe_compact(&commands) == 0);
    ClayJson *first_tool = clay_json_array_get(commands.conversation, 2);
    assert(strcmp(clay_json_string_value(clay_json_object_get(first_tool, "content")), long_output.data) == 0);

    /* Above budget: collapses the two oldest turns' tool results only. */
    commands.last_prompt_tokens = 200000;
    int collapsed = clay_commands_maybe_compact(&commands);
    assert(collapsed == 2);

    ClayJson *turn1_tool = clay_json_array_get(commands.conversation, 2);
    ClayJson *turn2_tool = clay_json_array_get(commands.conversation, 5);
    ClayJson *turn3_tool = clay_json_array_get(commands.conversation, 8);
    ClayJson *turn8_tool = clay_json_array_get(commands.conversation, 23);
    const char *turn1_content = clay_json_string_value(clay_json_object_get(turn1_tool, "content"));
    const char *turn2_content = clay_json_string_value(clay_json_object_get(turn2_tool, "content"));
    const char *turn3_content = clay_json_string_value(clay_json_object_get(turn3_tool, "content"));
    const char *turn8_content = clay_json_string_value(clay_json_object_get(turn8_tool, "content"));
    assert(strncmp(turn1_content, "[collapsed", 10) == 0);
    assert(strncmp(turn2_content, "[collapsed", 10) == 0);
    assert(strcmp(turn3_content, long_output.data) == 0);
    assert(strcmp(turn8_content, long_output.data) == 0);

    /* Idempotent: running again doesn't re-collapse already-collapsed content. */
    assert(clay_commands_maybe_compact(&commands) == 0);

    /* User/assistant text is never touched. */
    ClayJson *turn1_user = clay_json_array_get(commands.conversation, 1);
    assert(strcmp(clay_json_string_value(clay_json_object_get(turn1_user, "content")), "do something") == 0);

    clay_json_free(commands.conversation);
    clay_str_free(&long_output);
    test_multi_round_accumulation();
    test_configured_compaction_budget();
    assert(unlink(path) == 0);
    free(path);
    char *root = clay_storage_root();
    assert(root && rmdir(root) == 0);
    free(root);
    assert(rmdir(home) == 0);
    printf("context compact tests passed\n");
    return 0;
}
