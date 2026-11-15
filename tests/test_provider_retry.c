#include "clay/providers/retry.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
typedef struct { int64_t now; int stop; int sleeps; int oversleep; } Fake;
static int64_t now_ms(void *p) { return ((Fake *)p)->now; }
static void sleep_ms(unsigned ms, void *p) {
    Fake *f = p; f->sleeps++; f->now += ms + f->oversleep;
}
static int aborting(void *p) { Fake *f = p; return f->stop && f->sleeps >= f->stop; }
int main(void) {
    Fake f = {0};
    ClayProviderRetryClock c = {now_ms, sleep_ms, aborting, &f};
    ClayProviderRetry retry;
    ClayHttpResponse r = {.status = 429};
    assert(clay_provider_retry_init(&retry, &c));
    assert(!clay_provider_retry_wait(&retry, &c, &r, 1, 1, 0));
    assert(!clay_provider_retry_wait(&retry, &c, &r, 0, 0, 0));
    assert(clay_provider_retry_wait(&retry, &c, &r, 0, 1, 500) && f.now == 1500);
    assert(clay_provider_retry_wait(&retry, &c, &r, 0, 1, 999) && f.now == 4499);
    assert(!clay_provider_retry_wait(&retry, &c, &r, 0, 1, 0));
    f = (Fake){0}; assert(clay_provider_retry_init(&retry, &c));
    r.rate_header_count = 1; r.rate_headers[0].name = "retry-after";
    r.rate_headers[0].value[0] = 0;
    assert(!clay_provider_retry_wait(&retry, &c, &r, 0, 1, 0) && f.now == 0);
    strcpy(r.rate_headers[0].value, "3");
    assert(clay_provider_retry_wait(&retry, &c, &r, 0, 1, 0) && f.now == 3000);
    strcpy(r.rate_headers[0].value, "18446744073709551616");
    assert(!clay_provider_retry_wait(&retry, &c, &r, 0, 1, 0));
    strcpy(r.rate_headers[0].value, "31");
    assert(!clay_provider_retry_wait(&retry, &c, &r, 0, 1, 0));
    strcpy(r.rate_headers[0].value, "1"); r.rate_headers[1] = r.rate_headers[0];
    r.rate_header_count = 2;
    assert(!clay_provider_retry_wait(&retry, &c, &r, 0, 1, 0));
    r.rate_header_count = 0; r.rate_headers_dropped = 1;
    assert(!clay_provider_retry_wait(&retry, &c, &r, 0, 1, 0));
    r.rate_headers_dropped = 0; f.now = 59500;
    assert(!clay_provider_retry_wait(&retry, &c, &r, 0, 1, 0));
    f = (Fake){.stop = 2}; assert(clay_provider_retry_init(&retry, &c));
    assert(!clay_provider_retry_wait(&retry, &c, &r, 0, 1, 0));
    assert(f.now == 200 && retry.retries == 0);
    f = (Fake){.oversleep = 60000}; assert(clay_provider_retry_init(&retry, &c));
    assert(!clay_provider_retry_wait(&retry, &c, &r, 0, 1, 0));
    puts("provider retry tests passed");
    return 0;
}
