/* Run after make build:
   cc -std=gnu11 -Wall -Wextra -Iinclude -Isrc -c tests/test_message.c -o /tmp/clay-test-message.o
   cc $(find build -name '*.o' ! -path 'build/tests*' ! -path 'build/main.o' ! -path 'build/commands/message.o') /tmp/clay-test-message.o -o /tmp/clay-test-message -lcurl -lpthread
   /tmp/clay-test-message
*/
#define clay_commands_run_completion test_run_completion
#include "../src/commands/message.c"
#undef clay_commands_run_completion

#include <assert.h>
#include <sys/stat.h>
#include <unistd.h>

static int completion_result;
static int fail_save;
static int collapse_during_turn; /* armed */
static int collapsed_once;
static int sse_error;
static int usage_reports;
static const char *partial_saved;

int test_run_completion(ClayCommands *commands, ClayJson *messages,
                        const ClayToolSet *tools, int max_rounds,
                        const char *cache_key, const ClayOpenAICallbacks *callbacks) {
  (void)messages; (void)tools; (void)max_rounds; (void)cache_key; (void)callbacks;
  /* Pretend the provider reported a nearly full window, then let the
     request hook shrink the live array: a single turn's own tool results must
     be reachable, which is what the old per-turn check could not do. */
  if (collapse_during_turn && !collapsed_once) {
    commands->last_prompt_tokens = 120000;
    commands->input_tokens = 120000;
    commands->accounted_messages = 1;
    for (int round = 0; round < 3; round++) {
      ClayJson *tool = clay_openai_message("tool", "old result");
      ClayJson *blob = clay_json_object();
      clay_json_object_set(blob, "role", clay_json_string("tool"));
      ClayStr big;
      clay_str_init(&big);
      for (int i = 0; i < 1000; i++) clay_str_push_char(&big, 'z');
      clay_json_object_set(blob, "content", clay_json_string(big.data));
      clay_str_free(&big);
      (void)tool;
      clay_json_array_push(messages, blob);
    }
    /* Older than the last six user turns, which is what makes a tool result
       eligible for collapsing at all. */
    for (int turn = 0; turn < 6; turn++) {
      ClayJson *user = clay_openai_message("user", "next");
      ClayJson *assistant = clay_openai_message("assistant", "ok");
      clay_json_array_push(messages, user);
      clay_json_array_push(messages, assistant);
    }
    if (callbacks->before_request)
      callbacks->before_request(messages, callbacks->userdata);
    /* The turn's own trailing results are the ones that could never be
       reached before: check they were collapsed in the array being sent. */
    int collapsed_blobs = 0;
    for (size_t i = 0; i < clay_json_array_count(messages); i++) {
      ClayJson *message = clay_json_array_get(messages, i);
      if (strcmp(clay_json_string_value(clay_json_object_get(message, "role")),
                 "tool") != 0)
        continue;
      const char *text =
          clay_json_string_value(clay_json_object_get(message, "content"));
      if (text && strncmp(text, "[collapsed", 10) == 0) collapsed_blobs++;
    }
    assert(collapsed_blobs == 3);
    collapsed_once = 1;
  }
  /* One usage event per provider request. */
  for (int i = 0; i < usage_reports; i++) {
    ClayTokenUsage usage = {.input_tokens = 1000 + i, .output_tokens = 10,
                            .cached_input_tokens = 900,
                            .cached_input_tokens_known = 1};
    if (callbacks->on_usage_details)
      callbacks->on_usage_details(&usage, callbacks->userdata);
  }
  if (sse_error) {
    ClayOpenAIError error = {.status = 200, .from_sse = 1,
                             .kind = CLAY_OPENAI_ERROR_RATE_LIMIT,
                             .body = "secret-credential"};
    callbacks->on_http_error(&error, callbacks->userdata);
    callbacks->on_error(200, error.body, callbacks->userdata);
  }
  if (partial_saved) {
    callbacks->on_token("before", callbacks->userdata);
    clay_json_array_push(messages, clay_openai_message("assistant", partial_saved));
  }
  if (fail_save) {
    ClayStr path;
    clay_str_init(&path);
    clay_str_printf(&path, "%s/.clay/chats/%s/chat.json", getenv("HOME"),
                    clay_chat_id(commands->chat));
    assert(remove(path.data) == 0);
    assert(mkdir(path.data, 0700) == 0);
    clay_str_free(&path);
  }
  return completion_result;
}

int main(void) {
  ClayJson *saved_messages = clay_json_array();
  clay_json_array_push(saved_messages, clay_openai_message("user", "hi"));
  clay_json_array_push(saved_messages, clay_openai_message("assistant", "one"));
  clay_json_array_push(saved_messages, clay_openai_message("tool", "result"));
  clay_json_array_push(saved_messages, clay_openai_message("assistant", "two"));
  ClayStr saved_response; clay_str_init(&saved_response);
  clay_str_push(&saved_response, "onetwothree");
  save_unrecorded_response(saved_messages, 0, &saved_response);
  assert(clay_json_array_count(saved_messages) == 5);
  assert(!strcmp(clay_json_string_value(clay_json_object_get(clay_json_array_get(saved_messages, 4), "content")), "three"));
  save_unrecorded_response(saved_messages, 0, &saved_response);
  assert(clay_json_array_count(saved_messages) == 5);
  clay_str_free(&saved_response); clay_json_free(saved_messages);
  char home[] = "/tmp/clay-message-XXXXXX";
  assert(mkdtemp(home));
  assert(setenv("HOME", home, 1) == 0);
  ClayApp *app = clay_app_create();
  ClayCommands *commands = clay_commands_create(app);
  static const ClayProviderType type = {"test", "test", ""};
  ClayConnectedProvider provider = {.type = &type};
  *(ClayConnectedProvider *)clay_array_push(&commands->providers) = provider;
  commands->selected_provider = strdup("test");
  commands->selected_model = strdup("test");
  FILE *output = tmpfile();
  assert(output);
  int original_stdout = dup(STDOUT_FILENO);
  assert(original_stdout >= 0);
  fflush(stdout);
  assert(dup2(fileno(output), STDOUT_FILENO) >= 0);

  int results[] = {0, CLAY_RUN_OUT_OF_ROUNDS, 1, -1};
  usage_reports = 2;
  for (size_t i = 0; i < sizeof(results) / sizeof(results[0]); i++) {
    for (fail_save = 0; fail_save <= 1; fail_save++) {
      commands->chat = clay_chat_create("test");
      assert(commands->chat);
      completion_result = results[i];
      sse_error = results[i] == -1;
      off_t start = lseek(STDOUT_FILENO, 0, SEEK_CUR);
      clay_commands_run_message(commands, "hello");
      fflush(stdout);
      off_t end = lseek(STDOUT_FILENO, 0, SEEK_CUR);
      assert(start >= 0 && end >= start);
      char *text = calloc((size_t)(end - start) + 1, 1);
      assert(text);
      assert(pread(fileno(output), text, (size_t)(end - start), start) == end - start);
      assert((strstr(text, "Could not save the chat journal.") != NULL) == fail_save);
      if (sse_error) {
        assert(strstr(text, "SSE 429; HTTP 200"));
        assert(!strstr(text, "secret-credential"));
      }
      free(text);
      /* Two billed requests under "main", in order, with the provider and
         model the session selected. */
      ClayStr log_path;
      clay_str_init(&log_path);
      clay_str_printf(&log_path, "%s/.clay/chats/%s/requests.jsonl", home,
                      clay_chat_id(commands->chat));
      ClayStr log_text;
      if (clay_storage_read_limited(log_path.data, 1024 * 1024, &log_text) != 0)
        clay_str_init(&log_text);
      size_t record_count = 0;
      for (const char *cursor = log_text.data; cursor && *cursor;) {
        const char *line_end = strchr(cursor, '\n');
        assert(line_end);
        size_t line_len = (size_t)(line_end - cursor);
        char *line = malloc(line_len + 1);
        assert(line);
        memcpy(line, cursor, line_len);
        line[line_len] = '\0';
        ClayJson *record = clay_json_parse(line, NULL);
        free(line);
        assert(record);
        assert(strcmp(clay_json_string_value(clay_json_object_get(record, "kind")), "main") == 0);
        assert(strcmp(clay_json_string_value(clay_json_object_get(record, "provider")), "test") == 0);
        assert(strcmp(clay_json_string_value(clay_json_object_get(record, "model")), "test") == 0);
        assert(clay_json_number_value(clay_json_object_get(record, "round")) == (double)record_count);
        clay_json_free(record);
        record_count++;
        cursor = line_end + 1;
      }
      assert(record_count == (size_t)usage_reports);
      clay_str_free(&log_text);
      remove(log_path.data);
      clay_str_free(&log_path);
      ClayStr path;
      clay_str_init(&path);
      clay_str_printf(&path, "%s/.clay/chats/%s/chat.json", home,
                      clay_chat_id(commands->chat));
      assert((fail_save ? rmdir(path.data) : remove(path.data)) == 0);
      clay_str_clear(&path);
      clay_str_printf(&path, "%s/.clay/chats/%s", home,
                      clay_chat_id(commands->chat));
      assert(rmdir(path.data) == 0);
      clay_str_free(&path);
      clay_chat_destroy(commands->chat);
      commands->chat = NULL;
    }
  }
  sse_error = 0;
  usage_reports = 0;
  collapse_during_turn = 1;
  collapsed_once = 0;
  commands->chat = clay_chat_create("test");
  assert(commands->chat);
  clay_commands_run_message(commands, "hello");
  assert(collapsed_once == 1);
  collapse_during_turn = 0;
  {
    ClayStr path;
    clay_str_init(&path);
    clay_str_printf(&path, "%s/.clay/chats/%s/requests.jsonl", home,
                    clay_chat_id(commands->chat));
    remove(path.data);
    clay_str_clear(&path);
    clay_str_printf(&path, "%s/.clay/chats/%s/chat.json", home,
                    clay_chat_id(commands->chat));
    remove(path.data);
    clay_str_clear(&path);
    clay_str_printf(&path, "%s/.clay/chats/%s", home, clay_chat_id(commands->chat));
    rmdir(path.data);
    clay_str_free(&path);
  }
  clay_chat_destroy(commands->chat);
  commands->chat = NULL;
  const char *partials[] = {"before", "fore"};
  for (size_t i = 0; i < 2; i++) {
    fail_save = 0;
    completion_result = -1;
    partial_saved = partials[i];
    commands->chat = clay_chat_create("test");
    assert(commands->chat);
    clay_commands_run_message(commands, "hello");
    ClayStr path;
    clay_str_init(&path);
    clay_str_printf(&path, "%s/.clay/chats/%s/chat.json", home, clay_chat_id(commands->chat));
    FILE *f = fopen(path.data, "rb");
    assert(f);
    assert(fseek(f, 0, SEEK_END) == 0);
    long len = ftell(f);
    assert(len > 0 && fseek(f, 0, SEEK_SET) == 0);
    char *json = calloc((size_t)len + 1, 1);
    assert(json && fread(json, 1, (size_t)len, f) == (size_t)len);
    fclose(f);
    ClayJson *root = clay_json_parse(json, NULL);
    free(json);
    ClayJson *turns = clay_json_object_get(root, "turns");
    ClayJson *turn = clay_json_array_get(turns, clay_json_array_count(turns) - 1);
    ClayJson *records = clay_json_object_get(turn, "messages");
    assert(clay_json_array_count(records) == (i == 0 ? 2 : 3));
    ClayJson *last = clay_json_array_get(records, clay_json_array_count(records) - 1);
    assert(!strcmp(clay_json_string_value(clay_json_object_get(last, "text")), "before"));
    clay_json_free(root);
    assert(remove(path.data) == 0);
    clay_str_clear(&path);
    clay_str_printf(&path, "%s/.clay/chats/%s/requests.jsonl", home,
                    clay_chat_id(commands->chat));
    remove(path.data);
    clay_str_clear(&path);
    clay_str_printf(&path, "%s/.clay/chats/%s", home, clay_chat_id(commands->chat));
    assert(rmdir(path.data) == 0);
    clay_str_free(&path);
    clay_chat_destroy(commands->chat);
    commands->chat = NULL;
  }
  partial_saved = NULL;
  fflush(stdout);
  assert(dup2(original_stdout, STDOUT_FILENO) >= 0);
  close(original_stdout);
  fclose(output);
  clay_commands_destroy(commands);
  clay_app_destroy(app);
  ClayStr path;
  clay_str_init(&path);
  clay_str_printf(&path, "%s/.clay/chats/index.json", home);
  remove(path.data);
  clay_str_clear(&path);
  clay_str_printf(&path, "%s/.clay/chats", home);
  assert(rmdir(path.data) == 0);
  clay_str_clear(&path);
  clay_str_printf(&path, "%s/.clay/system_prompt.json", home);
  remove(path.data);
  clay_str_clear(&path);
  clay_str_printf(&path, "%s/.clay", home);
  assert(rmdir(path.data) == 0);
  assert(rmdir(home) == 0);
  clay_str_free(&path);
  return 0;
}
