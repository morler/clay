#include "clay/mcp.h"

#include "../src/commands/context.h"

#include "clay/storage.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static char mcp_home_template[] = "/tmp/clay_test_mcp_XXXXXX";
static size_t without_servers;
static ClayStr bare_tools_json;

int main(void) {
    /* The tool list must not depend on which servers are connected: a chat
       that connects one between requests would otherwise change its prefix
       and pay to re-cache it. The schema is fixed, and the per-server detail
       travels in a list result. */
    {   /* Before connecting, the registered tool set must already have mcp. */
        ClayApp *app = clay_app_create();
        ClayCommands *c = clay_commands_create(app);
        ClayToolSet set;
        clay_commands_tools_build(c, &c->plan, &set, 1);
        int found = 0;
        for (size_t i = 0; i < set.tools.count; i++) {
            ClayTool *t = clay_array_get((ClayArray *)&set.tools, i);
            if (strcmp(t->name, "mcp") == 0) found = 1;
        }
        assert(found);
        size_t without = set.tools.count;
        clay_str_init(&bare_tools_json);
        for (size_t i = 0; i < set.tools.count; i++) {
            ClayTool *t = clay_array_get((ClayArray *)&set.tools, i);
            ClayStr params;
            clay_str_init(&params);
            clay_json_stringify(t->parameters, &params);
            clay_str_printf(&bare_tools_json, "%s|%s|%s\n", t->name,
                            t->description ? t->description : "", params.data);
            clay_str_free(&params);
        }
        clay_commands_tools_free(&set);
        clay_commands_destroy(c);
        clay_app_destroy(app);
        without_servers = without;
    }
    ClayJson *schema_once = mcp_tool_schema();
    ClayJson *schema_twice = mcp_tool_schema();
    ClayStr first, second;
    clay_str_init(&first);
    clay_str_init(&second);
    clay_json_stringify(schema_once, &first);
    clay_json_stringify(schema_twice, &second);
    assert(strcmp(first.data, second.data) == 0);
    assert(strstr(first.data, "\"action\""));
    assert(strstr(first.data, "\"list\"") && strstr(first.data, "\"call\""));
    assert(!strstr(first.data, "mcp__")); /* no per-server names */
    clay_str_free(&first);
    clay_str_free(&second);
    clay_json_free(schema_once);
    clay_json_free(schema_twice);

    /* Config persistence, isolated from the real ~/.clay. */
    char *home = mkdtemp(mcp_home_template);
    assert(home);
    assert(setenv("HOME", home, 1) == 0);

    ClayArray servers;
    assert(clay_mcp_config_list(&servers) == 0);
    assert(servers.count == 0);
    clay_mcp_config_list_free(&servers);

    char *args[] = {"tests/fixtures/fake_mcp_server.py"};
    assert(clay_mcp_config_add("fake", "python3", args, 1) == 0);
    assert(clay_mcp_config_list(&servers) == 0);
    assert(servers.count == 1);
    ClayMcpServerConfig *config = clay_array_get(&servers, 0);
    assert(strcmp(config->name, "fake") == 0);
    assert(strcmp(config->command, "python3") == 0);
    assert(config->args.count == 1);
    clay_mcp_config_list_free(&servers);

    /* Adding again with the same name replaces, not duplicates. */
    assert(clay_mcp_config_add("fake", "python3", args, 1) == 0);
    assert(clay_mcp_config_list(&servers) == 0);
    assert(servers.count == 1);
    clay_mcp_config_list_free(&servers);

    assert(clay_mcp_config_remove("fake") == 0);
    assert(clay_mcp_config_list(&servers) == 0);
    assert(servers.count == 0);
    clay_mcp_config_list_free(&servers);

    /* Live protocol round-trip against a real (fake) MCP server. */
    char *server_argv[] = {"python3", "tests/fixtures/fake_mcp_server.py", NULL};
    ClayMcpServer *server = clay_mcp_connect("fake", "python3", server_argv);
    assert(server);
    assert(strcmp(clay_mcp_server_name(server), "fake") == 0);
    assert(clay_mcp_tool_count(server) == 2);

    const ClayMcpTool *echo = clay_mcp_find_tool(server, "echo");
    assert(echo);
    assert(strcmp(echo->description, "Echoes back its input.") == 0);
    assert(clay_json_type(echo->input_schema) == CLAY_JSON_OBJECT);
    assert(clay_mcp_find_tool(server, "nope") == NULL);

    ClayJson *echo_args = clay_json_object();
    clay_json_object_set(echo_args, "text", clay_json_string("hello from clay"));
    int is_error = 0;
    char *text = clay_mcp_call(server, "echo", echo_args, &is_error);
    clay_json_free(echo_args);
    assert(text);
    assert(!is_error);
    assert(strcmp(text, "hello from clay") == 0);
    free(text);

    is_error = 0;
    ClayJson *no_args = clay_json_object();
    text = clay_mcp_call(server, "fail", no_args, &is_error);
    clay_json_free(no_args);
    assert(text);
    assert(is_error);
    assert(strcmp(text, "it broke") == 0);
    free(text);

    /* Through the gateway, which is the only MCP tool registered now. */
    ClayCommands gateway_commands;
    memset(&gateway_commands, 0, sizeof(gateway_commands));
    clay_array_init(&gateway_commands.mcp_servers, sizeof(ClayMcpServer *));
    gateway_commands.mcp_connect_attempted = 1;
    clay_array_push_val(&gateway_commands.mcp_servers, &server);

    ClayJson *list_args = clay_json_object();
    clay_json_object_set(list_args, "action", clay_json_string("list"));
    ClayJson *listed = clay_mcp_gateway_tool(list_args, &gateway_commands);
    clay_json_free(list_args);
    assert(clay_json_bool_value(clay_json_object_get(listed, "ok")));
    ClayJson *listed_servers = clay_json_object_get(listed, "servers");
    assert(clay_json_array_count(listed_servers) == 1);
    ClayJson *listed_tools = clay_json_object_get(clay_json_array_get(listed_servers, 0), "tools");
    assert(clay_json_array_count(listed_tools) == 2);
    /* The schema the model needs to call a tool rides in the result, not in
       the tool list, and only for the tools actually connected. */
    ClayJson *first_tool = clay_json_array_get(listed_tools, 0);
    assert(strcmp(clay_json_string_value(clay_json_object_get(first_tool, "tool")), "echo") == 0);
    assert(clay_json_type(clay_json_object_get(first_tool, "arguments_schema")) == CLAY_JSON_OBJECT);
    clay_json_free(listed);

    ClayJson *call_args = clay_json_object();
    clay_json_object_set(call_args, "action", clay_json_string("call"));
    clay_json_object_set(call_args, "server", clay_json_string("fake"));
    clay_json_object_set(call_args, "tool", clay_json_string("echo"));
    ClayJson *call_arguments = clay_json_object();
    clay_json_object_set(call_arguments, "text", clay_json_string("via gateway"));
    clay_json_object_set(call_args, "arguments", call_arguments);
    ClayJson *called = clay_mcp_gateway_tool(call_args, &gateway_commands);
    clay_json_free(call_args);
    assert(clay_json_bool_value(clay_json_object_get(called, "ok")));
    assert(strcmp(clay_json_string_value(clay_json_object_get(called, "output")), "via gateway") == 0);
    clay_json_free(called);

    /* A tool-level error comes back as ok:false plus the server's text. */
    ClayJson *fail_args = clay_json_object();
    clay_json_object_set(fail_args, "action", clay_json_string("call"));
    clay_json_object_set(fail_args, "server", clay_json_string("fake"));
    clay_json_object_set(fail_args, "tool", clay_json_string("fail"));
    ClayJson *failed = clay_mcp_gateway_tool(fail_args, &gateway_commands);
    clay_json_free(fail_args);
    assert(!clay_json_bool_value(clay_json_object_get(failed, "ok")));
    assert(strcmp(clay_json_string_value(clay_json_object_get(failed, "error")), "it broke") == 0);
    clay_json_free(failed);

    /* An unknown server or tool says how to recover instead of failing blank. */
    ClayJson *unknown_args = clay_json_object();
    clay_json_object_set(unknown_args, "action", clay_json_string("call"));
    clay_json_object_set(unknown_args, "server", clay_json_string("nope"));
    clay_json_object_set(unknown_args, "tool", clay_json_string("echo"));
    ClayJson *unknown = clay_mcp_gateway_tool(unknown_args, &gateway_commands);
    clay_json_free(unknown_args);
    assert(!clay_json_bool_value(clay_json_object_get(unknown, "ok")));
    assert(strstr(clay_json_string_value(clay_json_object_get(unknown, "error")), "action=list"));
    clay_json_free(unknown);

    /* #139: connecting a server changes the results, never the tool list.
       The list result carries it; the schema stays byte-identical. */
    ClayJson *with_server = mcp_tool_schema();
    ClayStr after;
    clay_str_init(&after);
    clay_json_stringify(with_server, &after);
    ClayJson *before_json = mcp_tool_schema();
    ClayStr before;
    clay_str_init(&before);
    clay_json_stringify(before_json, &before);
    assert(strcmp(after.data, before.data) == 0);
    clay_str_free(&after);
    clay_str_free(&before);
    clay_json_free(with_server);
    clay_json_free(before_json);

    /* With the server configured in this HOME, a fresh session's tool list
       must be the same as one with nothing configured - the count and each
       byte. That is the property a prefix cache depends on. */
    {
        assert(clay_mcp_config_add("fake", "python3", args, 1) == 0);
        ClayApp *app = clay_app_create();
        ClayCommands *connected = clay_commands_create(app);
        ClayToolSet set;
        clay_commands_tools_build(connected, &connected->plan, &set, 1);
        assert(connected->mcp_servers.count == 1); /* it really connected */
        int gateway_count = 0;
        for (size_t i = 0; i < set.tools.count; i++) {
            ClayTool *t = clay_array_get((ClayArray *)&set.tools, i);
            if (strcmp(t->name, "mcp") == 0) gateway_count++;
        }
        assert(gateway_count == 1);
        size_t with_servers = set.tools.count;
        /* Byte for byte, not just the same count: a per-server tool sneaking
           in would change the count, but a changed description or schema
           would only show here. */
        ClayStr connected_json;
        clay_str_init(&connected_json);
        for (size_t i = 0; i < set.tools.count; i++) {
            ClayTool *t = clay_array_get((ClayArray *)&set.tools, i);
            ClayStr params;
            clay_str_init(&params);
            clay_json_stringify(t->parameters, &params);
            clay_str_printf(&connected_json, "%s|%s|%s\n", t->name,
                            t->description ? t->description : "", params.data);
            clay_str_free(&params);
        }
        clay_commands_tools_free(&set);
        /* Bytes first: a description or schema change keeps the count, so
           the count alone would pass a list that no longer matches. */
        assert(strcmp(bare_tools_json.data, connected_json.data) == 0);
        assert(without_servers == with_servers);
        clay_str_free(&connected_json);
        clay_commands_destroy(connected);
        clay_app_destroy(app);
        assert(clay_mcp_config_remove("fake") == 0);
    }

    /* /mcp remove also cuts the live connection: a removed server that the
       model can still call is the opposite of what the message says. */
    {
        assert(clay_mcp_config_add("fake", "python3", args, 1) == 0);
        ClayMcpServer *live = clay_mcp_connect("fake", "python3", server_argv);
        assert(live);
        ClayCommands *removing = clay_commands_create(NULL);
        assert(removing);
        clay_array_push_val(&removing->mcp_servers, &live);
        removing->mcp_connect_attempted = 1;
        assert(removing->mcp_servers.count == 1);

        clay_cmd_mcp("remove fake", removing);
        assert(removing->mcp_servers.count == 0);

        /* The list no longer names it, and a call says it is unknown. */
        ClayJson *list_args2 = clay_json_object();
        clay_json_object_set(list_args2, "action", clay_json_string("list"));
        ClayJson *listed2 = clay_mcp_gateway_tool(list_args2, removing);
        clay_json_free(list_args2);
        ClayStr listed_text;
        clay_str_init(&listed_text);
        clay_json_stringify(listed2, &listed_text);
        assert(!strstr(listed_text.data, "\"server\":\"fake\""));
        clay_str_free(&listed_text);
        clay_json_free(listed2);

        ClayJson *call_args2 = clay_json_object();
        clay_json_object_set(call_args2, "action", clay_json_string("call"));
        clay_json_object_set(call_args2, "server", clay_json_string("fake"));
        clay_json_object_set(call_args2, "tool", clay_json_string("echo"));
        ClayJson *called2 = clay_mcp_gateway_tool(call_args2, removing);
        clay_json_free(call_args2);
        assert(!clay_json_bool_value(clay_json_object_get(called2, "ok")));
        clay_json_free(called2);

        /* Removing a configured server that was never connected still works,
           and does not touch the array. */
        assert(clay_mcp_config_add("configured-only", "python3", args, 1) == 0);
        clay_cmd_mcp("remove configured-only", removing);
        assert(removing->mcp_servers.count == 0);

        clay_commands_destroy(removing);
    }

    clay_array_remove(&gateway_commands.mcp_servers, 0);
    clay_array_free(&gateway_commands.mcp_servers);

    clay_mcp_disconnect(server);

    /* Connecting to a nonexistent program fails cleanly. */
    char *bad_argv[] = {"clay-does-not-exist-anywhere", NULL};
    ClayMcpServer *bad = clay_mcp_connect("bad", "clay-does-not-exist-anywhere", bad_argv);
    assert(bad == NULL);

    /* This test's own HOME, torn down so a run leaves nothing behind.
       `home` is the template array itself, so it is never freed. */
    char *under_home = clay_storage_path("");
    if (under_home) {
        char *servers = clay_storage_path("mcp_servers.json");
        char *prompt = clay_storage_path("system_prompt.json");
        if (servers) remove(servers);
        if (prompt) remove(prompt);
        free(servers);
        free(prompt);
        rmdir(under_home);
        free(under_home);
    }
    rmdir(home);

    printf("mcp tests passed\n");
    return 0;
}
