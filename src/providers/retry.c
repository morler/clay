#include "clay/providers/retry.h"
#include <string.h>
#include <time.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <errno.h>
#endif

int64_t clay_provider_retry_now_ms(void *userdata) {
    (void)userdata;
#ifdef _WIN32
    return (int64_t)GetTickCount64();
#else
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return -1;
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
#endif
}
void clay_provider_retry_sleep_ms(unsigned ms, void *userdata) {
    (void)userdata;
#ifdef _WIN32
    Sleep(ms);
#else
    struct timespec ts = {ms / 1000, (long)(ms % 1000) * 1000000};
    while (nanosleep(&ts, &ts) != 0 && errno == EINTR) {}
#endif
}
unsigned clay_provider_retry_jitter(void) {
#ifdef _WIN32
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    uint64_t n = (uint64_t)t.QuadPart;
#else
    struct timespec t = {0};
    clock_gettime(CLOCK_MONOTONIC, &t);
    uint64_t n = (uint64_t)t.tv_sec * 1000000000ULL + (uint64_t)t.tv_nsec;
#endif
    n ^= (uintptr_t)&n;
    n ^= n >> 33; n *= UINT64_C(0xff51afd7ed558ccd); n ^= n >> 33;
    return (unsigned)(n % 1000);
}

int clay_provider_retry_init(ClayProviderRetry *retry, const ClayProviderRetryClock *clock) {
    int64_t now = clock->now_ms(clock->userdata);
    if (now < 0) return 0;
    *retry = (ClayProviderRetry){.started_ms = (uint64_t)now};
    return 1;
}

int clay_provider_retry_wait(ClayProviderRetry *retry, const ClayProviderRetryClock *clock,
                             const ClayHttpResponse *response, int permanent_quota,
                             int replay_safe, unsigned jitter) {
    if (response->status != 429 || permanent_quota || !replay_safe ||
        retry->retries >= 2 || jitter >= 1000 || response->rate_headers_dropped)
        return 0;
    int has_header = 0;
    for (size_t i = 0; i < response->rate_header_count; i++)
        if (!strcmp(response->rate_headers[i].name, "retry-after")) has_header = 1;
    uint64_t seconds = 0;
    if (has_header && !clay_http_retry_after(response, &seconds)) return 0;
    if (seconds > 30) return 0;
    uint64_t delay = (1000U << retry->retries) + jitter;
    if (seconds * 1000 > delay) delay = seconds * 1000;
    if (delay > 30000) return 0;
    int64_t raw = clock->now_ms(clock->userdata);
    if (raw < 0 || (uint64_t)raw < retry->started_ms) return 0;
    uint64_t now = (uint64_t)raw, elapsed = now - retry->started_ms;
    if (elapsed > 60000 || delay > 60000 - elapsed) return 0;
    uint64_t target = now + delay;
    if (target < now) return 0;
    for (;;) {
        if (clock->should_abort && clock->should_abort(clock->userdata)) return 0;
        raw = clock->now_ms(clock->userdata);
        if (raw < 0 || (uint64_t)raw < now) return 0;
        now = (uint64_t)raw;
        if (now - retry->started_ms > 60000) return 0;
        if (now >= target) break;
        uint64_t remaining = target - now;
        clock->sleep_ms((unsigned)(remaining < 100 ? remaining : 100), clock->userdata);
    }
    retry->retries++;
    return 1;
}
