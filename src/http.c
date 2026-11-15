#include "clay/http.h"

#include "clay/str.h"

#include <curl/curl.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct {
    ClayHttpResponse *response;
    ClayHttpChunkFn on_chunk;
    void *userdata;
    size_t received;
    size_t max_response_bytes;
    int in_headers;
} ClayHttpWriteCtx;

static int progress_cb(void *userdata, curl_off_t total_download, curl_off_t downloaded,
                       curl_off_t total_upload, curl_off_t uploaded) {
    (void)total_download;
    (void)downloaded;
    (void)total_upload;
    (void)uploaded;
    const ClayHttpRequest *req = userdata;
    return req->should_abort && req->should_abort(req->abort_userdata);
}

static int ascii_equal(const char *s, size_t len, const char *lower) {
    size_t i = 0;
    for (; i < len && lower[i]; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
        if (c != (unsigned char)lower[i]) return 0;
    }
    return i == len && lower[i] == '\0';
}

static size_t header_cb(char *ptr, size_t size, size_t nmemb, void *userdata) {
    ClayHttpWriteCtx *ctx = userdata;
    ClayHttpResponse *r = ctx->response;
    if (size && nmemb > SIZE_MAX / size) return 0;
    size_t len = size * nmemb;
    if (len >= 5 && memcmp(ptr, "HTTP/", 5) == 0) {
        const char *space = memchr(ptr, ' ', len);
        if (space && (size_t)(ptr + len - space) >= 4 &&
            space[1] >= '0' && space[1] <= '9' &&
            space[2] >= '0' && space[2] <= '9' &&
            space[3] >= '0' && space[3] <= '9') {
            r->status = (space[1] - '0') * 100 + (space[2] - '0') * 10 + space[3] - '0';
            r->rate_header_count = 0;
            r->rate_headers_dropped = 0;
            free(r->error_body);
            r->error_body = NULL;
            r->error_body_len = 0;
            r->error_body_truncated = 0;
            ctx->in_headers = 1;
        }
        return len;
    }
    if ((len == 2 && ptr[0] == '\r' && ptr[1] == '\n') ||
        (len == 1 && ptr[0] == '\n')) {
        ctx->in_headers = 0;
        return len;
    }
    if (!ctx->in_headers) return len;
    const char *colon = memchr(ptr, ':', len);
    if (!colon) return len;
    static const char *const allowed[] = {
        "retry-after", "x-ratelimit-limit-requests", "x-ratelimit-limit-tokens",
        "x-ratelimit-remaining-requests", "x-ratelimit-remaining-tokens",
        "x-ratelimit-reset-requests", "x-ratelimit-reset-tokens",
        "x-ratelimit-limit", "x-ratelimit-remaining", "x-ratelimit-reset",
        "ratelimit-limit", "ratelimit-remaining", "ratelimit-reset", "ratelimit-policy"
    };
    const char *name = NULL;
    for (size_t i = 0; i < sizeof(allowed) / sizeof(allowed[0]); i++) {
        if (ascii_equal(ptr, (size_t)(colon - ptr), allowed[i])) {
            name = allowed[i];
            break;
        }
    }
    if (!name) return len;
    const char *start = colon + 1, *end = ptr + len;
    while (start < end && (*start == ' ' || *start == '\t')) start++;
    while (end > start && (end[-1] == '\r' || end[-1] == '\n' ||
                           end[-1] == ' ' || end[-1] == '\t')) end--;
    size_t value_len = (size_t)(end - start);
    int invalid = 0;
    for (const char *p = start; p < end; p++)
        if (((unsigned char)*p < 32 && *p != '\t') || (unsigned char)*p == 127) invalid = 1;
    if (invalid || value_len > CLAY_HTTP_RATE_VALUE_LIMIT ||
        r->rate_header_count == CLAY_HTTP_RATE_HEADER_LIMIT) {
        r->rate_headers_dropped = 1;
        return len;
    }
    ClayHttpRateHeader *h = &r->rate_headers[r->rate_header_count++];
    h->name = name;
    memcpy(h->value, start, value_len);
    h->value[value_len] = '\0';
    return len;
}

static size_t write_cb(char *ptr, size_t size, size_t nmemb, void *userdata) {
    ClayHttpWriteCtx *ctx = userdata;
    size_t len = size * nmemb;

    if (size != 0 && len / size != nmemb) return 0;
    if (ctx->max_response_bytes > 0 &&
        (ctx->received > ctx->max_response_bytes || len > ctx->max_response_bytes - ctx->received)) {
        return 0;
    }
    ctx->received += len;

    ClayHttpResponse *r = ctx->response;
    if (r->status >= 300 && len) {
        size_t keep = CLAY_HTTP_ERROR_BODY_LIMIT - r->error_body_len;
        if (keep > len) keep = len;
        if (keep) {
            char *grown = realloc(r->error_body, r->error_body_len + keep + 1);
            if (!grown) return 0;
            r->error_body = grown;
            memcpy(r->error_body + r->error_body_len, ptr, keep);
            r->error_body_len += keep;
            r->error_body[r->error_body_len] = '\0';
        }
        if (keep < len) r->error_body_truncated = 1;
    }

    if (ctx->on_chunk) {
        return ctx->on_chunk(ptr, len, ctx->userdata) ? 0 : len;
    }

    if (len > SIZE_MAX - r->body_len - 1) return 0;
    char *grown = realloc(r->body, r->body_len + len + 1);
    if (!grown) return 0;
    r->body = grown;
    memcpy(r->body + r->body_len, ptr, len);
    r->body_len += len;
    r->body[r->body_len] = '\0';
    return len;
}

int clay_http_retry_after(const ClayHttpResponse *response, uint64_t *seconds) {
    if (response->rate_headers_dropped) return 0;
    const char *value = NULL;
    for (size_t i = 0; i < response->rate_header_count; i++) {
        if (!strcmp(response->rate_headers[i].name, "retry-after")) {
            if (value) return 0;
            value = response->rate_headers[i].value;
        }
    }
    if (!value || !*value) return 0;
    uint64_t n = 0;
    const char *p = value;
    while (*p >= '0' && *p <= '9') {
        unsigned digit = (unsigned)(*p++ - '0');
        if (n > (UINT64_MAX - digit) / 10) return 0;
        n = n * 10 + digit;
    }
    if (!*p) { *seconds = n; return 1; }
    if (p != value || strlen(value) != 29) return 0;

    static const char *const weekdays[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    static const char *const months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    static const int month_days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (value[3] != ',' || value[4] != ' ' || value[7] != ' ' || value[11] != ' ' ||
        value[16] != ' ' || value[19] != ':' || value[22] != ':' ||
        memcmp(value + 25, " GMT", 4)) return 0;
    const int digits[] = {5, 6, 12, 13, 14, 15, 17, 18, 20, 21, 23, 24};
    for (size_t i = 0; i < sizeof(digits) / sizeof(digits[0]); i++)
        if (value[digits[i]] < '0' || value[digits[i]] > '9') return 0;
    int weekday = 0, month = 0;
    while (weekday < 7 && memcmp(value, weekdays[weekday], 3)) weekday++;
    while (month < 12 && memcmp(value + 8, months[month], 3)) month++;
    if (weekday == 7 || month == 12) return 0;
    int year = (value[12]-'0')*1000 + (value[13]-'0')*100 + (value[14]-'0')*10 + value[15]-'0';
    int day = (value[5]-'0')*10 + value[6]-'0';
    int hour = (value[17]-'0')*10 + value[18]-'0';
    int minute = (value[20]-'0')*10 + value[21]-'0';
    int second = (value[23]-'0')*10 + value[24]-'0';
    int leap = year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
    if (year < 1 || day < 1 || day > month_days[month] + (month == 1 && leap) ||
        hour > 23 || minute > 59 || second > 59) return 0;
    int64_t days = 0;
    for (int y = 1; y < year; y++) days += 365 + (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0));
    for (int m = 0; m < month; m++) days += month_days[m] + (m == 1 && leap);
    days += day - 1;
    if ((days + 1) % 7 != weekday) return 0;
    int64_t epoch_seconds = (days - 719162) * 86400 + hour * 3600 + minute * 60 + second;
    time_t date = curl_getdate(value, NULL);
    if (date == (time_t)-1 || difftime(date, (time_t)0) != (double)epoch_seconds) return 0;
    time_t now = time(NULL);
    if (now == (time_t)-1) return 0;
    double delta = difftime(date, now);
    /* Beyond 2^53, double cannot represent every second exactly. */
    if (delta >= 9007199254740992.0) return 0;
    *seconds = delta <= 0 ? 0 : (uint64_t)delta;
    return 1;
}

int clay_http_init(void) {
    return curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK ? 0 : -1;
}

void clay_http_cleanup(void) {
    curl_global_cleanup();
}

int clay_http_request(const ClayHttpRequest *req, ClayHttpResponse *response) {
    memset(response, 0, sizeof(*response));

    CURL *curl = curl_easy_init();
    if (!curl) return -1;

    struct curl_slist *headers = NULL;
    for (size_t i = 0; i < req->header_count; i++) {
        ClayStr line;
        clay_str_init(&line);
        clay_str_printf(&line, "%s: %s", req->headers[i].name, req->headers[i].value);
        headers = curl_slist_append(headers, line.data);
        clay_str_free(&line);
    }

    ClayHttpWriteCtx ctx = {response, req->on_chunk, req->userdata, 0, req->max_response_bytes, 0};

    curl_easy_setopt(curl, CURLOPT_URL, req->url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &ctx);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, header_cb);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &ctx);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
#ifdef CURLSSLOPT_NATIVE_CA
    /* The Windows release uses libcurl with OpenSSL. Its normal CA bundle
       lives in the build environment, not next to the distributed .exe.
       Ask libcurl to include Windows' native certificate stores instead of
       requiring users to install a separate PEM bundle. */
    curl_easy_setopt(curl, CURLOPT_SSL_OPTIONS, (long)CURLSSLOPT_NATIVE_CA);
    curl_easy_setopt(curl, CURLOPT_PROXY_SSL_OPTIONS,
                     (long)CURLSSLOPT_NATIVE_CA);
#endif
    if (req->should_abort) {
        curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, progress_cb);
        curl_easy_setopt(curl, CURLOPT_XFERINFODATA, req);
    }
    if (req->timeout_seconds > 0) {
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, (long)req->timeout_seconds);
    }
    if (req->low_speed_limit > 0 && req->low_speed_seconds > 0) {
        curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, req->low_speed_limit);
        curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, req->low_speed_seconds);
    }

    if (req->method && strcmp(req->method, "POST") == 0) {
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, req->body ? req->body : "");
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)req->body_len);
    } else if (req->method && strcmp(req->method, "GET") != 0) {
        curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, req->method);
        if (req->body) {
            curl_easy_setopt(curl, CURLOPT_POSTFIELDS, req->body);
            curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)req->body_len);
        }
    }

    CURLcode rc = curl_easy_perform(curl);

    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    response->status = status;

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    return rc == CURLE_OK ? 0 : -1;
}

void clay_http_response_free(ClayHttpResponse *response) {
    free(response->body);
    response->body = NULL;
    response->body_len = 0;
    free(response->error_body);
    response->error_body = NULL;
    response->error_body_len = 0;
    response->error_body_truncated = 0;
    response->rate_header_count = 0;
    response->rate_headers_dropped = 0;
}
