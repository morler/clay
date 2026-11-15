#include "../src/providers/openai.c"
#include "../src/providers/grok.c"
#include <assert.h>
#include <stdio.h>

int main(void) {
  ClayOpenAI *client = clay_openai_create("https://example.test/v1", "", "model");
  ClayJson *messages = clay_json_array();
  assert(client && messages);
  clay_openai_set_openrouter_max_reasoning(client);
  ClayStr body = build_request_body(client, messages, NULL, 0);
  ClayJson *root = clay_json_parse(body.data, NULL);
  assert(strcmp(clay_json_string_value(clay_json_object_get(
                    clay_json_object_get(root, "reasoning"), "effort")), "max") == 0);
  assert(!clay_json_object_get(root, "reasoning_effort"));
  clay_json_free(root);
  clay_str_free(&body);

  clay_openai_set_reasoning_effort(client, "max");
  body = build_request_body(client, messages, NULL, 0);
  root = clay_json_parse(body.data, NULL);
  assert(!clay_json_object_get(root, "reasoning"));
  assert(strcmp(clay_json_string_value(clay_json_object_get(root, "reasoning_effort")), "max") == 0);
  clay_json_free(root);
  clay_str_free(&body);

  clay_openai_set_reasoning_effort(client, NULL);
  body = build_request_body(client, messages, NULL, 0);
  root = clay_json_parse(body.data, NULL);
  assert(!clay_json_object_get(root, "reasoning"));
  assert(!clay_json_object_get(root, "reasoning_effort"));
  clay_json_free(root);
  clay_str_free(&body);
  ClayGrokCredentials credentials = {"access", "refresh", NULL, 0};
  ClayGrok *grok = clay_grok_create(&credentials, "model");
  assert(grok);
  clay_grok_set_reasoning_effort(grok, "max");
  body = build_request_body(grok->openai, messages, NULL, 0);
  root = clay_json_parse(body.data, NULL);
  assert(strcmp(clay_json_string_value(clay_json_object_get(root, "reasoning_effort")), "max") == 0);
  assert(!clay_json_object_get(root, "reasoning"));
  clay_json_free(root);
  clay_str_free(&body);
  clay_grok_destroy(grok);
  clay_json_free(messages);
  clay_openai_destroy(client);
  puts("reasoning tests passed");
  return 0;
}
