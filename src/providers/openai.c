#include "scope.h"
#include "output_limit.h"
#include "clay/providers/openai.h"

#include "clay/array.h"
#include "clay/http.h"
#include "clay/providers/retry.h"
#include "clay/sse.h"
#include "clay/str.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#define CLAY_OPENAI_MODELS_RESPONSE_LIMIT (2 * 1024 * 1024)
#define CLAY_OPENAI_MODELS_LIMIT 10000
#define CLAY_OPENAI_STREAM_RESPONSE_LIMIT (16 * 1024 * 1024)
#define CLAY_OPENAI_ERROR_SAMPLE_LIMIT (64 * 1024)
#define CLAY_OPENAI_SSE_LINE_LIMIT (256 * 1024)
#define CLAY_OPENAI_CONTENT_LIMIT (4 * 1024 * 1024)
#define CLAY_OPENAI_TOOL_CALLS_LIMIT 128
#define CLAY_OPENAI_TOOL_FIELD_LIMIT (1024 * 1024)
#define CLAY_OPENAI_EXTRA_HEADERS_LIMIT 8

struct ClayOpenAI {
  ClayScope *scope; /* borrowed from the owning session */
  char *base_url;
  char *api_key;
  char *model;
  char *reasoning_effort;
  int openrouter_max_reasoning;
  char *prompt_cache_key;
  long output_limit;
  long context_window;
  ClayOpenAIHeader extra_headers[CLAY_OPENAI_EXTRA_HEADERS_LIMIT];
  size_t extra_header_count;
  long last_status;
  int last_run_replay_safe;
};

void clay_openai_set_scope(ClayOpenAI *client, ClayScope *scope) { client->scope = scope; }
ClayScope *clay_openai_scope(const ClayOpenAI *client) { return client->scope; }


int clay_openai_last_run_replay_safe(const ClayOpenAI *client) {
  return client && client->last_run_replay_safe;
}

int clay_openai_permanent_quota(const ClayHttpResponse *response) {
  int quota = 0;
  if (response->error_body && !response->error_body_truncated &&
      strlen(response->error_body) == response->error_body_len) {
    const char *end = NULL;
    ClayJson *root = clay_json_parse(response->error_body, &end);
    while (end && *end && isspace((unsigned char)*end)) end++;
    const ClayJson *obj = clay_json_object_get(root, "error");
    if (clay_json_type(obj) != CLAY_JSON_OBJECT)
      obj = clay_json_object_get(clay_json_object_get(root, "response"), "error");
    const char *code = clay_json_string_value(clay_json_object_get(obj, "code"));
    const char *type = clay_json_string_value(clay_json_object_get(obj, "type"));
    if (root && end && !*end && response->status >= 400 && response->status < 500 &&
        (!strcmp(code, "insufficient_quota") || !strcmp(type, "insufficient_quota") ||
         !strcmp(code, "billing_hard_limit_reached") || !strcmp(type, "billing_hard_limit_reached")))
      quota = 1;
    clay_json_free(root);
  }
  return quota;
}

void clay_openai_report_http_error(const ClayHttpResponse *response,
                                   const ClayOpenAICallbacks *callbacks) {
  if (!callbacks || !callbacks->on_http_error) return;
  ClayOpenAIError error = {0};
  error.status = response->status;
  error.body = response->error_body;
  error.body_len = response->error_body_len;
  error.body_truncated = response->error_body_truncated;
  error.rate_headers = response->rate_headers;
  error.rate_header_count = response->rate_header_count;
  error.rate_headers_dropped = response->rate_headers_dropped;
  error.retry_after_valid = clay_http_retry_after(response, &error.retry_after_seconds);
  if (response->status == 429) error.kind = CLAY_OPENAI_ERROR_RATE_LIMIT;
  if (clay_openai_permanent_quota(response)) error.kind = CLAY_OPENAI_ERROR_QUOTA;
  callbacks->on_http_error(&error, callbacks->userdata);
}

static int is_429(const ClayJson *value) {
  return (clay_json_type(value) == CLAY_JSON_NUMBER &&
          clay_json_number_value(value) == 429) ||
         (clay_json_type(value) == CLAY_JSON_STRING &&
          strcmp(clay_json_string_value(value), "429") == 0);
}

int clay_openai_sse_error_is_429(const ClayJson *root) {
  const char *type = clay_json_string_value(clay_json_object_get(root, "type"));
  const ClayJson *error = clay_json_object_get(root, "error");
  if (!strcmp(type, "response.failed"))
    error = clay_json_object_get(clay_json_object_get(root, "response"), "error");
  else if (*type && strcmp(type, "error"))
    return 0;
  if (clay_json_type(error) != CLAY_JSON_OBJECT)
    return 0;
  return is_429(clay_json_object_get(error, "status")) ||
         is_429(clay_json_object_get(error, "status_code")) ||
         is_429(clay_json_object_get(error, "code"));
}

void clay_openai_report_sse_error(const ClayHttpResponse *response,
                                  const char *body,
                                  const ClayOpenAICallbacks *callbacks) {
  if (!callbacks || !callbacks->on_http_error) return;
  ClayOpenAIError error = {0};
  ClayHttpResponse quota_response = *response;
  quota_response.status = 429;
  quota_response.error_body = (char *)body;
  quota_response.error_body_len = strlen(body);
  quota_response.error_body_truncated = 0;
  error.kind = clay_openai_permanent_quota(&quota_response) ? CLAY_OPENAI_ERROR_QUOTA : CLAY_OPENAI_ERROR_RATE_LIMIT;
  error.status = response->status;
  error.from_sse = 1;
  error.body = body;
  error.body_len = strlen(body);
  error.rate_headers = response->rate_headers;
  error.rate_header_count = response->rate_header_count;
  error.rate_headers_dropped = response->rate_headers_dropped;
  error.retry_after_valid = clay_http_retry_after(response, &error.retry_after_seconds);
  callbacks->on_http_error(&error, callbacks->userdata);
}

int clay_openai_url_is_supported(const char *base_url) {
  return base_url &&
         ((strncmp(base_url, "https://", 8) == 0 && base_url[8] != '\0') ||
          (strncmp(base_url, "http://", 7) == 0 && base_url[7] != '\0'));
}

ClayOpenAI *clay_openai_create(const char *base_url, const char *api_key,
                               const char *model) {
  return clay_openai_create_with_headers(base_url, api_key, model, NULL, 0);
}

ClayOpenAI *clay_openai_create_with_headers(
    const char *base_url, const char *api_key, const char *model,
    const ClayOpenAIHeader *headers, size_t header_count) {
  if (!clay_openai_url_is_supported(base_url))
    return NULL;
  if (header_count > CLAY_OPENAI_EXTRA_HEADERS_LIMIT)
    return NULL;
  ClayOpenAI *client = calloc(1, sizeof(ClayOpenAI));
  if (!client)
    return NULL;
  client->base_url = strdup(base_url);
  client->api_key = strdup(api_key ? api_key : "");
  client->model = strdup(model ? model : "");
  for (size_t i = 0; i < header_count; i++) {
    if (!headers[i].name || !headers[i].value) {
      clay_openai_destroy(client);
      return NULL;
    }
    client->extra_headers[i].name = strdup(headers[i].name);
    client->extra_headers[i].value = strdup(headers[i].value);
    if (!client->extra_headers[i].name || !client->extra_headers[i].value) {
      clay_openai_destroy(client);
      return NULL;
    }
  }
  client->extra_header_count = header_count;
  return client;
}

void clay_openai_destroy(ClayOpenAI *client) {
  if (!client)
    return;
  free(client->base_url);
  free(client->api_key);
  free(client->model);
  free(client->reasoning_effort);
  free(client->prompt_cache_key);
  for (size_t i = 0; i < client->extra_header_count; i++) {
    free((char *)client->extra_headers[i].name);
    free((char *)client->extra_headers[i].value);
  }
  free(client);
}

int clay_openai_set_api_key(ClayOpenAI *client, const char *api_key) {
  if (!client) return -1;
  char *copy = strdup(api_key ? api_key : "");
  if (!copy) return -1; /* keep the last known-good key; caller sees the failure */
  free(client->api_key);
  client->api_key = copy;
  return 0;
}

void clay_openai_set_prompt_cache_key(ClayOpenAI *client, const char *key) {
  if (!client)
    return;
  char *copy = key && *key ? strdup(key) : NULL;
  free(client->prompt_cache_key);
  client->prompt_cache_key = copy;
}

int clay_openai_set_extra_header(ClayOpenAI *client, const char *name,
                                 const char *value) {
  if (!client || !name || !*name || !value)
    return -1;
  for (size_t i = 0; i < client->extra_header_count; i++) {
    if (strcmp(client->extra_headers[i].name, name) != 0)
      continue;
    char *copy = strdup(value);
    if (!copy)
      return -1;
    free((char *)client->extra_headers[i].value);
    client->extra_headers[i].value = copy;
    return 0;
  }
  if (client->extra_header_count == CLAY_OPENAI_EXTRA_HEADERS_LIMIT)
    return -1;
  char *name_copy = strdup(name), *value_copy = strdup(value);
  if (!name_copy || !value_copy) {
    free(name_copy);
    free(value_copy);
    return -1;
  }
  client->extra_headers[client->extra_header_count++] =
      (ClayOpenAIHeader){name_copy, value_copy};
  return 0;
}

long clay_openai_last_status(const ClayOpenAI *client) {
  return client ? client->last_status : 0;
}

void clay_openai_set_reasoning_effort(ClayOpenAI *client, const char *effort) {
  free(client->reasoning_effort);
  client->reasoning_effort = effort ? strdup(effort) : NULL;
  client->openrouter_max_reasoning = 0;
}

void clay_openai_set_openrouter_max_reasoning(ClayOpenAI *client) {
  clay_openai_set_reasoning_effort(client, NULL);
  client->openrouter_max_reasoning = 1;
}

int clay_openai_list_models(ClayOpenAI *client, ClayArray *models) {
  ClayStr url;
  clay_str_init(&url);
  clay_str_printf(&url, "%s/models", client->base_url);

  ClayStr auth;
  clay_str_init(&auth);
  clay_str_printf(&auth, "Bearer %s", client->api_key);

  ClayHttpHeader headers[1 + CLAY_OPENAI_EXTRA_HEADERS_LIMIT];
  headers[0] = (ClayHttpHeader){"Authorization", auth.data};
  for (size_t i = 0; i < client->extra_header_count; i++)
    headers[i + 1] = (ClayHttpHeader){client->extra_headers[i].name,
                                      client->extra_headers[i].value};
  ClayHttpRequest req = {0};
  req.method = "GET";
  req.url = url.data;
  req.headers = headers;
  req.header_count = 1 + client->extra_header_count;
  req.timeout_seconds = 30;
  req.max_response_bytes = CLAY_OPENAI_MODELS_RESPONSE_LIMIT;

  ClayHttpResponse resp;
  int rc = clay_http_request(&req, &resp);
  client->last_status = resp.status;
  clay_str_free(&url);
  clay_str_free(&auth);
  if (rc != 0 || resp.status < 200 || resp.status >= 300) {
    clay_http_response_free(&resp);
    return -1;
  }

  ClayJson *root = clay_json_parse(resp.body, NULL);
  clay_http_response_free(&resp);
  if (!root)
    return -1;

  ClayJson *data = clay_json_object_get(root, "data");
  if (clay_json_type(data) != CLAY_JSON_ARRAY) {
    clay_json_free(root);
    return -1;
  }

  size_t count = clay_json_array_count(data);
  if (count > CLAY_OPENAI_MODELS_LIMIT) {
    clay_json_free(root);
    return -1;
  }
  for (size_t i = 0; i < count; i++) {
    ClayJson *entry = clay_json_array_get(data, i);
    ClayJson *id = clay_json_object_get(entry, "id");
    if (clay_json_type(id) != CLAY_JSON_STRING)
      continue;
    char *copy = strdup(clay_json_string_value(id));
    clay_array_push_val(models, &copy);
  }

  clay_json_free(root);
  return 0;
}

ClayJson *clay_openai_message(const char *role, const char *content) {
  ClayJson *m = clay_json_object();
  clay_json_object_set(m, "role", clay_json_string(role));
  clay_json_object_set(m, "content", clay_json_string(content));
  return m;
}

static int usage_number(const ClayJson *object, const char *key, long *out) {
  ClayJson *value = clay_json_object_get(object, key);
  if (clay_json_type(value) != CLAY_JSON_NUMBER)
    return 0;
  *out = (long)clay_json_number_value(value);
  return 1;
}

void clay_openai_usage_from_json(const ClayJson *usage, ClayTokenUsage *out) {
  memset(out, 0, sizeof(*out));
  if (clay_json_type(usage) != CLAY_JSON_OBJECT)
    return;

  if (!usage_number(usage, "prompt_tokens", &out->input_tokens))
    usage_number(usage, "input_tokens", &out->input_tokens);
  if (!usage_number(usage, "completion_tokens", &out->output_tokens))
    usage_number(usage, "output_tokens", &out->output_tokens);

  ClayJson *details = clay_json_object_get(usage, "prompt_tokens_details");
  if (clay_json_type(details) != CLAY_JSON_OBJECT)
    details = clay_json_object_get(usage, "input_tokens_details");
  if (clay_json_type(details) == CLAY_JSON_OBJECT)
    out->cached_input_tokens_known = usage_number(
        details, "cached_tokens", &out->cached_input_tokens);
  if (!out->cached_input_tokens_known)
    out->cached_input_tokens_known = usage_number(
        usage, "cached_tokens", &out->cached_input_tokens);
}

/* One tool call as it accumulates across streamed deltas: the API sends
   its id/name/argument-fragments as separate events, keyed by `index`. */
typedef struct {
  size_t index;
  ClayStr id;
  ClayStr name;
  ClayStr arguments;
} ClayToolCallAccum;

typedef struct {
  ClayStr raw; /* every byte seen, for error reporting */
  ClayStr content;
  ClayArray tool_calls; /* ClayToolCallAccum */
  ClaySseParser *sse;
  const ClayOpenAICallbacks *callbacks;
  int cancelled;
  int limit_exceeded;
  int output_limited;
  char *sse_error;
  int sse_failed;
  int sse_rate_limit;
  int usage_known; /* the stream reported usage; tokens are input + output */
  uint64_t usage_tokens;
  /* Some proxies repeat the totals on the first and the last chunk of one
     response; the last block is the bill, delivered once when it ends. */
  int usage_seen;
  ClayTokenUsage usage_pending;
  ClayOpenAIRetryState retry;
} ClayStreamState;

static void on_sse_data(const char *data, void *userdata);

static void stream_state_init(ClayStreamState *st,
                              const ClayOpenAICallbacks *callbacks) {
  clay_str_init(&st->raw);
  clay_str_init(&st->content);
  clay_array_init(&st->tool_calls, sizeof(ClayToolCallAccum));
  st->sse = clay_sse_create(CLAY_OPENAI_SSE_LINE_LIMIT, on_sse_data, st);
  st->callbacks = callbacks;
  st->cancelled = 0;
  st->limit_exceeded = 0;
  st->output_limited = 0;
  st->sse_error = NULL;
  st->sse_failed = 0;
  st->sse_rate_limit = 0;
  st->usage_known = 0;
  st->usage_tokens = 0;
  st->usage_seen = 0;
  memset(&st->usage_pending, 0, sizeof(st->usage_pending));
  st->retry = (ClayOpenAIRetryState){0};
}

static void stream_state_free(ClayStreamState *st) {
  free(st->sse_error);
  clay_str_free(&st->raw);
  clay_str_free(&st->content);
  for (size_t i = 0; i < st->tool_calls.count; i++) {
    ClayToolCallAccum *tc = clay_array_get(&st->tool_calls, i);
    clay_str_free(&tc->id);
    clay_str_free(&tc->name);
    clay_str_free(&tc->arguments);
  }
  clay_array_free(&st->tool_calls);
  clay_sse_destroy(st->sse);
}

/* Ends one request: the usage it reported (if any) reaches the caller exactly
   once, so a duplicated block cannot bill the request twice. */
static void stream_state_finish(ClayStreamState *st) {
  if (st->usage_seen && st->callbacks) {
    const ClayTokenUsage *usage = &st->usage_pending;
    if (st->callbacks->on_usage)
      st->callbacks->on_usage(usage->input_tokens, usage->output_tokens,
                              st->callbacks->userdata);
    if (st->callbacks->on_usage_details)
      st->callbacks->on_usage_details(usage, st->callbacks->userdata);
  }
  st->usage_seen = 0;
  stream_state_free(st);
}

static ClayToolCallAccum *tool_call_at(ClayArray *calls, size_t index) {
  for (size_t i = 0; i < calls->count; i++) {
    ClayToolCallAccum *tc = clay_array_get(calls, i);
    if (tc->index == index)
      return tc;
  }

  if (calls->count == CLAY_OPENAI_TOOL_CALLS_LIMIT)
    return NULL;

  ClayToolCallAccum tc;
  tc.index = index;
  clay_str_init(&tc.id);
  clay_str_init(&tc.name);
  clay_str_init(&tc.arguments);
  clay_array_push_val(calls, &tc);
  return clay_array_get(calls, calls->count - 1);
}

static int append_limited(ClayStr *out, const char *data, size_t len,
                          size_t limit) {
  if (out->len > limit || len > limit - out->len)
    return -1;
  clay_str_push_n(out, data, len);
  return 0;
}

/* Applies one `data: {...}` payload (already stripped of the prefix) to
   the accumulating stream state. */
static int process_sse_data(const char *json_text, ClayStreamState *st) {
  if (strcmp(json_text, "[DONE]") == 0)
    return 0;

  const char *end = NULL;
  ClayJson *root = clay_json_parse(json_text, &end);
  if (!root)
    return 0;
  while (end && *end && isspace((unsigned char)*end)) end++;

  const char *type = clay_json_string_value(clay_json_object_get(root, "type"));
  if (end && !*end &&
      ((!*type && clay_json_type(clay_json_object_get(root, "error")) == CLAY_JSON_OBJECT) ||
       !strcmp(type, "error") || !strcmp(type, "response.failed"))) {
    st->sse_failed = 1;
    st->sse_rate_limit = clay_openai_sse_error_is_429(root);
    st->sse_error = strdup(json_text);
    clay_json_free(root);
    return 0;
  }

  ClayJson *usage = clay_json_object_get(root, "usage");
  if (clay_json_type(usage) == CLAY_JSON_OBJECT) {
    ClayTokenUsage counted;
    clay_openai_usage_from_json(usage, &counted);
    if (counted.input_tokens >= 0 && counted.output_tokens >= 0) {
      st->usage_known = 1;
      st->usage_tokens = (uint64_t)counted.input_tokens + (uint64_t)counted.output_tokens;
    }
    st->usage_pending = counted;
    st->usage_seen = 1;
    if (st->callbacks &&
        (st->callbacks->on_usage || st->callbacks->on_usage_details))
      st->retry.callback_delivered = 1;
  }

  ClayJson *choice0 =
      clay_json_array_get(clay_json_object_get(root, "choices"), 0);
  ClayJson *delta = clay_json_object_get(choice0, "delta");
  if (strcmp(clay_json_string_value(clay_json_object_get(choice0, "finish_reason")),
             "length") == 0)
    st->output_limited = 1;

  {
    const char *reasoning = NULL;
    const char *keys[] = {"reasoning_content", "reasoning", "thinking"};
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
      ClayJson *value = clay_json_object_get(delta, keys[i]);
      if (clay_json_type(value) == CLAY_JSON_STRING) {
        reasoning = clay_json_string_value(value);
        break;
      }
    }
    if (reasoning && *reasoning) {
      st->retry.visible = 1;
      if (st->callbacks && st->callbacks->on_reasoning)
        st->callbacks->on_reasoning(reasoning, st->callbacks->userdata);
    }
  }

  ClayJson *content = clay_json_object_get(delta, "content");
  if (clay_json_type(content) == CLAY_JSON_STRING) {
    const char *text = clay_json_string_value(content);
    if (*text) st->retry.visible = 1;
    if (append_limited(&st->content, text, strlen(text),
                       CLAY_OPENAI_CONTENT_LIMIT) != 0) {
      clay_json_free(root);
      return -1;
    }
    if (st->callbacks && st->callbacks->on_token) {
      st->retry.callback_delivered = 1;
      st->callbacks->on_token(text, st->callbacks->userdata);
    }
  }

  ClayJson *tool_calls = clay_json_object_get(delta, "tool_calls");
  size_t n = clay_json_array_count(tool_calls);
  for (size_t i = 0; i < n; i++) {
    ClayJson *tc = clay_json_array_get(tool_calls, i);
    size_t idx =
        (size_t)clay_json_number_value(clay_json_object_get(tc, "index"));
    ClayToolCallAccum *acc = tool_call_at(&st->tool_calls, idx);
    if (!acc) {
      clay_json_free(root);
      return -1;
    }

    ClayJson *id = clay_json_object_get(tc, "id");
    if (clay_json_type(id) == CLAY_JSON_STRING &&
        append_limited(&acc->id, clay_json_string_value(id),
                       strlen(clay_json_string_value(id)),
                       CLAY_OPENAI_TOOL_FIELD_LIMIT) != 0) {
      clay_json_free(root);
      return -1;
    }

    ClayJson *fn = clay_json_object_get(tc, "function");
    ClayJson *name = clay_json_object_get(fn, "name");
    if (clay_json_type(name) == CLAY_JSON_STRING &&
        append_limited(&acc->name, clay_json_string_value(name),
                       strlen(clay_json_string_value(name)),
                       CLAY_OPENAI_TOOL_FIELD_LIMIT) != 0) {
      clay_json_free(root);
      return -1;
    }
    ClayJson *args = clay_json_object_get(fn, "arguments");
    if (clay_json_type(args) == CLAY_JSON_STRING &&
        append_limited(&acc->arguments, clay_json_string_value(args),
                       strlen(clay_json_string_value(args)),
                       CLAY_OPENAI_TOOL_FIELD_LIMIT) != 0) {
      clay_json_free(root);
      return -1;
    }
  }

  clay_json_free(root);
  return 0;
}

static void on_sse_data(const char *data, void *userdata) {
  ClayStreamState *st = userdata;
  if (!st->sse_failed && process_sse_data(data, st) != 0)
    st->limit_exceeded = 1;
}

/* libcurl write callback: SSE framing remains correct across arbitrary
   transport chunks; provider-specific JSON mapping stays above. */
static int on_http_chunk(const char *data, size_t len, void *userdata) {
  ClayStreamState *st = userdata;
  if (st->raw.len < CLAY_OPENAI_ERROR_SAMPLE_LIMIT) {
    size_t kept = CLAY_OPENAI_ERROR_SAMPLE_LIMIT - st->raw.len;
    if (kept > len)
      kept = len;
    clay_str_push_n(&st->raw, data, kept);
  }
  if (clay_sse_feed(st->sse, data, len) != 0)
    st->limit_exceeded = 1;
  return st->limit_exceeded ? -1 : 0;
}

static int should_abort_stream(void *userdata) {
  ClayStreamState *st = userdata;
  if (!st->callbacks || !st->callbacks->should_abort)
    return 0;
  st->cancelled = st->callbacks->should_abort(st->callbacks->userdata);
  return st->cancelled;
}

void clay_openai_set_output_limit(ClayOpenAI *client, long limit, long window) {
  client->output_limit = limit;
  client->context_window = window;
}

void clay_provider_apply_output_limit(ClayJson *request, const char *field,
                                      long limit, long window, long minimum) {
  if (limit < minimum)
    return;
  if (window > 0) {
    ClayStr input;
    clay_str_init(&input);
    clay_json_stringify(request, &input);
    /* Match Clay's estimate for history. Include instructions and tool
       schemas, and leave 256 tokens for framing and estimation error. */
    size_t estimated = input.len / 2 + input.len % 2;
    clay_str_free(&input);
    long available = window - 256;
    if (available <= minimum || estimated >= (uintmax_t)(available - minimum))
      limit = minimum;
    else if (limit > available - (long)estimated)
      limit = available - (long)estimated;
  }
  clay_json_object_set(request, field, clay_json_number((double)limit));
}

static ClayStr build_request_body(ClayOpenAI *client, const ClayJson *messages,
                                  const ClayTool *tools, size_t tool_count) {
  ClayJson *root = clay_json_object();
  clay_json_object_set(root, "model", clay_json_string(client->model));
  clay_json_object_set(root, "messages", clay_json_clone(messages));
  clay_json_object_set(root, "stream", clay_json_bool(1));
  if (client->prompt_cache_key)
    clay_json_object_set(root, "prompt_cache_key",
                         clay_json_string(client->prompt_cache_key));
  if (client->openrouter_max_reasoning) {
    ClayJson *reasoning = clay_json_object();
    clay_json_object_set(reasoning, "effort", clay_json_string("max"));
    clay_json_object_set(root, "reasoning", reasoning);
  } else if (client->reasoning_effort) {
    clay_json_object_set(root, "reasoning_effort",
                         clay_json_string(client->reasoning_effort));
  }

  ClayJson *stream_options = clay_json_object();
  clay_json_object_set(stream_options, "include_usage", clay_json_bool(1));
  clay_json_object_set(root, "stream_options", stream_options);

  if (tool_count > 0) {
    ClayJson *tools_json = clay_json_array();
    for (size_t i = 0; i < tool_count; i++) {
      ClayJson *fn = clay_json_object();
      clay_json_object_set(fn, "name", clay_json_string(tools[i].name));
      clay_json_object_set(
          fn, "description",
          clay_json_string(tools[i].description ? tools[i].description : ""));
      clay_json_object_set(fn, "parameters",
                           tools[i].parameters
                               ? clay_json_clone(tools[i].parameters)
                               : clay_json_object());

      ClayJson *tool = clay_json_object();
      clay_json_object_set(tool, "type", clay_json_string("function"));
      clay_json_object_set(tool, "function", fn);
      clay_json_array_push(tools_json, tool);
    }
    clay_json_object_set(root, "tools", tools_json);
  }

  clay_provider_apply_output_limit(root, "max_completion_tokens",
                                  client->output_limit, client->context_window, 1);
  ClayStr body;
  clay_str_init(&body);
  clay_json_stringify(root, &body);
  clay_json_free(root);
  return body;
}

/* Appends the assistant's tool-call message to `messages`, runs each
   call against `tools`, and appends the resulting "tool" messages. */
static void handle_tool_calls(ClayJson *messages, const ClayTool *tools,
                              size_t tool_count, ClayStreamState *st,
                              const ClayOpenAICallbacks *callbacks, int execute) {
  ClayJson *assistant = clay_json_object();
  clay_json_object_set(assistant, "role", clay_json_string("assistant"));
  clay_json_object_set(assistant, "content",
                       st->content.len > 0 ? clay_json_string(st->content.data)
                                           : clay_json_null());

  ClayJson *tool_calls_json = clay_json_array();
  for (size_t i = 0; i < st->tool_calls.count; i++) {
    ClayToolCallAccum *tc = clay_array_get(&st->tool_calls, i);

    ClayJson *fn = clay_json_object();
    clay_json_object_set(fn, "name", clay_json_string(tc->name.data));
    clay_json_object_set(fn, "arguments", clay_json_string(tc->arguments.data));

    ClayJson *call = clay_json_object();
    clay_json_object_set(call, "id", clay_json_string(tc->id.data));
    clay_json_object_set(call, "type", clay_json_string("function"));
    clay_json_object_set(call, "function", fn);
    clay_json_array_push(tool_calls_json, call);
  }
  clay_json_object_set(assistant, "tool_calls", tool_calls_json);
  clay_json_array_push(messages, assistant);
  if (!execute) return;

  for (size_t i = 0; i < st->tool_calls.count; i++) {
    ClayToolCallAccum *tc = clay_array_get(&st->tool_calls, i);

    if (callbacks && callbacks->on_tool_call) {
      callbacks->on_tool_call(tc->name.data, tc->arguments.data,
                              callbacks->userdata);
    }

    const ClayTool *tool = NULL;
    for (size_t j = 0; j < tool_count; j++) {
      if (strcmp(tools[j].name, tc->name.data) == 0) {
        tool = &tools[j];
        break;
      }
    }

    ClayJson *args = clay_json_parse(tc->arguments.data, NULL);
    ClayJson *result = tool && tool->fn ? tool->fn(args, tool->userdata) : NULL;
    if (!result)
      result = clay_json_object();
    clay_json_free(args);

    if (callbacks && callbacks->on_tool_result) {
      callbacks->on_tool_result(tc->name.data, result, callbacks->userdata);
    }

    ClayStr result_json;
    clay_str_init(&result_json);
    clay_json_stringify(result, &result_json);
    clay_json_free(result);

    ClayJson *tool_msg = clay_json_object();
    clay_json_object_set(tool_msg, "role", clay_json_string("tool"));
    clay_json_object_set(tool_msg, "tool_call_id",
                         clay_json_string(tc->id.data));
    clay_json_object_set(tool_msg, "content",
                         clay_json_string(result_json.data));
    clay_json_array_push(messages, tool_msg);

    clay_str_free(&result_json);
  }
}

int clay_openai_run_with_refresh(ClayOpenAI *client, ClayJson *messages,
                                 const ClayTool *tools, size_t tool_count, int max_rounds,
                                 const ClayOpenAICallbacks *callbacks,
                                 int (*refresh)(void *), void *refresh_userdata) {
  int refreshed = 0;
  client->last_run_replay_safe = 1;
  for (int round = 0; round < max_rounds; round++) {
    ClayStreamState st;
    stream_state_init(&st, callbacks);
    ClayProviderRetryClock clock = {clay_provider_retry_now_ms, clay_provider_retry_sleep_ms,
                                    should_abort_stream, &st};
    ClayProviderRetry retry;
    int retry_enabled = clay_provider_retry_init(&retry, &clock);
retry_request:;
    if (callbacks && callbacks->before_request)
      callbacks->before_request(messages, callbacks->userdata);
    ClayStr body = build_request_body(client, messages, tools, tool_count);

    ClayStr url;
    clay_str_init(&url);
    clay_str_printf(&url, "%s/chat/completions", client->base_url);

    ClayStr auth;
    clay_str_init(&auth);
    clay_str_printf(&auth, "Bearer %s", client->api_key);

    ClayHttpHeader headers[3 + CLAY_OPENAI_EXTRA_HEADERS_LIMIT];
    headers[0] = (ClayHttpHeader){"Authorization", auth.data};
    headers[1] = (ClayHttpHeader){"Content-Type", "application/json"};
    headers[2] = (ClayHttpHeader){"Accept", "text/event-stream"};
    for (size_t i = 0; i < client->extra_header_count; i++)
      headers[i + 3] = (ClayHttpHeader){client->extra_headers[i].name,
                                        client->extra_headers[i].value};

    ClayHttpRequest req = {0};
    req.method = "POST";
    req.url = url.data;
    req.headers = headers;
    req.header_count = 3 + client->extra_header_count;
    req.body = body.data;
    req.body_len = body.len;
    req.on_chunk = on_http_chunk;
    req.userdata = &st;
    req.should_abort = should_abort_stream;
    req.abort_userdata = &st;
    req.low_speed_limit = 1;
    req.low_speed_seconds = 60;
    req.max_response_bytes = CLAY_OPENAI_STREAM_RESPONSE_LIMIT;

    ClayHttpResponse resp;
    /* One permit per attempt, released before retry sleeps, refresh or tools. */
    ClayScope *scope = clay_openai_scope(client);
    uint64_t ticket = 0;
    int admitted = scope ? clay_scope_admit(scope, body.len, should_abort_stream, &st, &ticket) : 1;
    if (admitted != 1) {
      clay_str_free(&url);
      clay_str_free(&auth);
      clay_str_free(&body);
      stream_state_finish(&st);
      if (admitted == 0)
        return 1;
      if (callbacks && callbacks->on_error)
        callbacks->on_error(0, "request not admitted: scope queue, wait limit, or tpm budget",
                            callbacks->userdata);
      return -1;
    }
    int rc = clay_http_request(&req, &resp);
    if (scope)
      clay_scope_finish(scope, ticket, st.usage_known, st.usage_tokens);
    client->last_status = resp.status;
    if (!clay_openai_retry_safe(&st.retry)) client->last_run_replay_safe = 0;

    clay_str_free(&url);
    clay_str_free(&auth);
    clay_str_free(&body);

    if (st.cancelled) {
      clay_http_response_free(&resp);
      stream_state_finish(&st);
      return 1;
    }
    ClayHttpResponse decision = resp;
    if (st.sse_rate_limit && resp.status >= 200 && resp.status < 300 && st.sse_error) {
      decision.status = 429;
      decision.error_body = st.sse_error;
      decision.error_body_len = strlen(st.sse_error);
      decision.error_body_truncated = 0;
    }
    clay_scope_note_response(scope, &decision, clay_openai_permanent_quota(&decision));
    if (retry_enabled && rc == 0 && !st.limit_exceeded &&
        !decision.error_body_truncated &&
        clay_provider_retry_wait(&retry, &clock, &decision, clay_openai_permanent_quota(&decision),
                                 clay_openai_retry_safe(&st.retry),
                                 clay_provider_retry_jitter())) {
      clay_http_response_free(&resp);
      stream_state_finish(&st);
      stream_state_init(&st, callbacks);
      goto retry_request;
    }
    if (st.cancelled) {
      clay_http_response_free(&resp);
      stream_state_finish(&st);
      return 1;
    }
    if (rc == 0 && resp.status == 401 && refresh && !refreshed &&
        client->last_run_replay_safe) {
      refreshed = 1;
      int refresh_result = refresh(refresh_userdata);
      if (refresh_result == 2) {
        clay_http_response_free(&resp);
        stream_state_finish(&st);
        return 1;
      }
      if (refresh_result == 0) {
        clay_http_response_free(&resp);
        stream_state_finish(&st);
        stream_state_init(&st, callbacks);
        goto retry_request;
      }
    }
    if ((rc != 0 && !st.sse_failed) || resp.status < 200 || resp.status >= 300) {
      if (st.content.len)
        clay_json_array_push(messages, clay_openai_message("assistant", st.content.data));
      clay_openai_report_http_error(&resp, callbacks);
      if (callbacks && callbacks->on_error)
        callbacks->on_error(resp.status, st.raw.data, callbacks->userdata);
      clay_http_response_free(&resp);
      stream_state_finish(&st);
      return -1;
    }
    if (st.sse_failed) {
      const char *error = st.sse_error ? st.sse_error : "";
      if (st.content.len)
        clay_json_array_push(messages, clay_openai_message("assistant", st.content.data));
      if (st.sse_rate_limit)
        clay_openai_report_sse_error(&resp, error, callbacks);
      if (callbacks && callbacks->on_error)
        callbacks->on_error(resp.status, error, callbacks->userdata);
      clay_http_response_free(&resp);
      stream_state_finish(&st);
      return -1;
    }
    clay_http_response_free(&resp);

    /* A capped response can stop partway through a tool call. */
    if (st.output_limited || st.tool_calls.count == 0) {
      clay_json_array_push(messages,
                           clay_openai_message("assistant", st.content.data));
      stream_state_finish(&st);
      return 0;
    }

    st.retry.tool_executed = 1;
    client->last_run_replay_safe = 0;
    handle_tool_calls(messages, tools, tool_count, &st, callbacks, 1);
    stream_state_finish(&st);
  }

  /* Rounds spent with tool calls still coming: not a failure, just an
     unfinished turn the user can continue. */
  return CLAY_RUN_OUT_OF_ROUNDS;
}

int clay_openai_run(ClayOpenAI *client, ClayJson *messages,
                    const ClayTool *tools, size_t tool_count, int max_rounds,
                    const ClayOpenAICallbacks *callbacks) {
  return clay_openai_run_with_refresh(client, messages, tools, tool_count,
                                      max_rounds, callbacks, NULL, NULL);
}
