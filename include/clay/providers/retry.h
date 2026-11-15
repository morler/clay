#ifndef CLAY_PROVIDER_RETRY_H
#define CLAY_PROVIDER_RETRY_H
#include "clay/http.h"
#include <stdint.h>
typedef struct { uint64_t started_ms; unsigned retries; } ClayProviderRetry;
typedef struct {
    int64_t (*now_ms)(void *userdata); /* monotonic milliseconds, or -1 */
    void (*sleep_ms)(unsigned ms, void *userdata);
    int (*should_abort)(void *userdata);
    void *userdata;
} ClayProviderRetryClock;
int64_t clay_provider_retry_now_ms(void *userdata);
void clay_provider_retry_sleep_ms(unsigned ms, void *userdata);
unsigned clay_provider_retry_jitter(void);
/* Initialize before the first request; jitter is in [0, 999].
   On failure the caller retains the original response and error. */
int clay_provider_retry_init(ClayProviderRetry *retry, const ClayProviderRetryClock *clock);
int clay_provider_retry_wait(ClayProviderRetry *retry, const ClayProviderRetryClock *clock,
                             const ClayHttpResponse *response, int permanent_quota,
                             int replay_safe, unsigned jitter);
#endif
