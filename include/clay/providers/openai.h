#ifndef CLAY_PROVIDERS_OPENAI_H
#define CLAY_PROVIDERS_OPENAI_H

#include "clay/array.h"
#include "clay/json.h"
#include "clay/http.h"

#include <stddef.h>

#define CLAY_OPENAI_API_URL "https://api.openai.com/v1"

typedef struct ClayOpenAI ClayOpenAI;

/* Extra headers are for small OpenAI-compatible proxy variations. Their
   strings are copied by clay_openai_create_with_headers. */
typedef struct {
    const char *name;
    const char *value;
} ClayOpenAIHeader;

/* `base_url` has no trailing slash, e.g. "https://api.openai.com/v1" -
   "/chat/completions" is appended to it. HTTP and HTTPS URLs are supported. */
int clay_openai_url_is_supported(const char *base_url);
ClayOpenAI *clay_openai_create(const char *base_url, const char *api_key, const char *model);
ClayOpenAI *clay_openai_create_with_headers(const char *base_url,
                                             const char *api_key,
                                             const char *model,
                                             const ClayOpenAIHeader *headers,
                                             size_t header_count);
void clay_openai_destroy(ClayOpenAI *client);
int clay_openai_set_api_key(ClayOpenAI *client, const char *api_key);
/* Enables the provider's optional prompt-cache key. Generic OpenAI-compatible
   endpoints leave this unset unless their adapter explicitly supports it. */
void clay_openai_set_prompt_cache_key(ClayOpenAI *client, const char *key);
void clay_openai_set_output_limit(ClayOpenAI *client, long limit, long window);
/* Adds or replaces a provider-specific request header. */
int clay_openai_set_extra_header(ClayOpenAI *client, const char *name,
                                 const char *value);
long clay_openai_last_status(const ClayOpenAI *client);
/* NULL omits reasoning_effort from requests and uses the provider default. */
void clay_openai_set_reasoning_effort(ClayOpenAI *client, const char *effort);
/* OpenRouter Chat Completions uses reasoning.effort for max. */
void clay_openai_set_openrouter_max_reasoning(ClayOpenAI *client);

/* Retrieves every id returned by the provider's OpenAI-compatible
   `GET /models` endpoint. `models` must be a ClayArray of `char *`;
   this appends malloc'd ids which the caller owns. 0 on success. */
int clay_openai_list_models(ClayOpenAI *client, ClayArray *models);

/* Builds a `{"role": role, "content": content}` chat message. */
ClayJson *clay_openai_message(const char *role, const char *content);

/* `parameters` is a JSON Schema object, borrowed (not owned). `fn` gets
   the call's parsed arguments and returns a new ClayJson tool result,
   which clay_openai_run frees. */
typedef ClayJson *(*ClayToolFn)(const ClayJson *arguments, void *userdata);

typedef struct {
    const char *name;
    const char *description;
    ClayJson *parameters;
    ClayToolFn fn;
    void *userdata;
} ClayTool;

typedef enum {
    CLAY_OPENAI_ERROR_OTHER,
    CLAY_OPENAI_ERROR_RATE_LIMIT,
    CLAY_OPENAI_ERROR_QUOTA
} ClayOpenAIErrorKind;

typedef struct {
    ClayOpenAIErrorKind kind;
    long status; /* Actual HTTP status, including for SSE errors. */
    int from_sse;
    /* Borrowed until the callback returns; raw body may contain secrets. */
    const char *body;
    size_t body_len;
    int body_truncated;
    /* Allowlisted headers, borrowed until the callback returns. */
    const ClayHttpRateHeader *rate_headers;
    size_t rate_header_count;
    int rate_headers_dropped;
    int retry_after_valid;
    uint64_t retry_after_seconds;
} ClayOpenAIError;

/* All optional (NULL to ignore). */
typedef struct {
    long input_tokens;
    long output_tokens;
    long cached_input_tokens;
    int cached_input_tokens_known;
} ClayTokenUsage;

typedef struct {
    void (*on_token)(const char *text, void *userdata);
    void (*on_tool_call)(const char *name, const char *arguments_json, void *userdata);
    void (*on_tool_result)(const char *name, const ClayJson *result, void *userdata);
    void (*on_usage)(long input_tokens, long output_tokens, void *userdata);
    void (*on_error)(long status, const char *body, void *userdata);
    int (*should_abort)(void *userdata);
    void *userdata;
    /* Optional detailed usage. `cached_input_tokens_known` distinguishes an
       explicit zero from a provider that does not report cache usage. */
    void (*on_usage_details)(const ClayTokenUsage *usage, void *userdata);
    /* Optional streamed reasoning/summary text. Providers that do not expose
       it simply leave this callback unused. */
    void (*on_reasoning)(const char *text, void *userdata);
    /* Optional structured HTTP error; invoked in addition to on_error. */
    void (*on_http_error)(const ClayOpenAIError *error, void *userdata);
    /* Optional, once before every round's request body is built: the moment
       a caller can shrink the history it is about to send. `messages` is the
       live array, so what it changes is what goes out. */
    void (*before_request)(ClayJson *messages, void *userdata);
} ClayOpenAICallbacks;

/* Per-request replay boundary. Tool fragments are not committed side effects. */
typedef struct {
    int visible;
    int tool_executed;
    int callback_delivered;
} ClayOpenAIRetryState;

static inline int clay_openai_retry_safe(const ClayOpenAIRetryState *state) {
    return !state->visible && !state->tool_executed && !state->callback_delivered;
}

/* Adapter-only run: refresh once on a replay-safe 401 before reporting errors. */
int clay_openai_run_with_refresh(ClayOpenAI *client, ClayJson *messages,
                                 const ClayTool *tools, size_t tool_count, int max_rounds,
                                 const ClayOpenAICallbacks *callbacks,
                                 int (*refresh)(void *), void *refresh_userdata);

/* Whether replaying the entire last run is safe (used by auth adapters). */
int clay_openai_last_run_replay_safe(const ClayOpenAI *client);

/* For adapter use; event pointers are valid only during the callback. */
int clay_openai_permanent_quota(const ClayHttpResponse *response);
void clay_openai_report_http_error(const ClayHttpResponse *response,
                                   const ClayOpenAICallbacks *callbacks);

/* Adapter helpers for explicit structured SSE errors. */
int clay_openai_sse_error_is_429(const ClayJson *root);
void clay_openai_report_sse_error(const ClayHttpResponse *response, const char *body,
                                  const ClayOpenAICallbacks *callbacks);

/* Parses both Chat Completions and Responses-style usage objects. */
void clay_openai_usage_from_json(const ClayJson *usage, ClayTokenUsage *out);

/* Streams the model's reply to `messages` (OpenAI wire format), requesting
   final token usage when the compatible endpoint supports it. Runs any
   requested tool calls and resends until it answers with plain content or
   `max_rounds` is hit. Appends the assistant reply and any tool results to
   `messages` in place. Returns 0 on success, 1 when cancelled, 2 when it ran
   out of rounds with work still in flight, or -1 on failure. */
#define CLAY_RUN_OUT_OF_ROUNDS 2
int clay_openai_run(ClayOpenAI *client, ClayJson *messages, const ClayTool *tools, size_t tool_count,
                     int max_rounds, const ClayOpenAICallbacks *callbacks);

#endif /* CLAY_PROVIDERS_OPENAI_H */
