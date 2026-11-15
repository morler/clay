#include "../src/providers/openai_codex.c"
#include <assert.h>
#include <stdio.h>

int main(void) {
  ClayCodexCredentials credentials = {"access", "refresh", NULL, "account", 0};
  ClayOpenAICodex *client = clay_openai_codex_create(&credentials, "model");
  ClayJson *messages = clay_json_array();
  assert(client && messages);
  clay_openai_codex_set_reasoning_effort(client, "max");
  ClayStr body = request_body(client, messages, NULL, 0);
  ClayJson *root = clay_json_parse(body.data, NULL);
  assert(strcmp(clay_json_string_value(clay_json_object_get(
                    clay_json_object_get(root, "reasoning"), "effort")), "max") == 0);
  assert(!clay_json_object_get(root, "reasoning_effort"));
  clay_json_free(root);
  clay_str_free(&body);
  clay_json_free(messages);
  clay_openai_codex_destroy(client);
  puts("codex reasoning tests passed");
  return 0;
}
