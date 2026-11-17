/* The Codex request must keep its frozen prefix frozen. The environment and
   notes blocks append a system message whenever they change, and Codex used
   to fold every system message into the top-level `instructions`, so one
   changed block rewrote the start of every later request and lost the
   provider's prefix cache.

   This test pins the contract: only the frozen system prompt goes into
   `instructions`, and later system blocks stay in `input`, in order. */

#include "clay/json.h"
#include "clay/providers/openai_codex.h"
#include "clay/str.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/providers/openai_codex.c"

static ClayJson *context_block(const char *text) {
  ClayJson *message = clay_json_object();
  clay_json_object_set(message, "role", clay_json_string("system"));
  clay_json_object_set(message, "content", clay_json_string(text));
  return message;
}

static ClayJson *user_message(const char *text) {
  ClayJson *message = clay_json_object();
  clay_json_object_set(message, "role", clay_json_string("user"));
  clay_json_object_set(message, "content", clay_json_string(text));
  return message;
}

/* Everything in `input` whose role is system, in order. */
static size_t system_inputs(const ClayJson *body, const char **first,
                            const char **second) {
  ClayJson *input = clay_json_object_get(body, "input");
  size_t found = 0;
  for (size_t i = 0; i < clay_json_array_count(input); i++) {
    ClayJson *item = clay_json_array_get(input, i);
    if (strcmp(clay_json_string_value(clay_json_object_get(item, "role")),
               "system") != 0)
      continue;
    ClayJson *content = clay_json_object_get(item, "content");
    const char *text = clay_json_string_value(
        clay_json_object_get(clay_json_array_get(content, 0), "text"));
    if (found == 0 && first) *first = text;
    if (found == 1 && second) *second = text;
    found++;
  }
  return found;
}

int main(void) {
  const char *frozen = "you are clay";
  const char *environment = "Environment right now: Linux.";
  const char *notes = "Notes from earlier: the port is 8080.";

  ClayJson *messages = clay_json_array();
  clay_json_array_push(messages, clay_openai_message("system", frozen));
  clay_json_array_push(messages, user_message("hello"));
  clay_json_array_push(messages, user_message("what is the port?"));
  clay_json_array_push(messages, context_block(environment));
  clay_json_array_push(messages, context_block(notes));

  ClayOpenAICodex client = {0};
  client.model = strdup("gpt-5-codex");
  ClayStr body_text = request_body(&client, messages, NULL, 0);
  ClayJson *body = clay_json_parse(body_text.data, NULL);
  assert(body);

  const char *instructions =
      clay_json_string_value(clay_json_object_get(body, "instructions"));
  assert(instructions);
  /* The whole failure: everything that ever changed was folded in here. */
  assert(strcmp(instructions, frozen) == 0);
  assert(!strstr(instructions, "Environment right now"));
  assert(!strstr(instructions, "Notes from earlier"));

  /* Both blocks still reach the model, in order, below the frozen prefix. */
  const char *first = NULL, *second = NULL;
  assert(system_inputs(body, &first, &second) == 2);
  assert(strcmp(first, environment) == 0);
  assert(strcmp(second, notes) == 0);

  /* A second serialization of the same history is byte-identical, which is
     what the provider's cache keys on. */
  ClayStr again = request_body(&client, messages, NULL, 0);
  assert(again.len == body_text.len);
  assert(memcmp(again.data, body_text.data, again.len) == 0);
  clay_str_free(&again);

  /* How many bytes used to ride in `instructions` and now stay in `input`:
     one changed block rewrote this much of the request's start. */
  size_t instructions_bytes = strlen(instructions);
  size_t kept = 0;
  for (size_t i = 1; i < clay_json_array_count(messages); i++) {
    ClayJson *m = clay_json_array_get(messages, i);
    if (strcmp(clay_json_string_value(clay_json_object_get(m, "role")),
               "system") != 0)
      continue;
    kept += strlen(clay_json_string_value(clay_json_object_get(m, "content")));
  }
  printf("codex prefix: instructions %zu B, context blocks now in input: %zu B\n",
         instructions_bytes, kept);

  clay_json_free(body);
  clay_str_free(&body_text);
  clay_json_free(messages);
  free(client.model);
  puts("codex prefix test passed");
  return 0;
}
