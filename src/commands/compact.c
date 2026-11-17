#include "context.h"

#include <stdlib.h>
#include <string.h>

/* Manual, LLM-written alternative to the deterministic compaction in
   context.c: /compact asks the model itself to summarize the conversation.
   Unlike an automatic LLM-compaction fallback (which risks failing exactly
   when the context is already overloaded), this is user-triggered, so
   there's always room to make the call. Tool results are stripped to short
   previews before sending - they're usually most of the token count, and
   the summarizer mostly needs to know what happened, not the raw output. */

#define CLAY_COMPACT_TOOL_PREVIEW 300
/* Arguments are bounded too: a write's are a whole file. */
#define CLAY_COMPACT_ARGUMENTS_HEAD 120
#define CLAY_COMPACT_ARGUMENTS_TAIL 240
#define CLAY_COMPACT_SYSTEM_PROMPT                                             \
  "You are compacting a coding-assistant conversation so it can continue "     \
  "with less context. Read the "                                               \
  "transcript below and write a concise summary that preserves: what the "     \
  "user asked for, key decisions "                                             \
  "made, files touched and why, the current state of the task, and anything "  \
  "still pending. The user's own requests are kept word for word outside "     \
  "your summary, so do not repeat them - say what came of them. Keep it "      \
  "under about 1500 tokens and leave out command transcripts. Tool call "                                                  \
  "results were stripped to short previews to save space - if an important "   \
  "detail seems to be missing "                                                \
  "because of that, say so explicitly instead of guessing. Plain prose, no "   \
  "meta-commentary about being a "                                             \
  "summarizer."

static char *collapse_for_compact(const char *text) {
  if (!text)
    return strdup("");
  size_t len = strlen(text);
  if (len <= CLAY_COMPACT_TOOL_PREVIEW)
    return strdup(text);
  ClayStr out;
  clay_str_init(&out);
  clay_str_push_n(&out, text, CLAY_COMPACT_TOOL_PREVIEW);
  clay_str_printf(&out, "... (%zu bytes total, truncated)", len);
  return out.data;
}

/* One tool call's arguments, as much as the summarizer needs to know what
   was touched. Malloc'd; never NULL. */
static char *summarize_arguments(const char *name, const char *arguments) {
  if (!arguments || !*arguments) return strdup("");
  ClayJson *parsed = clay_json_parse(arguments, NULL);
  if (!parsed) {
    /* Not JSON (a truncated frame): keep the head so the call is still
       identifiable. */
    size_t length = strlen(arguments);
    if (length <= CLAY_COMPACT_ARGUMENTS_HEAD) return strdup(arguments);
    ClayStr head;
    clay_str_init(&head);
    clay_str_push_n(&head, arguments, CLAY_COMPACT_ARGUMENTS_HEAD);
    clay_str_printf(&head, "... (%zu bytes total)", length);
    return head.data;
  }
  ClayStr out;
  clay_str_init(&out);
  const char *path = clay_json_string_value(clay_json_object_get(parsed, "path"));
  if (path) clay_str_printf(&out, "%s", path);
  const char *pattern =
      clay_json_string_value(clay_json_object_get(parsed, "pattern"));
  if (pattern) clay_str_printf(&out, "%s%s", out.len ? ", " : "", pattern);
  const char *command =
      clay_json_string_value(clay_json_object_get(parsed, "command"));
  if (command) {
    const char *newline = strchr(command, '\n');
    clay_str_printf(&out, "%s%.*s%s", out.len ? ", " : "",
                    (int)(newline ? (size_t)(newline - command) : strlen(command)),
                    command,
                    newline && newline[1] ? " [...]" : "");
  }
  if (name && strcmp(name, "write") == 0) {
    const char *content =
        clay_json_string_value(clay_json_object_get(parsed, "content"));
    size_t bytes = content ? strlen(content) : 0;
    clay_str_printf(&out, "%s%zu bytes written", out.len ? ", " : "", bytes);
  }
  if (name && strcmp(name, "edit") == 0) {
    const char *old_text =
        clay_json_string_value(clay_json_object_get(parsed, "oldString"));
    const char *new_text =
        clay_json_string_value(clay_json_object_get(parsed, "newString"));
    clay_str_printf(&out, "%s%zu -> %zu bytes", out.len ? ", " : "",
                    old_text ? strlen(old_text) : 0,
                    new_text ? strlen(new_text) : 0);
  }
  if (!out.len) {
    /* Something else: the first scalar value, or the key list. */
    const char *keys[] = {"query", "url", "slug", "steps", "question", "name"};
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]) && !out.len; i++) {
      const char *value =
          clay_json_string_value(clay_json_object_get(parsed, keys[i]));
      if (value)
        clay_str_printf(&out, "%.*s", CLAY_COMPACT_ARGUMENTS_HEAD, value);
    }
  }
  if (!out.len) {
    /* Nothing we model knows: keep the head of the raw JSON. */
    size_t length = strlen(arguments);
    if (length > CLAY_COMPACT_ARGUMENTS_TAIL)
      clay_str_printf(&out, "%.*s...", CLAY_COMPACT_ARGUMENTS_TAIL, arguments);
    else
      clay_str_printf(&out, "%s", arguments);
  }
  clay_json_free(parsed);
  return out.data;
}

/* Flattens `conversation` (skipping the system prompt at index 0) into a
   plain-text transcript: user/assistant text verbatim, tool calls as a
   one-line "called name(args)", tool results collapsed. */
char *clay_commands_build_compact_transcript(ClayJson *conversation) {
  ClayStr out;
  clay_str_init(&out);
  for (size_t i = 1; i < clay_json_array_count(conversation); i++) {
    ClayJson *message = clay_json_array_get(conversation, i);
    const char *role =
        clay_json_string_value(clay_json_object_get(message, "role"));
    const char *content =
        clay_json_string_value(clay_json_object_get(message, "content"));

    if (strcmp(role, "user") == 0) {
      clay_str_printf(&out, "User: %s\n\n", content ? content : "");
    } else if (strcmp(role, "assistant") == 0) {
      if (content && *content)
        clay_str_printf(&out, "Assistant: %s\n\n", content);
      ClayJson *tool_calls = clay_json_object_get(message, "tool_calls");
      for (size_t j = 0; j < clay_json_array_count(tool_calls); j++) {
        ClayJson *call = clay_json_array_get(tool_calls, j);
        ClayJson *function = clay_json_object_get(call, "function");
        const char *name =
            clay_json_string_value(clay_json_object_get(function, "name"));
        const char *arguments =
            clay_json_string_value(clay_json_object_get(function, "arguments"));
        /* A write's arguments are the whole file and an edit's are both
           versions of the lines: replaying those would cost more than the
           summary saves, and the files themselves are still on disk. Name
           the target and the size instead. */
        char *summary = summarize_arguments(name, arguments);
        clay_str_printf(&out, "Assistant called %s(%s)\n",
                        name && *name ? name : "?", summary ? summary : "");
        free(summary);
      }
    } else if (strcmp(role, "tool") == 0) {
      char *preview = collapse_for_compact(content);
      clay_str_printf(&out, "Tool result: %s\n\n", preview);
      free(preview);
    }
  }
  return out.data;
}

/* The user's own words, which the summary must not paraphrase: they are the
   requirements. Cut per request so one pasted file cannot crowd out the
   summary, and say what was cut. */
#define CLAY_COMPACT_PROMPT_BYTES 4000
#define CLAY_COMPACT_PROMPTS_TOTAL_BYTES (80 * 1024)

char *clay_commands_collect_user_requests(const ClayJson *conversation) {
    ClayStr out;
    clay_str_init(&out);
    for (size_t i = 1; i < clay_json_array_count(conversation); i++) {
        ClayJson *message = clay_json_array_get(conversation, i);
        const char *role =
            clay_json_string_value(clay_json_object_get(message, "role"));
        if (!role || strcmp(role, "user") != 0) continue;
        const char *content =
            clay_json_string_value(clay_json_object_get(message, "content"));
        if (!content || !*content) continue;
        if (out.len >= CLAY_COMPACT_PROMPTS_TOTAL_BYTES) break;
        size_t length = strlen(content);
        size_t room = CLAY_COMPACT_PROMPT_BYTES;
        size_t remaining =
            out.len < CLAY_COMPACT_PROMPTS_TOTAL_BYTES
                ? CLAY_COMPACT_PROMPTS_TOTAL_BYTES - out.len
                : 0;
        if (room > remaining) room = remaining;
        size_t shown = length < room ? length : room;
        clay_str_printf(&out, "- %.*s", (int)shown, content);
        if (shown < length)
            clay_str_printf(&out, " [... %zu characters cut]", length - shown);
        clay_str_push_char(&out, '\n');
    }
    if (out.len == 0) {
        clay_str_free(&out);
        return NULL;
    }
    return out.data;
}

typedef struct {
  ClayCommands *commands;
  int round;
  ClayStr summary;
} CompactAccounting;

static void accumulate_token(const char *text, void *userdata) {
  CompactAccounting *accounting = userdata;
  if (text && *text)
    clay_str_push(&accounting->summary, text);
}

static void compact_usage(const ClayTokenUsage *usage, void *userdata) {
  CompactAccounting *accounting = userdata;
  clay_commands_record_request_usage(accounting->commands, "compact",
                                     accounting->round++, usage, 0);
}

void clay_cmd_compact(const char *args, void *user_data) {
  (void)args;
  ClayCommands *commands = user_data;

  if (!commands->chat || clay_json_array_count(commands->conversation) <= 1) {
    clay_sayc(CLAY_GRAY, "Nothing to compact yet.");
    return;
  }
  if (!commands->selected_provider || !commands->selected_model) {
    clay_sayc(CLAY_RED,
              "Select a provider and model with /model before compacting.");
    return;
  }
  ClayConnectedProvider *provider =
      clay_commands_find_provider(commands, commands->selected_provider);
  if (!provider) {
    clay_sayc(CLAY_RED, "Selected provider %s is not connected.",
              commands->selected_provider);
    return;
  }

  char *transcript =
      clay_commands_build_compact_transcript(commands->conversation);
  if (!*transcript) {
    free(transcript);
    clay_sayc(CLAY_GRAY, "Nothing to compact yet.");
    return;
  }

  ClayJson *messages = clay_json_array();
  clay_json_array_push(
      messages, clay_openai_message("system", CLAY_COMPACT_SYSTEM_PROMPT));
  clay_json_array_push(messages, clay_openai_message("user", transcript));
  free(transcript);

  CompactAccounting accounting = {commands, 0, {0}};
  clay_str_init(&accounting.summary);
  ClayOpenAICallbacks callbacks = {0};
  callbacks.on_token = accumulate_token;
  callbacks.on_usage_details = compact_usage;
  callbacks.userdata = &accounting;

  size_t before_count = clay_json_array_count(commands->conversation);
  ClayTask *task = clay_app_task_start(commands->app, "Compacting context");
  if (clay_term_is_interactive())
    clay_term_raw_enable();
  int rc = clay_commands_run_completion(commands, messages, NULL, 1,
                                        clay_chat_id(commands->chat),
                                        &callbacks);
  if (clay_term_is_interactive())
    clay_term_raw_disable();
  clay_json_free(messages);

  if (rc != 0 || accounting.summary.len == 0) {
    clay_app_task_fail(commands->app, task, "failed");
    clay_str_free(&accounting.summary);
    clay_sayc(CLAY_RED, "Could not compact the conversation.");
    return;
  }
  clay_app_task_success(commands->app, task, "%zu messages -> summary",
                        before_count);

  char *requests = clay_commands_collect_user_requests(commands->conversation);
  ClayJson *new_conversation = clay_json_array();
  clay_json_array_push(new_conversation, clay_json_clone(clay_json_array_get(
                                             commands->conversation, 0)));
  if (requests) {
    ClayStr block;
    clay_str_init(&block);
    clay_str_printf(&block,
                    "What the user asked for, word for word (this is the "
                    "requirement; the summary below is context):\n%s",
                    requests);
    clay_json_array_push(new_conversation,
                         clay_openai_message("user", block.data));
    clay_str_free(&block);
    free(requests);
  }
  ClayStr note;
  clay_str_init(&note);
  clay_str_printf(
      &note,
      "Summary of the conversation so far (compacted with /compact):\n%s",
      accounting.summary.data);
  clay_json_array_push(new_conversation,
                       clay_openai_message("system", note.data));
  clay_str_free(&note);
  clay_str_free(&accounting.summary);

  clay_json_free(commands->conversation);
  commands->conversation = new_conversation;
  /* The environment and notes are no longer in this history: send them
     again next turn instead of diffing against what was dropped. */
  clay_commands_forget_context_blocks(commands);

  clay_sayc(CLAY_GREEN,
            "Compacted. The conversation now starts from a summary.");
}
