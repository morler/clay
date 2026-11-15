#ifndef CLAY_HTTP_H
#define CLAY_HTTP_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    const char *name;
    const char *value;
} ClayHttpHeader;

/* Called per chunk of the response body, not aligned to any line/message
   boundary - callers needing whole lines buffer it themselves (see
   providers/openai.c). Return non-zero to abort the transfer. */
typedef int (*ClayHttpChunkFn)(const char *data, size_t len, void *userdata);
typedef int (*ClayHttpAbortFn)(void *userdata);

typedef struct {
    const char *method; /* "GET", "POST", ... */
    const char *url;
    const ClayHttpHeader *headers;
    size_t header_count;
    const char *body; /* request body, NULL for none */
    size_t body_len;
    ClayHttpChunkFn on_chunk; /* NULL to buffer the whole response into ClayHttpResponse instead */
    void *userdata;
    ClayHttpAbortFn should_abort;
    void *abort_userdata;
    int timeout_seconds; /* 0 = no total timeout */
    long low_speed_limit; /* 0 = disabled; useful for idle streaming connections */
    long low_speed_seconds;
    size_t max_response_bytes; /* 0 = unlimited */
} ClayHttpRequest;

#define CLAY_HTTP_RATE_HEADER_LIMIT 16
#define CLAY_HTTP_RATE_VALUE_LIMIT 512
#define CLAY_HTTP_ERROR_BODY_LIMIT (64 * 1024)

typedef struct {
    const char *name; /* canonical lowercase name; static storage */
    char value[CLAY_HTTP_RATE_VALUE_LIMIT + 1];
} ClayHttpRateHeader;

typedef struct {
    long status;
    char *body;      /* malloc'd, NUL-terminated; only populated when on_chunk was NULL. Caller frees. */
    size_t body_len;
    /* Allowlisted rate-limit headers in received order, including duplicates.
       Oversized/excess/invalid values are omitted and set rate_headers_dropped. */
    ClayHttpRateHeader rate_headers[CLAY_HTTP_RATE_HEADER_LIMIT];
    size_t rate_header_count;
    int rate_headers_dropped;
    /* Raw non-2xx body prefix, including for streaming; may contain sensitive data.
       Owned by the response and released by clay_http_response_free. */
    char *error_body;
    size_t error_body_len;
    int error_body_truncated;
} ClayHttpResponse;

/* Call once at process startup/shutdown; not safe to call concurrently
   with a request. */
int clay_http_init(void);
void clay_http_cleanup(void);

/* Synchronous. Returns 0 once the exchange completed - check
   response->status for the HTTP outcome, a 4xx/5xx included - nonzero
   only on a transport failure. Always free the response, including after
   failure, before reusing it. */
int clay_http_request(const ClayHttpRequest *req, ClayHttpResponse *response);

/* Returns 1 and sets seconds only for one unambiguous valid Retry-After.
   shortcut: obsolete RFC 850 and asctime dates are rejected; support them when needed.
   Dates accept only IMF-fixdate. Returns 0 for missing, invalid, duplicate or
   dropped headers; callers must not interpret failure as permission to retry. */
int clay_http_retry_after(const ClayHttpResponse *response, uint64_t *seconds);

void clay_http_response_free(ClayHttpResponse *response);

#endif /* CLAY_HTTP_H */
