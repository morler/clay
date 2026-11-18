#include "context.h"

#include <stdlib.h>
#include <string.h>

void clay_commands_connect_mcp_servers(ClayCommands *commands) {
    if (commands->mcp_connect_attempted) return;
    commands->mcp_connect_attempted = 1;

    ClayArray configs;
    if (clay_mcp_config_list(&configs) != 0 || configs.count == 0) {
        clay_mcp_config_list_free(&configs);
        return;
    }

    for (size_t i = 0; i < configs.count; i++) {
        ClayMcpServerConfig *config = clay_array_get(&configs, i);
        ClayTask *task = clay_app_task_start(commands->app, "Connecting to MCP server: %s", config->name);

        ClayArray argv_storage;
        clay_array_init(&argv_storage, sizeof(char *));
        clay_array_push_val(&argv_storage, &config->command);
        for (size_t j = 0; j < config->args.count; j++) {
            clay_array_push_val(&argv_storage, clay_array_get(&config->args, j));
        }
        char *null_terminator = NULL;
        clay_array_push_val(&argv_storage, &null_terminator);

        ClayMcpServer *server = clay_mcp_connect(config->name, config->command, argv_storage.data);
        clay_array_free(&argv_storage);

        if (!server) {
            clay_app_task_fail(commands->app, task, "could not connect");
            continue;
        }
        size_t tool_count = clay_mcp_tool_count(server);
        clay_app_task_success(commands->app, task, "%zu tool%s", tool_count, tool_count == 1 ? "" : "s");
        clay_array_push_val(&commands->mcp_servers, &server);
    }
    clay_mcp_config_list_free(&configs);
}

static void split_whitespace(const char *text, ClayArray *tokens) {
    clay_array_init(tokens, sizeof(char *));
    const char *p = text;
    while (*p) {
        while (*p == ' ') p++;
        if (!*p) break;
        const char *start = p;
        while (*p && *p != ' ') p++;
        ClayStr token;
        clay_str_init(&token);
        clay_str_push_n(&token, start, (size_t)(p - start));
        clay_array_push_val(tokens, &token.data);
    }
}

static void free_tokens(ClayArray *tokens) {
    for (size_t i = 0; i < tokens->count; i++) free(*(char **)clay_array_get(tokens, i));
    clay_array_free(tokens);
}

static void print_configured(void) {
    ClayArray configs;
    clay_mcp_config_list(&configs);
    if (configs.count == 0) {
        clay_sayc(CLAY_GRAY, "No MCP servers configured. Add one with /mcp add <name> <command> [args...].");
        clay_mcp_config_list_free(&configs);
        return;
    }
    clay_list_header("Configured MCP servers:");
    for (size_t i = 0; i < configs.count; i++) {
        ClayMcpServerConfig *config = clay_array_get(&configs, i);
        ClayStr line;
        clay_str_init(&line);
        clay_str_push(&line, config->command);
        for (size_t j = 0; j < config->args.count; j++) {
            clay_str_printf(&line, " %s", *(char **)clay_array_get(&config->args, j));
        }
        clay_list_step((int)(i + 1), config->name, line.data, NULL, 0);
        clay_str_free(&line);
    }
    clay_mcp_config_list_free(&configs);
}

void clay_cmd_mcp(const char *args, void *user_data) {
    ClayCommands *commands = user_data;

    if (args && strncmp(args, "add ", 4) == 0) {
        ClayArray tokens;
        split_whitespace(args + 4, &tokens);
        if (tokens.count < 2) {
            clay_sayc(CLAY_RED, "Usage: /mcp add <name> <command> [args...]");
        } else {
            char *name = *(char **)clay_array_get(&tokens, 0);
            char *command = *(char **)clay_array_get(&tokens, 1);
            size_t arg_count = tokens.count - 2;
            char **argv_tail = arg_count > 0 ? clay_array_get(&tokens, 2) : NULL;
            if (clay_mcp_config_add(name, command, argv_tail, arg_count) == 0) {
                clay_sayc(CLAY_GREEN, "Added MCP server %s - it connects the next time you send a message.", name);
            } else {
                clay_sayc(CLAY_RED, "Could not save that MCP server.");
            }
        }
        free_tokens(&tokens);
        return;
    }

    if (args && strncmp(args, "remove ", 7) == 0) {
        const char *name = args + 7;
        while (*name == ' ') name++;
        if (!*name) {
            clay_sayc(CLAY_RED, "Usage: /mcp remove <name>");
            return;
        }
        /* Also cut the live connection: removing the config while the model
           can still call the server is the opposite of what "remove" says. */
        for (size_t i = 0; i < commands->mcp_servers.count; i++) {
            ClayMcpServer *server = *(ClayMcpServer **)clay_array_get(&commands->mcp_servers, i);
            if (strcmp(clay_mcp_server_name(server), name) != 0) continue;
            clay_mcp_disconnect(server);
            clay_array_remove(&commands->mcp_servers, i);
            break;
        }
        if (clay_mcp_config_remove(name) == 0) clay_sayc(CLAY_GREEN, "Removed %s.", name);
        else clay_sayc(CLAY_RED, "Usage: /mcp remove <name>");
        return;
    }

    if (args && *args) {
        clay_sayc(CLAY_RED, "Usage: /mcp, /mcp add <name> <command> [args...], /mcp remove <name>");
        return;
    }

    if (commands->mcp_connect_attempted) {
        if (commands->mcp_servers.count == 0) {
            clay_sayc(CLAY_GRAY, "No MCP servers connected this session.");
        } else {
            for (size_t i = 0; i < commands->mcp_servers.count; i++) {
                ClayMcpServer *server = *(ClayMcpServer **)clay_array_get(&commands->mcp_servers, i);
                size_t tool_count = clay_mcp_tool_count(server);
                clay_sayc(CLAY_GREEN, "%s: connected, %zu tool%s.", clay_mcp_server_name(server), tool_count,
                         tool_count == 1 ? "" : "s");
            }
        }
    }
    print_configured();
}

/* One tool in front of every MCP server's tools. Registering each discovered
   tool instead makes the tool list depend on which servers happened to
   connect, so the prefix changes mid-session, and lets their own schemas
   grow its byte cost. The schemas ride in the list result instead, where
   only the call that needs one pays for it. */
ClayJson *mcp_tool_schema(void) {
    ClayJson *action = clay_json_object();
    clay_json_object_set(action, "type", clay_json_string("string"));
    ClayJson *action_enum = clay_json_array();
    clay_json_array_push(action_enum, clay_json_string("list"));
    clay_json_array_push(action_enum, clay_json_string("call"));
    clay_json_object_set(action, "enum", action_enum);

    ClayJson *server = clay_json_object();
    clay_json_object_set(server, "type", clay_json_string("string"));

    ClayJson *tool = clay_json_object();
    clay_json_object_set(tool, "type", clay_json_string("string"));

    ClayJson *arguments = clay_json_object();
    clay_json_object_set(arguments, "type", clay_json_string("object"));

    ClayJson *properties = clay_json_object();
    clay_json_object_set(properties, "action", action);
    clay_json_object_set(properties, "server", server);
    clay_json_object_set(properties, "tool", tool);
    clay_json_object_set(properties, "arguments", arguments);
    ClayJson *required = clay_json_array();
    clay_json_array_push(required, clay_json_string("action"));
    ClayJson *schema = clay_json_object();
    clay_json_object_set(schema, "type", clay_json_string("object"));
    clay_json_object_set(schema, "properties", properties);
    clay_json_object_set(schema, "required", required);
    clay_json_object_set(schema, "additionalProperties", clay_json_bool(0));
    return schema;
}

static ClayJson *mcp_list_result(ClayCommands *commands) {
    ClayJson *result = clay_json_object();
    ClayJson *servers = clay_json_array();
    for (size_t i = 0; i < commands->mcp_servers.count; i++) {
        ClayMcpServer *server = *(ClayMcpServer **)clay_array_get(&commands->mcp_servers, i);
        ClayJson *entry = clay_json_object();
        clay_json_object_set(entry, "server",
                             clay_json_string(clay_mcp_server_name(server)));
        ClayJson *tools = clay_json_array();
        for (size_t j = 0; j < clay_mcp_tool_count(server); j++) {
            const ClayMcpTool *tool = clay_mcp_tool_at(server, j);
            ClayJson *record = clay_json_object();
            clay_json_object_set(record, "tool", clay_json_string(tool->name));
            clay_json_object_set(record, "description",
                                 clay_json_string(tool->description ? tool->description : ""));
            /* The schema rides in the result, not in the tool list: only the
               call that needs it pays for it. */
            clay_json_object_set(record, "arguments_schema",
                                 clay_json_clone(tool->input_schema));
            clay_json_array_push(tools, record);
        }
        clay_json_object_set(entry, "tools", tools);
        clay_json_array_push(servers, entry);
    }
    clay_json_object_set(result, "ok", clay_json_bool(1));
    clay_json_object_set(result, "servers", servers);
    if (clay_json_array_count(servers) == 0)
        clay_json_object_set(
            result, "output",
            clay_json_string("No MCP servers are connected this session."));
    return result;
}

/* The tool's entry point; userdata is a ClayCommands*. */
ClayJson *clay_mcp_gateway_tool(const ClayJson *arguments, void *userdata) {
    ClayCommands *commands = userdata;
    clay_commands_connect_mcp_servers(commands);
    const char *action =
        clay_json_string_value(clay_json_object_get(arguments, "action"));
    if (!action || strcmp(action, "list") == 0)
        return mcp_list_result(commands);
    if (strcmp(action, "call") != 0) {
        ClayJson *result = clay_json_object();
        clay_json_object_set(result, "ok", clay_json_bool(0));
        clay_json_object_set(result, "error",
                             clay_json_string("action must be list or call"));
        return result;
    }
    const char *server_name =
        clay_json_string_value(clay_json_object_get(arguments, "server"));
    const char *tool_name =
        clay_json_string_value(clay_json_object_get(arguments, "tool"));
    ClayJson *result = clay_json_object();
    ClayMcpServer *server = NULL;
    for (size_t i = 0; server_name && i < commands->mcp_servers.count; i++) {
        ClayMcpServer *candidate =
            *(ClayMcpServer **)clay_array_get(&commands->mcp_servers, i);
        if (strcmp(clay_mcp_server_name(candidate), server_name) == 0) {
            server = candidate;
            break;
        }
    }
    if (!server || !tool_name || !clay_mcp_find_tool(server, tool_name)) {
        clay_json_object_set(result, "ok", clay_json_bool(0));
        clay_json_object_set(
            result, "error",
            clay_json_string("unknown server or tool; call with action=list"));
        return result;
    }
    ClayJson *call_arguments =
        clay_json_clone(clay_json_object_get(arguments, "arguments"));
    if (!call_arguments) call_arguments = clay_json_object();
    int is_error = 0;
    char *text = clay_mcp_call(server, tool_name, call_arguments, &is_error);
    clay_json_free(call_arguments);
    if (!text) {
        clay_json_object_set(result, "ok", clay_json_bool(0));
        clay_json_object_set(result, "error",
                             clay_json_string("MCP request failed (server may have exited)"));
        return result;
    }
    clay_json_object_set(result, "ok", clay_json_bool(!is_error));
    clay_json_object_set(result, "output", clay_json_string(text));
    clay_json_object_set(result, "output_truncated", clay_json_bool(0));
    if (is_error) clay_json_object_set(result, "error", clay_json_string(text));
    free(text);
    return result;
}
