#include "../src/commands/context.h"

#include "clay/json.h"
#include "clay/str.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Offline baseline for #78: how many bytes a realistic conversation sends,
   and what the request would be estimated to cost at clay's own yardstick
   (half the request bytes plus the output reserve, see scope.c). No network
   and no provider: the fixture is fixed, so a change in these numbers is a
   change in what clay sends. */

/* Same constants scope.c admits requests with. */
#define OUTPUT_RESERVE_TOKENS 4096

static size_t bytes_at_least(size_t tokens) { return tokens * 2; }

static void report(const char *label, const ClayJson *messages,
                   const ClayToolSet *set) {
    ClayStr serialized;
    clay_str_init(&serialized);
    clay_json_stringify(messages, &serialized);
    size_t payload = serialized.len;
    size_t tools = 0;
    for (size_t i = 0; i < set->tools.count; i++) {
        ClayTool *tool = clay_array_get((ClayArray *)&set->tools, i);
        ClayStr parameters;
        clay_str_init(&parameters);
        clay_json_stringify(tool->parameters, &parameters);
        tools += strlen(tool->name) + strlen(tool->description ? tool->description : "") +
                 parameters.len;
        clay_str_free(&parameters);
    }
    size_t total = payload + tools;
    size_t estimated = total / 2 + OUTPUT_RESERVE_TOKENS;
    printf("%-24s messages %7zu B  tools %7zu B  total %7zu B  ~%zu tokens\n",
           label, payload, tools, total, estimated);
    assert(payload > 0 && tools > 0);
    assert(bytes_at_least(estimated) >= total);
    clay_str_free(&serialized);
}

/* The same three shapes every session has: a system prompt, a user turn,
   an assistant answer with a tool call and its result. */
static ClayJson *fixture_conversation(const char *system_prompt) {
    ClayJson *messages = clay_json_array();
    clay_json_array_push(messages, clay_openai_message("system", system_prompt));
    clay_json_array_push(messages, clay_openai_message(
        "user", "Find where the shell command timeout is applied and show me "
                "the code."));
    clay_json_array_push(messages, clay_openai_message(
        "assistant", "I'll grep for it first."));
    ClayJson *call = clay_json_object();
    clay_json_object_set(call, "id", clay_json_string("call_1"));
    ClayJson *function = clay_json_object();
    clay_json_object_set(function, "name", clay_json_string("grep"));
    clay_json_object_set(function, "arguments",
                         clay_json_string("{\"pattern\":\"timeout\"}"));
    clay_json_object_set(call, "function", function);
    ClayJson *calls = clay_json_array();
    clay_json_array_push(calls, call);
    ClayJson *assistant = clay_json_object();
    clay_json_object_set(assistant, "role", clay_json_string("assistant"));
    clay_json_object_set(assistant, "content", clay_json_string(""));
    clay_json_object_set(assistant, "tool_calls", calls);
    clay_json_array_push(messages, assistant);
    clay_json_array_push(
        messages,
        clay_openai_message("tool",
                           "src/commands/message.c:9:#define CLAY_SHELL_OUTPUT_LIMIT "
                           "(64 * 1024)\nsrc/sandbox/linux.c:41:timeout_seconds = 120"));
    clay_json_array_push(messages, clay_openai_message(
        "assistant", "The timeout is applied in src/commands/message.c around "
                     "the shell_exec tool: it defaults to 120 seconds."));
    return messages;
}

int main(void) {
    ClayApp *app = clay_app_create();
    ClayCommands *commands = clay_commands_create(app);
    assert(commands);

    ClayToolSet set;
    clay_commands_tools_build(commands, &commands->plan, &set, 1);
    printf("tools: %zu\n", set.tools.count);

    /* A fresh chat: only the system prompt goes out. */
    ClayJson *fresh = clay_json_array();
    clay_json_array_push(fresh, clay_openai_message(
        "system", clay_commands_system_prompt_base()));
    report("fresh chat", fresh, &set);
    clay_json_free(fresh);

    /* A real turn: prompt + user + one tool round trip. */
    ClayJson *turn = fixture_conversation(clay_commands_system_prompt_base());
    report("one tool round trip", turn, &set);
    clay_json_free(turn);

    /* A provider that never reports usage would be charged the estimate, so
       the number above is the ceiling, not the bill. */
    clay_commands_tools_free(&set);
    clay_commands_destroy(commands);
    clay_app_destroy(app);
    return 0;
}
