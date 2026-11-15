#include "context.h"

#include <stdlib.h>
#include <string.h>

struct ClayScope {
  uint64_t generation;
  unsigned char endpoint[32];
  unsigned char model[32];
  pthread_mutex_t lock;
  int occupied;
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
    pthread_mutex_destroy(&scope->lock);
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
    if (scope) {
      scope->occupied = 0;
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

int clay_scope_acquire(ClayScope *scope, int (*should_abort)(void *), void *userdata) {
  for (;;) {
    if (should_abort && should_abort(userdata))
      return 0;
    pthread_mutex_lock(&scope->lock);
    int acquired = !scope->occupied;
    if (acquired)
      scope->occupied = 1;
    pthread_mutex_unlock(&scope->lock);
    if (acquired) {
      if (should_abort && should_abort(userdata)) {
        clay_scope_release(scope);
        return 0;
      }
      return 1;
    }
    /* shortcut: cancellation is polled every 20 ms, upgrade for explicit wakeups. */
    clay_term_sleep_ms(20);
  }
}

void clay_scope_release(ClayScope *scope) {
  pthread_mutex_lock(&scope->lock);
  scope->occupied = 0;
  pthread_mutex_unlock(&scope->lock);
}
