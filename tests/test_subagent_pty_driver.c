#define _GNU_SOURCE
#include "../src/commands/context.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

static int pty_probe(ClayScope *scope, const ClayOpenAICallbacks *callbacks, void *userdata) {
    (void)scope;
    int *batch = userdata;
    if ((*batch)++ == 1) {
        puts("SECOND");
        fflush(stdout);
        assert(!callbacks->should_abort(callbacks->userdata));
        return 0;
    }
    puts("READY");
    fflush(stdout);
    while (!callbacks->should_abort(callbacks->userdata))
        clay_term_sleep_ms(10);
    puts("CANCELLED");
    fflush(stdout);
    for (int i = 0; i < 40; i++) {
        assert(callbacks->should_abort(callbacks->userdata));
        clay_term_sleep_ms(10);
    }
    return 1;
}

int main(void) {
    ClayCommands c = {0};
    clay_array_init(&c.providers, sizeof(ClayConnectedProvider));
    clay_array_init(&c.tasks, sizeof(ClayBackgroundTask *));
    clay_array_init(&c.plan.todos, sizeof(ClayTodoItem));
    clay_array_init(&c.mcp_bindings, sizeof(ClayMcpToolBinding));
    clay_array_init(&c.mcp_servers, sizeof(ClayMcpServer *));
    c.mcp_connect_attempted = 1;
    c.mode = CLAY_MODE_ACT;
    c.scopes = clay_scope_registry_create();
    c.chat = clay_chat_create("system");
    assert(c.scopes && c.chat);
    assert(!setenv("OPENAI_API_KEY", "offline-key", 1));
    clay_commands_load_provider(&c, clay_commands_find_provider_type("openai"));
    c.selected_provider = "openai";
    c.selected_model = "model-a";
    int batch = 0;
    c.completion_test_hook = pty_probe;
    c.completion_test_userdata = &batch;
    clay_term_init();
    clay_term_raw_enable();
    ClayJson *args = clay_json_parse("{\"tasks\":[{\"description\":\"a\",\"prompt\":\"a\"}]}", NULL);
    assert(args);
    ClayJson *result = subagent_tool(args, &c);
    assert(!clay_json_bool_value(clay_json_object_get(result, "ok")));
    clay_json_free(result);
    result = subagent_tool(args, &c);
    clay_json_free(args);
    clay_json_free(result);
    puts("INPUT");
    fflush(stdout);
    char ch = 0;
    assert(clay_term_read_key(&ch) == CLAY_KEY_CHAR && ch == 'x');
    clay_term_raw_disable();
    puts("PRESERVED");
    return 0;
}
