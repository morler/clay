#define _GNU_SOURCE

#include "../src/commands/context.h"

#include "clay/json.h"

#include <assert.h>
#include <stdio.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

extern char *mkdtemp(char *template);

typedef struct {
    ClayConnectedProvider *provider;
    pthread_barrier_t barrier;
    int result[2];
} CodexRace;

typedef struct { CodexRace *race; int index; } CodexWorker;

static void *codex_worker(void *arg) {
    CodexWorker *worker = arg;
    CodexRace *race = worker->race;
    uint64_t version;
    ClayOpenAICodex *snapshot = clay_commands_codex_snapshot(race->provider, NULL, &version);
    assert(snapshot);
    pthread_barrier_wait(&race->barrier);
    ClayCodexCredentials rotated = {worker->index ? "new-b" : "new-a",
                                   worker->index ? "refresh-b" : "refresh-a",
                                   NULL, "account", 123};
    ClayOpenAICodex *client = clay_openai_codex_create(&rotated, NULL);
    assert(client);
    race->result[worker->index] = clay_commands_save_codex_credentials(race->provider, client, version);
    for (int i = 0; i < 1000; i++) {
        uint64_t current;
        ClayOpenAICodex *copy = clay_commands_codex_snapshot(race->provider, NULL, &current);
        assert(copy);
        ClayCodexCredentials creds = {0};
        assert(clay_openai_codex_copy_credentials(copy, &creds) == 0);
        assert((!strcmp(creds.access_token, "new-a") && !strcmp(creds.refresh_token, "refresh-a")) ||
               (!strcmp(creds.access_token, "new-b") && !strcmp(creds.refresh_token, "refresh-b")));
        clay_openai_codex_credentials_free(&creds);
        clay_openai_codex_destroy(copy);
    }
    clay_openai_codex_destroy(snapshot);
    clay_openai_codex_destroy(client);
    return NULL;
}

static void test_codex_concurrent_snapshot(void) {
    ClayProviderConfig config = {0};
    config.access_token = strdup("old");
    config.refresh_token = strdup("refresh");
    config.account_id = strdup("account");
    ClayConnectedProvider provider = {0};
    provider.config = &config;
    provider.environment_override = 1;
    CodexRace race = {.provider = &provider};
    CodexWorker workers[2] = {{&race, 0}, {&race, 1}};
    pthread_t threads[2];
    assert(!pthread_barrier_init(&race.barrier, NULL, 2));
    for (int i = 0; i < 2; i++) assert(!pthread_create(&threads[i], NULL, codex_worker, &workers[i]));
    for (int i = 0; i < 2; i++) assert(!pthread_join(threads[i], NULL));
    assert(race.result[0] + race.result[1] == 1);
    assert(provider.codex_version == 1);
    assert(!pthread_barrier_destroy(&race.barrier));
    free(config.access_token); free(config.refresh_token); free(config.account_id);
}

static void *grok_worker(void *arg) {
    CodexWorker *worker = arg;
    CodexRace *race = worker->race;
    uint64_t version;
    ClayGrok *snapshot = clay_commands_grok_snapshot(race->provider, NULL, &version);
    assert(snapshot);
    pthread_barrier_wait(&race->barrier);
    ClayGrokCredentials rotated = {worker->index ? "new-b" : "new-a",
                                   worker->index ? "refresh-b" : "refresh-a", NULL, 123};
    ClayGrok *client = clay_grok_create(&rotated, NULL);
    assert(client);
    race->result[worker->index] = clay_commands_save_grok_credentials(race->provider, client, version);
    ClayGrokCredentials old = {0};
    assert(clay_grok_copy_credentials(snapshot, &old) == 0);
    assert(!strcmp(old.access_token, "old") && !strcmp(old.refresh_token, "refresh"));
    clay_grok_credentials_free(&old);
    for (int i = 0; i < 1000; i++) {
        uint64_t current;
        ClayGrok *copy = clay_commands_grok_snapshot(race->provider, NULL, &current);
        assert(copy);
        ClayGrokCredentials creds = {0};
        assert(clay_grok_copy_credentials(copy, &creds) == 0);
        assert((!strcmp(creds.access_token, "new-a") && !strcmp(creds.refresh_token, "refresh-a")) ||
               (!strcmp(creds.access_token, "new-b") && !strcmp(creds.refresh_token, "refresh-b")));
        clay_grok_credentials_free(&creds);
        clay_grok_destroy(copy);
    }
    clay_grok_destroy(snapshot);
    clay_grok_destroy(client);
    return NULL;
}

static void test_grok_concurrent_snapshot(void) {
    ClayProviderConfig config = {0};
    config.access_token = strdup("old");
    config.refresh_token = strdup("refresh");
    ClayGrokCredentials initial = {config.access_token, config.refresh_token, NULL, 0};
    ClayConnectedProvider provider = {0};
    provider.config = &config;
    provider.environment_override = 1;
    provider.grok_client = clay_grok_create(&initial, NULL);
    assert(provider.grok_client);
    ClayGrok *persistent = provider.grok_client;
    CodexRace race = {.provider = &provider};
    CodexWorker workers[2] = {{&race, 0}, {&race, 1}};
    pthread_t threads[2];
    assert(!pthread_barrier_init(&race.barrier, NULL, 2));
    for (int i = 0; i < 2; i++) assert(!pthread_create(&threads[i], NULL, grok_worker, &workers[i]));
    for (int i = 0; i < 2; i++) assert(!pthread_join(threads[i], NULL));
    assert(race.result[0] + race.result[1] == 1);
    assert(provider.grok_version == 1 && provider.grok_client == persistent);
    ClayGrokCredentials original = {0};
    assert(clay_grok_copy_credentials(persistent, &original) == 0);
    assert(!strcmp(original.access_token, "old"));
    clay_grok_credentials_free(&original);
    clay_grok_destroy(persistent);
    assert(!pthread_barrier_destroy(&race.barrier));
    free(config.access_token); free(config.refresh_token);
}
static ClayJson *call(ClayCommands *commands, const char *json_args) {
    ClayJson *args = clay_json_parse(json_args, NULL);
    assert(args);
    ClayJson *result = subagent_tool(args, commands);
    clay_json_free(args);
    return result;
}

static int ok(ClayJson *result) {
    return clay_json_bool_value(clay_json_object_get(result, "ok"));
}

static const char *error_of(ClayJson *result) {
    return clay_json_string_value(clay_json_object_get(result, "error"));
}

static int has_tool(ClayToolSet *set, const char *name) {
    for (size_t i = 0; i < set->tools.count; i++) {
        const ClayTool *tool = clay_array_get(&set->tools, i);
        if (strcmp(tool->name, name) == 0) return 1;
    }
    return 0;
}

typedef struct {
    pthread_barrier_t barrier;
    int results[2];
} InterruptRace;

typedef struct {
    InterruptRace *race;
    int index;
} InterruptWorker;

static void *interrupt_worker(void *arg) {
    InterruptWorker *worker = arg;
    for (int i = 0; i < 1000; i++) {
        pthread_barrier_wait(&worker->race->barrier);
        worker->race->results[worker->index] = clay_term_take_interrupt();
        pthread_barrier_wait(&worker->race->barrier);
    }
    return NULL;
}

static void test_interrupt_consumed_once(void) {
    InterruptRace race = {0};
    InterruptWorker workers[2] = {{&race, 0}, {&race, 1}};
    pthread_t threads[2];
    clay_term_init();
    assert(!clay_term_take_interrupt());
    assert(!pthread_barrier_init(&race.barrier, NULL, 3));
    for (int i = 0; i < 2; i++)
        assert(!pthread_create(&threads[i], NULL, interrupt_worker, &workers[i]));
    for (int i = 0; i < 1000; i++) {
        assert(!raise(SIGINT));
        pthread_barrier_wait(&race.barrier);
        pthread_barrier_wait(&race.barrier);
        assert(race.results[0] + race.results[1] == 1);
        assert(!clay_term_take_interrupt());
    }
    for (int i = 0; i < 2; i++) assert(!pthread_join(threads[i], NULL));
    assert(!pthread_barrier_destroy(&race.barrier));
}

typedef struct {
    ClayScopeRegistry *registry;
    pthread_barrier_t *barrier;
    ClayScope *scope;
} ScopeThread;

static void *scope_thread(void *arg) {
    ScopeThread *thread = arg;
    pthread_barrier_wait(thread->barrier);
    for (int i = 0; i < 1000; i++) {
        ClayScope *scope = clay_scope_get(thread->registry, 1,
                                         "https://user:secret@example.test", "model-secret");
        assert(scope && (!thread->scope || scope == thread->scope));
        thread->scope = scope;
    }
    return NULL;
}

typedef struct {
    ClayScopeRegistry *registry;
    pthread_barrier_t *barrier;
    ClayScope *scope;
} FailureThread;

static void *failure_thread(void *arg) {
    FailureThread *thread = arg;
    pthread_barrier_wait(thread->barrier);
    thread->scope = clay_scope_get(thread->registry, 1, "endpoint", "model");
    return NULL;
}

static void test_concurrent_scope_failure(void) {
    ClayScopeRegistry *registry = clay_scope_registry_create();
    assert(registry);
    ClayScope *expected = clay_scope_get(registry, 1, "endpoint", "model");
    assert(expected);
    pthread_barrier_t barrier;
    assert(!pthread_barrier_init(&barrier, NULL, 8));
    pthread_t threads[8];
    FailureThread runs[8] = {0};
    clay_scope_test_fail_allocation(registry, 1);
    for (int i = 0; i < 8; i++) {
        runs[i].registry = registry;
        runs[i].barrier = &barrier;
        assert(!pthread_create(&threads[i], NULL, failure_thread, &runs[i]));
    }
    int failures = 0;
    for (int i = 0; i < 8; i++) {
        assert(!pthread_join(threads[i], NULL));
        if (!runs[i].scope) failures++;
        else assert(runs[i].scope == expected);
    }
    assert(failures == 1);
    pthread_barrier_destroy(&barrier);
    clay_scope_registry_destroy(registry);
}

typedef struct {
    ClayScope *scope;
    pthread_mutex_t lock;
    pthread_cond_t cond;
    int checked;
    int cancelled;
    int result;
} WaitProbe;

static int wait_abort(void *arg) {
    WaitProbe *p = arg;
    pthread_mutex_lock(&p->lock);
    p->checked++;
    pthread_cond_signal(&p->cond);
    int cancelled = p->cancelled;
    pthread_mutex_unlock(&p->lock);
    return cancelled;
}

static void *wait_scope(void *arg) {
    WaitProbe *p = arg;
    p->result = clay_scope_acquire(p->scope, wait_abort, p);
    if (p->result) clay_scope_release(p->scope);
    return NULL;
}

static int abort_after_acquire(void *arg) {
    int *calls = arg;
    return ++*calls == 2;
}

static void test_scope_wait(void) {
    ClayScopeRegistry *registry = clay_scope_registry_create();
    assert(registry);
    ClayScope *scope = clay_scope_get(registry, 1, "endpoint", "a");
    ClayScope *other = clay_scope_get(registry, 1, "endpoint", "b");
    assert(scope && other);
    assert(clay_scope_acquire(scope, NULL, NULL));
    WaitProbe independent = {.scope = other};
    assert(!pthread_mutex_init(&independent.lock, NULL));
    assert(!pthread_cond_init(&independent.cond, NULL));
    pthread_t other_thread;
    assert(!pthread_create(&other_thread, NULL, wait_scope, &independent));
    struct timespec deadline;
    assert(!clock_gettime(CLOCK_REALTIME, &deadline));
    deadline.tv_sec++;
    assert(!pthread_timedjoin_np(other_thread, NULL, &deadline));
    assert(independent.result);
    pthread_cond_destroy(&independent.cond);
    pthread_mutex_destroy(&independent.lock);
    WaitProbe p = {.scope = scope};
    assert(!pthread_mutex_init(&p.lock, NULL));
    assert(!pthread_cond_init(&p.cond, NULL));
    pthread_t thread;
    assert(!pthread_create(&thread, NULL, wait_scope, &p));
    pthread_mutex_lock(&p.lock);
    while (p.checked < 2) pthread_cond_wait(&p.cond, &p.lock);
    p.cancelled = 1;
    pthread_mutex_unlock(&p.lock);
    assert(!clock_gettime(CLOCK_REALTIME, &deadline));
    deadline.tv_sec++;
    assert(!pthread_timedjoin_np(thread, NULL, &deadline));
    assert(!p.result);
    pthread_mutex_lock(&p.lock);
    p.checked = p.cancelled = 0;
    pthread_mutex_unlock(&p.lock);
    assert(!pthread_create(&thread, NULL, wait_scope, &p));
    pthread_mutex_lock(&p.lock);
    while (p.checked < 2) pthread_cond_wait(&p.cond, &p.lock);
    pthread_mutex_unlock(&p.lock);
    clay_scope_release(scope);
    assert(!clock_gettime(CLOCK_REALTIME, &deadline));
    deadline.tv_sec++;
    assert(!pthread_timedjoin_np(thread, NULL, &deadline));
    assert(p.result);
    int calls = 0;
    assert(!clay_scope_acquire(scope, abort_after_acquire, &calls));
    assert(calls == 2);
    assert(clay_scope_acquire(scope, NULL, NULL));
    clay_scope_release(scope);
    pthread_cond_destroy(&p.cond);
    pthread_mutex_destroy(&p.lock);
    clay_scope_registry_destroy(registry);
}

static void test_scopes(void) {
    ClayScopeRegistry *registry = clay_scope_registry_create();
    ClayScopeRegistry *other = clay_scope_registry_create();
    assert(registry && other);
    pthread_barrier_t barrier;
    assert(pthread_barrier_init(&barrier, NULL, 8) == 0);
    pthread_t threads[8];
    ScopeThread runs[8] = {0};
    for (size_t i = 0; i < 8; i++) {
        runs[i].registry = registry;
        runs[i].barrier = &barrier;
        assert(pthread_create(&threads[i], NULL, scope_thread, &runs[i]) == 0);
    }
    for (size_t i = 0; i < 8; i++) {
        assert(pthread_join(threads[i], NULL) == 0);
        assert(runs[i].scope == runs[0].scope);
    }
    pthread_barrier_destroy(&barrier);
    ClayScope *same = runs[0].scope;
    assert(clay_scope_get(registry, 2, "https://user:secret@example.test", "model-secret") != same);
    assert(clay_scope_get(registry, 1, "https://other.test", "model-secret") != same);
    assert(clay_scope_get(registry, 1, "https://user:secret@example.test", "other") != same);
    assert(clay_scope_get(other, 1, "https://user:secret@example.test", "model-secret") != same);
    assert(!clay_scope_get(registry, 0, "x", "y"));
    /* Scope consists only of generation, two hashes and the next pointer. */
    struct registry_layout { pthread_mutex_t lock; unsigned char key[32]; void *head; };
    assert(!memmem(registry, sizeof(struct registry_layout), "secret", 6));
    size_t scope_size = sizeof(uint64_t) + 64 + sizeof(void *);
    assert(!memmem(same, scope_size, "secret", 6));
    assert(clay_scope_get(registry, 1, "https://user:secret@example.test", "model-secret") == same);
    clay_scope_registry_destroy(other);
    clay_scope_registry_destroy(registry);
}

static void test_provider_generations(void) {
    ClayCommands c = {0};
    clay_array_init(&c.providers, sizeof(ClayConnectedProvider));
    static const ClayProviderType types[] = {
      {"one", "one", "https://example.com"}, {"two", "two", "https://example.com"},
      {"three", "three", "https://example.com"}, {"four", "four", "https://example.com"},
      {"five", "five", "https://example.com"}, {"six", "six", "https://example.com"},
      {"seven", "seven", "https://example.com"}, {"eight", "eight", "https://example.com"},
      {"nine", "nine", "https://example.com"}
    };
    const char *envs[] = {"CLAY_ONE_API_KEY", "CLAY_TWO_API_KEY", "CLAY_THREE_API_KEY", "CLAY_FOUR_API_KEY", "CLAY_FIVE_API_KEY", "CLAY_SIX_API_KEY", "CLAY_SEVEN_API_KEY", "CLAY_EIGHT_API_KEY", "CLAY_NINE_API_KEY"};
    for (size_t i = 0; i < 9; i++) {
        assert(setenv(envs[i], "key", 1) == 0);
        clay_commands_load_provider(&c, &types[i]);
        assert(clay_commands_provider_generation(&c, types[i].id) == i + 1);
    }
    assert(c.providers.count == 9 && c.providers.cap > 8);
    uint64_t ninth = clay_commands_provider_generation(&c, "nine");
    ClayConnectedProvider *first = clay_commands_find_provider(&c, "one");
    assert(clay_config_save(first->config) == 0);
    assert(clay_commands_logout_provider(&c, "one") == 0);
    assert(clay_commands_provider_generation(&c, "nine") == ninth);
    clay_commands_load_provider(&c, &types[0]);
    assert(clay_commands_provider_generation(&c, "one") > ninth);
    ClayScopeRegistry *registry = clay_scope_registry_create();
    assert(registry);
    uint64_t before = clay_commands_provider_generation(&c, "two");
    ClayScope *old_scope = clay_scope_get(registry, before, "https://example.com", "model");
    assert(old_scope);
    clay_commands_load_provider(&c, &types[1]);
    assert(clay_commands_provider_generation(&c, "two") > before);
    assert(clay_scope_get(registry, clay_commands_provider_generation(&c, "two"),
                          "https://example.com", "model") != old_scope);
    assert(clay_scope_get(registry, before, "https://example.com", "model") == old_scope);
    clay_scope_registry_destroy(registry);
    assert(setenv("OPENAI_CODEX_ACCESS_TOKEN", "old", 1) == 0);
    assert(setenv("OPENAI_CODEX_REFRESH_TOKEN", "refresh", 1) == 0);
    assert(setenv("OPENAI_CODEX_ACCOUNT_ID", "account", 1) == 0);
    clay_commands_load_provider(&c, clay_commands_find_provider_type("openai-codex"));
    ClayConnectedProvider *codex = clay_commands_find_provider(&c, "openai-codex");
    assert(codex);
    uint64_t generation = codex->generation;
    ClayCodexCredentials rotated = {"new", "rotated", NULL, "account", 123};
    ClayOpenAICodex *client = clay_openai_codex_create(&rotated, NULL);
    assert(client && clay_commands_save_codex_credentials(codex, client, codex->codex_version) == 0);
    assert(codex->generation == generation && !strcmp(codex->config->refresh_token, "rotated"));
    clay_openai_codex_destroy(client);
    uint64_t current = clay_commands_provider_generation(&c, "two");
    c.provider_generation = UINT64_MAX;
    clay_commands_load_provider(&c, &types[1]);
    assert(clay_commands_provider_generation(&c, "two") == current);
    while (c.providers.count) {
        ClayConnectedProvider *p = clay_array_get(&c.providers, 0);
        assert(clay_config_save(p->config) == 0);
        assert(clay_commands_logout_provider(&c, p->type->id) == 0);
    }
    clay_array_free(&c.providers);
}


typedef struct {
    pthread_mutex_t lock;
    pthread_cond_t cond;
    pthread_t main_thread;
    ClayScope *worker_scope;
    ClayScope *main_scope;
    int entered;
    int release;
    int calls;
} CompletionProbe;

static int completion_probe(ClayScope *scope, const ClayOpenAICallbacks *callbacks, void *userdata) {
    CompletionProbe *p = userdata;
    pthread_mutex_lock(&p->lock);
    p->calls++;
    if (pthread_equal(pthread_self(), p->main_thread)) {
        p->main_scope = scope;
    } else {
        p->worker_scope = scope;
        p->entered = 1;
        pthread_cond_broadcast(&p->cond);
        while (!p->release) pthread_cond_wait(&p->cond, &p->lock);
    }
    pthread_mutex_unlock(&p->lock);
    if (callbacks && callbacks->on_token) callbacks->on_token("offline summary", callbacks->userdata);
    return 0;
}

static void *run_batch(void *arg) {
    ClayJson *result = call(arg, "{\"tasks\":[{\"description\":\"branch\",\"prompt\":\"test\"}]}");
    assert(ok(result));
    clay_json_free(result);
    return NULL;
}

typedef struct {
    pthread_barrier_t barrier;
    int results[2];
    pthread_mutex_t lock;
    int next;
} CancelProbe;

static int cancel_probe(ClayScope *scope, const ClayOpenAICallbacks *callbacks, void *userdata) {
    (void)scope;
    CancelProbe *p = userdata;
    pthread_mutex_lock(&p->lock);
    int index = p->next++;
    pthread_mutex_unlock(&p->lock);
    pthread_barrier_wait(&p->barrier);
    pthread_barrier_wait(&p->barrier);
    p->results[index] = callbacks->should_abort(callbacks->userdata);
    return 1;
}

static void *run_cancel_batch(void *arg) {
    ClayJson *result = call(arg, "{\"tasks\":[{\"description\":\"a\",\"prompt\":\"a\"},{\"description\":\"b\",\"prompt\":\"b\"}]}");
    assert(!ok(result));
    clay_json_free(result);
    return NULL;
}

static void test_completion_scopes(void) {
    ClayCommands c = {0};
    clay_array_init(&c.providers, sizeof(ClayConnectedProvider));
    clay_array_init(&c.tasks, sizeof(ClayBackgroundTask *));
    clay_array_init(&c.plan.todos, sizeof(ClayTodoItem));
    clay_array_init(&c.mcp_bindings, sizeof(ClayMcpToolBinding));
    clay_array_init(&c.mcp_servers, sizeof(ClayMcpServer *));
    c.mcp_connect_attempted = 1;
    c.mode = CLAY_MODE_ACT;
    c.scopes = clay_scope_registry_create();
    c.chat = clay_chat_create("system");
    assert(c.scopes && c.chat);
    assert(setenv("OPENAI_API_KEY", "offline-key", 1) == 0);
    const ClayProviderType *type = clay_commands_find_provider_type("openai");
    assert(type);
    clay_commands_load_provider(&c, type);
    assert(clay_commands_find_provider(&c, "openai"));
    c.selected_provider = "openai";
    c.selected_model = "model-a";
    CompletionProbe p = {.main_thread = pthread_self()};
    assert(!pthread_mutex_init(&p.lock, NULL));
    assert(!pthread_cond_init(&p.cond, NULL));
    c.completion_test_hook = completion_probe;
    c.completion_test_userdata = &p;
    pthread_t batch;
    assert(!pthread_create(&batch, NULL, run_batch, &c));
    pthread_mutex_lock(&p.lock);
    while (!p.entered) pthread_cond_wait(&p.cond, &p.lock);
    pthread_mutex_unlock(&p.lock);
    assert(clay_commands_run_completion(&c, NULL, NULL, 1, NULL, NULL) == 0);
    assert(p.main_scope && p.main_scope == p.worker_scope);
    ClayScope *old = p.main_scope;
    assert(clay_scope_get(c.scopes, clay_commands_provider_generation(&c, "openai"),
                          clay_commands_find_provider(&c, "openai")->config->base_url, "model-a") == old);
    pthread_mutex_lock(&p.lock);
    p.release = 1;
    pthread_cond_broadcast(&p.cond);
    pthread_mutex_unlock(&p.lock);
    assert(!pthread_join(batch, NULL));
    CancelProbe cancel = {0};
    assert(!pthread_barrier_init(&cancel.barrier, NULL, 3));
    assert(!pthread_mutex_init(&cancel.lock, NULL));
    c.completion_test_hook = cancel_probe;
    c.completion_test_userdata = &cancel;
    clay_term_init();
    assert(!pthread_create(&batch, NULL, run_cancel_batch, &c));
    pthread_barrier_wait(&cancel.barrier);
    assert(!raise(SIGINT));
    pthread_barrier_wait(&cancel.barrier);
    assert(!pthread_join(batch, NULL));
    assert(cancel.results[0] && cancel.results[1]);
    assert(!clay_term_take_interrupt());
    cancel.next = 0;
    assert(!pthread_create(&batch, NULL, run_cancel_batch, &c));
    pthread_barrier_wait(&cancel.barrier);
    pthread_barrier_wait(&cancel.barrier);
    assert(!pthread_join(batch, NULL));
    assert(!cancel.results[0] && !cancel.results[1]);
    pthread_barrier_destroy(&cancel.barrier);
    pthread_mutex_destroy(&cancel.lock);
    c.completion_test_hook = completion_probe;
    c.completion_test_userdata = &p;
    c.selected_model = "model-b";
    assert(clay_commands_run_completion(&c, NULL, NULL, 1, NULL, NULL) == 0);
    assert(p.main_scope != old);
    c.selected_model = "model-a";
    clay_commands_load_provider(&c, type);
    assert(clay_commands_run_completion(&c, NULL, NULL, 1, NULL, NULL) == 0);
    assert(p.main_scope != old);
    for (int i = 1; i <= 3; i++) {
        int calls = p.calls;
        c.selected_model = "failure-model";
        clay_scope_test_fail_allocation(c.scopes, i);
        assert(clay_commands_run_completion(&c, NULL, NULL, 1, NULL, NULL) == -1);
        assert(p.calls == calls);
    }
    clay_scope_test_fail_allocation(c.scopes, 0);
    assert(clay_commands_run_completion(&c, NULL, NULL, 1, NULL, NULL) == 0);

    /* Exercise both subscription clients and Grok's API-key path offline. */
    assert(setenv("GROK_ACCESS_TOKEN", "offline-access", 1) == 0);
    assert(setenv("GROK_REFRESH_TOKEN", "offline-refresh", 1) == 0);
    const char *ids[] = {"openai-codex", "grok"};
    for (size_t i = 0; i < 2; i++) {
        const ClayProviderType *t = clay_commands_find_provider_type(ids[i]);
        clay_commands_load_provider(&c, t);
        assert(clay_commands_find_provider(&c, ids[i]));
        c.selected_provider = (char *)ids[i];
        assert(clay_commands_run_completion(&c, NULL, NULL, 1, NULL, NULL) == 0);
        assert(p.main_scope == clay_scope_get(c.scopes,
            clay_commands_provider_generation(&c, ids[i]),
            clay_commands_find_provider(&c, ids[i])->config->base_url, c.selected_model));
        assert(p.main_scope != old);
    }
    assert(unsetenv("GROK_ACCESS_TOKEN") == 0);
    assert(unsetenv("GROK_REFRESH_TOKEN") == 0);
    assert(setenv("XAI_API_KEY", "offline-key", 1) == 0);
    clay_commands_load_provider(&c, clay_commands_find_provider_type("grok"));
    assert(!clay_commands_find_provider(&c, "grok")->grok_client);
    assert(clay_commands_run_completion(&c, NULL, NULL, 1, NULL, NULL) == 0);
    assert(p.main_scope == clay_scope_get(c.scopes,
        clay_commands_provider_generation(&c, "grok"),
        clay_commands_find_provider(&c, "grok")->config->base_url, c.selected_model));
    c.selected_provider = "openai";
    ClayConnectedProvider *provider = clay_commands_find_provider(&c, "openai");
    assert(clay_config_save(provider->config) == 0);
    c.selected_provider = NULL;
    c.selected_model = NULL;
    assert(clay_commands_logout_provider(&c, "openai") == 0);
    while (c.providers.count) {
        ClayConnectedProvider *p = clay_array_get(&c.providers, 0);
        assert(clay_config_save(p->config) == 0);
        assert(clay_commands_logout_provider(&c, p->type->id) == 0);
    }
    clay_array_free(&c.providers);
    clay_scope_registry_destroy(c.scopes);
    clay_chat_destroy(c.chat);
    clay_array_free(&c.tasks);
    clay_array_free(&c.plan.todos);
    clay_array_free(&c.mcp_bindings);
    clay_array_free(&c.mcp_servers);
    pthread_cond_destroy(&p.cond);
    pthread_mutex_destroy(&p.lock);
}

int main(void) {
    test_codex_concurrent_snapshot();
    test_grok_concurrent_snapshot();
    test_interrupt_consumed_once();
    char template[] = "/tmp/clay_test_subagent_XXXXXX";
    char *home = mkdtemp(template);
    assert(home);
    assert(setenv("HOME", home, 1) == 0);

    test_scope_wait();
    test_scopes();
    test_concurrent_scope_failure();
    test_provider_generations();
    test_completion_scopes();

    ClayCommands commands;
    memset(&commands, 0, sizeof(commands));
    clay_array_init(&commands.tasks, sizeof(ClayBackgroundTask *));
    clay_array_init(&commands.plan.todos, sizeof(ClayTodoItem));
    clay_array_init(&commands.mcp_bindings, sizeof(ClayMcpToolBinding));
    clay_array_init(&commands.mcp_servers, sizeof(ClayMcpServer *));
    commands.mcp_connect_attempted = 1; /* no servers to dial in a test */
    commands.mode = CLAY_MODE_ACT;

    ClayJson *result = call(&commands, "{\"tasks\":[]}");
    assert(!ok(result));
    assert(strstr(error_of(result), "non-empty"));
    clay_json_free(result);

    /* One call fans out, but not without limit. */
    result = call(&commands,
                  "{\"tasks\":[{\"description\":\"a\",\"prompt\":\"p\"},"
                  "{\"description\":\"b\",\"prompt\":\"p\"},"
                  "{\"description\":\"c\",\"prompt\":\"p\"},"
                  "{\"description\":\"d\",\"prompt\":\"p\"},"
                  "{\"description\":\"e\",\"prompt\":\"p\"}]}");
    assert(!ok(result));
    assert(strstr(error_of(result), "at most 4"));
    clay_json_free(result);

    result = call(&commands, "{\"tasks\":[{\"prompt\":\"do the thing\"}]}");
    assert(!ok(result));
    assert(strstr(error_of(result), "description"));
    clay_json_free(result);

    result = call(&commands, "{\"tasks\":[{\"description\":\"step one\"}]}");
    assert(!ok(result));
    assert(strstr(error_of(result), "prompt"));
    clay_json_free(result);

    /* Plan mode refuses before anything is spawned. */
    commands.mode = CLAY_MODE_PLAN;
    result = call(&commands, "{\"tasks\":[{\"description\":\"step one\",\"prompt\":\"do it\"}]}");
    assert(!ok(result));
    assert(strstr(error_of(result), "Plan mode"));
    clay_json_free(result);
    commands.mode = CLAY_MODE_ACT;

    /* Without a chat there is nowhere to record the run. */
    result = call(&commands, "{\"tasks\":[{\"description\":\"step one\",\"prompt\":\"do it\"}]}");
    assert(!ok(result));
    assert(strstr(error_of(result), "chat"));
    clay_json_free(result);

    /* A subagent gets the working tools but neither the user's ear nor the
       ability to spawn more subagents. */
    ClayToolSet delegated;
    clay_commands_tools_build(&commands, &commands.plan, &delegated, 0);
    assert(has_tool(&delegated, "read"));
    assert(has_tool(&delegated, "shell_exec"));
    assert(has_tool(&delegated, "todowrite"));
    assert(!has_tool(&delegated, "subagent"));
    assert(!has_tool(&delegated, "ask_user"));
    size_t delegated_count = delegated.tools.count;
    clay_commands_tools_free(&delegated);

    ClayToolSet top;
    clay_commands_tools_build(&commands, &commands.plan, &top, 1);
    assert(has_tool(&top, "subagent"));
    assert(has_tool(&top, "ask_user"));
    assert(top.tools.count == delegated_count + 2);
    clay_commands_tools_free(&top);

    /* Each run gets its own file under the chat. */
    ClayChat *chat = clay_chat_create("system");
    assert(chat);
    char *first = clay_chat_subagent_path(chat, "aaaa");
    char *second = clay_chat_subagent_path(chat, "bbbb");
    assert(first && second && strcmp(first, second) != 0);
    assert(strstr(first, "/subagents/aaaa.json"));
    assert(clay_term_write_file_atomic(first, "{}", 2) == 0);
    free(first);
    free(second);
    clay_chat_destroy(chat);

    ClayJson *schema = subagent_schema();
    ClayJson *properties = clay_json_object_get(schema, "properties");
    ClayJson *tasks = clay_json_object_get(properties, "tasks");
    assert(tasks);
    ClayJson *task = clay_json_object_get(tasks, "items");
    assert(clay_json_object_get(clay_json_object_get(task, "properties"), "description"));
    assert(clay_json_object_get(clay_json_object_get(task, "properties"), "prompt"));
    assert(clay_json_array_count(clay_json_object_get(schema, "required")) == 1);
    clay_json_free(schema);

    clay_array_free(&commands.tasks);
    clay_array_free(&commands.plan.todos);
    clay_array_free(&commands.mcp_bindings);
    clay_array_free(&commands.mcp_servers);
    printf("subagent tests passed\n");
    return 0;
}
