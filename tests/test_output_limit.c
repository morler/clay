#include "../src/commands/context.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const char *expected_url;
static const char *expected_field;
static long expected_limit;
static int requests;
static int test_tools;
static int test_chat_limit;
static int tool_calls;
static long output_tokens;

static const char *header(const ClayHttpRequest *req, const char *name) {
  for (size_t i = 0; i < req->header_count; i++)
    if (strcmp(req->headers[i].name, name) == 0)
      return req->headers[i].value;
  return NULL;
}

static const char *response_events(const ClayJson *body) {
  if (!test_tools)
    return "data: {\"type\":\"response.output_text.delta\",\"delta\":\"ok\"}\n\n"
           "data: {\"type\":\"response.completed\"}\n\n";
  ClayJson *tool = clay_json_array_get(clay_json_object_get(body, "tools"), 0);
  ClayJson *strict = clay_json_object_get(tool, "strict");
  assert(clay_json_type(strict) == CLAY_JSON_BOOL && !clay_json_bool_value(strict));
  ClayJson *input = clay_json_object_get(body, "input");
  ClayJson *previous = clay_json_array_get(input, 0);
  assert(strcmp(clay_json_string_value(clay_json_object_get(previous, "content")),
                "previous reply") == 0);
  if (!requests)
    return "data: {\"type\":\"response.output_item.added\",\"item\":{"
           "\"type\":\"function_call\",\"id\":\"fc_1\",\"call_id\":\"call_1\","
           "\"name\":\"lookup\",\"arguments\":\"\"}}\n\n"
           "data: {\"type\":\"response.function_call_arguments.delta\","
           "\"item_id\":\"fc_1\",\"delta\":\"{\\\"id\\\":7}\"}\n\n"
           "data: {\"type\":\"response.output_item.done\",\"item\":{"
           "\"type\":\"function_call\",\"id\":\"fc_1\",\"call_id\":\"call_1\","
           "\"name\":\"lookup\",\"arguments\":\"{\\\"id\\\":7}\"}}\n\n"
           "data: {\"type\":\"response.completed\"}\n\n";
  ClayJson *call = clay_json_array_get(input, 2);
  ClayJson *result = clay_json_array_get(input, 3);
  assert(strcmp(clay_json_string_value(clay_json_object_get(call, "arguments")),
                "{\"id\":7}") == 0);
  assert(strcmp(clay_json_string_value(clay_json_object_get(result, "call_id")),
                "call_1") == 0);
  assert(strcmp(clay_json_string_value(clay_json_object_get(result, "output")),
                "\"found\"") == 0);
  return "data: {\"type\":\"response.output_text.delta\",\"delta\":\"partial\"}\n\n"
         "data: {\"type\":\"response.output_item.added\",\"item\":{"
         "\"type\":\"function_call\",\"id\":\"fc_2\",\"call_id\":\"call_2\","
         "\"name\":\"lookup\",\"arguments\":\"\"}}\n\n"
         "data: {\"type\":\"response.incomplete\",\"response\":{"
         "\"incomplete_details\":{\"reason\":\"max_output_tokens\"},"
         "\"usage\":{\"input_tokens\":40,\"output_tokens\":32}}}\n\n";
}

int __wrap_clay_http_request(const ClayHttpRequest *req, ClayHttpResponse *resp) {
  assert(strcmp(req->url, expected_url) == 0);
  assert(strcmp(req->method, "POST") == 0);
  ClayJson *body = clay_json_parse(req->body, NULL);
  assert(body);
  const char *fields[] = {"max_completion_tokens", "max_output_tokens"};
  for (size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); i++) {
    const ClayJson *value = clay_json_object_get(body, fields[i]);
    if (expected_field && strcmp(expected_field, fields[i]) == 0) {
      assert(clay_json_type(value) == CLAY_JSON_NUMBER);
      if (clay_json_number_value(value) != expected_limit)
        fprintf(stderr, "%s: expected %ld, got %.0f\n", fields[i],
                expected_limit, clay_json_number_value(value));
      assert(clay_json_number_value(value) == expected_limit);
    } else {
      assert(!value);
    }
  }
  *resp = (ClayHttpResponse){.status = 200};
  const char *wire = "data: {\"choices\":[{\"delta\":{\"content\":\"ok\"}}]}\n\n";
  if (test_chat_limit) {
    wire = "data: {\"choices\":[{\"delta\":{\"content\":\"partial\",\"tool_calls\":[{"
           "\"index\":0,\"id\":\"call_1\",\"function\":{\"name\":\"lookup\","
           "\"arguments\":\"{\\\"id\\\":\"}}]}}]}\n\n"
           "data: {\"choices\":[{\"delta\":{},\"finish_reason\":\"length\"}]}\n\n";
  } else if (strcmp(req->url, "https://api.openai.com/v1/responses") == 0) {
    assert(strcmp(header(req, "Authorization"), "Bearer mock-responses-key") == 0);
    assert(!header(req, "ChatGPT-Account-ID"));
    wire = response_events(body);
  } else if (strcmp(req->url, "https://chatgpt.com/backend-api/codex/responses") == 0) {
    assert(strcmp(header(req, "Authorization"), "Bearer mock-oauth-token") == 0);
    assert(strcmp(header(req, "ChatGPT-Account-ID"), "mock-account") == 0);
    wire = response_events(body);
  }
  clay_json_free(body);
  requests++;
  assert(req->on_chunk(wire, strlen(wire), req->userdata) == 0);
  return 0;
}

static ClayJson *lookup(const ClayJson *arguments, void *unused) {
  (void)unused;
  assert(clay_json_number_value(clay_json_object_get(arguments, "id")) == 7);
  tool_calls++;
  return clay_json_string("found");
}

static void collect_usage(long input, long output, void *unused) {
  (void)unused;
  assert(input == 40);
  output_tokens = output;
}

static void run(ClayCommands *commands, const char *provider, const char *url,
                const char *field, long limit) {
  const ClayProviderType *type = clay_commands_find_provider_type(provider);
  assert(type);
  clay_commands_load_provider(commands, type);
  free(commands->selected_provider);
  commands->selected_provider = strdup(provider);
  assert(commands->selected_provider);
  expected_url = url;
  expected_field = field;
  expected_limit = limit;
  requests = 0;
  tool_calls = 0;
  ClayJson *messages = clay_json_array();
  ClayOpenAICallbacks callbacks = {0};
  ClayToolSet set = {0};
  ClayJson *parameters = NULL;
  if (test_tools || test_chat_limit) {
    parameters = clay_json_parse("{\"type\":\"object\",\"properties\":{"
                                 "\"id\":{\"type\":\"integer\"}}}", NULL);
    assert(parameters);
    clay_array_init(&set.tools, sizeof(ClayTool));
    ClayTool tool = {.name = "lookup", .parameters = parameters, .fn = lookup};
    clay_array_push_val(&set.tools, &tool);
    if (test_tools) {
      clay_json_array_push(messages, clay_openai_message("assistant", "previous reply"));
      callbacks.on_usage = collect_usage;
    }
  }
  clay_json_array_push(messages, clay_openai_message("user", "hi"));
  int rc = clay_commands_run_completion(commands, messages, parameters ? &set : NULL,
                                        2, NULL, &callbacks);
  if (rc != 0)
    fprintf(stderr, "%s: completion returned %d after %d requests\n", provider, rc, requests);
  assert(rc == 0);
  assert(requests == (test_tools ? 2 : 1));
  if (parameters) {
    assert(tool_calls == (test_tools ? 1 : 0));
    if (test_tools)
      assert(output_tokens == limit);
    ClayJson *reply = clay_json_array_get(messages, clay_json_array_count(messages) - 1);
    assert(strcmp(clay_json_string_value(clay_json_object_get(reply, "content")),
                  "partial") == 0);
    clay_array_free(&set.tools);
    clay_json_free(parameters);
  }
  clay_json_free(messages);
}

static void config(const char *path, const char *data) {
  FILE *file = fopen(path, "w");
  assert(file);
  assert(fputs(data, file) >= 0 && fclose(file) == 0);
}

int main(void) {
  char home[] = "/tmp/clay_output_limit_XXXXXX";
  assert(mkdtemp(home) && setenv("HOME", home, 1) == 0);
  assert(clay_storage_ensure_dir("") == 0);
  char *path = clay_storage_path("config.json");
  assert(path);
  assert(setenv("OPENAI_API_KEY", "mock-api-key", 1) == 0);
  unsetenv("OPENAI_BASE_URL");
  unsetenv("CLAY_OPENAI_BASE_URL");

  ClayCommands *commands = clay_commands_create(NULL);
  assert(commands);
  free(commands->selected_model);
  commands->selected_model = strdup("model");
  const char *chat = "https://api.openai.com/v1/chat/completions";
  run(commands, "openai", chat, NULL, 0);
  config(path, "{\"max_output_tokens\":2.5}");
  run(commands, "openai", chat, NULL, 0);
  config(path, "{\"max_output_tokens\":0,\"context_window\":512}");
  run(commands, "openai", chat, NULL, 0);
  config(path, "{\"max_output_tokens\":32}");
  test_chat_limit = 1;
  run(commands, "openai", chat, "max_completion_tokens", 32);
  test_chat_limit = 0;
  config(path, "{\"max_output_tokens\":200,\"context_window\":400}");
  run(commands, "openai", chat, "max_completion_tokens", 86);
  config(path, "{\"max_output_tokens\":200,\"context_window\":16}");
  run(commands, "openai", chat, "max_completion_tokens", 1);
  assert(setenv("OPENAI_BASE_URL", "https://example.test/api", 1) == 0);
  run(commands, "openai", "https://example.test/api/chat/completions", NULL, 0);

  assert(setenv("OPENAI_RESPONSES_API_KEY", "mock-responses-key", 1) == 0);
  const char *responses = "https://api.openai.com/v1/responses";
  run(commands, "openai-responses", responses, "max_output_tokens", 16);
  config(path, "{\"max_output_tokens\":15}");
  run(commands, "openai-responses", responses, NULL, 0);
  config(path, "{\"max_output_tokens\":32}");
  test_tools = 1;
  run(commands, "openai-responses", responses, "max_output_tokens", 32);
  test_tools = 0;
  assert(setenv("OPENAI_CODEX_ACCESS_TOKEN", "mock-oauth-token", 1) == 0);
  assert(setenv("OPENAI_CODEX_REFRESH_TOKEN", "mock-refresh-token", 1) == 0);
  assert(setenv("OPENAI_CODEX_ACCOUNT_ID", "mock-account", 1) == 0);
  unsetenv("OPENAI_CODEX_EXPIRES_AT");
  unsetenv("CLAY_CODEX_EXPIRES_AT");
  run(commands, "openai-codex", "https://chatgpt.com/backend-api/codex/responses",
      NULL, 0);

  const struct {
    const char *name;
    const char *key;
    const char *base;
  } compatible[] = {
      {"openrouter", "OPENROUTER_API_KEY", "OPENROUTER_BASE_URL"},
      {"groq", "GROQ_API_KEY", "GROQ_BASE_URL"},
      {"deepseek", "DEEPSEEK_API_KEY", "DEEPSEEK_BASE_URL"},
      {"mistral", "MISTRAL_API_KEY", "MISTRAL_BASE_URL"},
      {"together", "TOGETHER_API_KEY", "TOGETHER_BASE_URL"},
      {"custom", "CLAY_API_KEY", "CLAY_BASE_URL"},
      {"grok", "XAI_API_KEY", "GROK_BASE_URL"},
  };
  unsetenv("GROK_ACCESS_TOKEN");
  unsetenv("CLAY_GROK_ACCESS_TOKEN");
  unsetenv("GROK_REFRESH_TOKEN");
  unsetenv("CLAY_GROK_REFRESH_TOKEN");
  for (size_t i = 0; i < sizeof(compatible) / sizeof(compatible[0]); i++) {
    const ClayProviderType *type = clay_commands_find_provider_type(compatible[i].name);
    assert(type);
    const char *base = type->default_base_url ? type->default_base_url : "https://example.test";
    assert(setenv(compatible[i].key, "mock-api-key", 1) == 0);
    assert(setenv(compatible[i].base, base, 1) == 0);
    char url[128];
    int len = snprintf(url, sizeof(url), "%s/chat/completions", base);
    assert(len > 0 && (size_t)len < sizeof(url));
    run(commands, compatible[i].name, url, NULL, 0);
  }
  clay_commands_destroy(commands);
  assert(unlink(path) == 0);
  free(path);
  path = clay_storage_path("system_prompt.json");
  assert(path && unlink(path) == 0);
  free(path);
  char *dir = clay_storage_path("");
  assert(dir && rmdir(dir) == 0 && rmdir(home) == 0);
  free(dir);
  puts("output limit tests passed");
  return 0;
}
