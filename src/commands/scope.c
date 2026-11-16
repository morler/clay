#include "context.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

#define CLAY_SCOPE_TPM_WINDOW_MS 60000
#define CLAY_SCOPE_OUTPUT_RESERVE_TOKENS 4096

/* One admitted HTTP attempt's share of the tokens-per-minute window. */
typedef struct {
  uint64_t ticket;
  uint64_t at_ms;
  uint64_t tokens;
  int pending; /* reserved and not yet settled: never expires */
} ClayScopeSpend;

struct ClayScope {
  uint64_t generation;
  unsigned char endpoint[32];
  unsigned char model[32];
  pthread_mutex_t lock;
  pthread_cond_t released;
  int occupied;
  int waiting; /* callers blocked in acquire; capped at CLAY_SCOPE_MAX_WAITERS */
  int waits; /* test hook: number of cond waits so far */
  int cancel_poll_ms; /* test hook; 0 means the default */
  uint64_t cooldown_until_ms; /* monotonic ms; set by a 429, no admission before it */
  uint64_t pace_ms; /* minimum spacing between admissions; halves on success */
  uint64_t last_admit_ms;
  uint64_t tpm; /* 0: no local budget */
  uint64_t tpm_window_ms;
  ClayScopeSpend *spend;
  size_t spend_count, spend_cap;
  uint64_t next_ticket;
  struct ClayScope *next;
};

struct ClayScopeRegistry {
  pthread_mutex_t lock;
  unsigned char key[32];
  ClayScope *head;
  int fail_allocation;
};

ClayScopeRegistry *clay_scope_registry_create(void) {
  ClayScopeRegistry *registry = calloc(1, sizeof(*registry));
  if (!registry)
    return NULL;
  if (clay_term_random_bytes(registry->key, sizeof(registry->key)) != 0) {
    free(registry);
    return NULL;
  }
  if (pthread_mutex_init(&registry->lock, NULL) != 0) {
    free(registry);
    return NULL;
  }
  return registry;
}

void clay_scope_registry_destroy(ClayScopeRegistry *registry) {
  if (!registry)
    return;
  ClayScope *scope = registry->head;
  while (scope) {
    ClayScope *next = scope->next;
    pthread_cond_destroy(&scope->released);
    pthread_mutex_destroy(&scope->lock);
    free(scope->spend);
    free(scope);
    scope = next;
  }
  pthread_mutex_destroy(&registry->lock);
  free(registry);
}

void clay_scope_test_fail_allocation(ClayScopeRegistry *registry, int nth) {
  pthread_mutex_lock(&registry->lock);
  registry->fail_allocation = nth;
  pthread_mutex_unlock(&registry->lock);
}

void clay_scope_test_cancel_poll_ms(ClayScope *scope, int ms) {
  pthread_mutex_lock(&scope->lock);
  scope->cancel_poll_ms = ms;
  pthread_mutex_unlock(&scope->lock);
}

int clay_scope_test_waits(ClayScope *scope) {
  pthread_mutex_lock(&scope->lock);
  int waits = scope->waits;
  pthread_mutex_unlock(&scope->lock);
  return waits;
}

static void *scope_alloc(ClayScopeRegistry *registry, size_t size) {
  if (registry->fail_allocation && --registry->fail_allocation == 0)
    return NULL;
  return malloc(size);
}

static int scope_digest(ClayScopeRegistry *registry, const char *value,
                        unsigned char digest[32]) {
  if (!value)
    value = "";
  size_t len = strlen(value);
  if (len > SIZE_MAX - sizeof(registry->key))
    return -1;
  unsigned char *input = scope_alloc(registry, sizeof(registry->key) + len);
  if (!input)
    return -1;
  memcpy(input, registry->key, sizeof(registry->key));
  memcpy(input + sizeof(registry->key), value, len);
  clay_sha256(input, sizeof(registry->key) + len, digest);
  volatile unsigned char *wipe = input;
  for (size_t i = 0; i < sizeof(registry->key) + len; i++)
    wipe[i] = 0;
  free(input);
  return 0;
}

ClayScope *clay_scope_get(ClayScopeRegistry *registry, uint64_t generation,
                          const char *endpoint, const char *model) {
  if (!registry || !generation)
    return NULL;
  unsigned char endpoint_hash[32], model_hash[32];
  pthread_mutex_lock(&registry->lock);
  if (scope_digest(registry, endpoint, endpoint_hash) ||
      scope_digest(registry, model, model_hash)) {
    pthread_mutex_unlock(&registry->lock);
    return NULL;
  }
  ClayScope *scope = registry->head;
  while (scope && (scope->generation != generation ||
                   memcmp(scope->endpoint, endpoint_hash, 32) ||
                   memcmp(scope->model, model_hash, 32)))
    scope = scope->next;
  if (!scope) {
    scope = scope_alloc(registry, sizeof(*scope));
    if (scope && pthread_mutex_init(&scope->lock, NULL) != 0) {
      free(scope);
      scope = NULL;
    }
    if (scope && pthread_cond_init(&scope->released, NULL) != 0) {
      pthread_mutex_destroy(&scope->lock);
      free(scope);
      scope = NULL;
    }
    if (scope) {
      scope->occupied = 0;
      scope->waits = 0;
      scope->waiting = 0;
      scope->cancel_poll_ms = 0;
      scope->cooldown_until_ms = 0;
      scope->pace_ms = 0;
      scope->last_admit_ms = 0;
      scope->tpm = 0;
      scope->tpm_window_ms = CLAY_SCOPE_TPM_WINDOW_MS;
      scope->spend = NULL;
      scope->spend_count = scope->spend_cap = 0;
      scope->next_ticket = 0;
      scope->generation = generation;
      memcpy(scope->endpoint, endpoint_hash, 32);
      memcpy(scope->model, model_hash, 32);
      scope->next = registry->head;
      registry->head = scope;
    }
  }
  pthread_mutex_unlock(&registry->lock);
  return scope;
}

/* The wait deadline exists only so cancellation gets polled from the lock-free callback. */
#define CLAY_SCOPE_CANCEL_POLL_MS 20

static int scope_deadline(ClayScope *scope, struct timespec *deadline) {
  if (clock_gettime(CLOCK_REALTIME, deadline) != 0)
    return -1;
  int ms = scope->cancel_poll_ms > 0 ? scope->cancel_poll_ms : CLAY_SCOPE_CANCEL_POLL_MS;
  deadline->tv_sec += ms / 1000;
  deadline->tv_nsec += (long)(ms % 1000) * 1000000L;
  if (deadline->tv_nsec >= 1000000000L) {
    deadline->tv_sec++;
    deadline->tv_nsec -= 1000000000L;
  }
  return 0;
}

/* Bounds for one admission: at most CLAY_SCOPE_MAX_WAITERS queued callers, each
   waiting at most CLAY_SCOPE_MAX_WAIT_MS. Wakeups are not FIFO, so a late caller
   can overtake an earlier waiter: there is no fairness or anti-starvation guarantee.
   The wait budget must exceed the longest holder (one streamed HTTP attempt). */
#define CLAY_SCOPE_MAX_WAITERS 8
#define CLAY_SCOPE_MAX_WAIT_MS 600000

static long scope_elapsed_ms(const struct timespec *start) {
  struct timespec now;
  if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
    return CLAY_SCOPE_MAX_WAIT_MS;
  return (long)(now.tv_sec - start->tv_sec) * 1000L +
         (long)(now.tv_nsec - start->tv_nsec) / 1000000L;
}

static void scope_unqueue(ClayScope *scope, int queued) {
  if (!queued)
    return;
  pthread_mutex_lock(&scope->lock);
  scope->waiting--;
  pthread_mutex_unlock(&scope->lock);
}

/* Pacing: a 429 sets a cooldown from Retry-After and doubles the spacing between
   admissions; each success halves the spacing back to zero. Rate limits are never
   derived from response headers: no header with verified semantics is used. */
#define CLAY_SCOPE_BASE_COOLDOWN_MS 1000
#define CLAY_SCOPE_BASE_PACE_MS 1000
#define CLAY_SCOPE_MAX_PACE_MS 30000
#define CLAY_SCOPE_MAX_COOLDOWN_MS 30000

static uint64_t scope_now_ms(void) {
  struct timespec now;
  if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
    return 0;
  return (uint64_t)now.tv_sec * 1000 + (uint64_t)now.tv_nsec / 1000000;
}

/* Earliest monotonic ms at which the scope may admit; caller holds scope->lock. */
static uint64_t scope_ready_ms(const ClayScope *scope) {
  uint64_t ready = scope->cooldown_until_ms;
  if (scope->last_admit_ms + scope->pace_ms > ready)
    ready = scope->last_admit_ms + scope->pace_ms;
  return ready;
}

/* 1: permit held. 0: should_abort fired, no permit held. -1: queue full or wait
   budget spent, no permit held; callers must treat it as a failed admission. */
int clay_scope_acquire(ClayScope *scope, int (*should_abort)(void *), void *userdata) {
  struct timespec start;
  if (clock_gettime(CLOCK_MONOTONIC, &start) != 0)
    return -1;
  int queued = 0; /* one queue slot, held from the first block until exit */
  for (;;) {
    if (should_abort && should_abort(userdata)) {
      scope_unqueue(scope, queued);
      return 0;
    }
    pthread_mutex_lock(&scope->lock);
    uint64_t now = scope_now_ms();
    if (!scope->occupied && now >= scope_ready_ms(scope)) {
      scope->occupied = 1;
      scope->last_admit_ms = now;
      if (queued)
        scope->waiting--;
      pthread_mutex_unlock(&scope->lock);
      if (should_abort && should_abort(userdata)) {
        clay_scope_release(scope);
        return 0;
      }
      return 1;
    }
    if (!queued) {
      if (scope->waiting >= CLAY_SCOPE_MAX_WAITERS) {
        pthread_mutex_unlock(&scope->lock);
        return -1;
      }
      scope->waiting++;
      queued = 1;
    }
    if (scope_elapsed_ms(&start) >= CLAY_SCOPE_MAX_WAIT_MS) {
      scope->waiting--;
      pthread_mutex_unlock(&scope->lock);
      return -1;
    }
    scope->waits++;
    struct timespec deadline;
    if (scope_deadline(scope, &deadline) == 0)
      pthread_cond_timedwait(&scope->released, &scope->lock, &deadline);
    else
      pthread_cond_wait(&scope->released, &scope->lock);
    pthread_mutex_unlock(&scope->lock);
  }
}

/* Takes the permit only if it is free and the scope is neither cooling down nor pacing.
   Never waits and never polls abort. */
int clay_scope_try_acquire(ClayScope *scope) {
  uint64_t now = scope_now_ms();
  pthread_mutex_lock(&scope->lock);
  int acquired = !scope->occupied && now >= scope_ready_ms(scope);
  if (acquired) {
    scope->occupied = 1;
    scope->last_admit_ms = now;
  }
  pthread_mutex_unlock(&scope->lock);
  return acquired;
}

void clay_scope_release(ClayScope *scope) {
  pthread_mutex_lock(&scope->lock);
  scope->occupied = 0;
  pthread_cond_signal(&scope->released);
  pthread_mutex_unlock(&scope->lock);
}

/* Response signal for one HTTP attempt. Permanent quota errors are not rate limits. */
void clay_scope_note_response(ClayScope *scope, const ClayHttpResponse *response,
                              int permanent_quota) {
  if (!scope)
    return;
  if (response->status >= 200 && response->status < 300) {
    pthread_mutex_lock(&scope->lock);
    scope->pace_ms /= 2;
    if (scope->pace_ms < CLAY_SCOPE_BASE_PACE_MS)
      scope->pace_ms = 0;
    pthread_mutex_unlock(&scope->lock);
    return;
  }
  if (response->status != 429 || permanent_quota)
    return;
  /* No Retry-After: base cooldown. A valid one is honoured up to the cap. A present
     but invalid, duplicated or dropped header is a conflicting signal: strictest cap. */
  int present = response->rate_headers_dropped != 0;
  for (size_t i = 0; i < response->rate_header_count; i++)
    if (!strcmp(response->rate_headers[i].name, "retry-after"))
      present = 1;
  uint64_t cooldown = CLAY_SCOPE_BASE_COOLDOWN_MS, seconds = 0;
  if (present) {
    if (!clay_http_retry_after(response, &seconds) || seconds > CLAY_SCOPE_MAX_COOLDOWN_MS / 1000)
      cooldown = CLAY_SCOPE_MAX_COOLDOWN_MS;
    else
      cooldown = seconds * 1000;
  }
  uint64_t now = scope_now_ms();
  pthread_mutex_lock(&scope->lock);
  if (now + cooldown > scope->cooldown_until_ms)
    scope->cooldown_until_ms = now + cooldown;
  scope->pace_ms = scope->pace_ms ? scope->pace_ms * 2 : CLAY_SCOPE_BASE_PACE_MS;
  if (scope->pace_ms > CLAY_SCOPE_MAX_PACE_MS)
    scope->pace_ms = CLAY_SCOPE_MAX_PACE_MS;
  pthread_mutex_unlock(&scope->lock);
}

/* Tokens-per-minute budget. A request reserves an estimate before it is sent: half its
   request bytes (tokens are rarely under two bytes each, so this overestimates) plus
   CLAY_SCOPE_OUTPUT_RESERVE_TOKENS for the reply. Settlement replaces the estimate with
   the provider's reported usage; without usage the estimate stays until the window rolls. */
void clay_scope_set_tpm(ClayScope *scope, uint64_t tpm) {
  pthread_mutex_lock(&scope->lock);
  scope->tpm = tpm;
  pthread_mutex_unlock(&scope->lock);
}

/* Drops settled entries that left the window; caller holds scope->lock. */
static void spend_prune(ClayScope *scope, uint64_t now) {
  size_t kept = 0;
  for (size_t i = 0; i < scope->spend_count; i++) {
    ClayScopeSpend *e = &scope->spend[i];
    if (e->pending || e->at_ms + scope->tpm_window_ms > now)
      scope->spend[kept++] = *e;
  }
  scope->spend_count = kept;
}

static uint64_t spend_total(const ClayScope *scope) {
  uint64_t total = 0;
  for (size_t i = 0; i < scope->spend_count; i++)
    total += scope->spend[i].tokens;
  return total;
}

/* Takes the permit, then reserves the budget for one HTTP attempt. 1: admitted, *ticket
   set (0 when no tpm applies). 0: cancelled while waiting, nothing held. -1: the request
   can never fit, or bookkeeping failed, nothing held. Every 1 must be followed by exactly
   one clay_scope_finish, which releases the permit. */
int clay_scope_admit(ClayScope *scope, size_t request_bytes, int (*should_abort)(void *),
                     void *userdata, uint64_t *ticket) {
  *ticket = 0;
  if (!clay_scope_try_acquire(scope)) {
    int held = clay_scope_acquire(scope, should_abort, userdata);
    if (held != 1)
      return held;
  }
  struct timespec start;
  if (clock_gettime(CLOCK_MONOTONIC, &start) != 0) {
    clay_scope_release(scope);
    return -1;
  }
  uint64_t tokens = (uint64_t)(request_bytes / 2) + CLAY_SCOPE_OUTPUT_RESERVE_TOKENS;
  pthread_mutex_lock(&scope->lock);
  if (scope->tpm == 0) {
    pthread_mutex_unlock(&scope->lock);
    return 1;
  }
  if (tokens > scope->tpm) {
    pthread_mutex_unlock(&scope->lock);
    clay_scope_release(scope);
    return -1;
  }
  uint64_t now;
  for (;;) {
    now = scope_now_ms();
    spend_prune(scope, now);
    if (spend_total(scope) + tokens <= scope->tpm)
      break;
    pthread_mutex_unlock(&scope->lock);
    if (should_abort && should_abort(userdata)) {
      clay_scope_release(scope);
      return 0;
    }
    if (scope_elapsed_ms(&start) >= CLAY_SCOPE_MAX_WAIT_MS) {
      clay_scope_release(scope);
      return -1;
    }
    struct timespec nap = {0, 20000000L};
    nanosleep(&nap, NULL);
    pthread_mutex_lock(&scope->lock);
  }
  if (scope->spend_count == scope->spend_cap) {
    size_t cap = scope->spend_cap ? scope->spend_cap * 2 : 16;
    ClayScopeSpend *grown = realloc(scope->spend, cap * sizeof(*grown));
    if (!grown) {
      pthread_mutex_unlock(&scope->lock);
      clay_scope_release(scope);
      return -1;
    }
    scope->spend = grown;
    scope->spend_cap = cap;
  }
  *ticket = ++scope->next_ticket;
  scope->spend[scope->spend_count++] = (ClayScopeSpend){*ticket, now, tokens, 1};
  pthread_mutex_unlock(&scope->lock);
  return 1;
}

/* Settles the ticket (no-op for 0) and releases the permit taken by clay_scope_admit.
   usage_known: usage_tokens replaces the estimate, even when it is higher. */
void clay_scope_finish(ClayScope *scope, uint64_t ticket, int usage_known, uint64_t usage_tokens) {
  if (ticket) {
    pthread_mutex_lock(&scope->lock);
    for (size_t i = 0; i < scope->spend_count; i++) {
      ClayScopeSpend *e = &scope->spend[i];
      if (e->ticket == ticket && e->pending) {
        if (usage_known)
          e->tokens = usage_tokens;
        e->pending = 0;
        break;
      }
    }
    pthread_mutex_unlock(&scope->lock);
  }
  clay_scope_release(scope);
}

void clay_scope_test_tpm_window(ClayScope *scope, uint64_t ms) {
  pthread_mutex_lock(&scope->lock);
  scope->tpm_window_ms = ms;
  pthread_mutex_unlock(&scope->lock);
}

uint64_t clay_scope_test_spent(ClayScope *scope) {
  pthread_mutex_lock(&scope->lock);
  spend_prune(scope, scope_now_ms());
  uint64_t total = spend_total(scope);
  pthread_mutex_unlock(&scope->lock);
  return total;
}

uint64_t clay_scope_test_pace(ClayScope *scope) {
  pthread_mutex_lock(&scope->lock);
  uint64_t pace = scope->pace_ms;
  pthread_mutex_unlock(&scope->lock);
  return pace;
}

uint64_t clay_scope_test_cooldown_left(ClayScope *scope) {
  uint64_t now = scope_now_ms();
  pthread_mutex_lock(&scope->lock);
  uint64_t left = scope->cooldown_until_ms > now ? scope->cooldown_until_ms - now : 0;
  pthread_mutex_unlock(&scope->lock);
  return left;
}
