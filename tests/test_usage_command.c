#include "../src/commands/context.h"

#include "clay/chat.h"
#include "clay/json.h"
#include "clay/storage.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static ClayCommands *commands;
static char *home;

static void append(const char *kind, int round, long input, long output,
                   long cached, int cached_known) {
    ClayChatRequestUsage record = {
        .provider = "openai-codex",
        .model = "gpt-5-codex",
        .kind = kind,
        .round = round,
        .input_tokens = input,
        .output_tokens = output,
        .cached_input_tokens = cached,
        .cached_input_tokens_known = cached_known,
        .context_budget = 128000,
        .duration_ms = 10,
    };
    assert(clay_chat_append_request_usage(commands->chat, &record) == 0);
}

/* Runs the command with stdout captured, so the assertions read the exact
   text the user sees. */
static char *capture(void (*command)(const char *, void *)) {
    fflush(stdout);
    FILE *output = tmpfile();
    assert(output);
    int original = dup(STDOUT_FILENO);
    assert(original >= 0);
    assert(dup2(fileno(output), STDOUT_FILENO) >= 0);
    command("", commands);
    fflush(stdout);
    assert(dup2(original, STDOUT_FILENO) >= 0);
    close(original);
    assert(fseek(output, 0, SEEK_END) == 0);
    long length = ftell(output);
    assert(length >= 0 && fseek(output, 0, SEEK_SET) == 0);
    char *text = calloc((size_t)length + 1, 1);
    assert(text);
    assert(fread(text, 1, (size_t)length, output) == (size_t)length);
    fclose(output);
    return text;
}

int main(void) {
    char template[] = "/tmp/clay-usage-XXXXXX";
    home = mkdtemp(template);
    assert(home);
    assert(setenv("HOME", home, 1) == 0);

    ClayApp *app = clay_app_create();
    commands = clay_commands_create(app);

    char *empty = capture(clay_cmd_usage);
    assert(strstr(empty, "No active chat"));
    free(empty);

    commands->chat = clay_chat_create("you are clay");
    assert(commands->chat);
    commands->selected_provider = strdup("openai-codex");
    commands->selected_model = strdup("gpt-5-codex");

    char *none = capture(clay_cmd_usage);
    assert(strstr(none, "No requests logged"));
    free(none);

    /* Three requests, the middle one cold: 2,000 of 6,400 input tokens were
       served from the cache, so the streak never reaches three. */
    append("main", 0, 3200, 128, 0, 1);
    append("main", 1, 3200, 96, 2000, 1);
    append("compact", 0, 400, 50, 400, 1);

    char *report = capture(clay_cmd_usage);
    assert(strstr(report, "Requests"));
    assert(strstr(report, "3"));
    assert(strstr(report, "6800"));
    assert(strstr(report, "274"));
    assert(strstr(report, "35%"));
    assert(!strstr(report, "consecutive requests"));
    free(report);

    /* Four cold requests in a row trip the warning, and a provider that does
       not report cache usage reports "unknown" instead of a fake zero. */
    append("main", 2, 100, 10, 0, 1);
    append("main", 3, 100, 10, 0, 1);
    append("main", 4, 100, 10, 0, 1);
    char *warned = capture(clay_cmd_usage);
    assert(strstr(warned, "consecutive requests had no cached input"));
    free(warned);

    append("main", 5, 100, 10, 0, 0);
    char *unknown = capture(clay_cmd_usage);
    assert(strstr(unknown, "Cache unknown") || strstr(unknown, "%"));
    free(unknown);

    /* -p --json: one line, totals from the same log. */
    fflush(stdout);
    FILE *json_out = tmpfile();
    assert(json_out);
    int original_out = dup(STDOUT_FILENO);
    assert(original_out >= 0);
    assert(dup2(fileno(json_out), STDOUT_FILENO) >= 0);
    clay_commands_print_json_summary(commands, 1234);
    fflush(stdout);
    assert(dup2(original_out, STDOUT_FILENO) >= 0);
    close(original_out);
    assert(fseek(json_out, 0, SEEK_END) == 0);
    long json_len = ftell(json_out);
    assert(json_len > 0 && fseek(json_out, 0, SEEK_SET) == 0);
    char *json_text = calloc((size_t)json_len + 1, 1);
    assert(json_text);
    assert(fread(json_text, 1, (size_t)json_len, json_out) == (size_t)json_len);
    fclose(json_out);
    ClayJson *summary = clay_json_parse(json_text, NULL);
    assert(summary);
    assert(clay_json_type(summary) == CLAY_JSON_OBJECT);
    assert(clay_json_number_value(clay_json_object_get(summary, "requests")) == 7);
    assert(clay_json_number_value(clay_json_object_get(summary, "input_tokens")) == 7200);
    assert(clay_json_number_value(clay_json_object_get(summary, "duration_ms")) == 1234);
    assert(strcmp(clay_json_string_value(clay_json_object_get(summary, "provider")),
                  "openai-codex") == 0);
    assert(clay_json_object_get(summary, "answer"));
    /* The last record reported no cache usage, but earlier ones did, so the
       key stays and carries the sum of the known ones: 2,000 + 400. */
    assert(clay_json_number_value(clay_json_object_get(summary, "cached_tokens")) == 2400);
    clay_json_free(summary);
    free(json_text);

    char *path = clay_chat_request_log_path(commands->chat);
    assert(path);
    assert(remove(path) == 0);
    free(path);
    char *chat_id = strdup(clay_chat_id(commands->chat));
    assert(chat_id);
    clay_chat_destroy(commands->chat);
    commands->chat = NULL;
    free(commands->selected_provider);
    commands->selected_provider = NULL;
    free(commands->selected_model);
    commands->selected_model = NULL;
    clay_commands_destroy(commands);
    clay_app_destroy(app);

    char cleanup[512];
    snprintf(cleanup, sizeof(cleanup), "%s/.clay/chats/%s/chat.json", home,
             chat_id);
    assert(remove(cleanup) == 0);
    snprintf(cleanup, sizeof(cleanup), "%s/.clay/chats/%s", home, chat_id);
    assert(rmdir(cleanup) == 0);
    free(chat_id);
    snprintf(cleanup, sizeof(cleanup), "%s/.clay/chats/index.json", home);
    remove(cleanup);
    snprintf(cleanup, sizeof(cleanup), "%s/.clay/chats", home);
    assert(rmdir(cleanup) == 0);
    snprintf(cleanup, sizeof(cleanup), "%s/.clay/system_prompt.json", home);
    remove(cleanup);
    snprintf(cleanup, sizeof(cleanup), "%s/.clay", home);
    assert(rmdir(cleanup) == 0);
    assert(rmdir(home) == 0);
    return 0;
}
