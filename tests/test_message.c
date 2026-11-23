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

/* Scripted provider runs for the segment tests. A turn can call the
   completion helper more than once, and each call has to see what the
   previous one left in `messages`. */
static int scripted;
static int scripted_results[4];
static int scripted_count;
static int scripted_index;
static int scripted_max_rounds[4];
static int scripted_toolless[4];
static int scripted_saw_marker[4];

static int has_synthetic_marker(const ClayJson *messages) {
  for (size_t i = 0; i < clay_json_array_count(messages); i++) {
    const ClayJson *message = clay_json_array_get(messages, i);
    const char *content =
        clay_json_string_value(clay_json_object_get(message, "content"));
    if (content && strncmp(content, CLAY_SYNTHETIC_MESSAGE_PREFIX,
                           strlen(CLAY_SYNTHETIC_MESSAGE_PREFIX)) == 0)
      return 1;
  }
  return 0;
}

int test_run_completion(ClayCommands *commands, ClayJson *messages,
                        const ClayToolSet *tools, int max_rounds,
                        const char *cache_key, const ClayOpenAICallbacks *callbacks) {
  (void)cache_key;
  int result = completion_result;
  if (scripted) {
    int index = scripted_index++;
    assert(index < 4);
    result = index < scripted_count ? scripted_results[index] : 0;
    scripted_max_rounds[index] = max_rounds;
    scripted_toolless[index] = tools == NULL;
    scripted_saw_marker[index] = has_synthetic_marker(messages);
  }
  /* Pretend the provider reported a nearly full window, then let the
     request hook shrink the live array: a single turn's own tool results must
     be reachable, which is what the old per-turn check could not do. */
  if (collapse_during_turn && !collapsed_once) {
    commands->last_prompt_tokens = 127000;
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
  if (callbacks->on_reasoning) {
    callbacks->on_reasoning("let me think", callbacks->userdata);
    callbacks->on_reasoning(" about it", callbacks->userdata);
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
  return result;
}

static char *read_stdout_since(FILE *capture, off_t start) {
  fflush(stdout);
  off_t end = lseek(STDOUT_FILENO, 0, SEEK_CUR);
  assert(start >= 0 && end >= start);
  char *text = calloc((size_t)(end - start) + 1, 1);
  assert(text);
  assert(pread(fileno(capture), text, (size_t)(end - start), start) ==
         end - start);
  return text;
}

/* Reasoning is off unless asked for: nothing reaches the transcript while it
   streams or when it ends, but Ctrl+O still shows the collected text. The
   turn above ran a completion with reasoning callbacks and wrote none of it. */
static void test_hidden_reasoning(FILE *output) {
  assert(!clay_thinking_visible());
  off_t start = lseek(STDOUT_FILENO, 0, SEEK_CUR);
  clay_thinking_begin();
  clay_thinking_write("let me think");
  clay_thinking_write(" about it");
  clay_thinking_finish(1.2);
  char *streamed = read_stdout_since(output, start);
  assert(strcmp(streamed, "") == 0);
  free(streamed);
  assert(clay_thinking_can_toggle());
  start = lseek(STDOUT_FILENO, 0, SEEK_CUR);
  clay_thinking_toggle();
  char *expanded = read_stdout_since(output, start);
  assert(strstr(expanded, "let me think about it"));
  free(expanded);
  clay_thinking_forget();
  assert(!clay_thinking_can_toggle());

  /* Opting back in restores the streaming row and the closing summary. */
  clay_thinking_set_visible(1);
  start = lseek(STDOUT_FILENO, 0, SEEK_CUR);
  clay_thinking_begin();
  clay_thinking_write("thinking");
  clay_thinking_finish(0.5);
  char *shown = read_stdout_since(output, start);
  assert(strstr(shown, "Reasoning finished in 0.5s"));
  free(shown);
  clay_thinking_forget();
  clay_thinking_set_visible(0);
}

/* What reaches the model from a long output: its tail when the command
   succeeded, both ends when it failed, and always the total size. */
static void test_clip_output(void) {
  ClayStr text;
  clay_str_init(&text);
  for (int i = 0; i < 1000; i++) clay_str_printf(&text, "line %d\n", i);

  ClayStr tail;
  clay_commands_clip_output(text.data, 200, 0, &tail);
  assert(strstr(tail.data, "showing the last 200"));
  assert(strstr(tail.data, "line 999"));
  assert(!strstr(tail.data, "line 0\n"));
  assert(strlen(tail.data) < 300);
  clay_str_free(&tail);

  ClayStr both;
  clay_commands_clip_output(text.data, 200, 1, &both);
  assert(strstr(both.data, "the first 100 and the last 100"));
  assert(strstr(both.data, "line 0\n"));
  assert(strstr(both.data, "line 999"));
  assert(strlen(both.data) < 350);
  clay_str_free(&both);

  /* Short output is untouched. */
  ClayStr short_text;
  clay_commands_clip_output("ok\n", 200, 1, &short_text);
  assert(strcmp(short_text.data, "ok\n") == 0);
  clay_str_free(&short_text);
  clay_str_free(&text);
}

static void plan_write(ClayPlan *plan, const char *json) {
  ClayJson *args = clay_json_parse(json, NULL);
  assert(args);
  ClayJson *result = todowrite_tool(args, plan);
  clay_json_free(args);
  assert(clay_json_bool_value(clay_json_object_get(result, "ok")));
  clay_json_free(result);
}

static char *capture_plan_output(ClayCommands *commands, int with_status) {
  fflush(stdout);
  FILE *capture = tmpfile();
  assert(capture);
  int saved = dup(STDOUT_FILENO);
  assert(saved >= 0);
  assert(dup2(fileno(capture), STDOUT_FILENO) >= 0);
  render_plan(commands);
  if (with_status) clay_below_render_status();
  fflush(stdout);
  assert(fseek(capture, 0, SEEK_END) == 0);
  long length = ftell(capture);
  assert(length >= 0 && fseek(capture, 0, SEEK_SET) == 0);
  char *text = calloc((size_t)length + 1, 1);
  assert(text);
  assert(fread(text, 1, (size_t)length, capture) == (size_t)length);
  assert(dup2(saved, STDOUT_FILENO) >= 0);
  close(saved);
  fclose(capture);
  return text;
}

static int count_text(const char *text, const char *needle) {
  int count = 0;
  for (const char *cursor = text; (cursor = strstr(cursor, needle));
       cursor += strlen(needle))
    count++;
  return count;
}

/* The heading summarizes the whole plan once; later updates print only the
   steps that moved, and the live row numbers the step by its position even
   when failed/skipped steps come before it. */
static void test_plan_rendering(ClayCommands *commands) {
  int colors = clay_term_color_enabled();
  clay_term_set_color_enabled(0);
  ClayPlan *plan = &commands->plan;

  plan_write(plan, "{\"title\":\"All outcomes\",\"todos\":["
                   "{\"content\":\"Build\",\"status\":\"completed\"},"
                   "{\"content\":\"Deploy\",\"status\":\"failed\"},"
                   "{\"content\":\"Notify\",\"status\":\"skipped\"}]}");
  char *first = capture_plan_output(commands, 0);
  assert(count_text(first, "All outcomes") == 1);
  assert(strstr(first, "1/3 complete \xc2\xb7 1 failed \xc2\xb7 1 skipped"));
  assert(strstr(first, CLAY_ICON_CHECK " Build"));
  assert(strstr(first, CLAY_ICON_CROSS " Deploy"));
  assert(strstr(first, "- Notify"));
  free(first);
  clay_plan_clear(plan);

  plan_write(plan, "{\"title\":\"Two steps\",\"todos\":["
                   "{\"content\":\"First\",\"status\":\"in_progress\"},"
                   "{\"content\":\"Second\",\"status\":\"pending\"}]}");
  char *start = capture_plan_output(commands, 0);
  assert(count_text(start, "Two steps") == 1);
  assert(strstr(start, "0/2 complete"));
  free(start);

  plan_write(plan, "{\"title\":\"Two steps\",\"todos\":["
                   "{\"content\":\"First\",\"status\":\"completed\"},"
                   "{\"content\":\"Second\",\"status\":\"in_progress\"}]}");
  char *update = capture_plan_output(commands, 0);
  assert(!strstr(update, "Two steps"));
  assert(strstr(update, CLAY_ICON_CHECK " First"));
  assert(strstr(update, CLAY_ICON_ARROW " Second"));
  free(update);

  /* The schema makes title optional, so an update that omits it must not
     turn the heading back into the default. */
  plan_write(plan, "{\"todos\":["
                   "{\"content\":\"First\",\"status\":\"completed\"},"
                   "{\"content\":\"Second\",\"status\":\"failed\"}]}");
  char *untitled = capture_plan_output(commands, 0);
  assert(plan->title && strcmp(plan->title, "Two steps") == 0);
  assert(!strstr(untitled, "Plan"));
  assert(strstr(untitled, CLAY_ICON_CROSS " Second"));
  free(untitled);
  clay_plan_clear(plan);

  plan_write(plan, "{\"title\":\"Counter\",\"todos\":["
                   "{\"content\":\"Build\",\"status\":\"completed\"},"
                   "{\"content\":\"Deploy\",\"status\":\"failed\"},"
                   "{\"content\":\"Verify\",\"status\":\"in_progress\"}]}");
  char *row = capture_plan_output(commands, 1);
  assert(strstr(row, "3/3 Verify"));
  free(row);
  clay_plan_clear(plan);

  clay_term_set_color_enabled(colors);
}

static char *capture_tool_output(ClayCommands *commands, const char *name,
                                 const ClayJson *result) {
  ClayConversationStream stream = {0};
  stream.commands = commands;
  stream.tool_task = clay_task_start("reading: %s", name);
  fflush(stdout);
  FILE *capture = tmpfile();
  assert(capture);
  int saved = dup(STDOUT_FILENO);
  assert(saved >= 0);
  assert(dup2(fileno(capture), STDOUT_FILENO) >= 0);
  on_tool_result(name, result, &stream);
  fflush(stdout);
  assert(fseek(capture, 0, SEEK_END) == 0);
  long length = ftell(capture);
  assert(length >= 0 && fseek(capture, 0, SEEK_SET) == 0);
  char *text = calloc((size_t)length + 1, 1);
  assert(text);
  assert(fread(text, 1, (size_t)length, capture) == (size_t)length);
  assert(dup2(saved, STDOUT_FILENO) >= 0);
  close(saved);
  fclose(capture);
  return text;
}

/* A completed tool leaves one quiet receipt and no body; a failure keeps the
   same single line, naming the command or target and the reason. */
static void test_tool_receipts(ClayCommands *commands) {
  int colors = clay_term_color_enabled();
  clay_term_set_color_enabled(0);
  ClayJson *result = clay_json_object();
  clay_json_object_set(result, "ok", clay_json_bool(1));
  clay_json_object_set(result, "path", clay_json_string("src/a.c"));
  clay_json_object_set(result, "output", clay_json_string("int a = 1;\nint b = 2;\n"));
  char *receipt = capture_tool_output(commands, "read", result);
  assert(strstr(receipt, CLAY_ICON_DOT " Read: src/a.c"));
  assert(!strstr(receipt, "int a = 1"));
  assert(count_text(receipt, "\n") == 1);
  free(receipt);
  clay_json_free(result);

  result = clay_json_object();
  clay_json_object_set(result, "ok", clay_json_bool(0));
  clay_json_object_set(result, "path", clay_json_string("src/a.c"));
  clay_json_object_set(result, "error", clay_json_string("permission denied"));
  clay_json_object_set(result, "output", clay_json_string("boom line\n"));
  char *failure = capture_tool_output(commands, "read", result);
  assert(strstr(failure, CLAY_ICON_CROSS " Failed to read: src/a.c - permission denied"));
  assert(!strstr(failure, "boom line"));
  assert(count_text(failure, "\n") == 1);
  free(failure);
  clay_json_free(result);

  result = clay_json_object();
  clay_json_object_set(result, "ok", clay_json_bool(1));
  clay_json_object_set(result, "command", clay_json_string("make test"));
  clay_json_object_set(result, "output", clay_json_string("ok\n"));
  char *ran = capture_tool_output(commands, "shell_exec", result);
  assert(strstr(ran, CLAY_ICON_DOT " Ran $ make test"));
  assert(!strstr(ran, "ok\n"));
  free(ran);
  clay_json_free(result);

  result = clay_json_object();
  clay_json_object_set(result, "ok", clay_json_bool(0));
  clay_json_object_set(result, "command", clay_json_string("false"));
  clay_json_object_set(result, "exit_code", clay_json_number(1));
  clay_json_object_set(result, "output", clay_json_string("boom\n"));
  char *failed = capture_tool_output(commands, "shell_exec", result);
  assert(strstr(failed, CLAY_ICON_CROSS " Failed $ false exit 1"));
  assert(!strstr(failed, "boom"));
  assert(count_text(failed, "\n") == 1);
  free(failed);
  clay_json_free(result);

  clay_term_set_color_enabled(colors);
}

static char *read_chat_file(ClayCommands *commands, const char *home,
                            const char *name) {
  ClayStr path;
  clay_str_init(&path);
  clay_str_printf(&path, "%s/.clay/chats/%s/%s", home,
                  clay_chat_id(commands->chat), name);
  ClayStr text;
  if (clay_storage_read_limited(path.data, 4 * 1024 * 1024, &text) != 0)
    clay_str_init(&text);
  clay_str_free(&path);
  return text.data;
}

static void remove_chat_files(ClayCommands *commands, const char *home) {
  char path[1024];
  snprintf(path, sizeof(path), "%s/.clay/chats/%s/requests.jsonl", home,
           clay_chat_id(commands->chat));
  remove(path);
  snprintf(path, sizeof(path), "%s/.clay/chats/%s/chat.json", home,
           clay_chat_id(commands->chat));
  remove(path);
  snprintf(path, sizeof(path), "%s/.clay/chats/%s", home,
           clay_chat_id(commands->chat));
  rmdir(path);
}

static size_t request_rounds(const char *text) {
  size_t count = 0;
  for (const char *cursor = text; cursor && *cursor;) {
    const char *end = strchr(cursor, '\n');
    assert(end);
    size_t length = (size_t)(end - cursor);
    char *line = malloc(length + 1);
    assert(line);
    memcpy(line, cursor, length);
    line[length] = '\0';
    ClayJson *record = clay_json_parse(line, NULL);
    assert(record);
    assert(clay_json_number_value(clay_json_object_get(record, "round")) ==
           (double)count);
    clay_json_free(record);
    free(line);
    count++;
    cursor = end + 1;
  }
  return count;
}

/* One exhausted segment resumes with the tool results already in the
   conversation, and the synthetic marker never reaches the journal. */
static void test_auto_continue(ClayCommands *commands, const char *home) {
  commands->chat = clay_chat_create("test");
  assert(commands->chat);
  scripted = 1;
  scripted_count = 2;
  scripted_index = 0;
  scripted_results[0] = CLAY_RUN_OUT_OF_ROUNDS;
  scripted_results[1] = 0;
  usage_reports = 1;
  clay_commands_run_message(commands, "hello");
  assert(scripted_index == 2);
  assert(scripted_max_rounds[0] == CLAY_AGENT_MAX_ROUNDS);
  assert(scripted_max_rounds[1] == CLAY_AGENT_MAX_ROUNDS);
  assert(!scripted_toolless[0] && !scripted_toolless[1]);
  assert(!scripted_saw_marker[0]);
  assert(scripted_saw_marker[1]);

  char *journal = read_chat_file(commands, home, "chat.json");
  assert(journal);
  assert(!strstr(journal, CLAY_SYNTHETIC_MESSAGE_PREFIX));
  free(journal);
  char *requests = read_chat_file(commands, home, "requests.jsonl");
  assert(requests);
  assert(request_rounds(requests) == 2);
  free(requests);

  scripted = 0;
  usage_reports = 0;
  remove_chat_files(commands, home);
  clay_chat_destroy(commands->chat);
  commands->chat = NULL;
}

/* Two exhausted segments still leave one request that consumes the final
   tool results and asks for a status, with no tools attached. */
static void test_final_summary(ClayCommands *commands, const char *home) {
  commands->chat = clay_chat_create("test");
  scripted = 1;
  scripted_count = 3;
  scripted_index = 0;
  scripted_results[0] = CLAY_RUN_OUT_OF_ROUNDS;
  scripted_results[1] = CLAY_RUN_OUT_OF_ROUNDS;
  scripted_results[2] = 0;
  usage_reports = 1;
  clay_commands_run_message(commands, "hello");
  assert(scripted_index == 3);
  assert(scripted_max_rounds[0] == CLAY_AGENT_MAX_ROUNDS);
  assert(scripted_max_rounds[1] == CLAY_AGENT_MAX_ROUNDS);
  assert(scripted_max_rounds[2] == 1);
  assert(!scripted_toolless[0] && !scripted_toolless[1]);
  assert(scripted_toolless[2]);
  assert(scripted_saw_marker[2]);

  char *journal = read_chat_file(commands, home, "chat.json");
  assert(journal);
  assert(!strstr(journal, CLAY_SYNTHETIC_MESSAGE_PREFIX));
  assert(strstr(journal, "\"state\":\"incomplete\""));
  free(journal);
  /* The turn stopped mid-job, but what it did is real: a replay keeps it. */
  ClayJson *replay = clay_chat_openai_messages(commands->chat);
  assert(clay_json_array_count(replay) == 1);
  assert(strcmp(clay_json_string_value(
                    clay_json_object_get(clay_json_array_get(replay, 0), "content")),
                "hello") == 0);
  clay_json_free(replay);
  char *requests = read_chat_file(commands, home, "requests.jsonl");
  assert(requests);
  assert(request_rounds(requests) == 3);
  free(requests);

  scripted = 0;
  usage_reports = 0;
  remove_chat_files(commands, home);
  clay_chat_destroy(commands->chat);
  commands->chat = NULL;
}

/* Escape between segments stops the turn without appending a continuation. */
static int abort_immediately(void *userdata) {
  (void)userdata;
  return 1;
}

static void test_segment_cancel(ClayCommands *commands, const char *home) {
  commands->chat = clay_chat_create("test");
  ClayJson *messages = clay_json_array();
  ClayConversationStream stream = {0};
  stream.commands = commands;
  ClayOpenAICallbacks callbacks = {0};
  callbacks.should_abort = abort_immediately;
  callbacks.userdata = &stream;
  ClayToolSet tools = {0};
  scripted = 1;
  scripted_count = 1;
  scripted_index = 0;
  scripted_results[0] = CLAY_RUN_OUT_OF_ROUNDS;
  int exhausted = 0, forced = 0;
  int rc = run_agent_segments(commands, messages, &tools, &stream, &callbacks,
                              &exhausted, &forced);
  assert(rc == 1);
  assert(exhausted);
  assert(!forced);
  assert(scripted_index == 1);
  assert(!has_synthetic_marker(messages));
  scripted = 0;
  clay_json_free(messages);
  remove_chat_files(commands, home);
  clay_chat_destroy(commands->chat);
  commands->chat = NULL;
}

/* The whole-turn token net is checked between segments: once a turn has
   billed past the cap, no further segment starts, but the closing round still
   runs so the user gets a status instead of silence. */
static int netted_segment_calls;
static int netted_segment(ClayCommands *commands, ClayJson *messages,
                          const ClayToolSet *tools, int max_rounds,
                          const char *cache_key,
                          const ClayOpenAICallbacks *callbacks) {
  (void)commands;
  (void)messages;
  (void)cache_key;
  (void)callbacks;
  netted_segment_calls++;
  if (tools)
    return max_rounds == CLAY_AGENT_MAX_ROUNDS ? CLAY_RUN_OUT_OF_ROUNDS : 0;
  return 0;
}

static long netted_billed(void *userdata) {
  (void)userdata;
  return CLAY_AGENT_MAX_TURN_TOKENS;
}

static void test_turn_token_net(void) {
  ClayJson *messages = clay_json_array();
  clay_json_array_push(messages, clay_openai_message("user", "work"));
  ClayToolSet tools = {0};
  ClaySegmentPlan plan = {
      .run = netted_segment,
      .max_segments = CLAY_AGENT_MAX_SEGMENTS,
      .max_rounds = CLAY_AGENT_MAX_ROUNDS,
      .max_tokens = CLAY_AGENT_MAX_TURN_TOKENS,
      .auto_continue = CLAY_AUTO_CONTINUE_PROMPT,
      .final_summary = CLAY_FINAL_SUMMARY_PROMPT,
      .billed_tokens = netted_billed,
  };
  int exhausted = 0, forced = 0;
  int rc = clay_commands_run_segments(NULL, messages, &tools, NULL, NULL,
                                      &plan, &exhausted, &forced);
  assert(rc == 0);
  /* Segment one, then straight to the closing round: segment two is skipped. */
  assert(netted_segment_calls == 2);
  assert(exhausted && forced);
  assert(clay_commands_is_synthetic_message(
      clay_json_array_get(messages, clay_json_array_count(messages) - 1)));
  clay_json_free(messages);
}

/* The soft limit speaks once per segment, not once per request. */
static void test_soft_budget_nudge(ClayCommands *commands, const char *home) {
  commands->chat = clay_chat_create("test");
  commands->last_prompt_tokens = 0;
  commands->accounted_messages = 0;
  ClayJson *messages = clay_json_array();
  clay_json_array_push(messages, clay_openai_message("system", "system"));
  clay_json_array_push(messages, clay_openai_message("user", "work"));
  ClayConversationStream stream = {0};
  stream.commands = commands;
  stream.segment_round = CLAY_AGENT_SOFT_ROUNDS / 3 - 1; /* 15 */
  before_request(messages, &stream);
  assert(stream.nudges_sent == 0);
  assert(clay_json_array_count(messages) == 2);

  stream.segment_round = CLAY_AGENT_SOFT_ROUNDS / 3; /* 16 */
  before_request(messages, &stream);
  assert(stream.nudges_sent == 1);
  assert(clay_json_array_count(messages) == 3);
  const char *content = clay_json_string_value(
      clay_json_object_get(clay_json_array_get(messages, 2), "content"));
  assert(content &&
         strncmp(content, CLAY_SOFT_ROUND_MARKER,
                 strlen(CLAY_SOFT_ROUND_MARKER)) == 0);
  assert(strstr(content, "16 of 64"));

  stream.segment_round = (CLAY_AGENT_SOFT_ROUNDS * 2) / 3; /* 32 */
  before_request(messages, &stream);
  assert(stream.nudges_sent == 3);
  assert(clay_json_array_count(messages) == 4);
  content = clay_json_string_value(
      clay_json_object_get(clay_json_array_get(messages, 3), "content"));
  assert(content && strstr(content, "32 of 64"));

  stream.segment_round = CLAY_AGENT_SOFT_ROUNDS; /* 48 */
  before_request(messages, &stream);
  assert(stream.nudges_sent == 7);
  assert(clay_json_array_count(messages) == 5);
  content = clay_json_string_value(
      clay_json_object_get(clay_json_array_get(messages, 4), "content"));
  assert(content && strstr(content, "48 of 64"));

  before_request(messages, &stream);
  assert(clay_json_array_count(messages) == 5);
  assert(stream.nudges_sent == 7);

  clay_json_free(messages);
  remove_chat_files(commands, home);
  clay_chat_destroy(commands->chat);
  commands->chat = NULL;
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

  int results[] = {0, 1, -1};
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
        assert(clay_json_number_value(clay_json_object_get(record, "context_budget")) == 128000);
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
  fail_save = 0;
  completion_result = 0;
  test_soft_budget_nudge(commands, home);
  test_segment_cancel(commands, home);
  test_auto_continue(commands, home);
  test_final_summary(commands, home);
  test_turn_token_net();
  collapse_during_turn = 1;
  collapsed_once = 0;
  commands->chat = clay_chat_create("test");
  assert(commands->chat);
  clay_commands_run_message(commands, "hello");
  assert(collapsed_once == 1);
  collapse_during_turn = 0;
  /* A compaction drops the blocks; the next sync must send them whole, not
     as a difference against history that is gone. */
  {
    char *before = clay_commands_environment_block();
    assert(before);
    commands->environment_block = before;
    commands->notes_block = strdup("Notes from earlier in this conversation:\nport is 8080");
    clay_commands_forget_context_blocks(commands);
    assert(commands->environment_block == NULL);
    assert(commands->notes_block == NULL);
    clay_commands_sync_context_blocks(commands);
    size_t blocks = 0;
    for (size_t i = 0; i < clay_json_array_count(commands->conversation); i++) {
      ClayJson *message = clay_json_array_get(commands->conversation, i);
      if (strcmp(clay_json_string_value(clay_json_object_get(message, "role")),
                 "system") != 0)
        continue;
      blocks++;
    }
    assert(blocks == 3); /* frozen prompt + environment + notes */
    /* An unchanged environment appends nothing. */
    size_t count = clay_json_array_count(commands->conversation);
    clay_commands_sync_context_blocks(commands);
    assert(clay_json_array_count(commands->conversation) == count);
  }
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
  test_clip_output();
  test_plan_rendering(commands);
  test_tool_receipts(commands);
  test_hidden_reasoning(output);
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
